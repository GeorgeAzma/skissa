/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef KISSTUDIOEFFECTBRUSH_H
#define KISSTUDIOEFFECTBRUSH_H

#include <QWidget>

#include <kis_types.h>

class KisMainWindow;

/**
 * An effect painted on: it becomes a filter mask showing nowhere, and the brush paints where it shows (white on
 * the mask; the eraser takes it away). A small bar over the canvas finishes it: Apply makes it the layer's pixels and
 * the mask goes, Keep leaves the mask to edit later, Drop takes it away. Leaving the mask (another layer picked) keeps
 * it. The paint color is white while painting and comes back after.
 */
class KisStudioEffectBrush : public QWidget
{
public:
    /// Starts painting \p config (an effect named \p name) on \p layer in \p mainWindow
    static void start(KisMainWindow *mainWindow, KisLayerSP layer, KisFilterConfigurationSP config, const QString &name);

    ~KisStudioEffectBrush() override;

protected:
    void paintEvent(QPaintEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    KisStudioEffectBrush(KisMainWindow *mainWindow, KisLayerSP layer, KisFilterMaskSP mask, const QString &name);
    void apply();
    void keep();
    void drop();
    /// Back to what was before painting: the color, the layer picked; the bar goes
    void finish(bool pickLayer);
    void place();

    struct Private;
    Private *const d;
};

#endif
