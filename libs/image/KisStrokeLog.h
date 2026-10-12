/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTROKELOG_H
#define KISSTROKELOG_H

#include <functional>
#include <optional>

#include <QByteArray>
#include <QColor>
#include <QObject>
#include <QPolygon>
#include <QReadWriteLock>
#include <QRect>
#include <QVector>

#include "kis_types.h"
#include "kritaimage_export.h"

class KUndo2Command;

/// Sends KisStrokeLog::notifier()'s signal
class KRITAIMAGE_EXPORT KisStrokeLogNotifier : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;
    void notify() { Q_EMIT changed(); }
Q_SIGNALS:
    void changed();
};

/**
 * What brush painted what on a node: for every stroke the brush, its width
 * along the way, its color and roughly where it went, and where paint no
 * brush made (a fill, a gradient) covered it. It answers "which brush is
 * here?" and "which brush was last used on this layer?".
 *
 * Kept with the node (copied, saved and removed with it) in its paint
 * device's coordinates. Every change is an undo command, part of the
 * change of the pixels it follows.
 */
class KRITAIMAGE_EXPORT KisStrokeLog
{
public:
    struct Stroke {
        quint64 id {0};
        QString brush;              ///< the preset's name; empty for paint no brush made (a fill), which covers what is under it
        float brushSize {0};        ///< the brush's size setting when it was painted
        float size {0};             ///< the widest the brush was along it, in pixels
        QRgb color {0};
        bool erasing {false};
        int frame {-1};             ///< the paint device's frame it is on; -1 while the device has no frames
        QPolygon path;              ///< a brush's: where it went
        QByteArray widths;          ///< a brush's: for each point of the path, its width there in 255ths of size (none: size all along)
        QVector<QPolygon> area;     ///< paint no brush made: its outline(s), even-odd
        QVector<QPolygon> clip;     ///< the selection it was painted through, its outline(s), even-odd; none: no selection
        QRect bounds;               ///< around all of it

        bool isBrush() const { return !brush.isEmpty(); }
        /// Sets bounds from the rest
        void updateBounds();
        /// Whether it is at \p point (device coordinates)
        bool covers(const QPoint &point) const;
    };
    using Strokes = QVector<Stroke>;

    /// Moves a point (image coordinates)
    using Mapper = std::function<QPointF(const QPointF &)>;
    /// Whether a point is in what moves (image coordinates)
    using Selected = std::function<bool(const QPoint &)>;
    /// Whether the way from one point to the next is torn apart by the move (e.g. a wrap around)
    using Tears = std::function<bool(const QPointF &, const QPointF &)>;

    KisStrokeLog();
    KisStrokeLog(const KisStrokeLog &rhs);
    KisStrokeLog &operator=(const KisStrokeLog &rhs);

    Strokes strokes() const;
    bool isEmpty() const;

    /**
     * The newest brush stroke at \p devicePos on \p frame (see Stroke::frame).
     * False if there is none, or if paint no brush made covers it (then
     * \p covered is set).
     */
    bool strokeAt(const QPoint &devicePos, int frame, Stroke *result, bool *covered = nullptr) const;
    /// The newest brush stroke that was not erasing; false if none
    bool lastBrush(Stroke *result) const;
    /// The colors painted, newest first, each once
    QVector<QColor> colors(int count) const;

    /**
     * Compact: brushes and selections once each, points as steps, compressed.
     * Frames are saved as the time of their keyframe: \p frameToTime gives it
     * (-1: the device has no frames; -2: the frame is gone, its strokes are
     * not kept), \p timeToFrame the frame at a time (-1: none).
     */
    QByteArray save(const std::function<int(int frame)> &frameToTime) const;
    void load(const QByteArray &data, const std::function<int(int time)> &timeToFrame);

    /// The frame of \p device a stroke on it is on now (see Stroke::frame)
    static int currentFrame(KisPaintDeviceSP device);

    // changes, each an undo command

    /// Adds strokes (image coordinates) painted on \p node now
    static KUndo2Command *addCommand(KisNodeSP node, Strokes strokes);
    /// How a node's pixels moved, for its strokes to move alike
    struct Move {
        Mapper map;                 ///< where a point went (image coordinates)
        Selected selected;          ///< only what is here moved, over the rest (none: everything)
        QRect selectedBounds;       ///< around what is selected: the strokes outside it are not looked at
        Tears tears;                ///< where a stroke is cut in two
        /// Where the device was before its pixels moved, if moving them moved it too (none: it did not)
        std::optional<QPoint> offsetBefore;
    };
    /// What moves with a selection: its pixels selected more than half
    static Move moveIn(KisSelectionSP selection, Mapper map);
    /// Moves the strokes of \p node's current frame
    static KUndo2Command *mapCommand(KisNodeSP node, const Move &move);
    /// The strokes of \p sources (bottom to top) for \p target, which they were merged into
    static KUndo2Command *mergeCommand(KisNodeSP target, KisNodeList sources);

    /// The indexes of the fewest \p points (with \p widths) that keep the stroke's edges within a tenth of its width
    static QVector<int> simplifiedPath(const QVector<QPointF> &points, const QVector<qreal> &widths);

    /// A selection as outlines, small enough to keep with every stroke painted through it
    static QVector<QPolygon> outlineOf(KisSelectionSP selection);

    struct Pick {
        KisNodeSP node;
        Stroke stroke;
    };
    /// The newest brush stroke at \p imagePos on the topmost visible layer with paint there; false if no brush made that paint
    static bool pick(KisNodeSP root, const QPoint &imagePos, Pick *result);

    /// Says when any log changed
    static KisStrokeLogNotifier *notifier();

private:
    /// Adds them; the oldest strokes no longer kept are returned
    Strokes append(const Strokes &strokes);
    /// Puts back strokes \p append let go, as the oldest
    void restore(const Strokes &dropped);
    void remove(const QVector<quint64> &ids);
    void set(const Strokes &strokes);

    friend class KisStrokeLogAddCommand;
    friend class KisStrokeLogSetCommand;

    mutable QReadWriteLock m_lock;
    Strokes m_strokes;
};

#endif // KISSTROKELOG_H
