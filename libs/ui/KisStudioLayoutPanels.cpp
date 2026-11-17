/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioLayout.h"
#include "KisStudioLayout_p.h"

// Panels: showing and folding them, flyouts, pinning, groups and lanes, where they sit on the canvas


void KisStudioLayout::closeFlyout()
{
    QDockWidget *dock = d->flyout;
    if (!dock) {
        return;
    }
    releaseFlyout();
    parkPanel(dock);
    syncRail();
}

void KisStudioLayout::releaseFlyout()
{
    // the keys go back to the canvas, not to whatever widget Qt would pick when the flyout holding them hides
    if (d->flyout && d->flyout->isAncestorOf(QApplication::focusWidget())) KisStudioTools::focusCanvas(d->mainWindow);
    d->flyout = nullptr;
    d->flyoutAnchor = nullptr;
    if (d->flyoutWindow) {
        d->flyoutWindow->release();
        d->flyoutWindow->hide();
    }
}

void KisStudioLayout::dockAway(QDockWidget *dock)
{
    dock->setFloating(false);
    // Qt can't re-dock a panel that was never in a dock area, so add it to one to keep it from being saved floating
    if (dock->isFloating()) {
        const bool hidden = dock->isHidden();
        d->mainWindow->addDockWidget(Qt::RightDockWidgetArea, dock);
        if (hidden) dock->hide();
    }
}

void KisStudioLayout::showPanel(QDockWidget *dock)
{
    if (d->active && d->panels.contains(dock)) {
        togglePanel(dock, true);
    } else {
        dock->show();
        dock->raise();
    }
}

void KisStudioLayout::togglePanel(QDockWidget *dock, bool show)
{
    // a pinned panel put away stays away, also at the next start; one step of the undo history
    const bool wasHidden = d->hiddenPinned.contains(dock->objectName());
    const bool hides = !show && dock != d->flyout && d->pinned.contains(dock->objectName());
    if (wasHidden != hides) {
        commitArrangement();
        if (hides) d->hiddenPinned.insert(dock->objectName());
        else d->hiddenPinned.remove(dock->objectName());
        savePinned();
    }
    if (show) {
        if (d->pinned.contains(dock->objectName())) {
            if (d->panelsTucked) setPanelsTucked(false);
            dock->show();
            dock->raise();
        } else {
            showFlyout(dock);
        }
    } else if (dock == d->flyout) {
        closeFlyout();
    } else {
        dock->hide();
    }
}

void KisStudioLayout::showFlyout(QDockWidget *dock, QWidget *anchor)
{
    if (d->flyout && d->flyout != dock) {
        closeFlyout();
    }
    if (d->flyout == dock) {
        d->flyoutAnchor = anchor;
        if (dock->isHidden()) dock->show();
        positionFlyout();
        d->flyoutWindow->raise();
        return;
    }

    d->flyout = dock;
    d->flyoutAnchor = anchor;
    if (!d->flyoutWindow) {
        d->flyoutWindow = new FlyoutWindow(d->mainWindow);
    }
    if (dock->isFloating()) dockAway(dock);
    d->mainWindow->removeDockWidget(dock);
    d->flyoutWindow->hold(dock);
    positionFlyout();
    KisStudioMotion::showFaded(d->flyoutWindow);
    d->flyoutWindow->raise();
    syncRail();
}

void KisStudioLayout::positionFlyout()
{
    QWidget *canvasArea = mdiArea() ? mdiArea()->parentWidget() : nullptr;
    if (!d->flyout || !d->flyoutWindow || !canvasArea) {
        return;
    }

    const QRect area = QRect(canvasArea->mapToGlobal(QPoint(0, 0)), canvasArea->size()).marginsRemoved(canvasInsets(canvasArea));
    const int margin = 6;
    int contentsWidth = d->flyout->widget() ? d->flyout->widget()->sizeHint().width() : 0;
    int contentsHeight = qMax(d->flyout->sizeHint().height(), d->flyout->minimumSizeHint().height());
    // a QML page scrolls inside and tells no height of its own
    if (d->flyout->widget() && d->flyout->widget()->inherits("KisQQuickWidget")) {
        contentsHeight = qMax(contentsHeight, area.height() * 2 / 3);
    }
    // a scroll area has a small size hint, so fit the flyout to what it scrolls
    QScrollArea *scroll = d->flyout->findChild<QScrollArea *>();
    if (scroll && scroll->widget()) {
        const int frame = 2 * scroll->frameWidth();
        const int header = d->flyout->titleBarWidget() ? d->flyout->titleBarWidget()->sizeHint().height() : 0;
        contentsWidth = qMax(contentsWidth, scroll->widget()->sizeHint().width() + frame + scroll->verticalScrollBar()->sizeHint().width());
        contentsHeight = qMax(contentsHeight, header + scroll->widget()->sizeHint().height() + frame + 12);
    }
    // stay within the canvas area, as the main window clips anything beyond it
    const int width = qMax(0, qMin(qBound(260, contentsWidth + 2, 420), area.width() - 2 * margin));

    // as tall as its contents, as from a button
    QRect global(area.right() + 1 - margin - width, area.top() + margin, width, qBound(80, contentsHeight, area.height() - 2 * margin));
    // below the panel icons when nothing is pinned beside them, so another panel is still a click away
    if (KisStudioPanelColumn *column = d->panelColumn; column && column->isVisible()) {
        const QRect strip(column->mapToGlobal(QPoint()), column->size());
        if (strip.intersects(global)) {
            global.moveTop(strip.bottom() + 1 + margin);
            global.setBottom(qMin(global.bottom(), area.bottom() - margin));
        }
    }
    // beside the panels docked on that side, rather than over them
    Q_FOREACH (KisStudioPanelColumn *other, d->columns()) {
        if (other == d->panelColumn || !other->isVisible() || other->isLeft()) continue;
        const QRect pill(other->mapToGlobal(QPoint()), other->size());
        if (pill.intersects(global)) global.moveRight(pill.left() - 1 - margin);
    }
    if (d->flyoutAnchor) {
        // below what opened it, as tall as its contents, beside the tool rail rather than over it
        int left = qMax(area.left() + margin, d->flyoutAnchor->mapToGlobal(QPoint(0, 0)).x());
        if (QWidget *rail = d->brushRail; rail && rail->isVisible()) {
            const QRect at(rail->mapToGlobal(QPoint()), rail->size());
            if (at.left() < left + width && at.right() >= left) {
                left = at.center().x() < area.center().x() ? at.right() + 1 + margin : at.left() - margin - width;
            }
        }
        left = qBound(area.left() + margin, left, area.right() + 1 - margin - width);
        global = QRect(left, area.top() + margin, width, qBound(80, contentsHeight, area.height() - 2 * margin));
        // above a button low in the window (the timeline's), so it opens beside what opened it
        const QRect at(d->flyoutAnchor->mapToGlobal(QPoint()), d->flyoutAnchor->size());
        if (at.center().y() > area.center().y()) {
            global.moveBottom(qMin(at.top(), area.bottom() + 1) - 1 - margin);
            if (global.top() < area.top() + margin) global.setTop(area.top() + margin);
        }
    }
    QWidget *parent = d->flyoutWindow->parentWidget();
    d->flyoutWindow->setGeometry(QRect(parent->mapFromGlobal(global.topLeft()), global.size()));
}

