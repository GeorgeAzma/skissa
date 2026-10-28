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
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <KConfig>
#include <KConfigGroup>
#include <KSharedConfig>
#include <klocalizedstring.h>

#include <KritaVersionWrapper.h>

#include "KisSettingsRegistry.h"
#include <KisResourceLocator.h>
#include <KisResourceModel.h>
#include <KisResourceTypes.h>
#include <KisStorageModel.h>
#include <KisTag.h>
#include <KisTagModel.h>
#include <KisTagResourceModel.h>
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
    QStringLiteral("StudioEffects"), QStringLiteral("StudioBlendModeGroups"), QStringLiteral("StudioEffectGroups"), QStringLiteral("StudioBrushGroups"),
    QStringLiteral("StudioControlsGroups"), QStringLiteral("StudioPinnedBrushTips"), QStringLiteral("StudioRing"), QStringLiteral("StudioColorHues"), QStringLiteral("StudioColorShape"), QStringLiteral("StudioColorKeepLightness"),
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

namespace {

/// The kinds of resource whose tags are taken: those takeKritaResources() copies
const QStringList taggedTypes = {ResourceType::PaintOpPresets, ResourceType::Brushes,     ResourceType::Gradients,   ResourceType::Palettes,
                                 ResourceType::Patterns,       ResourceType::Workspaces,  ResourceType::Symbols,     ResourceType::GamutMasks,
                                 ResourceType::SeExprScripts,  ResourceType::LayerStyles};

/// Tags waiting for resources copied from Krita, which are read at the next start
QString pendingTagsPath()
{
    return KisProfile::configFile(QStringLiteral("krita-tags.json"));
}

/// A resource as Krita's database names it: found here by its md5, or else by its file in the same storage
QJsonObject resourceOf(const QSqlQuery &q)
{
    return {{QStringLiteral("type"), q.value(QStringLiteral("type")).toString()},
            {QStringLiteral("md5"), q.value(QStringLiteral("md5sum")).toString()},
            {QStringLiteral("file"), q.value(QStringLiteral("filename")).toString()},
            {QStringLiteral("storage"), q.value(QStringLiteral("location")).toString()}};
}

/**
 * What Krita's database holds beyond the files: its tags and what they tag, the resources it hides and whether
 * each bundle is on. Read from a copy, so Krita's own is never opened.
 */
QJsonObject readKritaTags(const QString &database)
{
    QTemporaryDir folder;
    const QString copy = folder.filePath(QStringLiteral("krita.sqlite"));
    if (!folder.isValid() || !QFile::copy(database, copy)) return {};
    QJsonObject result;
    const QString connection = QStringLiteral("skissa-krita-tags");
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        db.setDatabaseName(copy);
        db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
        if (db.open()) {
            const QString types = QStringLiteral("('") + taggedTypes.join(QStringLiteral("','")) + QStringLiteral("')");
            QSqlQuery q(db);
            QJsonArray tags;
            QMap<int, int> tagRows;
            q.exec(QStringLiteral("SELECT tags.id, tags.url, tags.name, resource_types.name AS type FROM tags "
                                  "JOIN resource_types ON resource_types.id = tags.resource_type_id "
                                  "WHERE tags.active = 1 AND resource_types.name IN ") + types);
            while (q.next()) {
                tagRows.insert(q.value(0).toInt(), tags.size());
                tags.append(QJsonObject{{QStringLiteral("type"), q.value(QStringLiteral("type")).toString()},
                                        {QStringLiteral("url"), q.value(QStringLiteral("url")).toString()},
                                        {QStringLiteral("name"), q.value(QStringLiteral("name")).toString()},
                                        {QStringLiteral("resources"), QJsonArray()}});
            }
            const QString resource = QStringLiteral("SELECT resource_types.name AS type, resources.md5sum, resources.filename, storages.location FROM resources "
                                                    "JOIN resource_types ON resource_types.id = resources.resource_type_id "
                                                    "JOIN storages ON storages.id = resources.storage_id ");
            q.exec(QStringLiteral("SELECT resource_tags.tag_id, resource_types.name AS type, resources.md5sum, resources.filename, storages.location "
                                  "FROM resource_tags JOIN resources ON resources.id = resource_tags.resource_id "
                                  "JOIN resource_types ON resource_types.id = resources.resource_type_id "
                                  "JOIN storages ON storages.id = resources.storage_id WHERE resource_tags.active = 1"));
            while (q.next()) {
                if (!tagRows.contains(q.value(0).toInt())) continue;
                QJsonObject tag = tags[tagRows.value(q.value(0).toInt())].toObject();
                QJsonArray tagged = tag.value(QStringLiteral("resources")).toArray();
                tagged.append(resourceOf(q));
                tag.insert(QStringLiteral("resources"), tagged);
                tags[tagRows.value(q.value(0).toInt())] = tag;
            }
            result.insert(QStringLiteral("tags"), tags);
            QJsonArray hidden;
            q.exec(resource + QStringLiteral("WHERE resources.status = 0 AND resources.temporary = 0 AND resource_types.name IN ") + types);
            while (q.next()) hidden.append(resourceOf(q));
            result.insert(QStringLiteral("hidden"), hidden);
            QJsonObject bundles;
            q.exec(QStringLiteral("SELECT location, active FROM storages JOIN storage_types ON storage_types.id = storages.storage_type_id "
                                  "WHERE storage_types.name = 'Bundle'"));
            while (q.next()) bundles.insert(q.value(0).toString(), q.value(1).toInt() != 0);
            result.insert(QStringLiteral("bundles"), bundles);
            db.close();
        }
    }
    QSqlDatabase::removeDatabase(connection);
    return result;
}

