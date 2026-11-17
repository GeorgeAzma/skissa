/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIOSYMBOLS_H
#define KISSTUDIOSYMBOLS_H

#include <QPoint>

class QMimeData;
class QWidget;
class KisView;

/**
 * Symbols (Krita's SVG symbol collections: balloons, effects, signs) picked from the shape tool's bar: a popup of their
 * pictures by collection, with the ones used lately first and a search. A click puts the symbol in the middle of the
 * view, a drag puts it where it is dropped; either way on the vector layer in use, or a new one.
 */
namespace KisStudioSymbols
{
/// Opens the picker under \p anchor
void openPicker(QWidget *anchor);
/// Whether a drag carries a symbol from the picker
bool carriesSymbol(const QMimeData *data);
/// Puts the dragged symbol on \p view's image where it was dropped (\p pos in the view's coordinates)
void drop(KisView *view, const QMimeData *data, const QPoint &pos);
}

#endif // KISSTUDIOSYMBOLS_H
