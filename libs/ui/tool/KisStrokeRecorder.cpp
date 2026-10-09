/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStrokeRecorder.h"

#include <QMutexLocker>
#include <QtMath>

#include <KoCompositeOpRegistry.h>
#include <brushengine/kis_paint_information.h>
#include <brushengine/kis_paintop_settings.h>
#include <KisOptimizedBrushOutline.h>
#include <kis_node.h>
#include <kis_selection.h>

namespace {

QPolygon mirrored(const QPolygon &polygon, const QPointF &center, bool horizontal, bool vertical)
{
    QPolygon result;
    result.reserve(polygon.size());
    Q_FOREACH (const QPoint &point, polygon) {
        result << QPoint(horizontal ? qRound(2 * center.x() - point.x()) : point.x(),
                         vertical ? qRound(2 * center.y() - point.y()) : point.y());
    }
    return result;
}

} // namespace

void KisStrokeRecorder::start(KisResourcesSnapshotSP resources, KisPaintOpPresetSP shownPreset)
{
    m_started = true;
    m_shownPreset = shownPreset;
    KisPaintOpPresetSP preset = resources ? resources->currentPaintOpPreset() : KisPaintOpPresetSP();
    if (!preset || !resources->currentNode()) {
        return;
    }
    m_template.brush = preset->name();
    m_size = preset->settings() ? preset->settings()->paintOpSize() : 0;
    m_template.brushSize = float(m_size);
    m_template.color = resources->currentFgColor().toQColor().rgba();
    m_template.erasing = resources->compositeOpId() == COMPOSITE_ERASE;
    // painted through a selection: only there
    m_selection = resources->activeSelection();
    resources->mirroring(&m_axesCenter, &m_mirrorHorizontal, &m_mirrorVertical);
    // the outline of another brush is no measure of this one
    if (m_shownPreset && m_shownPreset->name() != preset->name()) {
        m_shownPreset.clear();
    }
    m_active = m_size > 0;
}

qreal KisStrokeRecorder::widthAt(const KisPaintInformation &info) const
{
    if (!m_shownPreset || !m_shownPreset->settings()) {
        return m_size;
    }
    // as wide as the cursor shows it at this pressure (tilt, speed...)
    KisPaintOpSettings::OutlineMode mode;
    mode.isVisible = true;
    const QRectF rect = m_shownPreset->settings()->brushOutline(info, mode, 1.0).boundingRect();
    const qreal width = qMax(rect.width(), rect.height());
    return width > 0 ? width : m_size;
}

void KisStrokeRecorder::add(int part, const QPointF &pos, qreal width)
{
    QVector<Sample> &samples = m_parts[part];
    if (!samples.isEmpty() && samples.last().pos == pos) {
        // the brush resting in place (an airbrush): only how wide it got
        samples.last().width = qMax(samples.last().width, width);
        return;
    }
    samples.append({pos, width});
}

void KisStrokeRecorder::addPoint(int part, const KisPaintInformation &info)
{
    if (!m_active) return;
    add(part, info.pos(), widthAt(info));
}

void KisStrokeRecorder::addLine(int part, const KisPaintInformation &pi1, const KisPaintInformation &pi2)
{
    if (!m_active) return;
    if (m_parts.value(part).isEmpty()) addPoint(part, pi1);
    addPoint(part, pi2);
}

void KisStrokeRecorder::addCurve(int part, const KisPaintInformation &pi1, const QPointF &control1, const QPointF &control2,
                                 const KisPaintInformation &pi2)
{
    if (!m_active) return;
    if (m_parts.value(part).isEmpty()) addPoint(part, pi1);
    const qreal w1 = widthAt(pi1);
    const qreal w2 = widthAt(pi2);
    // along the curve, often enough that the straight steps between stay on it
    const qreal length = QLineF(pi1.pos(), control1).length() + QLineF(control1, control2).length()
        + QLineF(control2, pi2.pos()).length();
    const int steps = qBound(1, qCeil(length / 4.0), 64);
    for (int i = 1; i <= steps; i++) {
        const qreal t = qreal(i) / steps;
        const qreal u = 1 - t;
        const QPointF pos = u * u * u * pi1.pos() + 3 * u * u * t * control1 + 3 * u * t * t * control2 + t * t * t * pi2.pos();
        add(part, pos, w1 + (w2 - w1) * t);
    }
}

