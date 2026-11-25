/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioSelectionHandles.h"

#include <QPainterPath>
#include <QPointer>
#include <QTimer>

#include "KisStudioCanvasHandles.h"
#include "KisStudioSelectionAdjust.h"
#include "KisViewManager.h"
#include "canvas/kis_canvas2.h"
#include "kis_pixel_selection.h"
#include "kis_selection.h"
#include "kis_selection_manager.h"

namespace
{
/// The edge's points farthest right and farthest down, in image pixels
bool edgePoints(KisSelectionSP selection, QPointF *right, QPointF *bottom)
{
    QList<QPolygonF> rings;
    if (selection->outlineCacheValid()) {
        rings = selection->outlineCache().toSubpathPolygons();
    } else {
        for (const QPolygon &ring : selection->pixelSelection()->outline()) rings << QPolygonF(ring);
    }
    // the middle of the edge's rightmost and lowest runs, so on a box the grips sit mid side, apart from each other
    qreal maxX = -1e9, maxY = -1e9;
    for (const QPolygonF &ring : rings) {
        for (const QPointF &point : ring) {
            maxX = qMax(maxX, point.x());
            maxY = qMax(maxY, point.y());
        }
    }
    if (maxX < -1e8) return false;
    qreal top = 1e9, low = -1e9, left = 1e9, far = -1e9;
    for (const QPolygonF &ring : rings) {
        for (const QPointF &point : ring) {
            if (point.x() > maxX - 1) {
                top = qMin(top, point.y());
                low = qMax(low, point.y());
            }
            if (point.y() > maxY - 1) {
                left = qMin(left, point.x());
                far = qMax(far, point.x());
            }
        }
    }
    *right = QPointF(maxX, (top + low) / 2);
    *bottom = QPointF((left + far) / 2, maxY);
    return true;
}

class Grips : public QObject
{
public:
    QPointer<KisViewManager> viewManager;
    KisStudioSelectionAdjust *adjust {nullptr};
    QPointer<KisStudioCanvasHandles> handles;
    QPointF growFrom;
    QPointF softFrom;
    int grow {0};
    int soft {0};
    bool dragging {false};
    QTimer later;

    void show()
    {
        if (dragging || !viewManager) return;
        KisSelectionSP selection = viewManager->selection();
        QPointF right, bottom;
        if (!selection || !edgePoints(selection, &right, &bottom)) {
            if (handles) handles->end();
            return;
        }
        if (!handles) {
            handles = KisStudioCanvasHandles::start(viewManager->canvasBase());
            if (!handles) return;
            handles->ended = [this]() {
                handles = nullptr;
                if (dragging) adjust->end();
                dragging = false;
            };
            handles->moved = [this](int index, const QPointF &at, Qt::KeyboardModifiers) {
                if (!dragging) {
                    dragging = true;
                    grow = 0;
                    soft = 0;
                    adjust->begin();
                }
                // across the edge where the grip is: out grows, in shrinks; down softens
                if (index == 0) grow = qRound(at.x() - growFrom.x());
                else soft = qMax(0, qRound(at.y() - softFrom.y()));
                adjust->set(grow, soft);
                handles->setHandles({{index == 0 ? QPointF(growFrom.x() + grow, growFrom.y()) : growFrom, QColor(Qt::white)},
                                     {index == 1 ? QPointF(softFrom.x(), softFrom.y() + soft) : softFrom, QColor(), true}});
            };
            handles->released = [this](int) {
                adjust->end();
                dragging = false;
                later.start();
            };
        }
        growFrom = right;
        softFrom = bottom;
        handles->setHandles({{growFrom, QColor(Qt::white)}, {softFrom, QColor(), true}});
    }
};
}

void KisStudioSelectionHandles::attach(QObject *owner, KisViewManager *viewManager, KisStudioSelectionAdjust *adjust)
{
    if (!owner || !viewManager || !adjust) return;
    auto *grips = new Grips;
    grips->viewManager = viewManager;
    grips->adjust = adjust;
    grips->later.setSingleShot(true);
    grips->later.setInterval(0);
    QObject::connect(&grips->later, &QTimer::timeout, grips, [grips]() { grips->show(); });
    // a selection made, changed or gone: the grips go to its edge, after the change has settled
    QObject::connect(viewManager->selectionManager(), &KisSelectionManager::currentSelectionChanged, grips, [grips]() { grips->later.start(); });
    QObject::connect(owner, &QObject::destroyed, grips, [grips]() {
        if (grips->handles) grips->handles->end();
        grips->deleteLater();
    });
    grips->later.start();
}
