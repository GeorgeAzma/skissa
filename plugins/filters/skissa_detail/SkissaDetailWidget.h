/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef SKISSADETAILWIDGET_H
#define SKISSADETAILWIDGET_H

#include <QScopedPointer>

#include <KisEffectCanvasEditor.h>
#include <kis_config_widget.h>
#include <kis_types.h>

#include "SkissaDetail.h"
#include "SkissaLens.h"

class KisStudioValueRow;

/// One setting of an effect with a few sliders
struct SkissaSlider {
    const char *key;
    QString name;
    qreal maximum;
    int decimals;
    QString suffix;
    qreal shownPerValue {1}; ///< percent shows a fraction a hundred times larger
    QString tip;
    qreal minimum {0};
    /// Another setting kept in proportion to this one (Krita's blur up and down, to across)
    const char *follower {nullptr};
};

/**
 * The slider or two of an effect made of a larger one (Blur of Detail, Grain of Noise). A drag on the image up or
 * down changes its amount (or its only size), sideways its size.
 */
class SkissaSliders : public KisConfigWidget, public KisEffectCanvasEditor
{
    Q_OBJECT
    Q_INTERFACES(KisEffectCanvasEditor)
public:
    SkissaSliders(QWidget *parent, const QString &filterId, const QVector<SkissaSlider> &sliders);

    void setConfiguration(const KisPropertiesConfigurationSP config) override;
    KisPropertiesConfigurationSP configuration() const override;

    void canvasHovered(const QPoint &) override {}
    bool canvasPressed(const QPoint &pixel) override;
    QString canvasDragged(const QPointF &moved, const QPointF &at) override;
    void canvasReleased() override;
    Qt::Orientations canvasAxes() const override;

private:
    QString m_filterId;
    QVector<QPair<SkissaSlider, KisStudioValueRow *>> m_sliders;
    /// The settings as they came, so that those not shown go back as they were
    KisPropertiesConfigurationSP m_loaded;
    /// The rows a drag on the image changes up or down and sideways (-1: none), and their values at the press
    int m_vertical {-1};
    int m_horizontal {-1};
    qreal m_startVertical {0};
    qreal m_startHorizontal {0};
};

/**
 * Detail's panel: pictures of the image's busiest part under a few starting points, the curve of gain over the size
 * of features, and Edges. A drag up or down on the image strengthens or removes the size of feature that stands out
 * where it was pressed.
 */
class SkissaDetailWidget : public KisConfigWidget, public KisEffectCanvasEditor
{
    Q_OBJECT
    Q_INTERFACES(KisEffectCanvasEditor)
public:
    SkissaDetailWidget(QWidget *parent, KisPaintDeviceSP dev);
    ~SkissaDetailWidget() override;

    void setConfiguration(const KisPropertiesConfigurationSP config) override;
    KisPropertiesConfigurationSP configuration() const override;

    void canvasHovered(const QPoint &pixel) override;
    bool canvasPressed(const QPoint &pixel) override;
    QString canvasDragged(const QPointF &moved, const QPointF &at) override;
    void canvasReleased() override;
    /// Up or down only: the size pressed on stands out more or less
    Qt::Orientations canvasAxes() const override { return Qt::Vertical; }

    QSize sizeHint() const override;

private:
    void refresh();
    void refreshTiles();

    struct Private;
    QScopedPointer<Private> d;
};

/**
 * Noise's panel: pictures of the image under a few kinds of noise, the curve of how strong each size of it is, and
 * how much it changes lightness, color, transparency and the pixels' places. A drag up or down on the image makes
 * all of it stronger or weaker.
 */
class SkissaNoiseWidget : public KisConfigWidget, public KisEffectCanvasEditor
{
    Q_OBJECT
    Q_INTERFACES(KisEffectCanvasEditor)
public:
    SkissaNoiseWidget(QWidget *parent, KisPaintDeviceSP dev);
    ~SkissaNoiseWidget() override;

    void setConfiguration(const KisPropertiesConfigurationSP config) override;
    KisPropertiesConfigurationSP configuration() const override;

    void canvasHovered(const QPoint &pixel) override;
    bool canvasPressed(const QPoint &pixel) override;
    QString canvasDragged(const QPointF &moved, const QPointF &at) override;
    void canvasReleased() override;
    /// Up or down only: all the noise stronger or fainter
    Qt::Orientations canvasAxes() const override { return Qt::Vertical; }

    QSize sizeHint() const override;

private:
    void refresh();
    void refreshTiles();

    struct Private;
    QScopedPointer<Private> d;
};

/**
 * Lens's panel: pictures of the whole image through a few lenses, then how much of each thing a lens does. A press
 * on the image puts the lens's middle there; a drag moves it.
 */
