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

/// Emits changed() whenever any KisStrokeLog changes (see KisStrokeLog::notifier())
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
 * The brushes and colors a node was painted with, oldest first. Used to recall
 * a layer's last brush and the colors painted on it. It is copied, saved and
 * removed together with its node, and new strokes are added through an undo
 * command that belongs to the stroke itself.
 */
class KRITAIMAGE_EXPORT KisStrokeLog
{
public:
    struct Stroke {
        quint64 id {0};
        QString brush;       ///< the preset's name
        float brushSize {0}; ///< the brush size at the time of the stroke
        QRgb color {0};
        bool erasing {false};
    };
    using Strokes = QVector<Stroke>;

    KisStrokeLog();
    KisStrokeLog(const KisStrokeLog &rhs);
    KisStrokeLog &operator=(const KisStrokeLog &rhs);

    Strokes strokes() const;
    bool isEmpty() const;

    /// The newest non-erasing stroke; returns false if there is none
    bool lastBrush(Stroke *result) const;
    /// Up to \p count distinct painted colors, newest first
    QVector<QColor> colors(int count) const;

    QByteArray save() const;
    void load(const QByteArray &data);

    /// Returns an undo command that adds \p strokes to \p node's log
    static KUndo2Command *addCommand(KisNodeSP node, const Strokes &strokes);

    /// Shared notifier for changes to any log; lives in the main thread
    static KisStrokeLogNotifier *notifier();

private:
    /// Appends \p strokes and returns the oldest ones dropped to stay within the limit
    Strokes append(const Strokes &strokes);
    /// Puts strokes dropped by append() back at the start
    void restore(const Strokes &dropped);
    void remove(const QVector<quint64> &ids);
    void set(const Strokes &strokes);

    friend class KisStrokeLogAddCommand;

    mutable QReadWriteLock m_lock;
    Strokes m_strokes;
};

#endif // KISSTROKELOG_H
