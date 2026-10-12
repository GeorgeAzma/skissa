/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisSettingsTransfer.h"

#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

#include <KConfig>
#include <KConfigGroup>
#include <KSharedConfig>
#include <klocalizedstring.h>

#include <KritaVersionWrapper.h>

#include "KisSettingsRegistry.h"

#ifdef Q_OS_WIN
#include <windows.h>
#include <tlhelp32.h>
#include <wchar.h>
#endif

namespace KisSettingsTransfer
{

namespace {

const QString formatName = QStringLiteral("krita-settings");
/// Your sections and Quick (see KisStudioSettings), moved with the settings
const QString sectionsKey = QStringLiteral("StudioSettingsSections");
const QString backupSuffix = QStringLiteral(".before-krita-dev");

using Setting = KisSettingsRegistry::Setting;
using Kind = KisSettingsRegistry::Kind;

QJsonValue toJson(const Setting &setting, const QVariant &value)
{
    switch (setting.kind) {
    case Kind::Toggle: return value.toBool();
    case Kind::Number: return value.toDouble();
    case Kind::Choice: return setting.fallback.userType() == QMetaType::Int ? QJsonValue(value.toInt()) : QJsonValue(value.toString());
    case Kind::Color: {
        const QColor c = value.value<QColor>();
        return QStringLiteral("%1,%2,%3,%4").arg(c.red()).arg(c.green()).arg(c.blue()).arg(c.alpha());
    }
    case Kind::Text: return value.toString();
    }
    return QJsonValue();
}

QString nameOf(const Setting &setting)
{
    return setting.label.isEmpty() ? setting.id : setting.label;
}

QString samePath(const QString &path)
{
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath()).toLower();
}

QByteArray readAll(const QString &path, bool *ok)
{
    QFile file(path);
    *ok = file.open(QIODevice::ReadOnly);
    return *ok ? file.readAll() : QByteArray();
}

/// Plans moving the settings from \p source (or this Krita's, if null) to \p target (or this Krita's)
Plan plan(KConfig *source, KConfig *target, bool toKrita)
{
    Plan plan;
    KisSettingsRegistry *registry = KisSettingsRegistry::instance();
    Q_FOREACH (const Setting &s, registry->settings()) {
        if (s.flags & KisSettingsRegistry::Machine) continue;
        if (toKrita && ((s.flags & KisSettingsRegistry::Fork) || !s.storedInKritarc())) continue;
        if (!toKrita && source && !s.storedInKritarc()) continue;
        QVariant to;
        if (source) {
            to = registry->valueIn(s, *source);
            if (!to.isValid()) continue; // not set there: nothing to take
        } else {
            // only what was changed here: the rest stays as Krita has it
            if (registry->isDefault(s.id)) continue;
            to = registry->value(s.id);
        }
        QVariant from = target ? registry->valueIn(s, *target) : registry->value(s.id);
        const QVariant shownFrom = from.isValid() ? from : s.fallback;
        if (registry->sameValue(s, shownFrom, to)) continue;
        plan.changes << Change{s.id, from, to};
    }
    return plan;
}

} // namespace

QByteArray exportSettings()
{
    KisSettingsRegistry *registry = KisSettingsRegistry::instance();
    QJsonObject settings;
    Q_FOREACH (const Setting &s, registry->settings()) {
        if ((s.flags & KisSettingsRegistry::Machine) || registry->isDefault(s.id)) continue;
        settings.insert(s.id, toJson(s, registry->value(s.id)));
    }
    QJsonObject root;
    root.insert(QStringLiteral("format"), formatName);
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("krita"), KritaVersionWrapper::versionString());
    root.insert(QStringLiteral("saved"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    root.insert(QStringLiteral("settings"), settings);
    const QString sections = KSharedConfig::openConfig()->group(QString()).readEntry(sectionsKey, QString());
    const QJsonDocument sectionsDocument = QJsonDocument::fromJson(sections.toUtf8());
    if (sectionsDocument.isObject()) root.insert(QStringLiteral("sections"), sectionsDocument.object());
    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

bool writeFile(const QString &path, const QByteArray &data, QString *error)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
        if (error) *error = file.errorString();
        return false;
    }
    return true;
}

