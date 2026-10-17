/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: LGPL-2.0-or-later
 */
#include "KisResourceTagMenu.h"

#include <QMouseEvent>
#include <QPainter>
#include <QRegularExpression>
#include <QWidgetAction>

#include <klocalizedstring.h>

#include <KisModernStyle.h>
#include <KisTag.h>
#include <KisTagModel.h>
#include <KisTagResourceModel.h>

#include "KisTagChooserWidget.h"
#include "TagActions.h"

namespace {

/// A tag in the menu: its name, and ✕ to take the resource out of it or + to put it in
class TagRow : public QWidget
{
    // a class of its own: a press on it toggles the tag
    Q_OBJECT
public:
    TagRow(KoResourceSP resource, KisTagSP tag, bool in, QWidget *parent)
        : QWidget(parent)
        , m_resource(resource)
        , m_tag(tag)
        , m_in(in)
    {
        setMouseTracking(true);
        setAttribute(Qt::WA_Hover);
        setFixedHeight(28);
        setMinimumWidth(qMax(180, fontMetrics().horizontalAdvance(tag->name()) + 64));
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QColor ink = palette().color(QPalette::Text);
        const QRectF box = QRectF(rect()).adjusted(KisModernStyle::space1, 1, -KisModernStyle::space1, -1);
        if (underMouse()) {
            QColor hover = ink;
            hover.setAlphaF(0.08);
            painter.fillPath(KisModernStyle::squircle(box, KisModernStyle::cornerRadius), hover);
        }
        // the ones it is in in ink, the others quiet
        const QColor text = m_in ? ink : KisModernStyle::secondaryInk(ink);
        painter.setPen(text);
        const QRectF name = box.adjusted(KisModernStyle::space2, 0, -28, 0);
        painter.drawText(name, Qt::AlignLeft | Qt::AlignVCenter, fontMetrics().elidedText(m_tag->name(), Qt::ElideRight, int(name.width())));

        const QPointF mark(box.right() - 14, box.center().y());
        const qreal arm = m_in ? 3.5 : 4.5;
        painter.setPen(QPen(underMouse() ? ink : text, 1.5, Qt::SolidLine, Qt::RoundCap));
        if (m_in) {
            painter.drawLine(mark + QPointF(-arm, -arm), mark + QPointF(arm, arm));
            painter.drawLine(mark + QPointF(-arm, arm), mark + QPointF(arm, -arm));
        } else {
            painter.drawLine(mark + QPointF(-arm, 0), mark + QPointF(arm, 0));
            painter.drawLine(mark + QPointF(0, -arm), mark + QPointF(0, arm));
        }
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton || !rect().contains(event->pos())) return;
        KisTagResourceModel model(m_resource->resourceType().first);
        if (m_in) model.untagResources(m_tag, {m_resource->resourceId()});
        else model.tagResources(m_tag, {m_resource->resourceId()});
        m_in = !m_in;
        update();
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        // the menu would take it as a click on nothing
        event->accept();
    }

    bool event(QEvent *event) override
    {
        if (event->type() == QEvent::HoverEnter || event->type() == QEvent::HoverLeave) update();
        return QWidget::event(event);
    }

private:
    KoResourceSP m_resource;
    KisTagSP m_tag;
    bool m_in {false};
};

bool isSpecial(const KisTagSP &tag)
{
    return tag.isNull() || !tag->valid() || tag->id() < 0;
}

} // namespace

KisResourceTagMenu::KisResourceTagMenu(KoResourceSP resource, KisTagChooserWidget *tagChooser)
{
    // as the brush card names it: without the sorting prefix of Krita's own brushes ("b) ")
    QString name = resource->name().replace(QLatin1Char('_'), QLatin1Char(' '));
    name.remove(QRegularExpression(QStringLiteral("^[a-z]\\)\\s+")));
    QAction *label = new QAction(name, this);
    label->setIcon(QIcon(QPixmap::fromImage(resource->image())));
    addAction(label);
    addSeparator();

    const QString type = resource->resourceType().first;
    KisTagResourceModel tagged(type);
    tagged.setResourcesFilter(QVector<KoResourceSP>() << resource);
    QList<int> in;
    for (int i = 0; i < tagged.rowCount(); i++) {
        const KisTagSP tag = tagged.data(tagged.index(i, 0), Qt::UserRole + KisAllTagResourceModel::Tag).value<KisTagSP>();
        if (!isSpecial(tag)) in << tag->id();
    }

    KisTagModel tags(type);
    tags.sort(KisAllTagsModel::Name);
    QList<KisTagSP> first;
    QList<KisTagSP> rest;
    for (int i = 0; i < tags.rowCount(); i++) {
        const KisTagSP tag = tags.tagForIndex(tags.index(i, 0));
        if (isSpecial(tag)) continue;
        (in.contains(tag->id()) ? first : rest) << tag;
    }
    Q_FOREACH (const KisTagSP &tag, first + rest) {
        auto *row = new QWidgetAction(this);
        row->setDefaultWidget(new TagRow(resource, tag, in.contains(tag->id()), this));
        addAction(row);
    }

    NewTagResourceAction *newTag = new NewTagResourceAction(resource, this);
    // its own button adds it
    newTag->setIcon(QIcon());
    connect(newTag, SIGNAL(triggered(QString, KoResourceSP)), tagChooser, SLOT(addTag(QString, KoResourceSP)));
    addAction(newTag);
}

#include "KisResourceTagMenu.moc"
