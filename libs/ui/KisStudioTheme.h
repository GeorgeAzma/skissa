/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIOTHEME_H
#define KISSTUDIOTHEME_H

#include <QColor>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QVariant>
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

/**
 * A theme in the picker: its colors and the rest of the look (glass, the glow around the image, the font),
 * as the values of the settings it holds. The shipped themes are kept as the user's are, and can be edited,
 * renamed and deleted; they keep their shipped values to be reset to.
 */
struct Theme {
    QString id;   ///< a shipped theme's own ("dark"), or "own<n>"
    QString name; ///< empty for a shipped theme that keeps its own (translated) name
    QHash<QString, QVariant> values;
};

/// The settings a theme holds, in the order they are applied
KRITAUI_EXPORT QStringList held();
KRITAUI_EXPORT QVector<Theme> themes();
KRITAUI_EXPORT QString nameOf(const Theme &theme);
KRITAUI_EXPORT Params paramsOf(const Theme &theme);
/// The theme being used and edited; empty if none
KRITAUI_EXPORT QString currentTheme();

// Each of these is one step of the settings' undo history
KRITAUI_EXPORT void pick(const QString &id);
/// A copy of the current theme, picked
KRITAUI_EXPORT void add();
KRITAUI_EXPORT void rename(const QString &id, const QString &name);
/// Deletes \p id unless it is the last; the one beside it is picked if it was in use
KRITAUI_EXPORT void remove(const QString &id);
/// Gives a shipped theme its shipped name and values again
KRITAUI_EXPORT void reset(const QString &id);
/// Whether \p id is a shipped theme that differs from how it ships
KRITAUI_EXPORT bool isChanged(const QString &id);
/// Whether any shipped theme was deleted
KRITAUI_EXPORT bool anyDeleted();
KRITAUI_EXPORT void restoreDeleted();

/// Keeps \p value of the held setting \p id in the current theme; called as the setting is written
KRITAUI_EXPORT void remember(const QString &id, const QVariant &value);

/// The parameters stored in kritarc
KRITAUI_EXPORT Params current();
/// Stores \p params and switches the window to the resulting scheme
KRITAUI_EXPORT void apply(const Params &params);

/// The theme's name in the list of color schemes
KRITAUI_EXPORT QString schemeName();
/// Regenerates the scheme file if it is in use, once per run and before it is read,
/// so it follows any change in how the colors are derived. The first time, it also
/// finds (or keeps as a theme of its own) the look in use.
KRITAUI_EXPORT void remake();

/// The main colors \p params produce, for previews
struct Look {
    QColor window, view, text, selection, button;
};
KRITAUI_EXPORT Look look(const Params &params);

} // namespace KisStudioTheme

#endif // KISSTUDIOTHEME_H
