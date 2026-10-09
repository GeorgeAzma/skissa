/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTROKERECORDER_H
#define KISSTROKERECORDER_H

#include <QMap>
#include <QMutex>
#include <QPainterPath>
#include <QSharedPointer>

#include <KisStrokeLog.h>
#include <kis_paintop_preset.h>

#include "kis_resources_snapshot.h"
#include "kritaui_export.h"

class KisPaintInformation;

/**
 * Takes down what a stroke paints, as it is painted (in the GUI thread):
 * where the brush went and how wide it was there (with the pressure), and
 * the fills; mirrored as the canvas mirrors it, cut by the selection it is
 * painted through. The stroke's strategy keeps it in the node's
 * KisStrokeLog, as part of the stroke's undo.
 */
class KRITAUI_EXPORT KisStrokeRecorder
{
public:
    /**
     * What is painted with: the brush, its color, the selection it paints
     * through, the canvas's mirror. \p shownPreset is the brush as the
     * canvas has it, whose outline (as the cursor shows it) gives the
     * brush's width at a pressure; without it the brush is as wide
     * everywhere as its size. Nothing is taken down before.
     */
    void start(KisResourcesSnapshotSP resources, KisPaintOpPresetSP shownPreset = KisPaintOpPresetSP());
    bool isStarted() const { return m_started; }

    /// The brush at \p info, on part \p part of the stroke (each painter of a multibrush paints one)
    void addPoint(int part, const KisPaintInformation &info);
    void addLine(int part, const KisPaintInformation &pi1, const KisPaintInformation &pi2);
    void addCurve(int part, const KisPaintInformation &pi1, const QPointF &control1, const QPointF &control2,
                  const KisPaintInformation &pi2);
    /// An outline painted with the brush at its size, a part of its own
    void addOutline(const QPolygonF &outline, bool closed);
    /// Paint no brush made (a shape filled, a line drawn with a pen)
    void addArea(const QPainterPath &area);

    /// The stroke is over: what it painted is worked out
    void finish();
    /// What it painted (image coordinates), once finished
    KisStrokeLog::Strokes strokes() const;

private:
    struct Sample {
        QPointF pos;
        qreal width;
    };
    qreal widthAt(const KisPaintInformation &info) const;
    void add(int part, const QPointF &pos, qreal width);

    KisPaintOpPresetSP m_shownPreset;
    KisStrokeLog::Stroke m_template;
    qreal m_size {0};
    QPointF m_axesCenter;
    bool m_mirrorHorizontal {false};
    bool m_mirrorVertical {false};
    bool m_active {false};
    bool m_started {false};
    int m_nextOutline {-1};

    QMap<int, QVector<Sample>> m_parts;
    QVector<QPolygon> m_areas;

    mutable QMutex m_mutex;
    KisStrokeLog::Strokes m_strokes;
};

using KisStrokeRecorderSP = QSharedPointer<KisStrokeRecorder>;

#endif // KISSTROKERECORDER_H
