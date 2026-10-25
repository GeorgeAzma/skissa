/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisSettingsTransfer.h"

#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QDateTime>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
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
#include <KisResourceLocator.h>
#include <KisResourceTypes.h>
#include <KisProfile.h>

namespace KisSettingsTransfer
{

namespace {

const QString formatName = QStringLiteral("krita-settings");
/// The user's own sections and Quick (see KisStudioSettings), transferred along with the settings
const QString sectionsKey = QStringLiteral("StudioSettingsSections");

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

/// What the user arranged that isn't a setting, by the beginning of its key in kritarc's main group: panels and bars,
/// their buttons, pinned controls and items, the tool rail, rings, the color panel's style, the recorder's choices
const QStringList arrangedKeys = {
    QStringLiteral("StudioLayout"), QStringLiteral("StudioBarItems"), QStringLiteral("StudioViewPillItems"), QStringLiteral("StudioControlItems"),
    QStringLiteral("StudioControlsPicked"), QStringLiteral("StudioRailItems"), QStringLiteral("StudioHiddenVariants"), QStringLiteral("StudioBrushes"),
    QStringLiteral("StudioEffects"), QStringLiteral("StudioPinnedBrushTips"), QStringLiteral("StudioRing"), QStringLiteral("StudioColorStyle"),
    QStringLiteral("StudioColorCopyFormat"), QStringLiteral("StudioLastStrokeFollowsBrush"), QStringLiteral("BrushTipSizeLinked"),
    QStringLiteral("recorder/"), QStringLiteral("recorder_export/"),
};
/// Kept as they are here: the classic layout's window state and the recorder's folder belong to this computer
const QStringList notArranged = {QStringLiteral("StudioLayoutClassicState"), QStringLiteral("recorder/snapshotdirectory")};
/// Whole groups the user arranged: saved curves, each brush's smoothing, the brush sheet's columns, guides, the key scheme
const QStringList arrangedGroups = {QStringLiteral("SavedCurves"), QStringLiteral("StudioBrushSmoothing"), QStringLiteral("StudioBrushSheet"),
                                    QStringLiteral("StudioGuides"), QStringLiteral("Shortcut Schemes")};
const QString mainGroup = QStringLiteral("main");

bool isArranged(const QString &key)
{
    if (notArranged.contains(key)) return false;
    for (const QString &start : arrangedKeys) {
        if (key.startsWith(start)) return true;
    }
    return false;
}

QJsonObject entries(const KConfigGroup &group, bool all)
{
    QJsonObject object;
    const QMap<QString, QString> map = group.entryMap();
    for (auto it = map.constBegin(); it != map.constEnd(); ++it) {
        if (all || isArranged(it.key())) object.insert(it.key(), it.value());
    }
    return object;
}

KSharedConfigPtr shortcutsFile()
{
    return KSharedConfig::openConfig(KisProfile::configFile(QStringLiteral("kritashortcutsrc")));
}

/// The arrangement here: {"kritarc": {group: {key: value}}, "shortcuts": {action: keys}}, the main group as "main"
QJsonObject arrangement()
{
    KSharedConfigPtr config = KSharedConfig::openConfig();
    QJsonObject kritarc;
    const QJsonObject main = entries(config->group(QString()), false);
    if (!main.isEmpty()) kritarc.insert(mainGroup, main);
    for (const QString &name : arrangedGroups) {
        const QJsonObject group = entries(config->group(name), true);
        if (!group.isEmpty()) kritarc.insert(name, group);
    }
    QJsonObject root;
    if (!kritarc.isEmpty()) root.insert(QStringLiteral("kritarc"), kritarc);
    const QJsonObject shortcuts = entries(shortcutsFile()->group(QStringLiteral("Shortcuts")), true);
    if (!shortcuts.isEmpty()) root.insert(QStringLiteral("shortcuts"), shortcuts);
    return root;
}

QString samePath(const QString &path)
{
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath()).toLower();
}

/// Lists the changes that taking the settings in \p source would make here
Plan plan(KConfig *source)
{
    Plan plan;
    KisSettingsRegistry *registry = KisSettingsRegistry::instance();
    Q_FOREACH (const Setting &s, registry->settings()) {
        if ((s.flags & KisSettingsRegistry::Machine) || !s.storedInKritarc()) continue;
        const QVariant to = registry->valueIn(s, *source);
        if (!to.isValid()) continue; // not set in the source
        const QVariant from = registry->value(s.id);
        const QVariant shownFrom = from.isValid() ? from : registry->defaultValue(s.id);
        if (registry->sameValue(s, shownFrom, to)) continue;
        plan.changes << Change{s.id, from, to};
    }
    return plan;
}

bool fail(QString *error, const QString &message)
{
    if (error) *error = message;
    return false;
}

QString notFound()
{
    return i18nc("@info", "Krita's settings weren't found.");
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
    const QJsonObject arranged = arrangement();
    if (!arranged.isEmpty()) root.insert(QStringLiteral("arrangement"), arranged);
    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

bool writeFile(const QString &path, const QByteArray &data, QString *error)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) return fail(error, file.errorString());
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
    // Settings missing from the file were at their default there; they are left as they are here
    if (root.value(QStringLiteral("sections")).isObject()) {
        plan.sections = QJsonDocument(root.value(QStringLiteral("sections")).toObject()).toJson(QJsonDocument::Compact);
    }
    const QJsonObject arranged = root.value(QStringLiteral("arrangement")).toObject();
    if (!arranged.isEmpty() && arranged != arrangement()) plan.arrangement = QJsonDocument(arranged).toJson(QJsonDocument::Compact);
    return plan;
}

