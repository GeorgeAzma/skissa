/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LAYERCOLORDROP_H
#define LAYERCOLORDROP_H

#include <QObject>
#include <QPersistentModelIndex>

#include <KoColor.h>
#include <kis_types.h>

class LayerBox;
class QEvent;
class QDropEvent;

/**
 * A color dragged onto a layer row. On the layer's picture it recolors the layer's paint (a group's, the layers in
 * it), shown on the canvas and in the picture while it is held there; on the rest of the row it gives the row the
 * nearest color label. Between rows the list keeps it: a fill layer.
 */
class LayerColorDrop : public QObject
{
public:
    explicit LayerColorDrop(LayerBox *box);
    ~LayerColorDrop() override;

    /// Takes a drag event of the list's viewport; false leaves it to the list
    bool filter(QEvent *event);

private:
    enum class Part { None, Paint, Row };

    /// What the drag at \p event is over, and the row
    Part partAt(QDropEvent *event, QModelIndex *row) const;
    void show(Part part, const QModelIndex &row, const KoColor &color);
    void clear();
    /// Hands the drag to the list from here on (\p event entering it there), or takes it back
    void giveToList(QDropEvent *event);
    void takeFromList(QDropEvent *event);

    struct Private;
    Private *const d;
};

#endif // LAYERCOLORDROP_H
