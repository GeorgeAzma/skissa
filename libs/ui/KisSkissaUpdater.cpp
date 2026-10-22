/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisSkissaUpdater.h"

#include <QAction>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QCursor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QProcess>
#include <QSharedPointer>
#include <QTimer>
#include <QToolTip>
#include <QVersionNumber>

#include <kactioncollection.h>
#include <klocalizedstring.h>

#include "KisMainWindow.h"
#include "KisPart.h"
#include "KisProfile.h"
#include "KisSettingsRegistry.h"
#include "KisStudioItemCard.h"
#include "KisViewManager.h"
#include "kis_icon_utils.h"

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace
{

constexpr int firstLook = 30 * 1000;
constexpr int lookEvery = 4 * 60 * 60 * 1000;
/// Written by the release script: "Skissa <version>", then every file of the release
const QString fileList = QStringLiteral("skissa-files.txt");

QString installFolder()
{
    return QFileInfo(QCoreApplication::applicationDirPath() + QStringLiteral("/..")).canonicalFilePath();
}

QString readyFolder()
{
    return installFolder() + QStringLiteral(".update");
}

QString unpackFolder()
{
    return installFolder() + QStringLiteral(".unpacking");
}

/// The version a release's folder holds, empty when it isn't one
QString versionIn(const QString &folder)
{
    QFile list(folder + QLatin1Char('/') + fileList);
    if (!list.open(QIODevice::ReadOnly)) return QString();
    const QString first = QString::fromUtf8(list.readLine()).trimmed();
    return first.startsWith(QLatin1String("Skissa ")) ? first.mid(7) : QString();
}

/// Whether every file a release lists is in its folder
bool isComplete(const QString &folder)
{
    QFile list(folder + QLatin1Char('/') + fileList);
    if (!list.open(QIODevice::ReadOnly)) return false;
    list.readLine();
    while (!list.atEnd()) {
        const QString file = QString::fromUtf8(list.readLine()).trimmed();
        if (!file.isEmpty() && !QFileInfo::exists(folder + QLatin1Char('/') + file)) return false;
    }
    return true;
}

bool isNewer(const QString &version, const QString &than)
{
    return QVersionNumber::compare(QVersionNumber::fromString(version), QVersionNumber::fromString(than)) > 0;
}

/// Waits for the app to end, then swaps the folders; run by PowerShell from the profile folder
const char applyScript[] = R"ps(param([int]$Id, [string]$Install, [switch]$Relaunch)
$update = "$Install.update"
$old = "$Install.old"
$log = Join-Path $PSScriptRoot 'update.log'
function Note($text) { Add-Content -LiteralPath $log -Value ('{0:yyyy-MM-dd HH:mm:ss} {1}' -f (Get-Date), $text) }
function Move-Folder($from, $to) {
    for ($i = 0; $i -lt 60; $i++) {
        try { [IO.Directory]::Move($from, $to); return $true } catch { Start-Sleep -Milliseconds 500 }
    }
    $false
}
# files in a release's folder that the release didn't bring: the user's
function Strays($folder) {
    $listed = @{}
    Get-Content -LiteralPath (Join-Path $folder 'skissa-files.txt') | ForEach-Object { $listed[$_.ToLower()] = $true }
    Get-ChildItem -LiteralPath $folder -Recurse -File -Force | Where-Object {
        $relative = $_.FullName.Substring($folder.Length + 1).Replace('\', '/').ToLower()
        $relative -ne 'skissa-files.txt' -and -not $listed[$relative]
    }
}

Wait-Process -Id $Id -Timeout 60 -ErrorAction SilentlyContinue
if (-not (Test-Path -LiteralPath (Join-Path $update 'skissa-files.txt'))) { exit }

# the version before last goes, unless something of the user's is in it
if (Test-Path -LiteralPath $old) {
    $kept = (-not (Test-Path -LiteralPath (Join-Path $old 'skissa-files.txt'))) -or [bool](Strays $old)
    if (-not $kept) { Remove-Item -LiteralPath $old -Recurse -Force -ErrorAction SilentlyContinue }
    if (Test-Path -LiteralPath $old) { Move-Folder $old ($old + '-' + (Get-Date -Format 'yyyyMMdd-HHmmss')) | Out-Null }
}
if (-not (Move-Folder $Install $old)) { Note "$Install is in use"; exit }
if (-not (Move-Folder $update $Install)) {
    Move-Folder $old $Install | Out-Null
    Note "$update couldn't take its place"
    exit
}
# what the user kept in the program's folder comes along
if (Test-Path -LiteralPath (Join-Path $old 'skissa-files.txt')) {
    foreach ($file in Strays $old) {
        $target = Join-Path $Install $file.FullName.Substring($old.Length + 1)
        if (-not (Test-Path -LiteralPath $target)) {
            New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
            Move-Item -LiteralPath $file.FullName -Destination $target
        }
    }
}
Note "$Install updated"
if ($Relaunch) { Start-Process -FilePath (Join-Path $Install 'bin\krita.exe') }
)ps";

class Updater : public QObject
{
public:
    enum State { Idle, Looking, Downloading, Unpacking, Ready };

    static Updater *instance()
    {
        static Updater *updater = new Updater();
        return updater;
    }

    State state {Idle};
    QString found; ///< the version being fetched or ready
    bool relaunch {false};
    QVector<QPointer<QAction>> readyActions;

    void start()
    {
        if (!KisSkissaUpdater::canUpdate()) return;
        QTimer::singleShot(firstLook, this, [this]() { look(false); });
        auto *timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, [this]() { look(false); });
        timer->start(lookEvery);
    }

    /// Asks the homepage for its latest release; \p asked tells the user how it went
    void look(bool asked)
    {
        if (state == Looking || state == Downloading || state == Unpacking) {
            if (asked) tell(i18nc("@info, %1 a version", "Getting Skissa %1…", found));
            return;
        }
        if (!asked && !KisSettingsRegistry::instance()->value(QStringLiteral("StudioUpdates")).toBool()) return;

        QString feed = qEnvironmentVariable("KRITA_DEV_UPDATES");
        if (feed.isEmpty()) {
            feed = KisProfile::homepage().replace(QLatin1String("https://github.com/"), QLatin1String("https://api.github.com/repos/"))
                + QStringLiteral("/releases/latest");
        }
        QNetworkRequest request{QUrl(feed)};
        request.setRawHeader("Accept", "application/vnd.github+json");
        request.setRawHeader("User-Agent", "Skissa/" + KisSkissaUpdater::version().toUtf8());
        const State before = state;
        state = Looking;
        QNetworkReply *reply = network.get(request);
        connect(reply, &QNetworkReply::finished, this, [this, reply, asked, before]() {
            reply->deleteLater();
            state = before;
            if (reply->error() != QNetworkReply::NoError) {
                if (asked) tell(i18nc("@info", "Couldn't reach GitHub"));
                return;
            }
            const QJsonObject release = QJsonDocument::fromJson(reply->readAll()).object();
            QString latest = release.value(QStringLiteral("tag_name")).toString();
            if (latest.startsWith(QLatin1Char('v'))) latest.remove(0, 1);
            if (state == Ready && !isNewer(latest, found)) {
                if (asked) tell(readyText());
                return;
            }
            if (!isNewer(latest, KisSkissaUpdater::version())) {
                if (asked) tell(i18nc("@info, %1 a version", "Skissa %1 is the newest", KisSkissaUpdater::version()));
                return;
            }
            Q_FOREACH (const QJsonValue &value, release.value(QStringLiteral("assets")).toArray()) {
                const QJsonObject asset = value.toObject();
                const QString name = asset.value(QStringLiteral("name")).toString();
                if (!name.endsWith(QLatin1String("-windows-x64.zip"))) continue;
                fetch(latest, name, QUrl(asset.value(QStringLiteral("browser_download_url")).toString()),
                      asset.value(QStringLiteral("size")).toVariant().toLongLong(), asset.value(QStringLiteral("digest")).toString(), asked);
                return;
            }
            if (asked) tell(i18nc("@info, %1 a version", "Skissa %1 has no download for Windows", latest));
        });
    }

private:
    QNetworkAccessManager network;
    QFile download;

    Updater()
        : QObject(qApp)
    {
        connect(qApp, &QCoreApplication::aboutToQuit, this, &Updater::quitting);
        if (!KisSkissaUpdater::canUpdate()) return;
        // an update unpacked in an earlier run, unless this version has caught up with it
        const QString ready = versionIn(readyFolder());
        if (!ready.isEmpty() && isNewer(ready, KisSkissaUpdater::version())) {
            state = Ready;
            found = ready;
        } else if (QFileInfo::exists(readyFolder())) {
            QDir(readyFolder()).removeRecursively();
        }
        if (QFileInfo::exists(unpackFolder())) QDir(unpackFolder()).removeRecursively();
    }

    QString readyText() const
    {
        return i18nc("@info, %1 a version", "Skissa %1 starts the next time you open it", found);
    }

    void tell(const QString &text)
    {
        KisMainWindow *window = KisPart::instance()->currentMainwindow();
        if (window && window->activeView()) {
            window->viewManager()->showFloatingMessage(text, QIcon());
        } else {
            QToolTip::showText(QCursor::pos(), text);
        }
    }

    /// Back to how things were: an update ready from before stays ready
    void failed()
    {
        found = versionIn(readyFolder());
        state = found.isEmpty() ? Idle : Ready;
    }

    void fetch(const QString &version, const QString &name, const QUrl &url, qint64 size, const QString &digest, bool asked)
    {
        const QString folder = KisProfile::configFolder() + QStringLiteral("/updates");
        QDir().mkpath(folder);
        download.setFileName(folder + QLatin1Char('/') + name);
        if (!download.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            if (asked) tell(i18nc("@info", "The update didn't download; Skissa tries again later"));
            return;
        }
        state = Downloading;
        found = version;
        if (asked) tell(i18nc("@info, %1 a version", "Getting Skissa %1…", found));

        QNetworkRequest request(url);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        request.setRawHeader("User-Agent", "Skissa/" + KisSkissaUpdater::version().toUtf8());
        QNetworkReply *reply = network.get(request);
        auto hash = QSharedPointer<QCryptographicHash>::create(QCryptographicHash::Sha256);
        connect(reply, &QNetworkReply::readyRead, this, [this, reply, hash]() {
            const QByteArray data = reply->readAll();
            hash->addData(data);
            download.write(data);
        });
        connect(reply, &QNetworkReply::finished, this, [this, reply, hash, size, digest]() {
            reply->deleteLater();
            const QByteArray data = reply->readAll();
            hash->addData(data);
            download.write(data);
            const bool written = download.error() == QFileDevice::NoError && download.size() == size;
            download.close();
            const bool same = !digest.startsWith(QLatin1String("sha256:")) || digest.mid(7).toLatin1() == hash->result().toHex();
            if (reply->error() != QNetworkReply::NoError || !written || !same) {
                QFile::remove(download.fileName());
                failed();
                return;
            }
            unpack();
        });
    }

    /// Unpacked with Windows' own tar, beside the program's folder, so the swap is only renames
    void unpack()
    {
        state = Unpacking;
        QDir(unpackFolder()).removeRecursively();
        if (!QDir().mkpath(unpackFolder())) {
            QFile::remove(download.fileName());
            failed();
            return;
        }
        auto *tar = new QProcess(this);
        const QString program = qEnvironmentVariable("SystemRoot", QStringLiteral("C:\\Windows")) + QStringLiteral("\\System32\\tar.exe");
        connect(tar, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, [this, tar](int code, QProcess::ExitStatus status) {
            tar->deleteLater();
            QFile::remove(download.fileName());
            const QStringList inside = QDir(unpackFolder()).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
            const QString unpacked = inside.size() == 1 ? unpackFolder() + QLatin1Char('/') + inside.first() : QString();
            QDir(readyFolder()).removeRecursively();
            if (code != 0 || status != QProcess::NormalExit || unpacked.isEmpty() || versionIn(unpacked) != found || !isComplete(unpacked)
                || !QDir().rename(unpacked, readyFolder())) {
                QDir(unpackFolder()).removeRecursively();
                failed();
                return;
            }
            QDir(unpackFolder()).removeRecursively();
            state = Ready;
            Q_FOREACH (QPointer<QAction> action, readyActions) {
                if (action) action->setVisible(true);
            }
        });
        connect(tar, &QProcess::errorOccurred, this, [this, tar](QProcess::ProcessError error) {
            if (error != QProcess::FailedToStart) return;
            tar->deleteLater();
            QFile::remove(download.fileName());
            failed();
        });
        tar->start(program, {QStringLiteral("-xf"), download.fileName(), QStringLiteral("-C"), unpackFolder()});
    }

    /// Hands the swap to a script that waits for this process to end
    bool apply()
    {
        const QString script = KisProfile::configFolder() + QStringLiteral("/update.ps1");
        QFile file(script);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(applyScript) < 0) return false;
        file.close();
        QStringList arguments {QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"), QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"),
                               QStringLiteral("-WindowStyle"), QStringLiteral("Hidden"), QStringLiteral("-File"), QDir::toNativeSeparators(script),
                               QStringLiteral("-Id"), QString::number(QCoreApplication::applicationPid()),
                               QStringLiteral("-Install"), QDir::toNativeSeparators(installFolder())};
        if (relaunch) arguments << QStringLiteral("-Relaunch");
        QProcess process;
        process.setProgram(qEnvironmentVariable("SystemRoot", QStringLiteral("C:\\Windows"))
                           + QStringLiteral("\\System32\\WindowsPowerShell\\v1.0\\powershell.exe"));
        process.setArguments(arguments);
