/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSETTINGSTRANSFER_H
#define KISSETTINGSTRANSFER_H

#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVector>

#include "kritaui_export.h"

/**
 * Moves settings to and from a file, and to and from a regular Krita installation.
 * Only validated KisSettingsRegistry settings are transferred, never machine-specific
 * ones. A Plan lists every change before it is applied; Krita's kritarc is backed up
 * once and always replaced atomically.
 */
namespace KisSettingsTransfer
{

struct Change {
    QString id;
    QVariant from; ///< invalid if not set in the target
    QVariant to;
};

struct Plan {
    QVector<Change> changes;
    /// What is not transferred and why, one line each
    QStringList skipped;
    QString error;
    /// The user's sections and Quick from an imported file, empty if it has none
    QByteArray sections;
    /// What the user arranged from an imported file (panels, buttons, pinned items, curves, shortcuts), empty if it has
    /// none or it is the same here
    QByteArray arrangement;
};

/// The non-default, non-machine settings and the user's sections, as JSON
KRITAUI_EXPORT QByteArray exportSettings();
/// Writes \p data to \p path atomically
KRITAUI_EXPORT bool writeFile(const QString &path, const QByteArray &data, QString *error);
/// The changes importing \p data (from exportSettings()) would make here
KRITAUI_EXPORT Plan planImport(const QByteArray &data);
/// Replaces what the user arranged here with Plan::arrangement, after keeping the files it changes beside them; it takes
/// effect at the next start
KRITAUI_EXPORT bool applyArrangement(const QByteArray &data, QString *error);

/// The regular Krita installation's kritarc, or empty if there is none or it is this instance's own
KRITAUI_EXPORT QString kritaSettingsPath();
/// The changes taking Krita's settings would make here
KRITAUI_EXPORT Plan planFromKrita();

/// The regular Krita's resource folder (its brushes, bundles…), or empty if there is none or it is this one's own
KRITAUI_EXPORT QString kritaResourcesPath();
/// Copies the resources Krita has that aren't here yet into this one's resource folder: brushes, presets,
/// patterns, gradients, palettes, bundles and the like; Krita's are only read. How many files, or -1 on failure
KRITAUI_EXPORT int takeKritaResources(QString *error);

} // namespace KisSettingsTransfer

#endif // KISSETTINGSTRANSFER_H