class SkissaLensWidget : public KisConfigWidget, public KisEffectCanvasEditor
{
    Q_OBJECT
    Q_INTERFACES(KisEffectCanvasEditor)
public:
    SkissaLensWidget(QWidget *parent, KisPaintDeviceSP dev);
    ~SkissaLensWidget() override;

    void setConfiguration(const KisPropertiesConfigurationSP config) override;
    KisPropertiesConfigurationSP configuration() const override;

    void canvasHovered(const QPoint &pixel) override;
    bool canvasPressed(const QPoint &pixel) override;
    QString canvasDragged(const QPointF &moved, const QPointF &at) override;
    void canvasReleased() override;
    bool canvasMiddle(QPointF *at) const override;

    QSize sizeHint() const override;

private:
    void refresh();
    void refreshTiles();
    void placeCenter(const QPointF &at);

    struct Private;
    QScopedPointer<Private> d;
};

/**
 * Radial blur's panel: pictures of the whole image streaked a few ways, then the zoom, the spin, and how much of the
 * picture and of its light streaks. A press on the image puts the middle there; a drag moves it. A sample streak
 * drawn beside the middle has a knob at its end: drawn in, it zooms; around, it spins.
 */
class SkissaRadialWidget : public KisConfigWidget, public KisEffectCanvasEditor
{
    Q_OBJECT
    Q_INTERFACES(KisEffectCanvasEditor)
public:
    SkissaRadialWidget(QWidget *parent, KisPaintDeviceSP dev);
    ~SkissaRadialWidget() override;

    void setConfiguration(const KisPropertiesConfigurationSP config) override;
    KisPropertiesConfigurationSP configuration() const override;

    void canvasHovered(const QPoint &pixel) override;
    bool canvasPressed(const QPoint &pixel) override;
    QString canvasDragged(const QPointF &moved, const QPointF &at) override;
    void canvasReleased() override;
    bool canvasMiddle(QPointF *at) const override;
    bool canvasStreak(qreal pixelsPerScreen, QPolygonF *path) const override;
    QString canvasStreakDragged(const QPointF &end) override;

    QSize sizeHint() const override;

private:
    void refresh();
    void refreshTiles();
    void showLightRows();
    void placeCenter(const QPointF &at);

    struct Private;
    QScopedPointer<Private> d;
};

/**
 * Fewer colors' panel: where the colors come from, each a picture of the whole layer reduced that way, how many and
 * how bright, and the dithering, each way of it as a ramp. A drag up or down on the
 * image gives more or fewer colors, sideways lighter or darker.
 */
class SkissaFewerWidget : public KisConfigWidget, public KisEffectCanvasEditor
{
    Q_OBJECT
    Q_INTERFACES(KisEffectCanvasEditor)
public:
    SkissaFewerWidget(QWidget *parent, KisPaintDeviceSP dev);
    ~SkissaFewerWidget() override;

    void setConfiguration(const KisPropertiesConfigurationSP config) override;
    KisPropertiesConfigurationSP configuration() const override;

    void canvasHovered(const QPoint &pixel) override;
    bool canvasPressed(const QPoint &pixel) override;
    QString canvasDragged(const QPointF &moved, const QPointF &at) override;
    void canvasReleased() override;

    QSize sizeHint() const override;

private:
    void refresh();
    void refreshTiles();

    struct Private;
    QScopedPointer<Private> d;
};

/**
 * Drawing's panel: looks as pictures of a busy part of the layer, then what is drawn (Size, Lines, Shading, Flow),
 * how the shading is drawn as pictures (each way with its own setting), and where the ink goes as pictures, with the
 * ink's and paper's colors. A drag on the image: up for more shading, sideways for bolder lines.
 */
class SkissaInkWidget : public KisConfigWidget, public KisEffectCanvasEditor
{
    Q_OBJECT
    Q_INTERFACES(KisEffectCanvasEditor)
public:
    SkissaInkWidget(QWidget *parent, KisPaintDeviceSP dev);
    ~SkissaInkWidget() override;

    void setConfiguration(const KisPropertiesConfigurationSP config) override;
    KisPropertiesConfigurationSP configuration() const override;

    void canvasHovered(const QPoint &pixel) override;
    bool canvasPressed(const QPoint &pixel) override;
    QString canvasDragged(const QPointF &moved, const QPointF &at) override;
    void canvasReleased() override;

    QSize sizeHint() const override;

private:
    void refresh();
    void showRows();
    /// The tiles as they are, and their pictures made again after a pause
    void refreshTiles();
    void showTiles();
    void makePictures();

    struct Private;
    QScopedPointer<Private> d;
};

