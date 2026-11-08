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

/// The slider or two of an effect made of a larger one (Blur of Detail, Grain of Noise)
class SkissaSliders : public KisConfigWidget
{
    Q_OBJECT
public:
    SkissaSliders(QWidget *parent, const QString &filterId, const QVector<SkissaSlider> &sliders);

    void setConfiguration(const KisPropertiesConfigurationSP config) override;
    KisPropertiesConfigurationSP configuration() const override;

private:
    QString m_filterId;
    QVector<QPair<SkissaSlider, KisStudioValueRow *>> m_sliders;
    /// The settings as they came, so that those not shown go back as they were
    KisPropertiesConfigurationSP m_loaded;
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
 * picture and of its light streaks. A press on the image puts the middle there; a drag moves it.
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

    QSize sizeHint() const override;

private:
    void refresh();
    void refreshTiles();
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

#endif
