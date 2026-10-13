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
 * Picker for brush presets. Each tile shows a sample stroke in the current
 * color, rendered in the background and cached between openings. Brushes that
 * aren't pinned are grouped by purpose, guessed from their names and engines.
 * An alternate map view lays all brushes out by how their strokes look.
 */
class KRITAUI_EXPORT KisStudioBrushPicker : public KisStudioPicker
{
    Q_OBJECT
public:
    explicit KisStudioBrushPicker(KisMainWindow *mainWindow);
    ~KisStudioBrushPicker() override;

    /// Moves \p presetName to the front of the recently used brushes
    static void recordUse(const QString &presetName);
    /// The pinned brush \p step places from \p current (e.g. for the mouse wheel); empty past either end
    static QString neighbour(KisMainWindow *mainWindow, const QString &current, int step);
    /// Makes the preset named \p presetName the current brush
    static void use(KisViewManager *view, const QString &presetName);
    /// The last sample stroke rendered for this brush, in whatever color it was painted; may be null
    static QImage strokePicture(const QString &presetName);
    /// Strips the bundled presets' sorting prefix: "d) Ink-2 Fineliner" -> "Ink-2 Fineliner"
    static QString title(const QString &presetName);
    /// Whether the active tool paints with the current brush preset
    static bool paintToolActive();
    /// The user's pinned brushes, in their order
    static QStringList yourBrushes();
    static void setYourBrushes(const QStringList &presetNames);

private:
    /// The strokes are created first because the picker's Source needs them
    KisStudioBrushPicker(KisMainWindow *mainWindow, std::shared_ptr<KisStudioBrushStrokes> strokes);

    struct Private;
    const std::unique_ptr<Private> d;
};

#endif // KISSTUDIOBRUSHPICKER_H
