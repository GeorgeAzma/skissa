/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTROKELOG_H
#define KISSTROKELOG_H

#include <QByteArray>
#include <QColor>
#include <QObject>
#include <QReadWriteLock>
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
 * The brushes and colors a node was painted with, newest last: the brush last
 * used on a layer, and the colors painted on it. Kept with the node (copied,
 * saved and removed with it); adding to it is an undo command, part of the
 * stroke it follows.
 */
class KRITAIMAGE_EXPORT KisStrokeLog
{
public:
    struct Stroke {
        quint64 id {0};
        QString brush;       ///< the preset's name
        float brushSize {0}; ///< the brush's size setting when it painted
        QRgb color {0};
        bool erasing {false};
    };
    using Strokes = QVector<Stroke>;

    KisStrokeLog();
    KisStrokeLog(const KisStrokeLog &rhs);
    KisStrokeLog &operator=(const KisStrokeLog &rhs);

    Strokes strokes() const;
    bool isEmpty() const;

    /// The newest stroke that was not erasing; false if none
    bool lastBrush(Stroke *result) const;
    /// The colors painted, newest first, each once
    QVector<QColor> colors(int count) const;

    QByteArray save() const;
    void load(const QByteArray &data);

    /// Adds \p strokes painted on \p node now, as an undo command
    static KUndo2Command *addCommand(KisNodeSP node, const Strokes &strokes);

    /// Says when any log changed
    static KisStrokeLogNotifier *notifier();

private:
    /// Adds them; the oldest no longer kept are returned
    Strokes append(const Strokes &strokes);
    /// Puts back strokes \p append let go, as the oldest
    void restore(const Strokes &dropped);
    void remove(const QVector<quint64> &ids);
    void set(const Strokes &strokes);

    friend class KisStrokeLogAddCommand;

    mutable QReadWriteLock m_lock;
    Strokes m_strokes;
};

#endif // KISSTROKELOG_H
