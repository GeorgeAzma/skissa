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
 * Records what a stroke paints, for the node's KisStrokeLog: where the
 * brush went and how wide it was there, and the fills; mirrored as the
 * canvas mirrors it, clipped by the selection it is painted through.
 */
class KRITAUI_EXPORT KisStrokeRecorder
{
public:
    /**
     * Takes the brush, color, selection and mirroring. \p shownPreset's
     * outline gives the brush's width at a pressure; without it the width
     * is the brush's size. Nothing is recorded before.
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
    /// Its outline is worked out at the stroke's end, away from the GUI thread
    KisSelectionSP m_selection;
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