Plan planImport(const QByteArray &data)
{
    Plan plan;
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(data, &parseError);
    const QJsonObject root = document.object();
    if (parseError.error != QJsonParseError::NoError || root.value(QStringLiteral("format")).toString() != formatName) {
        plan.error = i18nc("@info", "This isn't a Krita settings file.");
        return plan;
    }
    if (root.value(QStringLiteral("version")).toInt() > 1) {
        plan.error = i18nc("@info", "This file is from a newer Krita.");
        return plan;
    }
    KisSettingsRegistry *registry = KisSettingsRegistry::instance();
    const QJsonObject settings = root.value(QStringLiteral("settings")).toObject();
    for (auto it = settings.constBegin(); it != settings.constEnd(); ++it) {
        const Setting *s = registry->setting(it.key());
        if (!s) {
            plan.skipped << i18nc("@info settings import", "%1: not in this Krita", it.key());
            continue;
        }
        if (s->flags & KisSettingsRegistry::Machine) {
            plan.skipped << i18nc("@info settings import", "%1: kept as this computer has it", nameOf(*s));
            continue;
        }
        const QVariant to = registry->normalized(*s, it.value().toVariant());
        if (!to.isValid()) {
            plan.skipped << i18nc("@info settings import", "%1: not a value it can have here", nameOf(*s));
            continue;
        }
        const QVariant from = registry->value(s->id);
        if (!registry->sameValue(*s, from, to)) plan.changes << Change{s->id, from, to};
    }
    // what is at its default there isn't in the file: changed here, it is kept
    if (root.value(QStringLiteral("sections")).isObject()) {
        plan.sections = QJsonDocument(root.value(QStringLiteral("sections")).toObject()).toJson(QJsonDocument::Compact);
    }
    return plan;
}

QString kritaSettingsPath()
{
    QString path = qEnvironmentVariable("KRITA_DEV_KRITA_SETTINGS");
    if (path.isEmpty()) {
        // this one runs with a profile of its own (Qt's test mode, see KRITA_DEV_PROFILE): Krita's is where it would be without
        if (!QStandardPaths::isTestModeEnabled()) return QString();
        QString own = QDir::cleanPath(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation));
        if (own.endsWith(QLatin1String("/qttest"))) {
            // Windows: <AppData>/qttest
            own.chop(int(qstrlen("/qttest")));
        } else if (own.endsWith(QLatin1String("/.qttest/config"))) {
            // Linux: ~/.qttest/config, for ~/.config (or XDG_CONFIG_HOME)
            own = QDir::cleanPath(qEnvironmentVariable("XDG_CONFIG_HOME", QDir::homePath() + QStringLiteral("/.config")));
        } else if (own.contains(QLatin1String("/.qttest/"))) {
            // macOS: ~/.qttest/Library/Preferences
            own.replace(QLatin1String("/.qttest/"), QLatin1String("/"));
        } else {
            return QString();
        }
        path = own + QStringLiteral("/kritarc");
    }
    const QString mine = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/kritarc");
    if (samePath(path) == samePath(mine)) return QString();
    return QFileInfo::exists(path) ? path : QString();
}

bool kritaRunning()
{
#ifdef Q_OS_WIN
    const QString self = samePath(QCoreApplication::applicationFilePath());
    const DWORD ownId = GetCurrentProcessId();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return true; // can't tell: as if it were
    bool running = false;
    PROCESSENTRY32W entry;
    entry.dwSize = sizeof(entry);
    for (BOOL more = Process32FirstW(snapshot, &entry); more && !running; more = Process32NextW(snapshot, &entry)) {
        if (entry.th32ProcessID == ownId || _wcsicmp(entry.szExeFile, L"krita.exe") != 0) continue;
        // another krita.exe: Krita, unless it is this one's build running again
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
        if (!process) {
            running = true;
            break;
        }
        wchar_t buffer[MAX_PATH * 2];
        DWORD size = DWORD(sizeof(buffer) / sizeof(buffer[0]));
        if (!QueryFullProcessImageNameW(process, 0, buffer, &size) || samePath(QString::fromWCharArray(buffer, int(size))) != self) {
            running = true;
        }
        CloseHandle(process);
    }
    CloseHandle(snapshot);
    return running;
#elif defined(Q_OS_LINUX)
    const QString self = QFileInfo(QCoreApplication::applicationFilePath()).canonicalFilePath();
    const QString ownId = QString::number(QCoreApplication::applicationPid());
    Q_FOREACH (const QString &pid, QDir(QStringLiteral("/proc")).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (pid == ownId) continue;
        const QString exe = QFileInfo(QStringLiteral("/proc/%1/exe").arg(pid)).canonicalFilePath();
        if (QFileInfo(exe).fileName().startsWith(QLatin1String("krita")) && exe != self) return true;
    }
    return false;
#else
    return false;
#endif
}

Plan planFromKrita()
{
    const QString path = kritaSettingsPath();
    if (path.isEmpty()) {
        Plan plan;
        plan.error = i18nc("@info", "Krita's settings weren't found.");
        return plan;
    }
    KConfig krita(path, KConfig::SimpleConfig);
    return plan(&krita, nullptr, false);
}

