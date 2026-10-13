/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KISCOLORDROP_H
#define KISCOLORDROP_H

#include <QObject>
#include <QPoint>
#include <QScopedPointer>

#include <kritaui_export.h>
#include <kis_types.h>

class KoColor;
class KisView;
class KisViewManager;
class QColor;
class QMimeData;
class QWidget;

/**
 * A color dragged onto the canvas fills the area under it. While the color is over the canvas the area
 * it would fill is shown in that color; resting a moment, then moving sideways, tunes how far it spreads.
 * The fill reaches the middle of the lines (see KisLineArtFill) on a layer of its own when the active
 * layer holds the lines. After a drop, a button by it keeps filling with the same color.
 */
class KRITAUI_EXPORT KisColorDrop : public QObject
{
    Q_OBJECT
public:
    explicit KisColorDrop(KisView *view);
    ~KisColorDrop() override;

    /// Whether \p data carries a color: from a palette, a swatch or another application.
    static bool carriesColor(const QMimeData *data);
    /// The color \p data carries, if any.
    static bool colorOf(const QMimeData *data, KoColor *color);

    /// The color is over the canvas at \p canvasPos (canvas widget coordinates).
    void hover(const KoColor &color, const QPoint &canvasPos);
    /// The color left the canvas, or the drag ended without a drop here.
    void leave();
    /// Fills at \p canvasPos with \p color. Returns false if there was nothing to fill there.
    bool drop(const KoColor &color, const QPoint &canvasPos);

    /// Drags \p color out of \p source, shown as a round swatch under the pointer. Returns when it is dropped.
    static void startDrag(QWidget *source, const QColor &color);

    /// Whether a color dropped on \p node can recolor it: a paint layer that can be edited
    static bool canRecolor(KisNodeSP node);
    /// Gives every pixel of \p node \p color, keeping its transparency. One undo step.
    static void recolor(KisImageSP image, KisNodeSP node, const KoColor &color);

    /// A new layer of flat colors under the line art of the active layer, one color per enclosed area.
    static void autoFlats(KisViewManager *viewManager);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    struct Private;
    QScopedPointer<Private> d;
};

#endif
