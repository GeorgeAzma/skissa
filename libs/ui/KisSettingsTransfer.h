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
 * Settings moved between Kritas: to a file and back, and from this Krita
 * to Krita itself (another installation, its own settings) and back.
 *
 * Only the settings KisSettingsRegistry describes move, and only as they
 * are valid where they arrive; what is about one computer (paths, the
 * renderer, memory) stays. Nothing is applied before it is shown: a plan
 * lists each change, from what to what, and what was left out and why.
 *
 * Krita's settings are changed on top of what they are (only the settings
 * changed here, the rest as they were), never while that Krita runs, after
 * a copy of them is kept (once: the one from before the first time), and
 * written whole or not at all: to a new file first, checked, then put in
 * place of the old in one step.
 */
namespace KisSettingsTransfer
{

struct Change {
    QString id;
    QVariant from; ///< invalid: not set there
    QVariant to;
};

struct Plan {
    QVector<Change> changes;
    /// What is not moved and why, one line each
    QStringList skipped;
    QString error;
    /// Your sections and Quick, from a file (empty if it has none)
    QByteArray sections;
};

/// The settings that differ from Krita's defaults (this computer's left out), and your sections, as JSON
KRITAUI_EXPORT QByteArray exportSettings();
/// Writes \p data to \p path whole or not at all
KRITAUI_EXPORT bool writeFile(const QString &path, const QByteArray &data, QString *error);
/// What importing \p data (from exportSettings()) would change here
KRITAUI_EXPORT Plan planImport(const QByteArray &data);

/// Krita's own settings file (kritarc), the one Krita uses when it isn't this one; empty if there is none
KRITAUI_EXPORT QString kritaSettingsPath();
/// Whether Krita (another than this one) is running now
KRITAUI_EXPORT bool kritaRunning();
/// What taking Krita's settings would change here
KRITAUI_EXPORT Plan planFromKrita();
/// What giving Krita this one's settings would change there
KRITAUI_EXPORT Plan planToKrita();
/// Writes \p changes into Krita's settings; false (and nothing changed) on any trouble
KRITAUI_EXPORT bool applyToKrita(const QVector<Change> &changes, QString *error);
/// When the copy of Krita's settings from before was kept; invalid if there is none
KRITAUI_EXPORT QDateTime kritaBackupTime();
/// Puts back Krita's settings as they were before this one first changed them
KRITAUI_EXPORT bool restoreKrita(QString *error);

} // namespace KisSettingsTransfer

#endif // KISSETTINGSTRANSFER_H