bool applyArrangement(const QByteArray &data, QString *error)
{
    const QJsonObject arranged = QJsonDocument::fromJson(data).object();
    if (arranged.isEmpty()) return fail(error, i18nc("@info", "That arrangement couldn't be read."));
    KisSettingsRegistry::instance()->flush();
    // what is replaced kept beside, never lost
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    for (const QString &name : {QStringLiteral("kritarc"), QStringLiteral("kritashortcutsrc")}) {
        const QString path = KisProfile::configFile(name);
        if (QFile::exists(path) && !QFile::copy(path, path + QStringLiteral(".before-import-") + stamp)) {
            return fail(error, i18nc("@info", "Your current arrangement couldn't be kept aside, so nothing was changed."));
        }
    }

    KSharedConfigPtr config = KSharedConfig::openConfig();
    const QJsonObject kritarc = arranged.value(QStringLiteral("kritarc")).toObject();
    // the file's arrangement as a whole: what it doesn't have goes back to its default
    KConfigGroup main = config->group(QString());
    const QStringList keys = main.keyList();
    for (const QString &key : keys) {
        if (isArranged(key)) main.deleteEntry(key);
    }
    const QJsonObject mainEntries = kritarc.value(mainGroup).toObject();
    for (auto it = mainEntries.constBegin(); it != mainEntries.constEnd(); ++it) {
        if (isArranged(it.key())) main.writeEntry(it.key(), it.value().toString());
    }
    for (const QString &name : arrangedGroups) {
        config->deleteGroup(name);
        KConfigGroup group = config->group(name);
        const QJsonObject groupEntries = kritarc.value(name).toObject();
        for (auto it = groupEntries.constBegin(); it != groupEntries.constEnd(); ++it) group.writeEntry(it.key(), it.value().toString());
    }
    if (!config->sync()) return fail(error, i18nc("@info", "The arrangement couldn't be saved."));

    KSharedConfigPtr shortcuts = shortcutsFile();
    shortcuts->deleteGroup(QStringLiteral("Shortcuts"));
    KConfigGroup keyGroup = shortcuts->group(QStringLiteral("Shortcuts"));
    const QJsonObject keyEntries = arranged.value(QStringLiteral("shortcuts")).toObject();
    for (auto it = keyEntries.constBegin(); it != keyEntries.constEnd(); ++it) keyGroup.writeEntry(it.key(), it.value().toString());
    if (!shortcuts->sync()) return fail(error, i18nc("@info", "The shortcuts couldn't be saved."));
    return true;
}

