/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIOSMOOTHING_H
#define KISSTUDIOSMOOTHING_H

#include <QObject>
#include <QWidget>

#include "kritaui_export.h"
#include "tool/kis_smoothing_options.h"

class KisMainWindow;

/**
 * The line smoothing of the brush tool in use, as one amount: 0 is off, and the rest is the smoothing
 * distance by ratios (as sizes are dragged). The brush chip on the top bar shows it behind the brush's name
 * and dragging it changes it; the brush editor's Stroke tab (KisStudioStrokePage) has the rest.
 * Each brush keeps its own smoothing, in the fork's config by the brush's name.
 */
class KRITAUI_EXPORT KisStudioSmoothing : public QObject
{
    Q_OBJECT
public:
    static KisStudioSmoothing *instance();
    /// Keeps each brush's smoothing for \p mainWindow, and gives it back when the brush is picked
    static void watch(KisMainWindow *mainWindow);

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
    /// Whether the brush in use paints whole pixels (pixel art), where pixel perfect lines are offered
    static bool pixelBrush(KisMainWindow *mainWindow);

    static qreal distanceToAmount(qreal distance);
    static qreal amountToDistance(qreal amount);

Q_SIGNALS:
    void sigChanged();
    /// Changed in the tool's own options
    void sigToolChanged();
    /// The tool's whole options asked for, from the Stroke tab
    void sigToolOptionsRequested(KisMainWindow *mainWindow);

private:
    KisStudioSmoothing() = default;
};

/**
 * How the brush tool lays a stroke down: its smoothing drawn on a wobbly line as it would come out, the kinds
 * as pills and each kind's few settings, pixel perfect lines for pixel brushes, Shapes, snapping to guides
 * when the image has some, and the tool's whole options.
 */
class KRITAUI_EXPORT KisStudioStrokePage : public QWidget
{
    Q_OBJECT
public:
    KisStudioStrokePage(KisMainWindow *mainWindow, QWidget *parent);
    ~KisStudioStrokePage() override;

    /// Whether the tool in use paints freehand with the brush, which is what this page is about
    static bool applies(KisMainWindow *mainWindow);

Q_SIGNALS:
    /// Its rows changed: the window holding it fits it again
    void sigResized();

private:
    void rebuild();

    struct Private;
    Private *const d;
};

#endif // KISSTUDIOSMOOTHING_H
