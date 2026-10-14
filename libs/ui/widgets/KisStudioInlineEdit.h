/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIOINLINEEDIT_H
#define KISSTUDIOINLINEEDIT_H

#include <QFont>
#include <QRect>
#include <QString>

#include <functional>

#include "kritaui_export.h"

class QLineEdit;
class QWidget;

/**
 * Typing a value where it is written: the text becomes editable in place, in its own font and spot,
 * with no box around it. Return or a click elsewhere keeps it, Escape leaves it as it was.
 */
namespace KisStudioInlineEdit
{
/// Edits \p text, drawn by \p owner in \p textRect with \p alignment and \p font; \p done gets the typed text
KRITAUI_EXPORT QLineEdit *open(QWidget *owner, const QRect &textRect, const QString &text, Qt::Alignment alignment,
                               const QFont &font, std::function<void(const QString &typed)> done);
}

#endif // KISSTUDIOINLINEEDIT_H