void KisStrokeRecorder::addOutline(const QPolygonF &outline, bool closed)
{
    if (!m_active || outline.isEmpty()) return;
    const int part = m_nextOutline--;
    Q_FOREACH (const QPointF &point, outline) add(part, point, m_size);
    if (closed && outline.size() > 2 && outline.first() != outline.last()) add(part, outline.first(), m_size);
}

void KisStrokeRecorder::addArea(const QPainterPath &area)
{
    if (!m_active) return;
    Q_FOREACH (const QPolygonF &polygon, area.toSubpathPolygons()) {
        if (polygon.size() >= 3) m_areas << polygon.toPolygon();
    }
}

void KisStrokeRecorder::finish()
{
    KisStrokeLog::Strokes strokes;
    if (!m_areas.isEmpty()) {
        // paint no brush made: it covers what was under it (and is under the brush's outline painted with it)
        KisStrokeLog::Stroke fill = m_template;
        fill.brush.clear();
        fill.erasing = false;
        fill.area = m_areas;
        strokes << fill;
    }
    for (auto it = m_parts.constBegin(); it != m_parts.constEnd(); ++it) {
        QVector<QPointF> points;
        QVector<qreal> widths;
        Q_FOREACH (const Sample &sample, it.value()) {
            points << sample.pos;
            widths << sample.width;
        }
        QVector<Sample> samples;
        Q_FOREACH (int i, KisStrokeLog::simplifiedPath(points, widths)) samples << it.value()[i];
        if (samples.isEmpty()) continue;
        KisStrokeLog::Stroke stroke = m_template;
        qreal widest = 0;
        Q_FOREACH (const Sample &sample, samples) widest = qMax(widest, sample.width);
        if (widest <= 0) continue;
        stroke.size = float(widest);
        Q_FOREACH (const Sample &sample, samples) {
            stroke.path << sample.pos.toPoint();
            stroke.widths.append(char(qBound(1, qRound(sample.width / widest * 255), 255)));
        }
        strokes << stroke;
    }

    // the canvas's mirror paints them over again
    const int painted = strokes.size();
    const QVector<QPair<bool, bool>> mirrors {
        {true, false}, {false, true}, {true, true}};
    for (const QPair<bool, bool> &mirror : mirrors) {
        if ((mirror.first && !m_mirrorHorizontal) || (mirror.second && !m_mirrorVertical)) continue;
        for (int i = 0; i < painted; i++) {
            KisStrokeLog::Stroke copy = strokes[i];
            copy.path = mirrored(copy.path, m_axesCenter, mirror.first, mirror.second);
            for (int k = 0; k < copy.area.size(); k++) copy.area[k] = mirrored(copy.area[k], m_axesCenter, mirror.first, mirror.second);
            strokes << copy;
        }
    }

    QMutexLocker locker(&m_mutex);
    m_strokes = strokes;
}

KisStrokeLog::Strokes KisStrokeRecorder::strokes() const
{
    QMutexLocker locker(&m_mutex);
    KisStrokeLog::Strokes strokes = m_strokes;
    // where the selection was when it was painted through (asked from the stroke's end, in its thread)
    if (m_selection && !strokes.isEmpty()) {
        const QVector<QPolygon> clip = KisStrokeLog::outlineOf(m_selection);
        for (int i = 0; i < strokes.size(); i++) strokes[i].clip = clip;
    }
    return strokes;
}