void KisStudioLayout::toggleToolOptions()
{
    QDockWidget *dock = d->mainWindow->dockWidget(QStringLiteral("sharedtooldocker"));
    if (!dock) {
        return;
    }
    if (d->flyout == dock) {
        closeFlyout();
    } else if (d->toolStrip && !d->toolStrip->optionsLeft()) {
        // the bar holds them all
        d->toolStrip->showMore();
    } else if (d->pinned.contains(dock->objectName())) {
        togglePanel(dock, true);
    } else {
        showFlyout(dock, d->toolChip);
    }
}

void KisStudioLayout::setPinned(QDockWidget *dock, bool pinned)
{
    commitArrangement();
    const QString id = dock->objectName();
    if (pinned) {
        d->pinned.insert(id);
        d->hiddenPinned.remove(id);
        if (dock == d->flyout) {
            releaseFlyout();
        }
        dockAway(dock);
        d->arrangement.clear();
        arrangePinned();
        dock->show();
        applyFold(dock, d->folded.contains(id));
    } else {
        d->pinned.remove(id);
        d->groups.remove(id);
        d->lanes.remove(id);
        applyFold(dock, false);
        const bool wasVisible = !dock->isHidden();
        dock->hide();
        if (wasVisible) {
            showFlyout(dock);
        } else {
            parkPanel(dock);
        }
        d->arrangement.clear();
        arrangePinned();
    }
    savePinned();
    syncRail();
}

void KisStudioLayout::arrangePinned()
{
    // in the rail's order; only redone when the order, a group, a lane or a side has changed
    const QStringList order = d->pinnedOrder();
    QString arrangement = d->panelsLeft ? QStringLiteral("L:") : QStringLiteral("R:");
    Q_FOREACH (const QString &id, order) {
        arrangement += QString::number(d->groupOf(id)) + QLatin1Char('.') + QString::number(d->laneOf(id)) + QLatin1Char(' ') + id + QLatin1Char(',');
    }
    if (arrangement == d->arrangement) {
        updateColumns();
        return;
    }
    d->arrangement = arrangement;

    KisStudioPanelColumn *column = panelColumn();
    if (!column) {
        return;
    }
    // group, then lane, then the panels top to bottom
    QMap<int, QMap<int, QList<QDockWidget *>>> lists;
    lists[0];
    Q_FOREACH (const QString &id, order) {
        QDockWidget *dock = d->mainWindow->dockWidget(id);
        if (dock && dock != d->flyout) lists[d->groupOf(id)][d->laneOf(id)] << dock;
    }
    QMap<int, KisStudioPanelColumn *> columns;
    columns.insert(0, column);
    for (auto it = lists.constBegin(); it != lists.constEnd(); ++it) {
        if (it.key() == 0) continue;
        if (KisStudioPanelColumn *own = groupColumn(it.key())) columns.insert(it.key(), own);
    }
    QList<QDockWidget *> placed;
    for (const auto &lanes : lists) {
        for (const QList<QDockWidget *> &list : lanes) placed += list;
    }
    Q_FOREACH (KisStudioPanelColumn *own, d->columns()) {
        Q_FOREACH (QDockWidget *dock, own->panels()) {
            if (!placed.contains(dock)) parkPanel(dock);
        }
    }
    // take them out of the window's dock areas so the canvas extends under the pills
    Q_FOREACH (QDockWidget *dock, placed) {
        bool inColumn = false;
        for (KisStudioPanelColumn *own : columns) {
            for (PanelStack *stack : own->stacks()) inColumn = inColumn || dock->parentWidget() == stack;
        }
        if (!inColumn) {
            const bool shown = !dock->isHidden();
            if (dock->isFloating()) dockAway(dock);
            d->mainWindow->removeDockWidget(dock);
            dock->setVisible(shown);
        }
    }
    column->setLeft(d->panelsLeft);
    if (columns.contains(1)) columns[1]->setLeft(!d->panelsLeft);
    bool renumbered = false;
    for (auto it = columns.constBegin(); it != columns.constEnd(); ++it) {
        const int group = it.key();
        // lanes numbered from 0 without gaps, each keeping its width
        const QMap<int, QList<QDockWidget *>> lanes = lists.value(group);
        const QVector<int> widths = d->laneWidths.value(group);
        QList<QList<QDockWidget *>> shown;
        QVector<int> kept;
        for (auto lane = lanes.constBegin(); lane != lanes.constEnd(); ++lane) {
            for (QDockWidget *dock : lane.value()) {
                const int now = shown.size();
                if (d->laneOf(dock->objectName()) == now) continue;
                if (now > 0) d->lanes.insert(dock->objectName(), now);
                else d->lanes.remove(dock->objectName());
                renumbered = true;
            }
            shown << lane.value();
            kept << widths.value(lane.key(), widths.value(0, it.value()->columnWidth()));
        }
        if (kept.isEmpty()) kept << widths.value(0, it.value()->columnWidth());
        if (kept != widths) {
            d->laneWidths.insert(group, kept);
            saveWidths(group);
        }
        it.value()->setLanes(shown, kept);
    }
    if (renumbered) {
        savePinned();
        d->arrangement.clear();
    }
    // floating groups left with no panels go; the one across the canvas stays for panels dropped by that edge
    for (auto it = d->groupColumns.begin(); it != d->groupColumns.end();) {
        if (it.key() >= 2 && !lists.contains(it.key())) {
            if (KisStudioPanelColumn *going = it.value()) {
                going->hide();
                going->deleteLater();
            }
            d->laneWidths.remove(it.key());
            it = d->groupColumns.erase(it);
        } else {
            ++it;
        }
    }
    restoreHeights();
    updateColumns();
}

