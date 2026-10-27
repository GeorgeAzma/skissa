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
#include <kis_node.h>

class KoColor;
class KisView;
class KisViewManager;
class QColor;
class QMimeData;
class QWidget;

/**
 * A color dragged onto the canvas fills the area under it. While the color is over the canvas the area
 * it would fill is shown in that color. The fill reaches the middle of the lines (see KisLineArtFill) on a
 * layer of its own when the active layer holds the lines. After a drop, a panel by it changes the gap it
 * closes, how far it spreads and whether it reaches under the lines, making the fill again, and keeps
 * filling with the same color.
 */
class KRITAUI_EXPORT KisColorDrop : public QObject
{
    Q_OBJECT
public:
    /// Where a fill goes: an existing layer, or a new "Colors" layer to add first (at \p parent, above \p above)
    struct FillTarget {
        KisNodeSP node;
        KisNodeSP newLayer;
        KisNodeSP parent;
        KisNodeSP above;

        KisNodeSP filled() const { return newLayer ? newLayer : node; }
    };

    /**
     * The layer a fill from \p point (image coordinates) paints, with \p active the current layer: when it
     * holds the lines there, a Colors layer of its own (below clear line art, Multiply above opaque), so the
     * lines stay on top. Inside \p selection, or on a layer holding no lines, the current layer. Every fill
     * (a drop, the Fill tool, Enclose and Fill) chooses so.
     */
    static FillTarget fillTarget(KisImageSP image, KisNodeSP active, const QPoint &point, KisSelectionSP selection = nullptr);
    /// Adds the new layer \p target asks for, as a job of \p stroke, so the fill and the layer are one step
    static void addFillLayer(KisImageSP image, KisStrokeId stroke, const FillTarget &target);
    /// The gap a fill closes when it is set to Auto (0): sized from the width of the lines near \p point
    static int autoGap(KisPaintDeviceSP reference, const QPoint &point, const QRect &bounds);
    /// Whether \p node is a layer of colors made by fills, which read the whole picture rather than themselves
    static bool isColorsLayer(KisNodeSP node);

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
    /// Fills from \p point (image coordinates) with the drop's options; the fill is one step
    bool fillAt(const KoColor &color, const QPoint &point);
    /// Takes back the last fill and makes it again with the panel's values, while it is the newest step
    void refillLast();

    struct Private;
    QScopedPointer<Private> d;
};

#endif