/// The id here of a resource from Krita's database, or -1 if it isn't here (yet)
int resourceIdHere(const QJsonObject &resource)
{
    QSqlQuery q;
    q.prepare(QStringLiteral("SELECT resources.id FROM resources "
                             "JOIN resource_types ON resource_types.id = resources.resource_type_id "
                             "JOIN storages ON storages.id = resources.storage_id "
                             "WHERE resource_types.name = :type "
                             "AND (resources.md5sum = :md5 OR (resources.filename = :file AND storages.location = :storage)) "
                             "ORDER BY storages.active DESC, resources.md5sum = :md5 DESC LIMIT 1"));
    q.bindValue(QStringLiteral(":type"), resource.value(QStringLiteral("type")).toString());
    q.bindValue(QStringLiteral(":md5"), resource.value(QStringLiteral("md5")).toString());
    q.bindValue(QStringLiteral(":file"), resource.value(QStringLiteral("file")).toString());
    q.bindValue(QStringLiteral(":storage"), resource.value(QStringLiteral("storage")).toString());
    return q.exec() && q.next() ? q.value(0).toInt() : -1;
}

/**
 * Tags, hides and turns bundles on or off here as \p krita (from readKritaTags()) has them. Returns what isn't here yet
 * and how many changes were made; tags and bundles are only added or matched, never deleted.
 */