QVector<int> KisStudioLayout::readWidths(int group) const
{
    KisConfig cfg(true);
    const QString key = widthKeyOf(group);
    QVector<int> widths;
    Q_FOREACH (const QString &width, cfg.readEntry<QString>(key + QStringLiteral("Lanes"), QString()).split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        widths << qMax(KisStudioPanelColumn::minimumColumnWidth, width.toInt());
    }
    if (!widths.isEmpty()) return widths;
    // as wide as the column with the panel buttons until its own width is set; that one falls back to the legacy key
    const int fallback = group == 0 ? cfg.readEntry<int>(QStringLiteral("StudioLayoutRightColumnWidth"), 270)
                                    : d->panelColumn ? d->panelColumn->columnWidth() : 270;
    return {cfg.readEntry<int>(key, fallback)};
}

void KisStudioLayout::saveWidths(int group)
{
    const QVector<int> widths = d->laneWidths.value(group);
    if (widths.isEmpty()) return;
    QStringList list;
    for (int width : widths) list << QString::number(width);
    KisConfig cfg(false);
    cfg.writeEntry(widthKeyOf(group), widths.first());
    cfg.writeEntry(widthKeyOf(group) + QStringLiteral("Lanes"), list.join(QLatin1Char(',')));
}

QString KisStudioLayout::widthKeyOf(int group) const
{
    return group == 0 ? columnWidthKey : group == 1 ? farWidthKey : groupKey(group, "Width");
}

bool KisStudioLayout::joinAt(KisStudioPanelColumn *column, const QRect &placed, bool dropped)
{
    QWidget *area = column->parentWidget();
    // within a snap of another group's side (or, for a single lane, its top or bottom), along enough of it
    const int snap = 16;
    const int alongAtLeast = 40;
    KisStudioPanelColumn *to = nullptr;
    Qt::Edge edge = Qt::RightEdge;
    int lane = 0;
    QRect indicator;
    int nearest = snap + 1;
    Q_FOREACH (KisStudioPanelColumn *other, d->columns()) {
        if (other == column || other->isHidden() || !other->panelsShown() || other->panels().isEmpty()) continue;
        // the column with the panel buttons and the one across from it keep to their sides
        if ((column->group <= 1 && other->group <= 1)) continue;
        const QRect them = other->targetGeometry();
        const int upright = qMin(placed.bottom(), them.bottom()) - qMax(placed.top(), them.top());
        const int across = qMin(placed.right(), them.right()) - qMax(placed.left(), them.left());
        auto consider = [&](Qt::Edge side, int distance, int along, int toLane, const QRect &bar) {
            if (qAbs(distance) >= nearest || along < alongAtLeast) return;
            nearest = qAbs(distance);
            to = other;
            edge = side;
            lane = toLane;
            indicator = bar;
        };
        consider(Qt::RightEdge, placed.left() - them.right() - 1, upright, 0, QRect(them.right() - 1, them.top() + 6, 3, them.height() - 12));
        consider(Qt::LeftEdge, them.left() - placed.right() - 1, upright, 0, QRect(them.left() - 1, them.top() + 6, 3, them.height() - 12));
        if (column->stacks().size() == 1) {
            // into the lane it is over
            int over = 0;
            for (int i = 0; i < other->stacks().size(); i++) {
                const QRect laneAt = other->laneRect(i).translated(them.topLeft() - other->geometry().topLeft());
                if (placed.center().x() >= laneAt.left()) over = i;
            }
            const QRect laneAt = other->laneRect(over).translated(them.topLeft() - other->geometry().topLeft());
            consider(Qt::BottomEdge, placed.top() - them.bottom() - 1, across, over, QRect(laneAt.left() + 4, them.bottom() - 1, laneAt.width() - 8, 3));
            consider(Qt::TopEdge, them.top() - placed.bottom() - 1, across, over, QRect(laneAt.left() + 4, them.top() - 1, laneAt.width() - 8, 3));
        }
    }
    if (!dropped) {
        // shown where it would join, as a dragged panel's place is
        if (to) {
            if (!d->panelIndicator) {
                d->panelIndicator = new QFrame(d->mainWindow);
                d->panelIndicator->setAutoFillBackground(true);
                d->panelIndicator->setBackgroundRole(QPalette::Highlight);
            }
            d->panelIndicator->setGeometry(QRect(area->mapTo(d->mainWindow, indicator.topLeft()), indicator.size()));
            d->panelIndicator->show();
            d->panelIndicator->raise();
        } else {
            d->hidePanelDrop();
        }
        return to;
    }
    d->hidePanelDrop();
    if (!to) return false;
    joinGroups(column, to, edge, lane, placed);
    return true;
}

QList<QStringList> KisStudioLayout::lanesOf(const KisStudioPanelColumn *column) const
{
    QList<QStringList> lanes;
    for (PanelStack *stack : column->stacks()) {
        QStringList ids;
        for (QDockWidget *dock : stack->panels()) ids << dock->objectName();
        if (!ids.isEmpty()) lanes << ids;
    }
    return lanes;
}

void KisStudioLayout::joinGroups(KisStudioPanelColumn *from, KisStudioPanelColumn *to, Qt::Edge edge, int lane, const QRect &placed)
{
    commitArrangement();
    const QList<QStringList> moving = lanesOf(from);
    const QList<QStringList> staying = lanesOf(to);
    const QVector<int> movingWidths = from->laneWidths().mid(0, moving.size());
    const QVector<int> stayingWidths = to->laneWidths().mid(0, staying.size());
    QList<QStringList> lanes;
    QVector<int> widths;
    if (edge == Qt::RightEdge || edge == Qt::LeftEdge) {
        lanes = edge == Qt::RightEdge ? staying + moving : moving + staying;
        widths = edge == Qt::RightEdge ? stayingWidths + movingWidths : movingWidths + stayingWidths;
    } else {
        lanes = staying;
        widths = stayingWidths;
        const QStringList ids = moving.value(0);
        if (lane >= 0 && lane < lanes.size()) lanes[lane] = edge == Qt::TopEdge ? ids + lanes[lane] : lanes[lane] + ids;
    }
    // the column with the panel buttons (or the one across from it) stays; a floating group goes into the other
    KisStudioPanelColumn *kept = from->group == 0 || (from->group == 1 && to->group != 0) ? from : to;
    for (int i = 0; i < lanes.size(); i++) {
        Q_FOREACH (const QString &id, lanes[i]) {
            if (kept->group > 0) d->groups.insert(id, kept->group);
            else d->groups.remove(id);
            if (i > 0) d->lanes.insert(id, i);
            else d->lanes.remove(id);
        }
    }
    d->laneWidths.insert(kept->group, widths);
    saveWidths(kept->group);
    savePinned();
    // a lane is its panels in the rail's order: put them in the order the lanes now have, where the first was
    QStringList panels = d->railPanels;
    QStringList joined;
    for (const QStringList &ids : lanes) {
        Q_FOREACH (const QString &id, ids) {
            if (panels.contains(id)) joined << id;
        }
    }
    int at = panels.size();
    Q_FOREACH (const QString &id, joined) at = qMin(at, int(panels.indexOf(id)));
    Q_FOREACH (const QString &id, joined) panels.removeAll(id);
    for (int i = 0; i < joined.size(); i++) panels.insert(qMin(at + i, int(panels.size())), joined[i]);
    // the joined pill where the two were
    const QRect box = placed | to->targetGeometry();
    QPointer<KisStudioPanelColumn> stays = kept;
    d->arrangement.clear();
    setRailPanels(panels, true);
    if (stays) stays->placeAt(box);
}