/**
 * Shape's panel: looks as pictures of an edge of the layer, then its parts as tabs (Outline, Glow, Shadow, Bevel, Grow),
 * each with its own rows and a dot while it is on. A drag on the image: in Shadow, moves the shadow; otherwise
 * sideways for the part's size, up for its strength.
 */
class SkissaShapeWidget : public KisConfigWidget, public KisEffectCanvasEditor
{
    Q_OBJECT
    Q_INTERFACES(KisEffectCanvasEditor)
public:
    SkissaShapeWidget(QWidget *parent, KisPaintDeviceSP dev);
    ~SkissaShapeWidget() override;

    void setConfiguration(const KisPropertiesConfigurationSP config) override;
    KisPropertiesConfigurationSP configuration() const override;

    void canvasHovered(const QPoint &pixel) override;
    bool canvasPressed(const QPoint &pixel) override;
    QString canvasDragged(const QPointF &moved, const QPointF &at) override;
    void canvasReleased() override;
    /// Outline and Grow have only a size; Glow and Bevel have a strength too, and the shadow moves either way
    Qt::Orientations canvasAxes() const override;

    QSize sizeHint() const override;

private:
    void refresh();
    void showRows();
    void refreshTiles();
    void edited();

    struct Private;
    QScopedPointer<Private> d;
};

/**
 * Light's panel: looks as pictures of the whole layer, the threshold, then the ways it spreads as tabs (Glow, Star,
 * Streak, Halation), each with its own rows and a dot while it is on. A drag on the image: up stronger, sideways larger.
 */
class SkissaLightWidget : public KisConfigWidget, public KisEffectCanvasEditor
{
    Q_OBJECT
    Q_INTERFACES(KisEffectCanvasEditor)
public:
    SkissaLightWidget(QWidget *parent, KisPaintDeviceSP dev);
    ~SkissaLightWidget() override;

    void setConfiguration(const KisPropertiesConfigurationSP config) override;
    KisPropertiesConfigurationSP configuration() const override;

    void canvasHovered(const QPoint &pixel) override;
    bool canvasPressed(const QPoint &pixel) override;
    QString canvasDragged(const QPointF &moved, const QPointF &at) override;
    void canvasReleased() override;

    QSize sizeHint() const override;

private:
    void refresh();
    void showRows();
    void edited();

    struct Private;
    QScopedPointer<Private> d;
};

/**
 * Painting's panel: looks as pictures of a busy part of the layer, then the brush's size, how crisp the strokes are,
 * and how much they follow the shapes. A drag on the image: sideways the brush's size, up crisper.
 */
class SkissaPaintWidget : public KisConfigWidget, public KisEffectCanvasEditor
{
    Q_OBJECT
    Q_INTERFACES(KisEffectCanvasEditor)
public:
    SkissaPaintWidget(QWidget *parent, KisPaintDeviceSP dev);
    ~SkissaPaintWidget() override;

    void setConfiguration(const KisPropertiesConfigurationSP config) override;
    KisPropertiesConfigurationSP configuration() const override;

    void canvasHovered(const QPoint &pixel) override;
    bool canvasPressed(const QPoint &pixel) override;
    QString canvasDragged(const QPointF &moved, const QPointF &at) override;
    void canvasReleased() override;

    QSize sizeHint() const override;

private:
    void refresh();

    struct Private;
    QScopedPointer<Private> d;
};

/**
 * Lens blur's panel: the aperture as pictures of its shape, then evenly out of focus or a band in focus as pictures
 * of the layer, the size, the aperture's angle, and the band's width and angle. A drag on the image: sideways the
 * size; with a band, a press puts its middle there and a drag moves it.
 */
class SkissaFocusWidget : public KisConfigWidget, public KisEffectCanvasEditor
{
    Q_OBJECT
    Q_INTERFACES(KisEffectCanvasEditor)
public:
    SkissaFocusWidget(QWidget *parent, KisPaintDeviceSP dev);
    ~SkissaFocusWidget() override;

    void setConfiguration(const KisPropertiesConfigurationSP config) override;
    KisPropertiesConfigurationSP configuration() const override;

    void canvasHovered(const QPoint &pixel) override;
    bool canvasPressed(const QPoint &pixel) override;
    QString canvasDragged(const QPointF &moved, const QPointF &at) override;
    void canvasReleased() override;
    bool canvasMiddle(QPointF *at) const override;
    Qt::Orientations canvasAxes() const override { return Qt::Horizontal; }

    QSize sizeHint() const override;

private:
    void refresh();
    void edited();
    void placeCenter(const QPointF &at);

    struct Private;
    QScopedPointer<Private> d;
};

#endif
