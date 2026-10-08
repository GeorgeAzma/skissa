/*
 *  SPDX-FileCopyrightText: 2023 Dmitry Kazakov <dimula73@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisMenuStyleDontCloseOnAlt.h"

#include "KisModernStyle.h"

KisMenuStyleDontCloseOnAlt::KisMenuStyleDontCloseOnAlt(QStyle *baseStyle)
    : QProxyStyle(KisModernStyle::create(baseStyle->objectName()))
{
}

int KisMenuStyleDontCloseOnAlt::styleHint(StyleHint hint, const QStyleOption *option, const QWidget *widget, QStyleHintReturn *returnData) const
{
    if (hint == QStyle::SH_MenuBar_AltKeyNavigation) {
        return false;
    }

    return QProxyStyle::styleHint(hint, option, widget, returnData);
}