void KisStudioLayout::separateLanes(KisStudioPanelColumn *column, int lane)
{
    commitArrangement();
    const QList<QStringList> lanes = lanesOf(column);
    if (lane <= 0 || lane >= lanes.size()) return;
    QWidget *area = column->parentWidget();
    const QVector<int> widths = column->laneWidths();
    const QRect stay = column->laneRect(0) | column->laneRect(lane - 1);
    const QRect go = column->laneRect(lane) | column->laneRect(lanes.size() - 1);
    const int group = freeGroup();
    for (int i = lane; i < lanes.size(); i++) {
        Q_FOREACH (const QString &id, lanes[i]) {
            d->groups.insert(id, group);
            if (i > lane) d->lanes.insert(id, i - lane);
            else d->lanes.remove(id);
        }
    }
    d->laneWidths.insert(group, widths.mid(lane));
    d->laneWidths.insert(column->group, widths.mid(0, lane));
    saveWidths(group);
    saveWidths(column->group);
    // the lanes taken off are a group of their own where they were, a little apart
    writeGroupPlace(group, go.translated(go.center().x() > stay.center().x() ? KisModernStyle::space3 : -KisModernStyle::space3, 0), area);
    savePinned();
    d->arrangement.clear();
    arrangePinned();
    // the lanes left keep their place
    column->placeAt(stay);
}

void KisStudioLayout::separateBelow(KisStudioPanelColumn *column, int lane, QDockWidget *above)
{
    commitArrangement();
    const QList<QStringList> lanes = lanesOf(column);
    if (!above || lane < 0 || lane >= lanes.size()) return;
    const QStringList ids = lanes[lane];
    const int at = ids.indexOf(above->objectName());
    if (at < 0 || at >= ids.size() - 1) return;
    QWidget *area = column->parentWidget();
    // the panels below, as tall as they are and as wide as their lane
    const QRect laneAt = column->laneRect(lane);
    QRect below;
    for (int i = at + 1; i < ids.size(); i++) {
        if (QDockWidget *dock = d->mainWindow->dockWidget(ids[i])) below |= QRect(dock->mapTo(area, QPoint()), dock->size());
    }
    below = QRect(laneAt.left(), below.top(), laneAt.width(), below.height() + 2 * KisStudioPanelColumn::margin);
    const QRect stay = column->targetGeometry();
    const int group = freeGroup();
    for (int i = at + 1; i < ids.size(); i++) {
        d->groups.insert(ids[i], group);
        d->lanes.remove(ids[i]);
    }
    d->laneWidths.insert(group, {column->laneWidths().value(lane, column->columnWidth())});
    saveWidths(group);
    // a group of their own where they were, a little apart
    writeGroupPlace(group, below.translated(0, KisModernStyle::space3), area);
    savePinned();
    d->arrangement.clear();
    arrangePinned();
    // the panels left keep their place
    column->placeAt(stay);
}

void KisStudioLayout::writeGroupPlace(int group, const QRect &rect, QWidget *area)
{
    const int margin = KisStudioPanelColumn::margin;
    const bool left = rect.center().x() < area->width() / 2;
    const int across = left ? rect.x() - margin : area->width() - margin - rect.right() - 1;
    // the room a group's place is a share of: the whole height (see KisStudioPanelColumn::spanY)
    const int top = margin;
    const int bottom = area->height() - margin;
    const int roomX = area->width() - 2 * margin - rect.width();
    const int roomY = bottom - top - rect.height();
    KisSettingsRegistry::writeLater(groupKey(group, "Side"), left ? QStringLiteral("left") : QStringLiteral("right"));
    const QSize grown = roomGrowth(area);
    writeSpot(groupKey(group, "Spot"), QPointF(shareAt(across, roomX, grown.width()), shareAt(rect.y() - top, roomY, grown.height())), rect.height());
}

int KisStudioLayout::freeGroup() const
{
    int group = 2;
    while (d->groupColumns.contains(group) || std::find(d->groups.cbegin(), d->groups.cend(), group) != d->groups.cend()) group++;
    return group;
}

KisStudioPanelColumn *KisStudioLayout::panelColumn()
{
    if (!d->panelColumn) {
        d->panelColumn = createColumn(0, d->panelsLeft);
        if (!d->panelColumn) return nullptr;
        d->panelColumn->setSpot(readSpot(panelSpotKey), readSpotHeight(panelSpotKey));
        d->panelColumn->moved = [this](bool left, const QPointF &spot) {
            writeSpot(panelSpotKey, spot, d->panelColumn ? d->panelColumn->refHeight() : 0);
            if (left != d->panelsLeft) setPanelsSide(left);
            else updateCanvasInsets();
            commitArrangement();
        };
        // a tap folds its own panels, as every group's grip does; panels tucked away before come back
        d->panelColumn->tapped = [this]() {
            if (d->panelsTucked) setPanelsTucked(false);
            else foldGroup(0);
        };
    }
    if (d->rail) d->panelColumn->setStrip(d->rail);
    return d->panelColumn;
}

