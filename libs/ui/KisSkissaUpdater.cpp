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
#include <QFutureWatcher>
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
#include <QtConcurrent>

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
# a folder this script is in can't be renamed
Set-Location -LiteralPath $PSScriptRoot
$update = "$Install.update"
$old = "$Install.old"
$log = Join-Path $PSScriptRoot 'update.log'
function Note($text) { Add-Content -LiteralPath $log -Value ('{0:yyyy-MM-dd HH:mm:ss} {1}' -f (Get-Date), $text) }
function Move-Folder($from, $to, $tries = 60) {
    for ($i = 0; $i -lt $tries; $i++) {
        try { [IO.Directory]::Move($from, $to); return $true } catch { Start-Sleep -Milliseconds 500 }
    }
    $false
}
# what is in $from into $to, all of it or none
function Move-Contents($from, $to) {
    New-Item -ItemType Directory -Force -Path $to | Out-Null
    $moved = @()
    foreach ($item in Get-ChildItem -LiteralPath $from -Force) {
        if (-not (Move-Folder $item.FullName (Join-Path $to $item.Name))) {
            foreach ($name in $moved) { Move-Folder (Join-Path $to $name) (Join-Path $from $name) | Out-Null }
            return $false
        }
        $moved += $item.Name
    }
    $true
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
    $list = Join-Path $old 'skissa-files.txt'
    if (-not (Test-Path -LiteralPath $list)) {
        # not a release: kept whole, aside
        Move-Folder $old ($old + '-' + (Get-Date -Format 'yyyyMMdd-HHmmss')) | Out-Null
    } elseif (-not [bool](Strays $old)) {
        Remove-Item -LiteralPath $old -Recurse -Force -ErrorAction SilentlyContinue
    } else {
        # only the user's files stay aside, not a whole install for each update
        Get-Content -LiteralPath $list | Select-Object -Skip 1 | ForEach-Object {
            Remove-Item -LiteralPath (Join-Path $old $_) -Force -ErrorAction SilentlyContinue
        }
        Remove-Item -LiteralPath $list -Force -ErrorAction SilentlyContinue
        Get-ChildItem -LiteralPath $old -Recurse -Directory -Force | Sort-Object { $_.FullName.Length } -Descending |
            Where-Object { -not (Get-ChildItem -LiteralPath $_.FullName -Force) } | Remove-Item -Force -ErrorAction SilentlyContinue
        Move-Folder $old ($old + '-' + (Get-Date -Format 'yyyyMMdd-HHmmss')) | Out-Null
    }
}
# the folder itself, or what is in it while something holds the folder open (a window or a service working there)
$whole = Move-Folder $Install $old 20
if (-not $whole -and -not (Move-Contents $Install $old)) { Note "$Install is in use"; exit }
$placed = if ($whole) { Move-Folder $update $Install } else { Move-Contents $update $Install }
if (-not $placed) {
    if ($whole) { Move-Folder $old $Install | Out-Null } else { Move-Contents $old $Install | Out-Null }
    Note "$update couldn't take its place"
    exit
}
if (-not $whole) { Remove-Item -LiteralPath $update -Force -ErrorAction SilentlyContinue }
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
if ($Relaunch) {
    # a release from before the program was renamed has only krita.exe
    $program = Join-Path $Install 'bin\skissa.exe'
    if (-not (Test-Path -LiteralPath $program)) { $program = Join-Path $Install 'bin\krita.exe' }
    Start-Process -FilePath $program -WorkingDirectory $PSScriptRoot
}
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
    QVector<QPointer<QAction>> checkActions;
    qint64 received {0};
    qint64 total {0};
    int shownPercent {-1};
    /// The user asked for this check, so the download's progress is shown to them
    bool telling {false};

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
            if (asked && state == Downloading) {
                // from now on the message follows the download
                telling = true;
                shownPercent = -1;
                progress();
            } else if (asked) {
                tell(i18nc("@info, %1 a version", "Downloading Skissa %1…", found));
            }
            return;
        }
        if (!asked && !KisSettingsRegistry::instance()->value(QStringLiteral("StudioUpdates")).toBool()) return;

        QString feed = qEnvironmentVariable("KRITA_DEV_UPDATES");
        if (feed.isEmpty()) {
            feed = KisProfile::homepage().replace(QLatin1String("https://github.com/"), QLatin1String("https://api.github.com/repos/"))
                + QStringLiteral("/releases/latest");
        }
        QNetworkRequest request{QUrl(feed)};
        // a stalled connection fails instead of keeping the updater busy for the rest of the session
        request.setTransferTimeout(60000);
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
                if (asked) tell(i18nc("@info", "Skissa is up to date"));
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
            if (QFileInfo::exists(unpackFolder())) QtConcurrent::run([]() { QDir(unpackFolder()).removeRecursively(); });
        } else {
            // what an earlier run left, removed while the window comes up
            QtConcurrent::run([]() {
                if (QFileInfo::exists(readyFolder())) QDir(readyFolder()).removeRecursively();
                if (QFileInfo::exists(unpackFolder())) QDir(unpackFolder()).removeRecursively();
            });
        }
    }

    QString readyText() const
    {
        return i18nc("@info, %1 a version", "Restart Skissa to update to %1", found);
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
        if (telling) tell(i18nc("@info", "Couldn't download the update"));
        settled();
        found = versionIn(readyFolder());
        state = found.isEmpty() ? Idle : Ready;
    }

    /// How far the download is, on Check for updates and, once asked for, as the message over the canvas
    void progress()
    {
        const int percent = total > 0 ? int(received * 100 / total) : 0;
        if (percent == shownPercent) return;
        shownPercent = percent;
        const QString text = i18nc("@action, %1 a version, %2 a percentage", "Downloading Skissa %1 · %2%", found, percent);
        Q_FOREACH (QPointer<QAction> action, checkActions) {
            if (action) action->setText(text);
        }
        if (telling) tell(text);
    }

    /// Check for updates says so again once the download is over, either way
    void settled()
    {
        telling = false;
        shownPercent = -1;
        Q_FOREACH (QPointer<QAction> action, checkActions) {
            if (action) action->setText(i18nc("@action", "Check for updates"));
        }
    }

    /// Downloads \p name, carrying on from what an earlier run fetched of it, and checks it against \p digest once whole
    void fetch(const QString &version, const QString &name, const QUrl &url, qint64 size, const QString &digest, bool asked)
    {
        const QString folder = KisProfile::configFolder() + QStringLiteral("/updates");
        QDir().mkpath(folder);
        // what earlier runs left of other versions goes
        Q_FOREACH (const QFileInfo &file, QDir(folder).entryInfoList(QDir::Files)) {
            if (file.fileName() != name) QFile::remove(file.absoluteFilePath());
        }
        download.setFileName(folder + QLatin1Char('/') + name);
        qint64 have = download.exists() ? download.size() : 0;
        if (have > size) {
            QFile::remove(download.fileName());
            have = 0;
        }
        if (!download.open(have > 0 ? (QIODevice::WriteOnly | QIODevice::Append) : (QIODevice::WriteOnly | QIODevice::Truncate))) {
            if (asked) tell(i18nc("@info", "Couldn't download the update"));
            return;
        }
        state = Downloading;
        found = version;
        total = size;
        received = have;
        telling = asked;
        shownPercent = -1;
        progress();
        if (have == size) {
            download.close();
            verify(digest);
            return;
        }

        QNetworkRequest request(url);
        request.setTransferTimeout(60000);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        request.setRawHeader("User-Agent", "Skissa/" + KisSkissaUpdater::version().toUtf8());
        if (have > 0) request.setRawHeader("Range", "bytes=" + QByteArray::number(have) + '-');
        QNetworkReply *reply = network.get(request);
        auto started = QSharedPointer<bool>::create(false);
        auto take = [this, reply, have, started]() {
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (status != 200 && status != 206) {
                // an error page isn't part of the file
                reply->readAll();
                return;
            }
            if (!*started) {
                *started = true;
                // a server that can't carry on sends all of it again
                if (have > 0 && status != 206) {
                    download.resize(0);
                    received = 0;
                }
            }
            const QByteArray data = reply->readAll();
            download.write(data);
            received += data.size();
            progress();
        };
        connect(reply, &QNetworkReply::readyRead, this, take);
        connect(reply, &QNetworkReply::finished, this, [this, reply, take, size, digest]() {
            reply->deleteLater();
            take();
            const bool written = download.error() == QFileDevice::NoError && download.size() == size;
            download.close();
            if (reply->error() != QNetworkReply::NoError || !written) {
                // what came stays, for the next run to carry on from
                failed();
                return;
            }
            verify(digest);
        });
    }

    /// The whole download against GitHub's checksum, off the window's thread, then unpacked
    void verify(const QString &digest)
    {
        state = Unpacking;
        const QString path = download.fileName();
        auto *done = new QFutureWatcher<QByteArray>(this);
        connect(done, &QFutureWatcher<QByteArray>::finished, this, [this, done, digest, path]() {
            done->deleteLater();
            if (digest.startsWith(QLatin1String("sha256:")) && digest.mid(7).toLatin1() != done->result()) {
                QFile::remove(path);
                failed();
                return;
            }
            unpack();
        });
        done->setFuture(QtConcurrent::run([path]() {
            QFile file(path);
            QCryptographicHash hash(QCryptographicHash::Sha256);
            if (file.open(QIODevice::ReadOnly)) hash.addData(&file);
            return hash.result().toHex();
        }));
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
            // thousands of files checked, moved and removed: off the window's thread
            const bool unpackedWell = code == 0 && status == QProcess::NormalExit;
            const QString version = found;
            auto *done = new QFutureWatcher<bool>(this);
            connect(done, &QFutureWatcher<bool>::finished, this, [this, done]() {
                done->deleteLater();
                if (!done->result()) {
                    failed();
                    return;
                }
                state = Ready;
                if (telling) tell(readyText());
                settled();
                Q_FOREACH (QPointer<QAction> action, readyActions) {
                    if (action) action->setVisible(true);
                }
            });
            done->setFuture(QtConcurrent::run([unpackedWell, version]() {
                const QStringList inside = QDir(unpackFolder()).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
                const QString unpacked = inside.size() == 1 ? unpackFolder() + QLatin1Char('/') + inside.first() : QString();
                QDir(readyFolder()).removeRecursively();
                const bool ready = unpackedWell && !unpacked.isEmpty() && versionIn(unpacked) == version && isComplete(unpacked)
                    && QDir().rename(unpacked, readyFolder());
                QDir(unpackFolder()).removeRecursively();
                return ready;
            }));
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
        return powerShell(arguments);
    }

    /// Runs PowerShell with \p arguments, hidden and on its own after this process ends
    static bool powerShell(const QStringList &arguments)
    {
        QProcess process;
        process.setProgram(qEnvironmentVariable("SystemRoot", QStringLiteral("C:\\Windows"))
                           + QStringLiteral("\\System32\\WindowsPowerShell\\v1.0\\powershell.exe"));
        process.setArguments(arguments);
        // not in the program's folder, which the update renames: Skissa started from a shortcut works in its bin folder
        process.setWorkingDirectory(KisProfile::configFolder());
#ifdef Q_OS_WIN
        process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *args) {
            args->flags |= CREATE_NO_WINDOW;
        });
#endif
        return process.startDetached();
    }

    void quitting()
    {
        // the settings on disk before anything starts that reads them
        KisSettingsRegistry::instance()->flush();
        if (state == Ready && apply()) return;
        if (!relaunch) return;
#ifdef Q_OS_WIN
        // started once this one has ended: a second one starting while this one still runs would hand itself over to it
        QString program = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
        program.replace(QLatin1Char('\''), QStringLiteral("''"));
        const QString command = QStringLiteral("Wait-Process -Id %1 -Timeout 60 -ErrorAction SilentlyContinue; Start-Process -FilePath '%2' -WorkingDirectory (Split-Path -Parent '%2')")
                                    .arg(QCoreApplication::applicationPid())
                                    .arg(program);
        if (powerShell({QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"), QStringLiteral("-WindowStyle"), QStringLiteral("Hidden"),
                        QStringLiteral("-Command"), command})) {
            return;
        }
#endif
        QProcess::startDetached(QCoreApplication::applicationFilePath(), QStringList());
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
    QAction *check = add(QStringLiteral("skissa_check_updates"), i18nc("@action", "Check for updates"), [updater]() {
        updater->look(true);
    });
    check->setVisible(canUpdate());
    updater->checkActions << check;
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
