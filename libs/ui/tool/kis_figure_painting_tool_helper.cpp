/*
 *  SPDX-FileCopyrightText: 2011 Dmitry Kazakov <dimula73@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "kis_figure_painting_tool_helper.h"

#include <KoCanvasResourceProvider.h>

#include "kis_resources_snapshot.h"
#include <kis_distance_information.h>
#include "kis_image.h"
#include "kis_painter.h"
#include <strokes/KisFreehandStrokeInfo.h>
#include "KisAsynchronousStrokeUpdateHelper.h"
#include "KisStrokeRecorder.h"

#include <QPainterPathStroker>


KisFigurePaintingToolHelper::KisFigurePaintingToolHelper(const KUndo2MagicString &name,
                                                         KisImageWSP image,
                                                         KisNodeSP currentNode,
                                                         KoCanvasResourceProvider *resourceManager,
                                                         KisToolShapeUtils::StrokeStyle strokeStyle,
                                                         KisToolShapeUtils::FillStyle fillStyle,
                                                         QTransform fillTransform)
{
    m_strokesFacade = image.data();

    m_resources =
        new KisResourcesSnapshot(image,
                                 currentNode,
                                 resourceManager);

    setupPaintStyles(m_resources, strokeStyle, fillStyle, fillTransform);
    m_brushOutline = strokeStyle != KisToolShapeUtils::StrokeStyleNone;
    m_filled = fillStyle != KisToolShapeUtils::FillStyleNone;

    KisFreehandStrokeInfo *strokeInfo = new KisFreehandStrokeInfo();

    FreehandStrokeStrategy *stroke =
        new FreehandStrokeStrategy(m_resources, strokeInfo, name);
    m_recorder.reset(new KisStrokeRecorder());
    stroke->setStrokeRecorder(m_recorder);

    m_strokeId = m_strokesFacade->startStroke(stroke);
}

void KisFigurePaintingToolHelper::setupPaintStyles(KisResourcesSnapshotSP resources,
                                                   KisToolShapeUtils::StrokeStyle strokeStyle,
                                                   KisToolShapeUtils::FillStyle fillStyle,
                                                   QTransform fillTransform)
{
    using namespace KisToolShapeUtils;

    const KoColor fgColor = resources->currentFgColor();
    const KoColor bgColor = resources->currentBgColor();

    switch (strokeStyle) {
    case StrokeStyleNone:
        resources->setStrokeStyle(KisPainter::StrokeStyleNone);
        break;
    case StrokeStyleForeground:
        resources->setStrokeStyle(KisPainter::StrokeStyleBrush);
        break;
    case StrokeStyleBackground:
        resources->setStrokeStyle(KisPainter::StrokeStyleBrush);

        resources->setFGColorOverride(bgColor);
        resources->setBGColorOverride(fgColor);

        if (fillStyle == FillStyleForegroundColor) {
            fillStyle = FillStyleBackgroundColor;
        } else if (fillStyle == FillStyleBackgroundColor) {
            fillStyle = FillStyleForegroundColor;
        }

        break;
    };

    switch (fillStyle) {
    case FillStyleForegroundColor:
        resources->setFillStyle(KisPainter::FillStyleForegroundColor);
        break;
    case FillStyleBackgroundColor:
        resources->setFillStyle(KisPainter::FillStyleBackgroundColor);
        break;
    case FillStylePattern:
        resources->setFillStyle(KisPainter::FillStylePattern);
        break;
    case FillStyleNone:
        resources->setFillStyle(KisPainter::FillStyleNone);
        break;
    }

    resources->setFillTransform(fillTransform);
}

KisFigurePaintingToolHelper::~KisFigurePaintingToolHelper()
{
    if (m_recorder->isStarted()) m_recorder->finish();
    m_strokesFacade->addJob(m_strokeId,
        new KisAsynchronousStrokeUpdateHelper::UpdateData(true));
    m_strokesFacade->endStroke(m_strokeId);
}

KisStrokeRecorder *KisFigurePaintingToolHelper::recorder()
{
    if (!m_recorder->isStarted()) m_recorder->start(m_resources);
    return m_recorder.data();
}

void KisFigurePaintingToolHelper::paintLine(const KisPaintInformation &pi0,
                                            const KisPaintInformation &pi1)
{
    if (m_brushOutline) recorder()->addOutline(QPolygonF(QVector<QPointF>{pi0.pos(), pi1.pos()}), false);
    m_strokesFacade->addJob(m_strokeId,
        new FreehandStrokeStrategy::Data(0,
                                         pi0, pi1));
}

void KisFigurePaintingToolHelper::paintPolyline(const vQPointF &points)
{
    if (m_brushOutline) recorder()->addOutline(QPolygonF(points), false);
    m_strokesFacade->addJob(m_strokeId,
        new FreehandStrokeStrategy::Data(0,
                                         FreehandStrokeStrategy::Data::POLYLINE,
                                         points));
}

void KisFigurePaintingToolHelper::paintPolygon(const vQPointF &points)
{
    if (m_filled) {
        QPainterPath area;
        area.addPolygon(QPolygonF(points));
        recorder()->addArea(area);
    }
    if (m_brushOutline) recorder()->addOutline(QPolygonF(points), true);
    m_strokesFacade->addJob(m_strokeId,
        new FreehandStrokeStrategy::Data(0,
                                         FreehandStrokeStrategy::Data::POLYGON,
                                         points));
}

void KisFigurePaintingToolHelper::paintRect(const QRectF &rect)
{
    if (m_filled) {
        QPainterPath area;
        area.addRect(rect);
        recorder()->addArea(area);
    }
    if (m_brushOutline) recorder()->addOutline(QPolygonF(rect), true);
    m_strokesFacade->addJob(m_strokeId,
        new FreehandStrokeStrategy::Data(0,
                                         FreehandStrokeStrategy::Data::RECT,
                                         rect));
}

void KisFigurePaintingToolHelper::paintEllipse(const QRectF &rect)
{
    QPainterPath ellipse;
    ellipse.addEllipse(rect);
    if (m_filled) recorder()->addArea(ellipse);
    if (m_brushOutline) recorder()->addOutline(ellipse.toFillPolygon(), true);
    m_strokesFacade->addJob(m_strokeId,
        new FreehandStrokeStrategy::Data(0,
                                         FreehandStrokeStrategy::Data::ELLIPSE,
                                         rect));
}

void KisFigurePaintingToolHelper::paintPainterPath(const QPainterPath &path)
{
    if (m_filled) recorder()->addArea(path);
    if (m_brushOutline) {
        Q_FOREACH (const QPolygonF &outline, path.toSubpathPolygons()) recorder()->addOutline(outline, false);
    }
    m_strokesFacade->addJob(m_strokeId,
        new FreehandStrokeStrategy::Data(0,
                                         FreehandStrokeStrategy::Data::PAINTER_PATH,
                                         path));
}

void KisFigurePaintingToolHelper::setFGColorOverride(const KoColor &color)
{
    m_resources->setFGColorOverride(color);
}

void KisFigurePaintingToolHelper::setBGColorOverride(const KoColor &color)
{
    m_resources->setBGColorOverride(color);
}

void KisFigurePaintingToolHelper::setSelectionOverride(KisSelectionSP m_selection)
{
    m_resources->setSelectionOverride(m_selection);
}

void KisFigurePaintingToolHelper::setBrush(const KisPaintOpPresetSP &brush)
{
    m_resources->setBrush(brush);
}

void KisFigurePaintingToolHelper::paintPainterPathQPen(const QPainterPath path, const QPen &pen, const KoColor &color)
{
    // drawn with a pen, not the brush
    recorder()->addArea(QPainterPathStroker(pen).createStroke(path));
    m_strokesFacade->addJob(m_strokeId,
        new FreehandStrokeStrategy::Data(0,
                                         FreehandStrokeStrategy::Data::QPAINTER_PATH,
                                         path, pen, color));
}

void KisFigurePaintingToolHelper::paintPainterPathQPenFill(const QPainterPath path, const QPen &pen, const KoColor &color)
{
    recorder()->addArea(path.united(QPainterPathStroker(pen).createStroke(path)));
    m_strokesFacade->addJob(m_strokeId,
        new FreehandStrokeStrategy::Data(0,
                                         FreehandStrokeStrategy::Data::QPAINTER_PATH_FILL,
                                         path, pen, color));
}
