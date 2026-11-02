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

class KisDoubleSliderSpinBox;

/// The slider or two of an effect made of Detail: a radius, an amount, edges
class SkissaDetailSliders : public KisConfigWidget
{
    Q_OBJECT
public:
    SkissaDetailSliders(QWidget *parent, SkissaDetail::Kind kind);

    void setConfiguration(const KisPropertiesConfigurationSP config) override;
    KisPropertiesConfigurationSP configuration() const override;

private:
    struct Slider {
        const char *key;
        KisDoubleSliderSpinBox *box;
        qreal shownPerValue; ///< percent shows a fraction a hundred times larger
    };

    SkissaDetail::Kind m_kind;
    QVector<Slider> m_sliders;
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
    QString canvasDragged(const QPointF &moved, bool fine) override;
    void canvasReleased() override;

    QSize sizeHint() const override;

private:
    void refresh();
    void refreshTiles();

    struct Private;
    QScopedPointer<Private> d;
};

#endif
