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
 * A layer's right click: only what can't be done on its row by hand. The everyday commands as icons always in
 * the same places, its color label, then a few rows for the rarer ones, opening their menus beside the card.
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
