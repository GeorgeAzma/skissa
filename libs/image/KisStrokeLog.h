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
 * What brush painted what on a node: for every stroke, the brush, its
 * width along the way, its color and roughly where it went (a point every
 * so often), and where paint that is no brush's (a fill, a gradient)
 * covered it. Asked "which brush is here?" it gives the newest stroke
 * under a point, so that a brush (with its size and color) can be taken
 * from the canvas, and a layer knows the brush last used on it and the
 * colors painted on it.
 *
 * Kept with the node, so that it is copied, duplicated, saved and removed
 * with it, in the node's paint device coordinates (relative to the
 * device's offset), so that moving the layer moves it too. Every change
 * is an undo command, part of the change of the pixels it follows: a
 * stroke undone is gone from here too, a transform moves its strokes.
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
     * Small: the brushes and the selections once each, every stroke as
     * indexes into them and its points as the first one and then steps from
     * one to the next; compressed. Frames are saved as the time of their
     * keyframe, as frame ids are not kept in the file: \p frameToTime gives
     * it for a frame (-1 for none: the device has no frames; -2: the frame
     * is gone, the strokes on it are not kept), \p timeToFrame the frame
     * at a time (-1: none).
     */
    QByteArray save(const std::function<int(int frame)> &frameToTime) const;
    void load(const QByteArray &data, const std::function<int(int time)> &timeToFrame);

    /// The frame of \p device a stroke on it is on now (see Stroke::frame)
    static int currentFrame(KisPaintDeviceSP device);

    // ------------------------------------------------------------ changes, each an undo command

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
    /// The strokes of \p node's current frame moved as \p move says
    static KUndo2Command *mapCommand(KisNodeSP node, const Move &move);
    /// All the strokes of \p sources (bottom to top) for \p target, which they were merged into
    static KUndo2Command *mergeCommand(KisNodeSP target, KisNodeList sources);

    /**
     * The fewest of a brush's \p points (with its \p widths there) from which
     * the stroke, its edges included, is redrawn within a tenth of its width
     * (and half a pixel): the indexes of those kept, the ends always.
     */
    static QVector<int> simplifiedPath(const QVector<QPointF> &points, const QVector<qreal> &widths);

    /// A selection as outlines, small enough to keep with every stroke painted through it
    static QVector<QPolygon> outlineOf(KisSelectionSP selection);

    // ------------------------------------------------------------ what is seen

    struct Pick {
        KisNodeSP node;
        Stroke stroke;
    };
    /**
     * The brush stroke seen at \p imagePos: on the topmost visible layer
     * with paint there, the newest stroke under it. False if that paint is
     * no brush's, or there is none.
     */
    static bool pick(KisNodeSP root, const QPoint &imagePos, Pick *result);

    /// Says when any log changed (e.g. for colors shown from it)
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