KisStudioPanelColumn *KisStudioLayout::groupColumn(int group)
{
    if (group <= 0) return panelColumn();
    if (KisStudioPanelColumn *column = d->groupColumns.value(group)) return column;
    KisConfig cfg(true);
    const QString spotKey = group == 1 ? farSpotKey : groupKey(group, "Spot");
    const bool left = group == 1 ? !d->panelsLeft : cfg.readEntry<QString>(groupKey(group, "Side"), QString()) == QLatin1String("left");
    KisStudioPanelColumn *column = createColumn(group, left);
    if (!column) return nullptr;
    column->setObjectName(group == 1 ? QStringLiteral("studioFarPanels") : QStringLiteral("studioPanelGroup%1").arg(group));
    // it has no panel buttons to drag it by; a tap folds its panels
    auto *grip = new DragGrip(column);
    grip->setObjectName(QStringLiteral("studioGroupGrip"));
    grip->setToolTip(i18nc("@info:tooltip a group of panels' grip", "Drag: move\nClick: fold"));
    column->setStrip(grip);
    column->tapped = [this, group]() { foldGroup(group); };
    column->setSpot(readSpot(spotKey), readSpotHeight(spotKey));
    column->moved = [this, group, spotKey](bool left, const QPointF &spot) {
        KisStudioPanelColumn *own = d->groupColumns.value(group);
        writeSpot(spotKey, spot, own ? own->refHeight() : 0);
        if (group == 1) {
            if (left == d->panelsLeft) {
                setPanelsSide(!left);
                return;
            }
        } else {
            KisSettingsRegistry::writeLater(groupKey(group, "Side"), left ? QStringLiteral("left") : QStringLiteral("right"));
        }
        updateCanvasInsets();
        commitArrangement();
    };
    d->groupColumns.insert(group, column);
    return column;
}

int KisStudioLayout::newPanelGroup(const QPoint &windowPos)
{
    const int group = freeGroup();
    QMdiArea *mdi = mdiArea();
    QWidget *area = mdi ? mdi->parentWidget() : nullptr;
    if (!area) return group;
    // where it was dropped, as the shares a column keeps (see shareOf()): its top middle under the pointer
    const QPoint at = area->mapFrom(d->mainWindow, windowPos);
    const bool left = at.x() < area->width() / 2;
    const int width = d->panelColumn ? d->panelColumn->columnWidth() : 270;
    const int margin = KisStudioPanelColumn::margin;
    const int x = at.x() - width / 2;
    const int across = left ? x - margin : area->width() - margin - width - x;
    // the room a group's place is a share of: the whole height (see KisStudioPanelColumn::spanY)
    const int top = margin;
    const int bottom = area->height() - margin;
    // its height is not known yet: a guess, so the share puts its top about where it was dropped
    const int guess = qMin(320, bottom - top);
    KisConfig cfg(false);
    cfg.writeEntry(groupKey(group, "Side"), left ? QStringLiteral("left") : QStringLiteral("right"));
    d->laneWidths.insert(group, {width});
    saveWidths(group);
    const QSize grown = roomGrowth(area);
    writeSpot(groupKey(group, "Spot"), QPointF(shareOf(across, area->width() - 2 * margin - width, grown.width()),
                                               shareOf(at.y() - 12 - top, bottom - top - guess, grown.height())), guess);
    return group;
}

KisStudioPanelColumn *KisStudioLayout::createColumn(int group, bool left)
{
    QMdiArea *area = mdiArea();
    if (!area) return nullptr;
    if (!d->laneWidths.contains(group)) d->laneWidths.insert(group, readWidths(group));
    auto *column = new KisStudioPanelColumn(area->parentWidget(), left, d->laneWidths.value(group));
    column->group = group;
    QPointer<KisStudioPanelColumn> self = column;
    column->resized = [this, self, group](bool done) {
        // refit the views only when a width drag ends; doing it on every move costs most of a frame
        updateCanvasInsets(done);
        if (done && self) {
            d->laneWidths.insert(group, self->laneWidths());
            saveWidths(group);
            commitArrangement();
        }
    };
    column->stackMade = [this](PanelStack *stack) {
        // a group with no panel shown is hidden: one shown again brings it back
        stack->shownChanged = [this]() { QTimer::singleShot(0, this, [this]() { updateColumns(); }); };
        stack->userHeight = [this](const QDockWidget *dock) { return d->heights.value(dock->objectName(), 0); };
        stack->settled = [this](QDockWidget *dock, int height) {
            d->heights.insert(dock->objectName(), height);
            saveHeights();
        };
        stack->resizedByUser = [this](QDockWidget *dock, int height, bool done) {
            if (height > 0) d->heights.insert(dock->objectName(), height);
            else d->heights.remove(dock->objectName());
            if (done) {
                saveHeights();
                commitArrangement();
            }
        };
    };
    column->joining = [this](KisStudioPanelColumn *dragged, const QRect &placed, bool dropped) { return joinAt(dragged, placed, dropped); };
    column->separated = [this, self](int lane) {
        if (self) separateLanes(self, lane);
    };
    column->separatedBelow = [this, self](int lane, QDockWidget *above) {
        if (self) separateBelow(self, lane, above);
    };
    return column;
}

void KisStudioLayout::setPanelsTucked(bool tucked)
{
    commitArrangement();
    d->panelsTucked = tucked;
    KisSettingsRegistry::writeLater(panelsHiddenKey, tucked);
    if (d->stripGrip) {
        d->stripGrip->setToolTip(tucked ? i18nc("@info:tooltip", "Click: show panels\nDrag: move")
                                        : i18nc("@info:tooltip a group of panels' grip", "Drag: move\nClick: fold"));
    }
    updateColumns();
}

void KisStudioLayout::setToolRailTucked(bool tucked)
{
    commitArrangement();
    d->toolRailTucked = tucked;
    KisSettingsRegistry::writeLater(toolRailHiddenKey, tucked);
    if (auto *rail = static_cast<RailOverlay *>(d->brushRail.data())) rail->setTucked(tucked);
}

void KisStudioLayout::setTopBarTucked(bool tucked)
{
    commitArrangement();
    d->topBarTucked = tucked;
    KisSettingsRegistry::writeLater(topBarHiddenKey, tucked);
    if (d->topPill) d->topPill->setTucked(tucked);
    updateCanvasInsets();
}

void KisStudioLayout::setPanelsSide(bool left)
{
    commitArrangement();
    d->panelsLeft = left;
    KisSettingsRegistry::writeLater(panelSideKey, left ? QStringLiteral("left") : QStringLiteral("right"));
    d->arrangement.clear();
    arrangePinned();
}

