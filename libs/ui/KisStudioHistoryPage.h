/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIOHISTORYPAGE_H
#define KISSTUDIOHISTORYPAGE_H

#include <functional>

class QDockWidget;

/**
 * Skissa's History panel over Krita's: a line to drag through the steps above the list, snapshots as marks on both
 * (Krita's snapshot panel driven through its own buttons), and recipes, the steps a command made kept to run again.
 */
namespace KisStudioHistoryPage
{
/// Puts the page over Krita's History panel; what is returned takes it away again
std::function<void()> apply(QDockWidget *history);
}

#endif // KISSTUDIOHISTORYPAGE_H
