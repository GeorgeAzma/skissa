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
 * Thumbnails that show what a brush preset paints: a short wavy stroke drawn
 * by the preset's own paintop engine. Rendered on demand, one at a time in the
 * background, and cached. Used only when "Show brushes as strokes" is on
 * (see isEnabled()).
 */
class KRITAUI_EXPORT KisPresetStrokeThumbnails : public QObject
{
    Q_OBJECT
public:
    static KisPresetStrokeThumbnails *instance();
    ~KisPresetStrokeThumbnails() override;

    /// Whether brushes are shown as strokes instead of their preset icons
    static bool isEnabled();
    static void setEnabled(bool on);

    /**
     * The stroke painted by preset \p id (version \p md5) in color \p ink,
     * on a transparent background. Returns a null image while it is still
     * being rendered (changed() is emitted when it is ready), or when the
     * engine paints nothing by itself (e.g. smudge or filter brushes).
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
