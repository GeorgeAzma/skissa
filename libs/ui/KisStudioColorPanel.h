/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIOCOLORPANEL_H
#define KISSTUDIOCOLORPANEL_H

#include <QDockWidget>

#include <KoCanvasObserverBase.h>
#include <KoDockFactoryBase.h>

#include <memory>

#include "kritaui_export.h"

/**
 * One color panel in perceptual coordinates.
 *
 * HSV's value is not lightness: at the same "value" yellow is far lighter
 * than blue, so HSV pickers make value structure hard to control. The
 * pickers here are OKLCH, where equal steps look like equal steps: a ring of
 * hues around the hue's square of lightness (down) and chroma (across),
 * which shows where sRGB ends (its shape changes with the hue); or, compact,
 * a track each for lightness, chroma and hue beside the color. Krita's
 * classic HSV ring and triangle is the third choice, for those used to it.
 * Below: the current and previous color and the colors last painted with.
 */
class KRITAUI_EXPORT KisStudioColorPanel : public QDockWidget, public KoCanvasObserverBase
{
    Q_OBJECT
public:
    KisStudioColorPanel();
    ~KisStudioColorPanel() override;

    QString observerName() override { return QStringLiteral("KisStudioColorPanel"); }
    void setCanvas(KoCanvasBase *canvas) override;
    void unsetCanvas() override;

    static QString id();
    /// Registers the panel's factory once, before the main window creates its dockers
    static void registerFactory();

    struct Private;

private:
    /// No taller than the picker needs, where that is less than the room it is given
    void fitHeight();

    const std::unique_ptr<Private> d;
};

class KisStudioColorPanelFactory : public KoDockFactoryBase
{
public:
    QString id() const override;
    DockPosition defaultDockPosition() const override { return DockRight; }
    QDockWidget *createDockWidget() override;
};

#endif // KISSTUDIOCOLORPANEL_H
