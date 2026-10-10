/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIOGLASS_H
#define KISSTUDIOGLASS_H

#include <QColor>

#include "kritaui_export.h"

class QPainter;
class QPainterPath;
class QWidget;

/**
 * Frosted glass for what lies over the canvas (the tool rail, the zoom
 * pill, panels opened over it, pickers, cards, messages): the artwork
 * behind, blurred, under a tint, so that what is covered still shows its
 * colors and the canvas is not cut off by solid blocks.
 *
 * What is behind is the image itself (not a screenshot), drawn as the
 * canvas shows it (zoomed, rotated, mirrored) at a fraction of the
 * resolution, and blurred in linear light, as light mixes: a bright
 * stroke glows, it does not turn into grey mud. It follows the canvas as
 * it is painted on, panned or zoomed, a few times a second at most.
 *
 * Where there is no canvas behind (over a panel, or with no image open)
 * it is the plain panel color, as before.
 */
class KRITAUI_EXPORT KisStudioGlass
{
public:
    /// Paints \p widget's background in \p shape (widget coordinates): glass over the canvas, the panel color elsewhere
    static void paint(QPainter &painter, QWidget *widget, const QPainterPath &shape);

    /**
     * Makes \p widget's surfaces see-through (and its fields a darker
     * glass), for it to be shown on a glass background; or (\p on false) as
     * it was. Its menus and dropdowns, windows of their own, stay as they are.
     */
    static void setSeeThrough(QWidget *widget, bool on);

    /// The color a glass background is tinted with: what text is read against
    static QColor tint(const QWidget *widget);

    /// Whether glass is on (the setting); off, backgrounds are the plain panel color
    static bool isEnabled();
    static void setEnabled(bool on);
};

#endif // KISSTUDIOGLASS_H
