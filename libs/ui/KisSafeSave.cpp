/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisSafeSave.h"

#include <QDir>
#include <QFileInfo>
#include <QTemporaryFile>
#include <QThread>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#ifdef Q_OS_WIN
namespace
{

/// \p path in the form Windows takes for paths longer than MAX_PATH
std::wstring longPath(const QString &path)
{
    QString native = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
    if (!native.startsWith(QLatin1String("\\\\?\\"))) {
        native = native.startsWith(QLatin1String("\\\\")) ? QStringLiteral("\\\\?\\UNC\\") + native.mid(2) : QStringLiteral("\\\\?\\") + native;
    }
    return native.toStdWString();
}

bool moveFile(const QString &from, const QString &to)
{
    return MoveFileExW(longPath(from).c_str(), longPath(to).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

/// \p replacement takes \p replaced's place as the same file to Windows: its permissions, streams, creation
/// time and a cloud folder's version history stay
bool replaceFile(const QString &replaced, const QString &replacement)
{
    return ReplaceFileW(longPath(replaced).c_str(), longPath(replacement).c_str(), nullptr,
                        REPLACEFILE_IGNORE_MERGE_ERRORS | REPLACEFILE_IGNORE_ACL_ERRORS, nullptr, nullptr);
}

/// On the disk, not only in the cache; shared, since a virus scanner or a cloud folder may be reading it
bool flushFile(const QString &path)
{
    HANDLE handle = CreateFileW(longPath(path).c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    const bool flushed = handle != INVALID_HANDLE_VALUE && FlushFileBuffers(handle);
    if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    return flushed;
}

} // namespace
#endif

QFileDevice::FileError KisSafeSave::replace(QTemporaryFile &written, const QString &location)
{
#ifdef Q_OS_WIN
    const QString sibling = location + QStringLiteral(".saving");
    QFile::remove(sibling);
    bool swapped = false;
    if (written.copy(sibling)) {
        // before it takes the old file's place; swapped in even if that failed, which is no worse than copying over
        for (int attempt = 0; attempt < 3 && !flushFile(sibling); attempt++) QThread::msleep(50);
        const bool hadOld = QFile::exists(location);
        // a cloud folder may hold the file a moment, so try again
        for (int attempt = 0; attempt < 5 && !swapped; attempt++) {
            if (attempt) QThread::msleep(100);
            swapped = (hadOld && replaceFile(location, sibling)) || moveFile(sibling, location);
        }
        if (!swapped) QFile::remove(sibling);
    }
    if (swapped) return QFileDevice::NoError;

    // the old file set aside, not deleted, until the new one is in its place
    const QString aside = location + QStringLiteral(".old");
    QFile::remove(aside);
    const bool hadOld = QFile::exists(location);
    if (hadOld && !moveFile(location, aside)) {
        written.setAutoRemove(false);
        return QFileDevice::RenameError;
    }
    if (!written.copy(location)) {
        if (hadOld) moveFile(aside, location);
        written.setAutoRemove(false);
        return written.error() != QFileDevice::NoError ? written.error() : QFileDevice::CopyError;
    }
    // the old file goes only once the new one is surely on the disk
    if (hadOld && flushFile(location)) QFile::remove(aside);
    return QFileDevice::NoError;
#else
    QFile::remove(location);
    if (!written.copy(location)) {
        written.setAutoRemove(false);
        return written.error();
    }
    return QFileDevice::NoError;
#endif
}

QString KisSafeSave::besides(const QString &location)
{
    const QFileInfo info(location);
    // the extension kept, as some formats are told by it
    return info.dir().filePath(info.completeBaseName() + QStringLiteral(".saving.") + info.suffix());
}

QFileDevice::FileError KisSafeSave::replaceWith(const QString &written, const QString &location)
{
#ifdef Q_OS_WIN
    for (int attempt = 0; attempt < 3 && !flushFile(written); attempt++) QThread::msleep(50);
    const bool hadOld = QFile::exists(location);
    // a cloud folder may hold the file a moment, so try again
    for (int attempt = 0; attempt < 5; attempt++) {
        if (attempt) QThread::msleep(100);
        if ((hadOld && replaceFile(location, written)) || moveFile(written, location)) return QFileDevice::NoError;
    }
    // the new file stays beside the old one, which is untouched
    return QFileDevice::RenameError;
#else
    if (QFile::exists(location) && !QFile::remove(location)) return QFileDevice::RemoveError;
    return QFile::rename(written, location) ? QFileDevice::NoError : QFileDevice::RenameError;
#endif
}
