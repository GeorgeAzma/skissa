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
};

/// The non-default, non-machine settings and the user's sections, as JSON
KRITAUI_EXPORT QByteArray exportSettings();
/// Writes \p data to \p path atomically
KRITAUI_EXPORT bool writeFile(const QString &path, const QByteArray &data, QString *error);
/// The changes importing \p data (from exportSettings()) would make here
KRITAUI_EXPORT Plan planImport(const QByteArray &data);

/// The regular Krita installation's kritarc, or empty if there is none or it is this instance's own
KRITAUI_EXPORT QString kritaSettingsPath();
/// Whether another Krita process is running
KRITAUI_EXPORT bool kritaRunning();
/// The changes taking Krita's settings would make here
KRITAUI_EXPORT Plan planFromKrita();
/// The changes giving Krita these settings would make there
KRITAUI_EXPORT Plan planToKrita();
/// Writes \p changes into Krita's kritarc; on failure returns false and leaves it untouched
KRITAUI_EXPORT bool applyToKrita(const QVector<Change> &changes, QString *error);
/// When the backup of Krita's original kritarc was made; invalid if there is none
KRITAUI_EXPORT QDateTime kritaBackupTime();
/// Restores Krita's kritarc from the backup made before it was first changed
KRITAUI_EXPORT bool restoreKrita(QString *error);

} // namespace KisSettingsTransfer

#endif // KISSETTINGSTRANSFER_H
