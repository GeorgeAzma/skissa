/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef SKISSACOLORWIDGET_H
#define SKISSACOLORWIDGET_H

#include <QScopedPointer>

#include <KisEffectCanvasEditor.h>
#include <kis_config_widget.h>
#include <kis_types.h>

/**
 * The Color effect's panel: pictures of the image under a few starting points, the curve being edited (chosen by
 * what it changes, then what it goes by), and discs for the white balance and the tints. A drag up or down on the
 * image bends the curve at the value of the pixel pressed on. Its last tab is the gradient map: looks, the stops
 * over how much of the image is at each lightness, and a drag on the image recolors the stop at that lightness.
 */
class SkissaColorWidget : public KisConfigWidget, public KisEffectCanvasEditor
{
    Q_OBJECT
    Q_INTERFACES(KisEffectCanvasEditor)
public:
    SkissaColorWidget(QWidget *parent, KisPaintDeviceSP dev);
    ~SkissaColorWidget() override;

    void setConfiguration(const KisPropertiesConfigurationSP config) override;
    KisPropertiesConfigurationSP configuration() const override;

    void canvasHovered(const QPoint &pixel) override;
    bool canvasPressed(const QPoint &pixel) override;
    QString canvasDragged(const QPointF &moved, const QPointF &at) override;
    void canvasReleased() override;
    /// Up or down bends the curve at the value pressed on; on the map, sideways also turns the stop's hue
    Qt::Orientations canvasAxes() const override;

    /// As tall as the rows are at the panel's usual width: the panel sizes itself by this
    QSize sizeHint() const override;

private:
    void refresh();
    void refreshChips();
    void refreshMap();
    void mapChanged();

    struct Private;
    QScopedPointer<Private> d;
};

#endif