Plan planToKrita()
{
    const QString path = kritaSettingsPath();
    if (path.isEmpty()) {
        Plan plan;
        plan.error = i18nc("@info", "Krita's settings weren't found.");
        return plan;
    }
    KConfig krita(path, KConfig::SimpleConfig);
    Plan p = plan(nullptr, &krita, true);
    int forkOnly = 0;
    int machine = 0;
    Q_FOREACH (const Setting &s, KisSettingsRegistry::instance()->settings()) {
        if (KisSettingsRegistry::instance()->isDefault(s.id)) continue;
        if (s.flags & KisSettingsRegistry::Machine) machine++;
        else if ((s.flags & KisSettingsRegistry::Fork) || !s.storedInKritarc()) forkOnly++;
    }
    if (forkOnly) p.skipped << i18ncp("@info", "%1 setting Krita doesn't have", "%1 settings Krita doesn't have", forkOnly);
    if (machine) p.skipped << i18ncp("@info", "%1 setting about this computer", "%1 settings about this computer", machine);
    return p;
}

bool applyToKrita(const QVector<Change> &changes, QString *error)
{
    auto fail = [error](const QString &message) {
        if (error) *error = message;
        return false;
    };
    const QString path = kritaSettingsPath();
    if (path.isEmpty()) return fail(i18nc("@info", "Krita's settings weren't found."));
    if (kritaRunning()) return fail(i18nc("@info", "Close Krita first: it would write its own settings over these."));
    bool ok = false;
    const QByteArray original = readAll(path, &ok);
    if (!ok) return fail(i18nc("@info", "Krita's settings couldn't be read."));

    // the copy to go back to: the one from before the first time, never one already changed
    const QString backup = path + backupSuffix;
    if (!QFileInfo::exists(backup)) {
        QString why;
        if (!writeFile(backup, original, &why)) return fail(i18nc("@info", "A copy of Krita's settings couldn't be kept: %1", why));
        const QByteArray kept = readAll(backup, &ok);
        if (!ok || kept != original) return fail(i18nc("@info", "A copy of Krita's settings couldn't be kept."));
    }

    // changed on a copy first: if anything goes wrong, Krita's file is as it was
    const QString draft = path + QStringLiteral(".krita-dev-draft");
    QFile::remove(draft);
    QString why;
    if (!writeFile(draft, original, &why)) return fail(why);
    KisSettingsRegistry *registry = KisSettingsRegistry::instance();
    QStringList groupsBefore;
    int keysBefore = 0;
    {
        KConfig before(draft, KConfig::SimpleConfig);
        groupsBefore = before.groupList();
        keysBefore = before.group(QString()).keyList().size();
    }
    {
        KConfig config(draft, KConfig::SimpleConfig);
        Q_FOREACH (const Change &change, changes) {
            if (const Setting *s = registry->setting(change.id)) registry->writeIn(*s, config, change.to);
        }
        if (!config.sync()) {
            QFile::remove(draft);
            return fail(i18nc("@info", "Krita's settings couldn't be written."));
        }
    }
    // read back: every change there, and nothing else lost
    {
        KConfig check(draft, KConfig::SimpleConfig);
        bool good = check.group(QString()).keyList().size() >= keysBefore;
        Q_FOREACH (const QString &group, groupsBefore) {
            good = good && check.hasGroup(group);
        }
        Q_FOREACH (const Change &change, changes) {
            const Setting *s = registry->setting(change.id);
            good = good && s && registry->sameValue(*s, registry->valueIn(*s, check), change.to);
        }
        if (!good) {
            QFile::remove(draft);
            return fail(i18nc("@info", "Krita's settings came out wrong, so they weren't changed."));
        }
    }
    const QByteArray changed = readAll(draft, &ok);
    QFile::remove(draft);
    QFile::remove(draft + QStringLiteral(".lock"));
    if (!ok) return fail(i18nc("@info", "Krita's settings couldn't be written."));
    // in one step: Krita finds either its old file or the new one, never half of one
    if (!writeFile(path, changed, &why)) return fail(why);
    return true;
}

QDateTime kritaBackupTime()
{
    const QString path = kritaSettingsPath();
    if (path.isEmpty()) return QDateTime();
    const QFileInfo backup(path + backupSuffix);
    return backup.exists() ? backup.lastModified() : QDateTime();
}

bool restoreKrita(QString *error)
{
    auto fail = [error](const QString &message) {
        if (error) *error = message;
        return false;
    };
    const QString path = kritaSettingsPath();
    if (path.isEmpty()) return fail(i18nc("@info", "Krita's settings weren't found."));
    if (kritaRunning()) return fail(i18nc("@info", "Close Krita first: it would write its own settings over these."));
    const QString backup = path + backupSuffix;
    bool ok = false;
    const QByteArray kept = readAll(backup, &ok);
    if (!ok || kept.isEmpty()) return fail(i18nc("@info", "There is no copy of Krita's settings to go back to."));
    // what they were until now, should they be wanted after all
    const QByteArray current = readAll(path, &ok);
    QString why;
    if (ok && !writeFile(path + QStringLiteral(".krita-dev-replaced"), current, &why)) return fail(why);
    if (!writeFile(path, kept, &why)) return fail(why);
    QFile::remove(backup);
    return true;
}

} // namespace KisSettingsTransfer
