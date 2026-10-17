/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: LGPL-2.0-or-later
 */
#ifndef KISRESOURCETAGMENU_H
#define KISRESOURCETAGMENU_H

#include <QMenu>

#include <KoResource.h>

class KisTagChooserWidget;

/**
 * A resource's menu: its picture and name, then every tag, the ones it is in first. A click on a tag puts the
 * resource in it or takes it out (✕ and +), and the menu stays open for the next; a new tag is typed at the end.
 */
class KisResourceTagMenu : public QMenu
{
    Q_OBJECT
public:
    KisResourceTagMenu(KoResourceSP resource, KisTagChooserWidget *tagChooser);
};

#endif // KISRESOURCETAGMENU_H
