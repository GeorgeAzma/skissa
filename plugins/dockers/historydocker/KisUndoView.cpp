/* This file is part of the KDE project
 * SPDX-FileCopyrightText: 2010 Matus Talcik <matus.talcik@gmail.com>
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */
/****************************************************************************
**
** Copyright (C) 2010 Nokia Corporation and/or its subsidiary(-ies).
** All rights reserved.
** Contact: Nokia Corporation (qt-info@nokia.com)
**
** This file is part of the QtGui module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL$
** No Commercial Usage
** This file contains pre-release code and may not be distributed.
** You may use this file in accordance with the terms and conditions
** contained in the Technology Preview License Agreement accompanying
** this package.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU Lesser General Public License version 2.1 requirements
** will be met: http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, Nokia gives you certain additional
** rights.  These rights are described in the Nokia Qt LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
** If you have questions regarding the use of this file, please contact
** Nokia at qt-info@nokia.com.
**
**
**
**
**
**
**
**
** $QT_END_LICENSE$
**
****************************************************************************/
#include <kundo2qstack.h>
#include "KisUndoView.h"
#include "KisUndoModel.h"

#ifndef QT_NO_UNDOVIEW

#include <kundo2group.h>
#include <QAbstractItemModel>
#include <QPointer>
#include <QIcon>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QWidgetAction>
#include <QGridLayout>
#include <kis_config.h>
#include <KisLanczos.h>
#include <KSharedConfig>
#include <KConfigGroup>
#include <QStyledItemDelegate>
#include <QWheelEvent>
#include <QScrollBar>
#include <QTimer>
#include <KisKineticScroller.h>

#include "kis_double_parse_spin_box.h"
#include "kis_int_parse_spin_box.h"

/*!
    \class KisUndoView
    \brief The KisUndoView class displays the contents of a KUndo2QStack.
    \since 4.2

    \ingroup advanced

    KisUndoView is a QListView which displays the list of commands pushed on an undo stack.
    The most recently executed command is always selected. Selecting a different command
    results in a call to KUndo2QStack::setIndex(), rolling the state of the document
    backwards or forward to the new command.

    The stack can be set explicitly with setStack(). Alternatively, a KUndo2Group object can
    be set with setGroup(). The view will then update itself automatically whenever the
    active stack of the group changes.

    \image KisUndoView.png
*/

namespace
{
KConfigGroup historyGroup()
{
    return KSharedConfig::openConfig()->group(QStringLiteral("StudioHistory"));
}

/// Shows each step's picture at the list's size; the model keeps them larger for the history line
class PictureDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    int size {35};

protected:
    void initStyleOption(QStyleOptionViewItem *option, const QModelIndex &index) const override
    {
        QStyledItemDelegate::initStyleOption(option, index);
        const QImage picture = index.data(Qt::DecorationRole).value<QImage>();
        if (picture.isNull()) return;
        const qreal ratio = picture.devicePixelRatio();
        const QSize fitted = (picture.size() / ratio).scaled(QSize(size, size), Qt::KeepAspectRatio).expandedTo(QSize(1, 1));
        const QString key = QString::number(picture.cacheKey()) + QLatin1Char('/') + QString::number(size);
        if (key != m_key) {
            QImage shown = KisLanczos::scaled(picture, fitted * ratio);
            shown.setDevicePixelRatio(ratio);
            m_key = key;
            m_icon = QIcon(QPixmap::fromImage(shown));
        }
        option->icon = m_icon;
        option->decorationSize = fitted;
    }

private:
    // the last one made: each row is asked for more than once while painted
    mutable QString m_key;
    mutable QIcon m_icon;
};
}

class KisUndoViewPrivate
{
public:
    KisUndoViewPrivate() {}

#ifndef QT_NO_UNDOGROUP
    QPointer<KUndo2Group> group {0};
#endif
    KisUndoModel *model {0};
    KisUndoView *q {0};
    PictureDelegate *pictures {nullptr};
    /// Whether the list follows its newest step, or keeps where it was scrolled to
    bool atEnd {true};
    int scrolledTo {0};

    void init(KisUndoView* view);
};

void KisUndoViewPrivate::init(KisUndoView* view)
{
    q = view;
    model = new KisUndoModel(q);
    model->setDevicePixelRatio(view->devicePixelRatioF());
    q->setModel(model);
    q->setSelectionModel(model->selectionModel());
    pictures = new PictureDelegate(q);
    pictures->size = qBound(20, historyGroup().readEntry("PictureSize", 35), 128);
    q->setItemDelegate(pictures);
    // each change rebuilds the list: Qt's scroll to the current row made it jump while painting
    q->setAutoScroll(false);
}

/*!
    Constructs a new view with parent \a parent.
*/

