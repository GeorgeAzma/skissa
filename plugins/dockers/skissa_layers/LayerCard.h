/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LAYERCARD_H
#define LAYERCARD_H

#include <functional>

#include <kis_types.h>

#include "widgets/KisStudioPopup.h"

class LayerBox;
class QModelIndex;

/**
 * A layer's right click: its picture and name (typed into in place), its blend mode as a picture, its opacity,
 * its switches with their words, the everyday commands as tiles always in the same places, and the rest as
 * a few quieter buttons that open their menus. What it changes shows in it at once.
 */
class LayerCard : public KisStudioPopup
{
public:
    LayerCard(LayerBox *box, KisNodeSP node);
    ~LayerCard() override;

    /// Shows the card beside the layer panel, level with \p row (global coordinates), toward the window's middle
    void popupBesideRow(const QRect &row, QWidget *panel);

private:
    void refresh();

    struct Private;
    Private *const d;
};

#endif // LAYERCARD_H
