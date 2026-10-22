/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisProfile.h"

#include <QCoreApplication>
#include <QDir>
#include <QStandardPaths>

namespace KisProfile
{

QString name()
{
    return QStringLiteral("Skissa");
}

QString homepage()
{
    return QStringLiteral("https://github.com/GeorgeAzma/skissa");
}

bool isApart()
{
    return !qEnvironmentVariableIsSet("KRITA_DEV_PROFILE") || qEnvironmentVariableIsSet("KRITA_DEV_APART");
}

QString configFolder()
{
    QString folder = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    if (isApart()) {
        folder += QLatin1Char('/') + name();
        // made once: settings kept in files of their own ask for the folder at every read
        thread_local QString made;
        if (folder != made) {
            QDir().mkpath(folder);
            made = folder;
        }
    }
    return folder;
}

QString configFile(const QString &fileName)
{
    return configFolder() + QLatin1Char('/') + fileName;
}

QString tempFolder()
{
    const QString folder = QDir::tempPath() + QLatin1Char('/') + name()
        + (qEnvironmentVariableIsSet("KRITA_DEV_PROFILE") ? QStringLiteral(" dev") : QString());
    QDir().mkpath(folder);
    return folder;
}

void apply()
{
    // Qt puts app data in <AppData>/<organization>/<application>
    if (isApart()) {
        QCoreApplication::setOrganizationName(name());
    }
}

}
