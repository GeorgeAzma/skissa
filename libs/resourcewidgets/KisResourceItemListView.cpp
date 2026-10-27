/* This file is part of the KDE project
 * SPDX-FileCopyrightText: 2019 Wolthera van Hövell tot Westerflier <griffinvalley@gmail.com>
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "KisResourceItemListView.h"

#include <QCoreApplication>
#include <QDrag>
#include <QEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QHoverEvent>
#include <QScroller>
#include <QScrollBar>
#include <QHelpEvent>

#include "KisIconToolTip.h"
#include <KisResourceModel.h>


struct  Q_DECL_HIDDEN KisResourceItemListView::Private
{
    Private(KisResourceItemListView *_q) : tip(_q) {}

    ListViewMode viewMode = ListViewMode::IconGrid;
    bool strictSelectionMode {false};
    KisIconToolTip tip;

    QScroller* scroller {0};
    QString prev_scrollbar_style;

    QSize requestedItemSize = QSize(56, 56);
    /// Where the pointer is over the items, while it is
    QPoint hoverPos {-1, -1};
    QString dragMimeType;
    QString dragPrefix;
    QPersistentModelIndex pressed; ///< the item a drag takes out
};

KisResourceItemListView::KisResourceItemListView(QWidget *parent)
    : QListView(parent)
    , m_d(new Private(this))
{
    setSelectionMode(QAbstractItemView::SingleSelection);
    setContextMenuPolicy(Qt::DefaultContextMenu);   
    setResizeMode(QListView::Adjust);
    setUniformItemSizes(true);

    // Default configuration
    setViewMode(QListView::IconMode);
    setGridSize(QSize(56, 56));
    setIconSize(QSize(56, 56));
    setResizeMode(QListView::Adjust);
    setUniformItemSizes(true);

    m_d->scroller = KisKineticScroller::createPreconfiguredScroller(this);
    if (m_d->scroller) {
        connect(m_d->scroller, SIGNAL(stateChanged(QScroller::State)), this, SLOT(slotScrollerStateChange(QScroller::State)));
    }

    connect(this, SIGNAL(clicked(QModelIndex)), SIGNAL(currentResourceClicked(const QModelIndex &)));

    // the item under the pointer is lit (the style gives hover only to buttons and the like)
    viewport()->setAttribute(Qt::WA_Hover);
    // after a scroll, the item lit is the one now under the pointer, not the one that was (Qt waits for a move)
    auto hoverAgain = [this]() {
        if (m_d->hoverPos.x() < 0) return;
        QHoverEvent moved(QEvent::HoverMove, m_d->hoverPos, m_d->hoverPos);
        QCoreApplication::sendEvent(viewport(), &moved);
    };
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, hoverAgain);
    connect(horizontalScrollBar(), &QScrollBar::valueChanged, this, hoverAgain);

    m_d->prev_scrollbar_style = horizontalScrollBar()->styleSheet();
}

KisResourceItemListView::~KisResourceItemListView()
{
}

void KisResourceItemListView::setListViewMode(ListViewMode viewMode)
{
    m_d->viewMode = viewMode;

    auto restoreScrollbar = [&, this] () {
        horizontalScrollBar()->setStyleSheet(m_d->prev_scrollbar_style);
        setVerticalScrollBarPolicy(Qt::ScrollBarPolicy::ScrollBarAsNeeded);
    };

    switch (viewMode) {
    case ListViewMode::IconGrid: {
        setViewMode(ViewMode::IconMode);
        setFlow(Flow::LeftToRight);
        setWrapping(true);
        restoreScrollbar();

        setItemSize(m_d->requestedItemSize);
        break;
    }
    case ListViewMode::IconStripHorizontal: {
        setViewMode(ViewMode::IconMode);
        setFlow(Flow::LeftToRight);
        setWrapping(false);

        // this is the only way to hide it and not have it occupy space
        horizontalScrollBar()->setStyleSheet("QScrollBar::horizontal {height: 0px;}");
        setVerticalScrollBarPolicy(Qt::ScrollBarPolicy::ScrollBarAlwaysOff);

        setItemSize(m_d->requestedItemSize);
        break;
    }
    case ListViewMode::Detail: {
        setViewMode(ViewMode::ListMode);
        setFlow(Flow::TopToBottom);
        setWrapping(false);
        restoreScrollbar();
        setItemSize(m_d->requestedItemSize);
        // horizontalScrollBar()->setStyleSheet(m_d->prev_scrollbar_style);
        setVerticalScrollBarPolicy(Qt::ScrollBarPolicy::ScrollBarAsNeeded);
        setHorizontalScrollBarPolicy(Qt::ScrollBarPolicy::ScrollBarAsNeeded);
        break;
    }
    }
}

void KisResourceItemListView::setItemSize(QSize size)
{
    m_d->requestedItemSize = size;

    switch (m_d->viewMode) {
    case ListViewMode::IconGrid: {
        setGridSize(size);
        setIconSize(size);
        break;
    }
    case ListViewMode::IconStripHorizontal: {
        // you can not set the item size in strip mode
        // it is configured automatically based on size
        break;
    }
    case ListViewMode::Detail: {
        const int w = width() - horizontalScrollBar()->width();
        setGridSize(QSize(w, size.height()));
        setIconSize(QSize(size));
        break;
    }
    }
}

void KisResourceItemListView::setStrictSelectionMode(bool enable)
{
    m_d->strictSelectionMode = enable;
}

void KisResourceItemListView::setFixedToolTipThumbnailSize(const QSize &size)
{
    m_d->tip.setFixedToolTipThumbnailSize(size);
}

void KisResourceItemListView::setToolTipShouldRenderCheckers(bool value)
{
    m_d->tip.setToolTipShouldRenderCheckers(value);
}

void KisResourceItemListView::rowsAboutToBeRemoved(const QModelIndex &parent, int start, int end)
{
    // QAbstractItemView moves the current index if the row it points to is removed,
    // which we don't want for strict selections
    QModelIndex current = currentIndex();
    if (selectionMode() == SingleSelection
            && m_d->strictSelectionMode
            && current.isValid()
            && current.row() >= start
            && current.row() <= end) {

        selectionModel()->clear();
    }
    QListView::rowsAboutToBeRemoved(parent, start, end);
}

void KisResourceItemListView::selectionChanged(const QItemSelection &selected, const QItemSelection &deselected)
{
    // base class takes care of viewport updates
    QListView::selectionChanged(selected, deselected);

    if (selected.isEmpty()) {
        Q_EMIT currentResourceChanged(QModelIndex());
    }
    else {
        Q_EMIT currentResourceChanged(selected.indexes().first());
    }
}

void KisResourceItemListView::setDragKey(const QString &mimeType, const QString &prefix)
{
    m_d->dragMimeType = mimeType;
    m_d->dragPrefix = prefix;
    setMovement(QListView::Static);
    setDragEnabled(true);
    setDragDropMode(QAbstractItemView::DragOnly);
}

void KisResourceItemListView::startDrag(Qt::DropActions)
{
    const QModelIndex dragged = m_d->pressed;
    if (m_d->dragMimeType.isEmpty() || !dragged.isValid()) return;
    const QString name = dragged.data(Qt::UserRole + KisAbstractResourceModel::Name).toString();
    // owned by the application: the list may be rebuilt while the drag runs
    auto *drag = new QDrag(qApp);
    auto *mime = new QMimeData();
    mime->setData(m_d->dragMimeType, (m_d->dragPrefix + name).toUtf8());
    drag->setMimeData(mime);
    const QImage picture = dragged.data(Qt::UserRole + KisAbstractResourceModel::Thumbnail).value<QImage>();
    if (!picture.isNull()) drag->setPixmap(QPixmap::fromImage(picture.scaled(40, 40, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
    drag->exec(Qt::CopyAction);
    drag->deleteLater();
}

QItemSelectionModel::SelectionFlags KisResourceItemListView::selectionCommand(const QModelIndex &index, const QEvent *event) const
{
    // items can be dragged out: picked on release, so taking one out doesn't pick it
    if (!m_d->dragMimeType.isEmpty() && event && event->type() == QEvent::MouseButtonPress) {
        return QItemSelectionModel::NoUpdate;
    }
    // ...and on the release, which a single-selection view otherwise ignores
    if (!m_d->dragMimeType.isEmpty() && event && event->type() == QEvent::MouseButtonRelease && index.isValid()
        && index == m_d->pressed && selectionMode() == SingleSelection) {
        return QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows;
    }
    QItemSelectionModel::SelectionFlags cmd = QListView::selectionCommand(index, event);

    // avoid deselecting the current item by Ctrl-clicking in single selection mode
    if (selectionMode() == SingleSelection
            && m_d->strictSelectionMode
            && cmd.testFlag(QItemSelectionModel::Deselect)) {

        cmd = QItemSelectionModel::NoUpdate;
    }
    return cmd;
}

void KisResourceItemListView::contextMenuEvent(QContextMenuEvent *event)
{
    QListView::contextMenuEvent(event);
    Q_EMIT contextMenuRequested(event->globalPos());
}

bool KisResourceItemListView::viewportEvent(QEvent *event)
{
    if (!model()) return true;

    if (event->type() == QEvent::MouseButtonPress) {
        m_d->pressed = indexAt(static_cast<QMouseEvent *>(event)->pos());
    }
    if (event->type() == QEvent::HoverEnter || event->type() == QEvent::HoverMove) {
        m_d->hoverPos = static_cast<QHoverEvent *>(event)->pos();
    } else if (event->type() == QEvent::HoverLeave) {
        m_d->hoverPos = QPoint(-1, -1);
    }

    if (event->type() == QEvent::ToolTip) {
        QHelpEvent *he = static_cast<QHelpEvent *>(event);
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
        QStyleOptionViewItem option = viewOptions();
#else
        QStyleOptionViewItem option;
        initViewItemOption(&option);
#endif
        QModelIndex index = model()->buddy(indexAt(he->pos()));
        if (index.isValid()) {
            option.rect = visualRect(index);
            m_d->tip.showTip(he->pos(), option, index);
            return true;
        }
        m_d->tip.hide();
    }

    return QListView::viewportEvent(event);
}

void KisResourceItemListView::resizeEvent(QResizeEvent *event)
{
    QListView::resizeEvent(event);

    switch (m_d->viewMode) {
    case ListViewMode::IconStripHorizontal: {
        const int height = event->size().height();
        setGridSize(QSize(height, height));
        setIconSize(QSize(height, height));
        break;
    }
    case ListViewMode::Detail: {
        setItemSize(m_d->requestedItemSize);
        break;
    }
    case ListViewMode::IconGrid: {
        break;
    }
    }
    scrollTo(currentIndex(), QAbstractItemView::PositionAtCenter);
}
