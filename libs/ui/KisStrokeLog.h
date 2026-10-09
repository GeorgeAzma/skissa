/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTROKELOG_H
#define KISSTROKELOG_H

#include <QColor>
#include <QObject>
#include <QPolygon>
#include <QRect>
#include <QSharedPointer>
#include <QUuid>
#include <QVector>

#include <kis_types.h>

#include "kritaui_export.h"

/**
 * What painted what in an image: for every brush stroke, the layer, the
 * brush, its size and color, and roughly where it went (a few points along
 * it). Asked "which stroke is here?" it gives the newest one under a point
 * that still has paint there, so that a brush (with its size and color)
 * can be taken from the canvas, and a layer knows the brush last used on
 * it and the colors painted on it.
 *
 * Kept with the document (a few bytes a stroke, compressed): one log per
 * image, made on first use.
 */
class KRITAUI_EXPORT KisStrokeLog : public QObject
{
    Q_OBJECT
public:
    struct Stroke {
        QUuid node;
        QString brush;      ///< the preset's name
        qreal size {0};     ///< the brush size, in pixels
        QColor color;
        bool erasing {false};
        QPolygon path;      ///< image pixels, a point every so often
        QRect bounds;       ///< around the path, the brush's radius included
    };

    /// The log of \p image, made the first time it is asked for
    static KisStrokeLog *forImage(KisImageWSP image);
    /// The log of \p image if it has one (e.g. from a saving thread, which must not make one)
    static KisStrokeLog *existing(const KisImage *image);
    /// A copy of one image's log for another, e.g. a document copied for saving
    static void copy(KisImageSP from, KisImageSP to);

    void add(const Stroke &stroke);
    /// The newest stroke under \p imagePos (on a visible layer that still has paint there); null if none
    const Stroke *strokeAt(KisImageSP image, const QPoint &imagePos) const;
    /// The newest stroke on a layer that was not erasing; null if none
    const Stroke *lastOn(const QUuid &node) const;
    /// The colors painted on a layer, newest first, each once
    QVector<QColor> colorsOn(const QUuid &node, int count) const;

    QByteArray save() const;
    void load(const QByteArray &data);
    bool isEmpty() const { return m_strokes.isEmpty(); }

Q_SIGNALS:
    void changed();

private:
    explicit KisStrokeLog(QObject *parent);

    QVector<Stroke> m_strokes;
};

#endif // KISSTROKELOG_H
