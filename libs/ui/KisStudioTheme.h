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
 * The theme, made of a few choices instead of picked from files: how light the
 * window is, a tint it takes, the accent of what is chosen and how strongly
 * text stands out. Presets are such choices made already. The colors are
 * written as a color scheme (KritaStudioTheme.colors, among the user's data)
 * and the window takes it.
 */
namespace KisStudioTheme
{

struct Params {
    qreal lightness {0.13};    ///< of the window, 0 black to 1 white
    QColor tint {Qt::gray};    ///< the hue the window takes
    qreal tintAmount {0};      ///< how much of it, 0 none to 1
    QColor accent;             ///< what is chosen; invalid: a gray as the window's

    bool operator==(const Params &other) const;
    bool operator!=(const Params &other) const { return !(*this == other); }
};

struct Preset {
    QString name;
    Params params;
};

KRITAUI_EXPORT QVector<Preset> presets();

/// What is set now (from kritarc)
KRITAUI_EXPORT Params current();
/// Keeps \p params and makes the window take them
KRITAUI_EXPORT void apply(const Params &params);

/// The theme's name among the color schemes
KRITAUI_EXPORT QString schemeName();
/// Makes the scheme again from the settings, if it is the one in use: once a run, before it is read (how its
/// colors are made may have changed since it was written)
KRITAUI_EXPORT void remake();

/// The colors \p params give, for a picture of them: window, field, text, accent, button
struct Look {
    QColor window, view, text, selection, button;
};
KRITAUI_EXPORT Look look(const Params &params);

} // namespace KisStudioTheme

#endif // KISSTUDIOTHEME_H