void KisStudioLayout::setToolRailSide(bool left)
{
    commitArrangement();
    d->toolRailLeft = left;
    KisSettingsRegistry::writeLater(toolRailSideKey, left ? QStringLiteral("left") : QStringLiteral("right"));
    if (auto *rail = static_cast<RailOverlay *>(d->brushRail.data())) rail->setPlace(left, d->toolRailSpot);
}

void KisStudioLayout::parkPanel(QDockWidget *dock)
{
    KisStudioGlass::setSeeThrough(dock, false);
    keepUpright(dock, false);
    dock->hide();
    d->mainWindow->addDockWidget(Qt::RightDockWidgetArea, dock);
    dock->hide();
}

void KisStudioLayout::showNewImagePopup()
{
    auto *popup = new KisStudioPopup(d->mainWindow);
    popup->setAttribute(Qt::WA_DeleteOnClose);
    auto *layout = new QVBoxLayout(popup);
    popup->fitLayout(layout, KisModernStyle::space3);
    QPointer<KisStudioPopup> self = popup;
    layout->addWidget(KisStudioStartPage::createNewTiles(d->mainWindow, popup, [self]() {
        if (self) self->close();
    }));
    popup->adjustSize();
    // centred under the top of the window, where the menu opens
    QWidget *area = d->mainWindow->centralWidget();
    const QRect at(area->mapToGlobal(QPoint()), area->size());
    popup->popupAt(QPoint(at.center().x() - popup->width() / 2, at.top() + 64));
}

void KisStudioLayout::showHome(bool show)
{
    // back only to an image that is open
    if (show == d->mainWindow->isWelcomeScreenShown() || (!show && !d->mainWindow->activeView())) return;
    d->mainWindow->showWelcomeScreen(show);
    if (show && d->startPage) d->startPage->setFocus(Qt::OtherFocusReason);
}

void KisStudioLayout::showStartPage(bool show)
{
    if (show && !d->startPage) {
        auto *welcome = d->mainWindow->findChild<KisWelcomePageWidget *>();
        QWidget *viewport = welcome ? welcome->parentWidget() : nullptr;
        auto *scroller = viewport ? qobject_cast<QScrollArea *>(viewport->parentWidget()) : nullptr;
        if (!scroller || scroller->widget() != welcome) return;
        d->classicWelcome = scroller->takeWidget();
        // kept as the window's own, for what still looks it up
        d->classicWelcome->setParent(d->mainWindow);
        d->classicWelcome->hide();
        d->startPage = new KisStudioStartPage(d->mainWindow);
        scroller->setWidget(d->startPage);
        // the window shows it after laying out for it, and raises it over the pills: lay out again once it shows
        d->startPage->installEventFilter(this);
    } else if (!show && d->startPage) {
        QWidget *viewport = d->startPage->parentWidget();
        auto *scroller = viewport ? qobject_cast<QScrollArea *>(viewport->parentWidget()) : nullptr;
        if (scroller) {
            scroller->takeWidget();
            if (d->classicWelcome) {
                scroller->setWidget(d->classicWelcome);
                d->classicWelcome->show();
            }
        }
        delete d->startPage;
    }
}

void KisStudioLayout::placeWelcomePage()
{
    QWidget *welcome = d->startPage ? static_cast<QWidget *>(d->startPage) : d->mainWindow->findChild<KisWelcomePageWidget *>();
    QWidget *rail = d->brushRail;
    if (!welcome || !rail) return;
    QMargins room;
    if (rail->isVisible()) {
        const QRect at(rail->mapTo(d->mainWindow, QPoint()), rail->size());
        const QRect page(welcome->mapTo(d->mainWindow, QPoint()), welcome->size());
        if (at.center().x() < page.center().x()) room.setLeft(qMax(0, at.right() + 13 - page.left()));
        else room.setRight(qMax(0, page.right() + 13 - at.left()));
    }
    if (welcome->contentsMargins() != room) welcome->setContentsMargins(room);
}

void KisStudioLayout::foldGroup(int group)
{
    KisStudioPanelColumn *column = group == 0 ? d->panelColumn.data() : d->groupColumns.value(group).data();
    if (!column) return;
    // folded all, unless all are folded already
    bool open = false;
    Q_FOREACH (QDockWidget *dock, column->panels()) open = open || (!dock->isHidden() && !d->folded.contains(dock->objectName()));
    Q_FOREACH (QDockWidget *dock, column->panels()) {
        if (!dock->isHidden() && d->folded.contains(dock->objectName()) != open) gripClicked(dock->objectName());
    }
}

void KisStudioLayout::updateColumns()
{
    Q_FOREACH (KisStudioPanelColumn *column, d->columns()) {
        // the only panel of a group needs no title bar of its own: the group's grip moves and folds it; folded,
        // the bar shows its name
        const QList<QDockWidget *> docks = column->panels();
        const bool alone = column != d->panelColumn && docks.size() == 1;
        bool allFolded = !docks.isEmpty();
        Q_FOREACH (QDockWidget *dock, docks) {
            PanelGrip *title = d->grips.value(dock->objectName());
            allFolded = allFolded && d->folded.contains(dock->objectName());
            if (!title || title->isAlone() == alone) continue;
            title->setAlone(alone);
            for (PanelStack *stack : column->stacks()) stack->later();
        }
        // the strip's grip says so while the panels are tucked away (see setPanelsTucked())
        QWidget *grip = column == d->panelColumn ? (d->panelsTucked ? nullptr : d->stripGrip.data())
                                                 : column->findChild<QWidget *>(QStringLiteral("studioGroupGrip"), Qt::FindDirectChildrenOnly);
        if (grip) {
            grip->setToolTip(allFolded ? i18nc("@info:tooltip a group of panels' grip", "Drag: move\nClick: unfold")
                                       : i18nc("@info:tooltip a group of panels' grip", "Drag: move\nClick: fold"));
        }
    }
    Q_FOREACH (KisStudioPanelColumn *column, d->columns()) {
        bool any = false;
        Q_FOREACH (QDockWidget *dock, column->panels()) {
            any = any || !dock->isHidden();
        }
        column->setPanelsShown(any && !arePanelsHidden() && !d->panelsTucked);
        // the other groups are nothing without their panels; the column with the panel buttons keeps its strip
        const bool kept = column == d->panelColumn || column->panelsShown();
        column->setVisible(d->active && kept && (!isRailHidden() || d->peeking.contains(QPointer<QWidget>(column))));
    }
    updateCanvasInsets();
}