QString kritaSettingsPath()
{
    QString path = qEnvironmentVariable("KRITA_DEV_KRITA_SETTINGS");
    if (path.isEmpty() && KisProfile::isApart()) {
        // a regular Krita's kritarc is loose in the config folder, where the fork has its own folder
        path = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/kritarc");
    } else if (path.isEmpty()) {
        // The dev build runs in Qt's test mode with its own profile (see KRITA_DEV_PROFILE);
        // the regular kritarc is where the config folder would be outside test mode.
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
    const QString mine = KisProfile::configFile(QStringLiteral("kritarc"));
    if (samePath(path) == samePath(mine)) return QString();
    return QFileInfo::exists(path) ? path : QString();
}

Plan planFromKrita()
{
    const QString path = kritaSettingsPath();
    if (path.isEmpty()) {
        Plan p;
        p.error = notFound();
        return p;
    }
    KConfig krita(path, KConfig::SimpleConfig);
    return plan(&krita);
}

QString kritaResourcesPath()
{
    QString path = qEnvironmentVariable("KRITA_DEV_KRITA_RESOURCES");
    if (path.isEmpty()) {
        // where Krita's settings say, or where it keeps them by default
        const QString settings = kritaSettingsPath();
        if (!settings.isEmpty()) {
            KConfig krita(settings, KConfig::SimpleConfig);
            path = krita.group(QString()).readEntry(KisResourceLocator::resourceLocationKey, QString());
        }
    }
    if (path.isEmpty()) {
#if defined(Q_OS_WIN)
        path = qEnvironmentVariable("APPDATA") + QStringLiteral("/krita");
#elif defined(Q_OS_MACOS)
        path = QDir::homePath() + QStringLiteral("/Library/Application Support/krita");
#else
        path = qEnvironmentVariable("XDG_DATA_HOME", QDir::homePath() + QStringLiteral("/.local/share")) + QStringLiteral("/krita");
#endif
    }
    if (samePath(path) == samePath(KisResourceLocator::instance()->resourceLocationBase())) return QString();
    return QFileInfo(path).isDir() ? path : QString();
}

int takeKritaResources(QString *error)
{
    const QString from = kritaResourcesPath();
    if (from.isEmpty()) {
        if (error) *error = i18nc("@info", "Krita's brushes weren't found.");
        return -1;
    }
    const QDir to(KisResourceLocator::instance()->resourceLocationBase());
    // the resources, not Krita's caches, scripts, input profiles or shortcuts
    const QStringList folders = {ResourceType::PaintOpPresets, ResourceType::Brushes,      ResourceType::Gradients,  ResourceType::Palettes,
                                 ResourceType::Patterns,       ResourceType::Workspaces,   ResourceType::Symbols,    ResourceType::WindowLayouts,
                                 ResourceType::Sessions,       ResourceType::GamutMasks,   ResourceType::SeExprScripts,
                                 ResourceType::LayerStyles,    QStringLiteral("bundles")};
    int copied = 0;
    Q_FOREACH (const QString &folder, folders) {
        const QDir source(QDir(from).filePath(folder));
        if (!source.exists()) continue;
        QDirIterator it(source.path(), QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString file = it.next();
            const QString target = to.filePath(folder + QLatin1Char('/') + source.relativeFilePath(file));
            if (QFileInfo::exists(target)) continue;
            QDir().mkpath(QFileInfo(target).absolutePath());
            if (QFile::copy(file, target)) copied++;
        }
    }
    return copied;
}

} // namespace KisSettingsTransfer
