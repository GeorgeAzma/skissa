/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIOTIPS_H
#define KISSTUDIOTIPS_H

#include <QKeySequence>
#include <QString>
#include <QStringList>

#include <functional>

#include <klocalizedstring.h>

#include "kritaui_export.h"

class QAbstractButton;

/**
 * Tooltips teach the gestures. A tooltip is written as what the control is,
 * then a line per gesture ("Drag: change"); it is shown as the name with the
 * gestures in a row under it, each a little picture of the mouse doing it and
 * a word, and the shortcut after the name.
 */
namespace KisStudioTips
{

inline QString tip(const QString &what, const QStringList &gestures)
{
    QStringList lines;
    if (!what.isEmpty()) lines << what;
    lines << gestures;
    return lines.join(QLatin1Char('\n'));
}

inline QString dragChange() { return i18nc("@info:tooltip a gesture", "Drag: change"); }
inline QString doubleClickType() { return i18nc("@info:tooltip a gesture", "Double-click: type"); }
inline QString rightClickReset() { return i18nc("@info:tooltip a gesture", "Right-click: reset"); }
inline QString scrollStep() { return i18nc("@info:tooltip a gesture", "Scroll: step"); }

/// The gestures of a value dragged on its own label: drag, type, reset
inline QStringList valueGestures(bool resets = true)
{
    QStringList gestures = {dragChange(), doubleClickType()};
    if (resets) gestures << rightClickReset();
    return gestures;
}

/// \p tooltip as shown: its gesture lines as pictures, \p keys after its first line; plain text if it has neither
KRITAUI_EXPORT QString html(const QString &tooltip, const QString &keys = QString());

/// Shows every tooltip so, with the shortcut \p shortcutOf gives a button
KRITAUI_EXPORT void install(std::function<QKeySequence(QAbstractButton *)> shortcutOf);

} // namespace KisStudioTips

#endif // KISSTUDIOTIPS_H
