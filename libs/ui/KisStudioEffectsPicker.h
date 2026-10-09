/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIOEFFECTSPICKER_H
#define KISSTUDIOEFFECTSPICKER_H

#include "widgets/KisStudioPicker.h"

#include <memory>

class KisMainWindow;
struct KisStudioEffectThumbnails;

/**
 * All the effects (filters), to browse and to keep your own of at hand.
 *
 * Every effect is a tile with a small picture of your image with it applied
 * (a sample picture while the image is blank), made in the background as
 * the picker opens. Yours come first, in your order; above them the ones
 * used lately that are not yours yet; "All effects" shows the rest grouped
 * by what they do (soften, sharpen, light and tone, color...). Resting the
 * pointer on one shows it on the canvas; a click opens it to adjust, as
 * choosing it anywhere else does.
 */
class KRITAUI_EXPORT KisStudioEffectsPicker : public KisStudioPicker
{
    Q_OBJECT
public:
    explicit KisStudioEffectsPicker(KisMainWindow *mainWindow);
    ~KisStudioEffectsPicker() override;

    /// An effect was used, from here or anywhere else: it goes first among the recent ones
    static void recordUse(const QString &filterId);

private:
    /// The thumbnails exist before the picker: its Source paints them
    KisStudioEffectsPicker(KisMainWindow *mainWindow, std::shared_ptr<KisStudioEffectThumbnails> thumbnails);

    struct Private;
    const std::unique_ptr<Private> d;
};

#endif // KISSTUDIOEFFECTSPICKER_H
