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
 * All the effects (filters), each a tile showing it applied to your image,
 * made in the background as the picker opens. Resting the pointer on one
 * previews it on the canvas; a click opens it to adjust.
 */
class KRITAUI_EXPORT KisStudioEffectsPicker : public KisStudioPicker
{
    Q_OBJECT
public:
    explicit KisStudioEffectsPicker(KisMainWindow *mainWindow);
    ~KisStudioEffectsPicker() override;

    /// An effect was used, from here or elsewhere: first among the recent ones
    static void recordUse(const QString &filterId);

private:
    /// The thumbnails exist before the picker: its Source paints them
    KisStudioEffectsPicker(KisMainWindow *mainWindow, std::shared_ptr<KisStudioEffectThumbnails> thumbnails);

    struct Private;
    const std::unique_ptr<Private> d;
};

#endif // KISSTUDIOEFFECTSPICKER_H
