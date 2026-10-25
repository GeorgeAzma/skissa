/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioBrushTweaks.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QSaveFile>
#include <QTimer>

#include <KisPaintOpPresetUpdateProxy.h>
#include <KisProfile.h>
#include <kis_paintop_preset.h>
#include <kis_paintop_settings.h>

#include "KisMainWindow.h"
#include "KisResourceServerProvider.h"
#include "KisViewManager.h"
#include "kis_canvas_resource_provider.h"
#include "kis_config.h"

namespace
{

class Keeper : public QObject
{
public:
    static Keeper *instance()
    {
        static Keeper *keeper = new Keeper();
        return keeper;
    }

    void watch(KisMainWindow *window)
    {
        KisViewManager *view = window ? window->viewManager() : nullptr;
        KisCanvasResourceProvider *resources = view ? view->canvasResourceProvider() : nullptr;
        if (!resources) return;
        if (!m_restored) {
            m_restored = true;
            restore();
            // the brush in use was picked before: its size and opacity on the canvas as picking it sets them
            const KisPaintOpPresetSP current = resources->currentPreset();
            if (current && m_seen.contains(current)) {
                resources->setSize(current->settings()->paintOpSize());
                resources->setOpacity(current->settings()->paintOpOpacity());
                resources->setFlow(current->settings()->paintOpFlow());
            }
        }
        connect(resources, &KisCanvasResourceProvider::sigPaintOpPresetChanged, this, [this](const KisPaintOpPresetSP &preset) { follow(preset); });
        follow(resources->currentPreset());
    }

private:
    Keeper()
        : QObject(qApp)
    {
        m_store.setSingleShot(true);
        m_store.setInterval(500);
        connect(&m_store, &QTimer::timeout, this, [this]() { store(); });
        connect(qApp, &QCoreApplication::aboutToQuit, this, [this]() { store(); });
    }

    static QString path() { return KisProfile::configFile(QStringLiteral("brush-tweaks.json")); }

    QJsonObject read() const
    {
        QFile file(path());
        if (!file.open(QIODevice::ReadOnly)) return QJsonObject();
        return QJsonDocument::fromJson(file.readAll()).object();
    }

    void write(const QJsonObject &tweaks)
    {
        if (tweaks.isEmpty()) {
            QFile::remove(path());
            return;
        }
        QSaveFile file(path());
        if (!file.open(QIODevice::WriteOnly)) return;
        file.write(QJsonDocument(tweaks).toJson(QJsonDocument::Compact));
        file.commit();
    }

    /// The brush in use: its changes are kept as they happen
    void follow(const KisPaintOpPresetSP &preset)
    {
        if (!preset || preset->filename().isEmpty()) return;
        bool known = false;
        for (const KisPaintOpPresetSP &seen : qAsConst(m_seen)) known = known || seen == preset;
        if (!known) {
            m_seen << preset;
            if (QPointer<KisPaintOpPresetUpdateProxy> proxy = preset->updateProxy()) {
                connect(proxy.data(), &KisPaintOpPresetUpdateProxy::sigSettingsChanged, this, [this]() { m_store.start(); });
            }
        }
        // a brush picked may have been saved or put back since
        m_store.start();
    }

    /// The tweaked brushes seen in this session written, the others' entries kept, the saved ones' dropped
    void store()
    {
        m_store.stop();
        if (!KisConfig(true).useDirtyPresets()) return;
        QJsonObject tweaks = read();
        for (const KisPaintOpPresetSP &preset : qAsConst(m_seen)) {
            if (!preset) continue;
            const QString file = preset->filename();
            if (preset->isDirty()) {
                QJsonObject tweak;
                tweak.insert(QStringLiteral("md5"), preset->md5Sum(false));
                tweak.insert(QStringLiteral("settings"), preset->settings()->toXML());
                tweaks.insert(file, tweak);
            } else {
                tweaks.remove(file);
            }
        }
        if (tweaks != m_written) {
            write(tweaks);
            m_written = tweaks;
        }
    }

    /// The tweaks kept from before, back on their brushes
    void restore()
    {
        const QJsonObject tweaks = read();
        m_written = tweaks;
        if (tweaks.isEmpty() || !KisConfig(true).useDirtyPresets()) return;
        KisPaintOpPresetResourceServer *server = KisResourceServerProvider::instance()->paintOpPresetServer();
        if (!server) return;
        for (auto it = tweaks.constBegin(); it != tweaks.constEnd(); ++it) {
            const KisPaintOpPresetSP preset = server->resource(QString(), it.key(), QString());
            const QJsonObject tweak = it.value().toObject();
            // saved again or replaced since: the file wins, its entry goes at the next store
            if (!preset || preset->isDirty() || preset->md5Sum(false) != tweak.value(QStringLiteral("md5")).toString()) continue;
            if (!preset->settings()->fromXML(tweak.value(QStringLiteral("settings")).toString())) continue;
            preset->setDirty(true);
            m_seen << preset;
            if (QPointer<KisPaintOpPresetUpdateProxy> proxy = preset->updateProxy()) {
                connect(proxy.data(), &KisPaintOpPresetUpdateProxy::sigSettingsChanged, this, [this]() { m_store.start(); });
                proxy->notifySettingsChanged();
            }
        }
    }

    QTimer m_store;
    QVector<KisPaintOpPresetSP> m_seen;
    QJsonObject m_written;
    bool m_restored {false};
};

} // namespace

void KisStudioBrushTweaks::watch(KisMainWindow *window)
{
    Keeper::instance()->watch(window);
}
