/*
 *  SPDX-FileCopyrightText: 2022 Dmitry Kazakov <dimula73@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisSmudgeLengthOptionData.h"

#include <kis_paintop_settings.h>

bool KisSmudgeLengthOptionMixInImpl::read(const KisPropertiesConfiguration *setting)
{
    mode = (Mode)setting->getInt("SmudgeRateMode", SMEARING_MODE);
    smearAlpha = setting->getBool("SmudgeRateSmearAlpha", true);
    // the smudge engine of Krita 5 always; presets of before it paint with it too
    useNewEngine = true;

    return true;
}

void KisSmudgeLengthOptionMixInImpl::write(KisPropertiesConfiguration *setting) const
{
    setting->setProperty("SmudgeRateMode", mode);
    setting->setProperty("SmudgeRateSmearAlpha", smearAlpha);
    setting->setProperty("SmudgeRateUseNewEngine", useNewEngine);
}