#ifdef Q_OS_WIN
        process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) {
            args->flags |= CREATE_NO_WINDOW;
        });
#endif
        return process.startDetached();
    }

    void quitting()
    {
        if (state == Ready && apply()) return;
        if (relaunch) QProcess::startDetached(QCoreApplication::applicationFilePath(), QStringList());
    }
};

} // namespace

QString KisSkissaUpdater::version()
{
    return QStringLiteral(SKISSA_VERSION);
}

bool KisSkissaUpdater::canUpdate()
{
#ifdef Q_OS_WIN
    static const bool released = QFileInfo::exists(installFolder() + QLatin1Char('/') + fileList);
    return released;
#else
    return false;
#endif
}

void KisSkissaUpdater::addActions(KisMainWindow *window)
{
    Updater *updater = Updater::instance();
    static bool started = false;
    if (!started) {
        started = true;
        updater->start();
    }

    auto add = [window](const QString &name, const QString &text, std::function<void()> run) {
        QAction *action = window->actionCollection()->action(name);
        if (action) return action;
        action = new QAction(KisIconUtils::loadIcon(QStringLiteral("studio_refresh")), text, window);
        action->setObjectName(name);
        KisStudioShortcuts::restore(action);
        window->actionCollection()->addAction(name, action);
        window->addAction(action);
        QObject::connect(action, &QAction::triggered, window, run);
        return action;
    };
    add(QStringLiteral("skissa_check_updates"), i18nc("@action", "Check for updates"), [updater]() {
        updater->look(true);
    })->setVisible(canUpdate());
    QAction *ready = add(QStringLiteral("skissa_restart_to_update"), i18nc("@action", "Restart to update"), []() {
        restart();
    });
    ready->setVisible(updater->state == Updater::Ready);
    updater->readyActions << ready;
}

void KisSkissaUpdater::restart()
{
    Q_FOREACH (QPointer<KisMainWindow> window, KisPart::instance()->mainWindows()) {
        if (window && !window->close()) return; // the user cancelled at an unsaved image
    }
    Updater::instance()->relaunch = true;
    QTimer::singleShot(0, qApp, &QCoreApplication::quit);
}
