/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIOTHEME_H
#define KISSTUDIOTHEME_H

#include <QColor>
#include <QString>
#include <QVector>

#include "kritaui_export.h"

/**
 * A theme generated from a few parameters (lightness, tint and accent) instead
 * of picked from scheme files. The colors are written to a color scheme,
 * SkissaTheme.colors in the user's data folder, which the window then uses.
 */
namespace KisStudioTheme
{

struct Params {
    qreal lightness {0.252};   ///< window lightness, 0 black to 1 white; defaults to the Dark preset
    QColor tint {Qt::gray};    ///< hue mixed into the backgrounds
    qreal tintAmount {0};      ///< 0 no tint to 1 full
    QColor accent;             ///< selection color; invalid means a neutral gray

    bool operator==(const Params &other) const;
    bool operator!=(const Params &other) const { return !(*this == other); }
};

struct Preset {
    QString name;
    Params params;
};

KRITAUI_EXPORT QVector<Preset> presets();

/// The parameters stored in kritarc
KRITAUI_EXPORT Params current();
/// Stores \p params and switches the window to the resulting scheme
KRITAUI_EXPORT void apply(const Params &params);

/// The theme's name in the list of color schemes
KRITAUI_EXPORT QString schemeName();
/// Regenerates the scheme file if it is in use, once per run and before it is read,
/// so it follows any change in how the colors are derived.
KRITAUI_EXPORT void remake();

/// The main colors \p params produce, for previews
struct Look {
    QColor window, view, text, selection, button;
};
KRITAUI_EXPORT Look look(const Params &params);

} // namespace KisStudioTheme

#endif // KISSTUDIOTHEME_H