void KisStudioLayout::updateCanvasInsets(bool views)
{
    placeWindowHandle();
    // the zoom pill keeps off the panels wherever they go, not only off those docked
    if (d->hud) d->hud->reposition();
    QMdiArea *mdi = mdiArea();
    QWidget *area = mdi ? mdi->parentWidget() : nullptr;
    if (!area) {
        return;
    }
    // how far each side's pill reaches in, and whether it has its panels out (rather than only its strip)
    int pillLeft = 0;
    int pillRight = 0;
    int fullLeft = 0;
    int fullRight = 0;
    Q_FOREACH (KisStudioPanelColumn *column, d->columns()) {
        // one floating over the canvas leaves it be
        if (column->isHidden() || !column->isDocked()) continue;
        const bool left = column->isLeft();
        const int reach = left ? column->targetGeometry().right() + 1 : area->width() - column->targetGeometry().x();
        // the tool rail sits beside the pill, even when only its strip shows
        (left ? pillLeft : pillRight) = qMax(left ? pillLeft : pillRight, reach);
        if (column->panelsShown()) (left ? fullLeft : fullRight) = qMax(left ? fullLeft : fullRight, reach);
    }
    int top = 0;
    int bottom = 0;
    QVector<QRect> bars;
    for (TopPill *pill : {d->topPill.data(), d->windowPill.data()}) {
        if (pill && !pill->isHidden()) bars << pill->geometry();
        // a pill only peeking over the canvas doesn't push anything, nor does one dragged away from the edges
        if (!pill || pill->isHidden() || d->peeking.contains(pill)) continue;
        const QRect at = pill->geometry();
        if (at.top() <= TopPill::margin) top = qMax(top, at.bottom() + 1);
        else if (at.bottom() + 1 >= area->height() - TopPill::margin) bottom = qMax(bottom, area->height() - at.top());
    }
    // with its panels shown, the pill covers the canvas's side, so the HUD and the view keep clear of it
    const QMargins insets(fullLeft, top, fullRight, bottom);
    if (canvasInsets(area) != insets || area->property(leftPillProperty).toInt() != pillLeft
        || area->property(rightPillProperty).toInt() != pillRight) {
        area->setProperty(leftInsetProperty, insets.left());
        area->setProperty(rightInsetProperty, insets.right());
        area->setProperty(topInsetProperty, insets.top());
        area->setProperty(bottomInsetProperty, insets.bottom());
        Q_FOREACH (KisStudioPanelColumn *column, d->columns()) {
            column->place();
        }
        area->setProperty(leftPillProperty, pillLeft);
        area->setProperty(rightPillProperty, pillRight);
        QCoreApplication::postEvent(area, new QEvent(QEvent::LayoutRequest));
        if (d->flyout) positionFlyout();
    } else if (bars != d->barsPlacedFor) {
        // a bar grew (another tool's options) or moved: the groups beside it make way, and come back once it shrinks
        Q_FOREACH (KisStudioPanelColumn *column, d->columns()) {
            column->place();
        }
    }
    d->barsPlacedFor = bars;

    // each view fits, zooms and rotates around the part the pills leave uncovered
    if (!views) {
        return;
    }
    Q_FOREACH (QMdiSubWindow *subWindow, mdi->subWindowList()) {
        KisView *view = qobject_cast<KisView *>(subWindow->widget());
        KisCanvas2 *canvas = view ? view->canvasBase() : nullptr;
        QWidget *canvasWidget = canvas ? canvas->canvasWidget() : nullptr;
        if (!canvasWidget) continue;
        const QRect widget(canvasWidget->mapToGlobal(QPoint()), canvasWidget->size());
        QMarginsF viewInsets;
        viewInsets.setTop(qMax(0, area->mapToGlobal(QPoint(0, top)).y() - widget.top()));
        if (fullLeft) viewInsets.setLeft(qMax(0, area->mapToGlobal(QPoint(fullLeft, 0)).x() - widget.left()));
        if (fullRight) viewInsets.setRight(qMax(0, widget.right() + 1 - area->mapToGlobal(QPoint(area->width() - fullRight, 0)).x()));
        if (bottom) viewInsets.setBottom(qMax(0, widget.bottom() + 1 - area->mapToGlobal(QPoint(0, area->height() - bottom)).y()));
        if (view->canvasController()) view->canvasController()->setViewportInsets(viewInsets);
    }
}

void KisStudioLayout::restoreHeights()
{
    // fold once the column holds the panels, then fit them
    QTimer::singleShot(0, this, [this]() {
        Q_FOREACH (const QString &id, d->pinnedOrder()) {
            applyFold(d->mainWindow->dockWidget(id), d->folded.contains(id) && d->pinned.contains(id));
        }
        Q_FOREACH (KisStudioPanelColumn *column, d->columns()) {
            for (PanelStack *stack : column->stacks()) stack->later();
        }
    });
}

void KisStudioLayout::saveHeights()
{
    QStringList entries;
    for (auto it = d->heights.constBegin(); it != d->heights.constEnd(); ++it) {
        entries << it.key() + QLatin1Char('=') + QString::number(it.value());
    }
    entries.sort();
    KisSettingsRegistry::writeLater(heightsKey, entries);
}

