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
 * Picks an effect (filter) from tiles that show each one applied to the image,
 * rendered in the background as the picker opens. Hovering a tile previews the
 * effect on the canvas; a click opens it for adjusting.
 */
class KRITAUI_EXPORT KisStudioEffectsPicker : public KisStudioPicker
{
    Q_OBJECT
public:
    explicit KisStudioEffectsPicker(KisMainWindow *mainWindow);
    ~KisStudioEffectsPicker() override;

    /// Moves \p filterId to the front of the recent effects; call whenever an effect is used.
    static void recordUse(const QString &filterId);

private:
    /// The thumbnails must exist before the base class is built, since its Source paints them
    KisStudioEffectsPicker(KisMainWindow *mainWindow, std::shared_ptr<KisStudioEffectThumbnails> thumbnails);

    struct Private;
    const std::unique_ptr<Private> d;
};

#endif // KISSTUDIOEFFECTSPICKER_H