KisUndoView::KisUndoView(QWidget *parent)
    : QListView(parent)
    , d(new KisUndoViewPrivate)
{
    d->init(this);

    auto noteScroll = [this]() {
        QScrollBar *bar = verticalScrollBar();
        d->atEnd = bar->value() >= bar->maximum() - 2;
        d->scrolledTo = bar->value();
    };
    // after the action: actionTriggered comes before the bar moves
    connect(verticalScrollBar(), &QAbstractSlider::actionTriggered, this, [noteScroll]() { QTimer::singleShot(0, noteScroll); });
    connect(verticalScrollBar(), &QAbstractSlider::sliderReleased, this, noteScroll);

    QScroller *scroller = KisKineticScroller::createPreconfiguredScroller(this);
    if (scroller) {
        connect(scroller, SIGNAL(stateChanged(QScroller::State)),
                this, SLOT(slotScrollerStateChanged(QScroller::State)));
        connect(scroller, &QScroller::stateChanged, this, [noteScroll](QScroller::State state) {
            if (state == QScroller::Inactive) noteScroll();
        });
    }
}

/*!
    Constructs a new view with parent \a parent and sets the observed stack to \a stack.
*/

void KisUndoView::updateGeometries()
{
    QListView::updateGeometries();
    QScrollBar *bar = verticalScrollBar();
    bar->setValue(d->atEnd ? bar->maximum() : d->scrolledTo);
}

void KisUndoView::wheelEvent(QWheelEvent *event)
{
    if (!(event->modifiers() & Qt::ControlModifier)) {
        QListView::wheelEvent(event);
        return;
    }
    event->accept();
    const int steps = event->angleDelta().y() / 120;
    if (!steps) return;
    const int size = qBound(20, d->pictures->size + 5 * steps, 128);
    if (size == d->pictures->size) return;
    d->pictures->size = size;
    historyGroup().writeEntry("PictureSize", size);
    scheduleDelayedItemsLayout();
    viewport()->update();
}

KisUndoView::KisUndoView(KUndo2QStack *stack, QWidget *parent)
    : KisUndoView(parent)
{
    setStack(stack);
}

#ifndef QT_NO_UNDOGROUP

/*!
    Constructs a new view with parent \a parent and sets the observed group to \a group.

    The view will update itself automatically whenever the active stack of the group changes.
*/

KisUndoView::KisUndoView(KUndo2Group *group, QWidget *parent)
    : KisUndoView(parent)
{
    setGroup(group);
}

#endif // QT_NO_UNDOGROUP

/*!
    Destroys this view.
*/

KisUndoView::~KisUndoView()
{
    delete d;

}

/*!
    Returns the stack currently displayed by this view. If the view is looking at a
    KUndo2Group, this the group's active stack.

    \sa setStack() setGroup()
*/

KUndo2QStack *KisUndoView::stack() const
{

    return d->model->stack();
}

/*!
    Sets the stack displayed by this view to \a stack. If \a stack is 0, the view
    will be empty.

    If the view was previously looking at a KUndo2Group, the group is set to 0.

    \sa stack() setGroup()
*/

void KisUndoView::setStack(KUndo2QStack *stack)
{

#ifndef QT_NO_UNDOGROUP
    setGroup(0);
#endif
    d->model->setStack(stack);
}

#ifndef QT_NO_UNDOGROUP

/*!
    Sets the group displayed by this view to \a group. If \a group is 0, the view will
    be empty.

    The view will update itself automatically whenever the active stack of the group changes.

    \sa group() setStack()
*/

void KisUndoView::setGroup(KUndo2Group *group)
{


    if (d->group == group)
        return;

    if (d->group != 0) {
        disconnect(d->group, SIGNAL(activeStackChanged(KUndo2QStack*)),
                d->model, SLOT(setStack(KUndo2QStack*)));
    }

    d->group = group;

    if (d->group != 0) {
        connect(d->group, SIGNAL(activeStackChanged(KUndo2QStack*)),
                d->model, SLOT(setStack(KUndo2QStack*)));
        d->model->setStack(d->group->activeStack());
    } else {
        d->model->setStack(0);
    }
}

/*!
    Returns the group displayed by this view.

    If the view is not looking at group, this function returns 0.

    \sa setGroup() setStack()
*/

KUndo2Group *KisUndoView::group() const
{

    return d->group;
}

#endif // QT_NO_UNDOGROUP

/*!
    \property KisUndoView::emptyLabel
    \brief the label used for the empty state.

    The empty label is the topmost element in the list of commands, which represents
    the state of the document before any commands were pushed on the stack. The default
    is the string "<empty>".
*/

void KisUndoView::setEmptyLabel(const QString &label)
{

    d->model->setEmptyLabel(label);
}

QString KisUndoView::emptyLabel() const
{

    return d->model->emptyLabel();
}

/*!
    \property KisUndoView::cleanIcon
    \brief the icon used to represent the clean state.

    A stack may have a clean state set with KUndo2QStack::setClean(). This is usually
    the state of the document at the point it was saved. KisUndoView can display an
    icon in the list of commands to show the clean state. If this property is
    a null icon, no icon is shown. The default value is the null icon.
*/

void KisUndoView::setCleanIcon(const QIcon &icon)
{

    d->model->setCleanIcon(icon);

}

QIcon KisUndoView::cleanIcon() const
{

    return d->model->cleanIcon();
}

void KisUndoView::setCanvas(KisCanvas2 *canvas) {
    d->model->setCanvas(canvas);
}

#endif // QT_NO_UNDOVIEW
