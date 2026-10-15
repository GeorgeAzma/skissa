/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIOTIPS_H
#define KISSTUDIOTIPS_H

#include <QString>
#include <QStringList>

#include <klocalizedstring.h>

/**
 * Tooltips teach the gestures: what a control is on the first line, then each
 * gesture on a line of its own, worded the same everywhere ("Drag: change").
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

} // namespace KisStudioTips

#endif // KISSTUDIOTIPS_H
