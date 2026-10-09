/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISBLENDMODEPICKER_H
#define KISBLENDMODEPICKER_H

#include <KoID.h>

#include "KisStudioPicker.h"
#include "kritaui_export.h"

/**
 * Picks a blend mode from your own list of them (see KisStudioPicker).
 *
 * The list starts with about thirty common modes, ordered by what they do
 * (darken, lighten, contrast, compare, color); scrolling over a blend mode
 * box steps through it in that order, then on into all the others. Every
 * mode shows a swatch made with its real composite op.
 */
class KRITAUI_EXPORT KisBlendModePicker : public KisStudioPicker
{
    Q_OBJECT
public:
    /**
     * @param available the ids that can be used in the current color space
     */
    KisBlendModePicker(const QString &current, const QSet<QString> &available, QWidget *parent = nullptr);

    /// Your modes, in your order
    static QStringList pinnedModes();
    static void setPinnedModes(const QStringList &ids);
    /// What scrolling steps through: your modes, then all the others as the picker shows them
    static QStringList scrollOrder();

Q_SIGNALS:
    /// The highlighted mode changed; an empty id when the preview should end
    void previewed(const QString &id);
    void picked(const KoID &op);

private:
    static Source source();
};

#endif // KISBLENDMODEPICKER_H
