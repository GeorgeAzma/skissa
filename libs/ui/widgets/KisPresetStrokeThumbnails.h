/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_PRESET_STROKE_THUMBNAILS_H
#define KIS_PRESET_STROKE_THUMBNAILS_H

#include <QColor>
#include <QImage>
#include <QObject>
#include <QScopedPointer>

#include "kritaui_export.h"

/**
 * Brushes shown by what they paint: a short wave painted with each brush
 * preset, made by its own engine. Made when first asked for, one at a time
 * in the background (a preset's engine paints it as on the canvas), and
 * kept. Off unless "Show brushes as strokes" is on (see isEnabled()).
 */
class KRITAUI_EXPORT KisPresetStrokeThumbnails : public QObject
{
    Q_OBJECT
public:
    static KisPresetStrokeThumbnails *instance();
    ~KisPresetStrokeThumbnails() override;

    /// The setting: brushes as strokes rather than as their pictures
    static bool isEnabled();
    static void setEnabled(bool on);

    /**
     * The stroke of the preset with resource \p id (as of \p md5), in \p ink
     * on transparent: a null image while it is being made (changed() is
     * emitted when it is ready) or when its engine paints nothing of its
     * own (a smudge, a filter: its picture is shown then).
     */
    QImage stroke(int id, const QString &md5, const QColor &ink);

Q_SIGNALS:
    void changed();

private:
    KisPresetStrokeThumbnails();
    void next();
    void finished();

    struct Private;
    QScopedPointer<Private> d;
};

#endif
