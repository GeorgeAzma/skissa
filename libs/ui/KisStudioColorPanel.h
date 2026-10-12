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
 * The color panel: OKLCH pickers, where equal steps look equal (a hue ring around a lightness/chroma square, or
 * sliders), or Krita's classic HSV ring and triangle; the current, previous and recent colors under them.
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
    /// Once, before the main window creates its dockers
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
