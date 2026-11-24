/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIOIMAGEPAGE_H
#define KISSTUDIOIMAGEPAGE_H

class KisMainWindow;
class QMdiArea;
class QWidget;

/**
 * What the image's name opens: the open and recent images as pictures, New and Open, this image's
 * saving and whole-image changes drawn from the picture itself, then All commands, Gestures and Help.
 */
namespace KisStudioImagePage
{
void open(KisMainWindow *window, QMdiArea *area, QWidget *anchor);
}

#endif
