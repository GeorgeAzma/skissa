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
 * All the brushes (presets), each a tile with a stroke it painted in the
 * current color, made in the background and kept for the next time. The
 * others are grouped by what they are for, worked out from their names and
 * engines; a map shows them all by how their strokes look.
 */
class KRITAUI_EXPORT KisStudioBrushPicker : public KisStudioPicker
{
    Q_OBJECT
public:
    explicit KisStudioBrushPicker(KisMainWindow *mainWindow);
    ~KisStudioBrushPicker() override;

    /// A brush was used, from here or elsewhere: first among the recent ones
    static void recordUse(const QString &presetName);
    /// The next or previous one of yours after the current brush, e.g. for the wheel; empty at either end
    static QString neighbour(KisMainWindow *mainWindow, const QString &current, int step);
    /// Makes the brush with that name the current one
    static void use(KisViewManager *view, const QString &presetName);
    /// A stroke the brush painted for the picker, if any yet (in whatever color it was then)
    static QImage strokePicture(const QString &presetName);
    /// "d) Ink-2 Fineliner" -> "Ink-2 Fineliner": without the bundled presets' sorting prefix
    static QString title(const QString &presetName);
    /// Whether the active tool paints with the brush
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
