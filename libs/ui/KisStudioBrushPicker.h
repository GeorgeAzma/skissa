/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIOBRUSHPICKER_H
#define KISSTUDIOBRUSHPICKER_H

#include "widgets/KisStudioPicker.h"

#include <memory>

class KisMainWindow;
class KisViewManager;
struct KisStudioBrushStrokes;

/**
 * All the brushes (presets), to pick one and to keep your own at hand.
 *
 * Every brush is a tile with a stroke it actually painted, in your current
 * color, made in the background as the picker opens (and kept for the next
 * time). The ones used lately come first, then yours in your order (the
 * favorite brushes of the pop-up palette until you change them); "All
 * brushes" shows the rest grouped by what they are for: sketching, ink,
 * paint, blending, erasing... worked out from their names and engines, so
 * that nothing needs tagging. Searching finds names, tags and those kinds.
 */
class KRITAUI_EXPORT KisStudioBrushPicker : public KisStudioPicker
{
    Q_OBJECT
public:
    explicit KisStudioBrushPicker(KisMainWindow *mainWindow);
    ~KisStudioBrushPicker() override;

    /// A brush was used, picked here or anywhere else: it goes first among the recent ones
    static void recordUse(const QString &presetName);
    /// The next or previous one of yours after the current brush, e.g. for the wheel; empty at either end
    static QString neighbour(KisMainWindow *mainWindow, const QString &current, int step);
    /// Makes the brush with that name the current one
    static void use(KisViewManager *view, const QString &presetName);
    /// A stroke the brush painted for the picker, if it painted one yet (in whatever color it was then)
    static QImage strokePicture(const QString &presetName);
    /// "d) Ink-2 Fineliner" -> "Ink-2 Fineliner": without the prefix the bundled presets sort themselves with
    static QString title(const QString &presetName);
    /// The active tool paints with the brush: a brush taken from the canvas or a layer, the canvas ring, are for it
    static bool paintToolActive();
    /// Your brushes, in your order
    static QStringList yourBrushes();
    static void setYourBrushes(const QStringList &presetNames);

private:
    /// The strokes exist before the picker: its Source paints them
    KisStudioBrushPicker(KisMainWindow *mainWindow, std::shared_ptr<KisStudioBrushStrokes> strokes);

    struct Private;
    const std::unique_ptr<Private> d;
};

#endif // KISSTUDIOBRUSHPICKER_H
