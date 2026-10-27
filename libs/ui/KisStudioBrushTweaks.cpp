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
#include <QSet>
#include <QThreadPool>
#include <QTimer>
#include <QtConcurrentRun>

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
        // one thread, so the writes land in order
        m_writer.setMaxThreadCount(1);
        connect(&m_store, &QTimer::timeout, this, [this]() { store(); });
        connect(qApp, &QCoreApplication::aboutToQuit, this, [this]() {
            store();
            m_writer.waitForDone();
        });
    }

    static QString path() { return KisProfile::configFile(QStringLiteral("brush-tweaks.json")); }

    QJsonObject read() const
    {
        QFile file(path());
        if (!file.open(QIODevice::ReadOnly)) return QJsonObject();
        return QJsonDocument::fromJson(file.readAll()).object();
    }

    /// \p tweaks on disk, written on the writer's thread
    void write(const QJsonObject &tweaks)
    {
        const QByteArray bytes = tweaks.isEmpty() ? QByteArray() : QJsonDocument(tweaks).toJson(QJsonDocument::Compact);
        (void)QtConcurrent::run(&m_writer, [bytes]() {
            if (bytes.isEmpty()) {
                QFile::remove(path());
                return;
            }
            QSaveFile file(path());
            if (!file.open(QIODevice::WriteOnly)) return;
            file.write(bytes);
            file.commit();
        });
    }

    /// Notes \p preset's changes, to be written with the next store
    void listen(const KisPaintOpPresetSP &preset)
    {
        if (QPointer<KisPaintOpPresetUpdateProxy> proxy = preset->updateProxy()) {
            const KisPaintOpPreset *raw = preset.data();
            connect(proxy.data(), &KisPaintOpPresetUpdateProxy::sigSettingsChanged, this, [this, raw]() {
                m_changed.insert(raw);
                m_store.start();
            });
        }
    }

    /// The brush in use: its changes are kept as they happen
    void follow(const KisPaintOpPresetSP &preset)
    {
        if (!preset || preset->filename().isEmpty()) return;
        bool known = false;
        for (const KisPaintOpPresetSP &seen : qAsConst(m_seen)) known = known || seen == preset;
        if (!known) {
            m_seen << preset;
            listen(preset);
        }
        // a brush picked may have been saved or put back since
        m_store.start();
    }

    /// The brushes changed since the last store written, the others' entries kept, the saved ones' dropped
    void store()
    {
        m_store.stop();
        if (!KisConfig(true).useDirtyPresets()) return;
        QJsonObject tweaks = m_written;
        for (const KisPaintOpPresetSP &preset : qAsConst(m_seen)) {
            if (!preset) continue;
            const QString file = preset->filename();
            if (!preset->isDirty()) {
                tweaks.remove(file);
            } else if (m_changed.contains(preset.data()) || !tweaks.contains(file)) {
                QJsonObject tweak;
                tweak.insert(QStringLiteral("md5"), preset->md5Sum(false));
                tweak.insert(QStringLiteral("settings"), preset->settings()->toXML());
                tweaks.insert(file, tweak);
            }
        }
        m_changed.clear();
        if (tweaks != m_written) {
            write(tweaks);
            m_written = tweaks;
        }
    }

    /// The tweaks kept from before, back on their brushes
    void restore()
    {
        QJsonObject tweaks = read();
        m_written = tweaks;
        if (tweaks.isEmpty() || !KisConfig(true).useDirtyPresets()) return;
        KisPaintOpPresetResourceServer *server = KisResourceServerProvider::instance()->paintOpPresetServer();
        if (!server) return;
        QStringList stale;
        for (auto it = tweaks.constBegin(); it != tweaks.constEnd(); ++it) {
            const KisPaintOpPresetSP preset = server->resource(QString(), it.key(), QString());
            const QJsonObject tweak = it.value().toObject();
            // saved again or replaced since, or gone: the file wins, and its entry goes
            if (!preset || preset->isDirty() || preset->md5Sum(false) != tweak.value(QStringLiteral("md5")).toString()) {
                stale << it.key();
                continue;
            }
            if (!preset->settings()->fromXML(tweak.value(QStringLiteral("settings")).toString())) continue;
            preset->setDirty(true);
            m_seen << preset;
            listen(preset);
            if (QPointer<KisPaintOpPresetUpdateProxy> proxy = preset->updateProxy()) proxy->notifySettingsChanged();
        }
        // the entries of brushes deleted or changed in their file don't pile up
        if (!stale.isEmpty()) {
            for (const QString &file : qAsConst(stale)) tweaks.remove(file);
            write(tweaks);
            m_written = tweaks;
        }
    }

    QTimer m_store;
    QVector<KisPaintOpPresetSP> m_seen;
    QSet<const KisPaintOpPreset *> m_changed;
    /// What is on disk, or on its way there
    QJsonObject m_written;
    QThreadPool m_writer;
    bool m_restored {false};
};

} // namespace

void KisStudioBrushTweaks::watch(KisMainWindow *window)
{
    Keeper::instance()->watch(window);
}
