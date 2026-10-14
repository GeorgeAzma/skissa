/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIOSMOOTHING_H
#define KISSTUDIOSMOOTHING_H

#include <QObject>
#include <QWidget>

#include <functional>

#include "kritaui_export.h"
#include "tool/kis_smoothing_options.h"
#include "widgets/KisStudioPopup.h"

class KisMainWindow;

/**
 * The line smoothing of the brush tool in use, as one amount: 0 is off, and the rest is the smoothing
 * distance by ratios (as sizes are dragged). The brush chip on the top bar shows it behind the brush's name,
 * dragging it changes it, and a click opens the smoothing menu (KisStudioSmoothingPopup).
 */
class KRITAUI_EXPORT KisStudioSmoothing : public QObject
{
    Q_OBJECT
public:
    static KisStudioSmoothing *instance();

    /// The smoothing of the brush tool in use in \p mainWindow's view; null when that tool has none
    static KisSmoothingOptionsSP options(KisMainWindow *mainWindow);
    /// 0 when off, up to 1; -1 without smoothing to show
    static qreal amount(KisMainWindow *mainWindow);
    /// Off at 0; turning it on picks the kind used last (weighted at first)
    static void setAmount(KisMainWindow *mainWindow, qreal amount);
    /// The amount as the number it is ("50") or "Off"
    static QString text(KisMainWindow *mainWindow);
    /// The smoothing kind, given to the tool so its own options follow
    static void setType(KisMainWindow *mainWindow, KisSmoothingOptions::SmoothingType type);
    /// After the options were changed directly: the tool's own options and everything showing it follow
    static void changed(KisMainWindow *mainWindow);

    static qreal distanceToAmount(qreal distance);
    static qreal amountToDistance(qreal amount);

Q_SIGNALS:
    void sigChanged();

private:
    KisStudioSmoothing() = default;
};

/// Smoothing, drawn on a wobbly line as it would come out, its kinds as pills and each kind's few settings
class KRITAUI_EXPORT KisStudioSmoothingPopup : public KisStudioPopup
{
    Q_OBJECT
public:
    /// Opens the menu below \p anchor, or closes it if open; \p everything shows the tool's whole options
    static void toggle(KisMainWindow *mainWindow, QWidget *anchor, std::function<void()> everything);
    ~KisStudioSmoothingPopup() override;

private:
    KisStudioSmoothingPopup(KisMainWindow *mainWindow, std::function<void()> everything);
    void rebuild();

    struct Private;
    Private *const d;
};

#endif // KISSTUDIOSMOOTHING_H
