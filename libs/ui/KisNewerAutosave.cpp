/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "KisNewerAutosave.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <KisProfile.h>

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

bool putAway(const QString &autosave)
{
    if (!QFile::exists(autosave)) return true;
    if (QFile::moveToTrash(autosave)) return true;
    // a folder of its own, so the autosaves there aren't offered again at start
    const QDir folder(QDir(KisProfile::tempFolder()).filePath(QStringLiteral("declined")));
    if (!QDir().mkpath(folder.path())) return false;
    const QString name = QFileInfo(autosave).fileName();
    QString target = folder.filePath(name);
    for (int i = 2; QFile::exists(target); i++) target = folder.filePath(QStringLiteral("%1 %2").arg(QString::number(i), name));
    // another drive's file is copied, then removed
    return QFile::rename(autosave, target);
}

} // namespace KisNewerAutosave
