/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "KisNewerAutosave.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>

namespace KisNewerAutosave
{

QString of(const QString &path)
{
    const QFileInfo image(path);
    if (path.isEmpty() || !image.exists()) return QString();
    QString newest;
    QDateTime newestTime = image.lastModified();
    // as KisDocument::generateAutoSaveFileName names it, hidden or not
    for (const QString &prefix : {QStringLiteral("."), QString()}) {
        const QFileInfo autosave(image.dir().filePath(prefix + image.fileName() + QStringLiteral("-autosave.kra")));
        if (autosave.exists() && autosave.lastModified() > newestTime) {
            newest = autosave.filePath();
            newestTime = autosave.lastModified();
        }
    }
    return newest;
}

} // namespace KisNewerAutosave