void KisStudioLayout::gripClicked(const QString &id)
{
    commitArrangement();
    QDockWidget *dock = d->mainWindow->dockWidget(id);
    if (!dock) {
        return;
    }
    // on a flyout, the grip closes it like its rail button does
    if (!d->pinned.contains(id)) {
        if (dock == d->flyout) closeFlyout();
        return;
    }
    // finish a fold still running from a quick previous click
    const QString foldName = QStringLiteral("studioFold_") + id;
    if (auto *running = findChild<QVariantAnimation *>(foldName, Qt::FindDirectChildrenOnly)) {
        running->setObjectName(QString());
        if (running->state() == QAbstractAnimation::Running) running->setCurrentTime(running->duration());
    }
    const bool fold = !d->folded.contains(id);
    if (fold) {
        d->folded.insert(id);
    } else {
        d->folded.remove(id);
    }
    KisSettingsRegistry::writeLater(foldedKey, QStringList(d->folded.values()));
    PanelGrip *grip = d->grips.value(id);
    const int from = dock->height();
    QWidget *content = dock->widget();
    if (!grip || !content || dock->isHidden()) {
        applyFold(dock, fold);
        return;
    }
    int to;
    if (fold) {
        // let it shrink and show the name at once; the content is squeezed when the animation ends
        content->setProperty("studioMinHeight", content->minimumHeight());
        content->setProperty("studioMaxHeight", content->maximumHeight());
        content->setMinimumHeight(0);
        grip->setFolded(true, dock->windowTitle());
        updateColumns();
        to = grip->minimumHeight();
    } else {
        applyFold(dock, false);
        updateColumns();
        to = d->heights.value(id, qMax(160, dock->sizeHint().height()));
        dock->setMaximumHeight(from);
    }
    auto *animation = new QVariantAnimation(this);
    animation->setObjectName(foldName);
    animation->setStartValue(from);
    animation->setEndValue(to);
    animation->setDuration(KisModernStyle::enterDuration + 40);
    animation->setEasingCurve(KisModernStyle::motionCurve());
    QPointer<QDockWidget> target = dock;
    connect(animation, &QVariantAnimation::valueChanged, this, [this, target, fold](const QVariant &value) {
        if (!target) return;
        target->setMaximumHeight(value.toInt());
        if (auto *stack = target ? dynamic_cast<PanelStack *>(target->parentWidget()) : nullptr) stack->relayout(true);
    });
    connect(animation, &QVariantAnimation::finished, this, [this, target, fold, to]() {
        if (target) {
            if (fold) {
                QWidget *content = target->widget();
                content->setProperty("studioFolded", true);
                content->setMaximumHeight(0);
                target->setMaximumHeight(to);
            } else {
                target->setMaximumHeight(QWIDGETSIZE_MAX);
            }
        }
        if (auto *stack = target ? dynamic_cast<PanelStack *>(target->parentWidget()) : nullptr) stack->relayout(true);
    });
    animation->start(QAbstractAnimation::DeleteWhenStopped);
}

void KisStudioLayout::applyFold(QDockWidget *dock, bool fold)
{
    if (!dock || !dock->widget()) {
        return;
    }
    PanelGrip *grip = d->grips.value(dock->objectName());
    QWidget *content = dock->widget();
    const bool isFolded = content->property("studioFolded").toBool();
    if (isFolded == fold && (!grip || grip->isFolded() == fold)) {
        return;
    }
    // squeeze the content to zero height rather than hiding it, which would narrow the column
    if (fold) {
        content->setProperty("studioFolded", true);
        content->setProperty("studioMinHeight", content->minimumHeight());
        content->setProperty("studioMaxHeight", content->maximumHeight());
        content->setMinimumHeight(0);
        content->setMaximumHeight(0);
    } else if (isFolded) {
        content->setProperty("studioFolded", false);
        content->setMaximumHeight(content->property("studioMaxHeight").toInt());
        content->setMinimumHeight(content->property("studioMinHeight").toInt());
    }
    if (grip) {
        grip->setFolded(fold, dock->windowTitle());
    }
    dock->setMaximumHeight(fold && grip ? grip->minimumHeight() : QWIDGETSIZE_MAX);
    updateColumns();
    if (auto *stack = dynamic_cast<PanelStack *>(dock->parentWidget())) stack->later();
}

void KisStudioLayout::dropPanel(const QString &id, const QPoint &globalPos, bool fromRail)
{
    commitArrangement();
    d->hidePanelDrop();
    QDockWidget *dock = d->mainWindow->dockWidget(id);
    if (!dock) {
        return;
    }
    const PanelDrop drop = d->panelDropAt(globalPos, id);

    if (!drop.column) {
        if (fromRail) {
            takeOffRail(id);
        } else if (d->pinned.contains(id)) {
            setPinned(dock, false);
        }
        return;
    }

    d->pinned.insert(id);
    d->hiddenPinned.remove(id);
    const int group = drop.fresh ? newPanelGroup(drop.at) : drop.group;
    if (group > 0) d->groups.insert(id, group);
    else d->groups.remove(id);
    const int lane = drop.fresh ? 0 : drop.lane;
    if (drop.newLane && !drop.fresh) {
        // a new lane: those from there on move one along, and it is as wide as a new group
        Q_FOREACH (const QString &other, d->pinned) {
            if (other != id && d->groupOf(other) == group && d->laneOf(other) >= lane) d->lanes.insert(other, d->laneOf(other) + 1);
        }
        QVector<int> widths = d->laneWidths.value(group);
        widths.insert(qBound(0, lane, int(widths.size())), d->panelColumn ? d->panelColumn->columnWidth() : 270);
        d->laneWidths.insert(group, widths);
        saveWidths(group);
    }
    if (lane > 0) d->lanes.insert(id, lane);
    else d->lanes.remove(id);
    savePinned();
    if (dock == d->flyout) {
        releaseFlyout();
    }
    dockAway(dock);

    // the columns follow the rail's order, so insert it on the rail where it lands in its column
    QStringList panels = d->railPanels;
    panels.removeAll(id);
    int at = drop.before.isEmpty() ? -1 : panels.indexOf(drop.before);
    if (at < 0) {
        Q_FOREACH (QDockWidget *other, d->column(id, group, drop.newLane ? -1 : lane)) {
            at = qMax(at, panels.indexOf(other->objectName()) + 1);
        }
    }
    panels.insert(at < 0 ? panels.size() : qMin(at, panels.size()), id);
    d->arrangement.clear();
    dock->show();
    setRailPanels(panels, true);
}

void KisStudioLayout::savePinned()
{
    QStringList pinned = d->pinned.values();
    pinned.sort();
    KisSettingsRegistry::writeLater(pinnedKey, pinned);
    QStringList far;
    QStringList floating;
    for (auto it = d->groups.constBegin(); it != d->groups.constEnd(); ++it) {
        if (!d->pinned.contains(it.key())) continue;
        if (it.value() == 1) far << it.key();
        else if (it.value() >= 2) floating << it.key() + QLatin1Char('=') + QString::number(it.value());
    }
    QStringList lanes;
    for (auto it = d->lanes.constBegin(); it != d->lanes.constEnd(); ++it) {
        if (d->pinned.contains(it.key()) && it.value() > 0) lanes << it.key() + QLatin1Char('=') + QString::number(it.value());
    }
    QStringList hidden;
    Q_FOREACH (const QString &id, d->hiddenPinned) {
        if (d->pinned.contains(id)) hidden << id;
    }
    far.sort();
    floating.sort();
    lanes.sort();
    hidden.sort();
    KisSettingsRegistry::writeLater(hiddenPinnedKey, hidden);
    KisSettingsRegistry::writeLater(farKey, far);
    KisSettingsRegistry::writeLater(groupsKey, floating);
    KisSettingsRegistry::writeLater(lanesKey, lanes);
}