QJsonObject applyKritaTags(const QJsonObject &krita, int *changes)
{
    QJsonArray tagsLeft;
    for (const QJsonValue &value : krita.value(QStringLiteral("tags")).toArray()) {
        const QJsonObject tagged = value.toObject();
        const QString type = tagged.value(QStringLiteral("type")).toString();
        KisTagModel tags(type);
        KisTagSP tag = tags.tagForUrl(tagged.value(QStringLiteral("url")).toString());
        if (!tag) {
            // made by its url, which is its name unless a bundle named it
            const QString url = tagged.value(QStringLiteral("url")).toString();
            const QString name = tagged.value(QStringLiteral("name")).toString();
            if (!tags.addTag(url, false, {})) continue;
            tag = tags.tagForUrl(url);
            if (!tag) continue;
            if (!name.isEmpty() && name != url) tags.renameTag(tag, name, false);
            (*changes)++;
        } else if (!tag->active()) {
            tags.setTagActive(tag);
            (*changes)++;
        }
        QSet<int> already;
        QSqlQuery q;
        q.prepare(QStringLiteral("SELECT resource_id FROM resource_tags WHERE tag_id = :tag AND active = 1"));
        q.bindValue(QStringLiteral(":tag"), tag->id());
        if (q.exec()) {
            while (q.next()) already.insert(q.value(0).toInt());
        }
        KisTagResourceModel tagResources(type);
        QVector<int> ids;
        QJsonArray left;
        for (const QJsonValue &resource : tagged.value(QStringLiteral("resources")).toArray()) {
            const int id = resourceIdHere(resource.toObject());
            if (id < 0) left.append(resource);
            else if (!already.contains(id) && !ids.contains(id)) ids << id;
        }
        if (!ids.isEmpty() && tagResources.tagResources(tag, ids)) *changes += ids.size();
        if (!left.isEmpty()) {
            QJsonObject rest = tagged;
            rest.insert(QStringLiteral("resources"), left);
            tagsLeft.append(rest);
        }
    }
    QJsonArray hiddenLeft;
    for (const QJsonValue &value : krita.value(QStringLiteral("hidden")).toArray()) {
        const QJsonObject resource = value.toObject();
        const int id = resourceIdHere(resource);
        if (id < 0) {
            hiddenLeft.append(resource);
            continue;
        }
        KisResourceModel resources(resource.value(QStringLiteral("type")).toString());
        const QModelIndex index = resources.indexForResourceId(id);
        if (index.isValid() && resources.setResourceInactive(index)) (*changes)++;
    }
    QJsonObject bundlesLeft = krita.value(QStringLiteral("bundles")).toObject();
    KisStorageModel *storages = KisStorageModel::instance();
    for (int row = 0; row < storages->rowCount(); row++) {
        const QModelIndex index = storages->index(row, 0);
        const QString location = index.data(Qt::UserRole + KisStorageModel::Location).toString();
        if (!bundlesLeft.contains(location)) continue;
        const bool on = bundlesLeft.take(location).toBool();
        if (index.data(Qt::UserRole + KisStorageModel::Active).toBool() != on && storages->setData(index, on, Qt::CheckStateRole)) (*changes)++;
    }
    return {{QStringLiteral("tags"), tagsLeft}, {QStringLiteral("hidden"), hiddenLeft}, {QStringLiteral("bundles"), bundlesLeft}};
}

/// Whether \p left (from applyKritaTags()) still waits for anything
bool waits(const QJsonObject &left)
{
    return !left.value(QStringLiteral("tags")).toArray().isEmpty() || !left.value(QStringLiteral("hidden")).toArray().isEmpty()
        || !left.value(QStringLiteral("bundles")).toObject().isEmpty();
}

} // namespace

int takeKritaTags()
{
    const QString from = kritaResourcesPath();
    const QString database = QDir(from).filePath(QStringLiteral("resourcecache.sqlite"));
    if (from.isEmpty() || !QFileInfo::exists(database)) return 0;
    int changes = 0;
    const QJsonObject left = applyKritaTags(readKritaTags(database), &changes);
    if (waits(left)) {
        // the resources just copied are read at the next start
        QString error;
        writeFile(pendingTagsPath(), QJsonDocument(left).toJson(QJsonDocument::Compact), &error);
        changes += left.value(QStringLiteral("tags")).toArray().size() + left.value(QStringLiteral("hidden")).toArray().size()
            + left.value(QStringLiteral("bundles")).toObject().size();
    } else {
        QFile::remove(pendingTagsPath());
    }
    return changes;
}

void takePendingKritaTags()
{
    QFile file(pendingTagsPath());
    if (!file.open(QIODevice::ReadOnly)) return;
    const QJsonObject pending = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    // what still isn't here was left out of Krita's files (caches, scripts) or failed to load: not waited for again
    file.remove();
    int changes = 0;
    applyKritaTags(pending, &changes);
}

} // namespace KisSettingsTransfer
