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
 * than blue, so HSV pickers make value structure hard to control. The plane
 * here is OKLCH: lightness down, chroma across, at the hue chosen on the
 * strip below; equal steps look like equal steps. Colors outside sRGB are
 * left out, so the plane shows what is actually reachable (its shape changes
 * with the hue). Below: the current and previous color and the colors last
 * painted with.
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
