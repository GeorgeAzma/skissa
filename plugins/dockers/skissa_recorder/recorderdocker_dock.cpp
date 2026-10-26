/*
 *  SPDX-FileCopyrightText: 2019 Shi Yan <billconan@gmail.net>
 *  SPDX-FileCopyrightText: 2020 Dmitrii Utkin <loentar@gmail.com>
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: LGPL-2.1-only
 */

#include "recorderdocker_dock.h"
#include "recorder_config.h"
#include "recorder_writer.h"
#include "recorder_const.h"
#include "recorder_snapshots_manager.h"
#include "recorder_export.h"
#include "recorder_export_settings.h"
#include "recorder_export_config.h"
#include "recorder_export_preprocessor.h"

#include <klocalizedstring.h>
#include <kis_action_registry.h>
#include <kis_canvas2.h>
#include <kis_icon_utils.h>
#include <kis_statusbar.h>
#include <KisDocument.h>
#include <kis_config.h>
#include <KisModernStyle.h>
#include <KisViewManager.h>
#include <KoDocumentInfo.h>
#include <kactioncollection.h>
#include <KisPart.h>
#include "KisMainWindow.h"
#include "KoFileDialog.h"
#ifndef Q_OS_ANDROID
#include "animation/KisFFMpegWrapper.h"
#endif

#include <QAbstractButton>
#include <QActionGroup>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

#ifdef Q_OS_ANDROID
#include <QFile>
#include <QThreadPool>
#endif

namespace
{
const QString keyActionRecordToggle = "recorder_record_toggle";
const QString keyActionExport = "recorder_export";

const QString activeColorGreen(" color='#5cab25'");
const QString inactiveColorGreen(" color='#b4e196'");
const QString activeColorOrange(" color='#ca8f14'");
const QString inactiveColorOrange(" color='#ffe5af'");
const QString activeColorRed(" color='#da4453'");
const QString inactiveColorRed(" color='#f2c4c9'");
const QString inactiveColorGray(" color='#3e3e3e'");

const QColor recordRed(0xe5, 0x48, 0x4d);

/// The video's frame rate when its length is worked out.
constexpr int videoFps = 30;

QColor inkOf(const QWidget *widget, qreal alpha)
{
    QColor ink = widget->palette().color(QPalette::WindowText);
    ink.setAlphaF(alpha);
    return ink;
}

/// m:ss
QString clock(qint64 seconds)
{
    return QStringLiteral("%1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QLatin1Char('0'));
}

/**
 * The recording itself: the latest frame, or any earlier one while dragging across it. The play button
 * runs it as the video will. A double or right click goes back to the latest frame.
 */
class TimelapseView : public QWidget
{
public:
    explicit TimelapseView(QWidget *parent)
        : QWidget(parent)
    {
        setMouseTracking(true);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        setToolTip(i18nc("@info:tooltip the time-lapse preview, scrubbed", "Earlier ← → now"));
        m_play.setInterval(1000 / videoFps);
        QObject::connect(&m_play, &QTimer::timeout, this, [this]() {
            if (m_index < 0 || m_index >= m_frames.size() - 1) {
                m_play.stop();
                m_index = -1;
            } else {
                m_index++;
            }
            update();
        });
    }

    void setFrames(const QStringList &frames)
    {
        if (frames == m_frames) return;
        const QString last = m_frames.isEmpty() ? QString() : m_frames.last();
        m_frames = frames;
        if (m_index >= m_frames.size()) m_index = -1;
        if (m_frames.isEmpty() || m_frames.last() != last) m_cache.clear();
        // its height follows the frames' shape (set when one is read), not their number
        update();
    }

    bool hasHeightForWidth() const override { return true; }

    int heightForWidth(int width) const override
    {
        const QSize frame = m_aspect.isEmpty() ? QSize(3, 2) : m_aspect;
        return qBound(80, width * frame.height() / qMax(1, frame.width()), 320);
    }

    QSize sizeHint() const override { return QSize(260, heightForWidth(260)); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QRectF tile = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        const QPainterPath shape = KisModernStyle::squircle(tile, KisModernStyle::cardRadius);
        painter.fillPath(shape, inkOf(this, 0.06));

        if (m_frames.isEmpty()) {
            // nothing yet: the record mark, faintly
            painter.setPen(QPen(inkOf(this, 0.18), 2));
            painter.setBrush(Qt::NoBrush);
            const QPointF center = tile.center();
            painter.drawEllipse(center, 16, 16);
            painter.setPen(Qt::NoPen);
            painter.setBrush(inkOf(this, 0.18));
            painter.drawEllipse(center, 7, 7);
            return;
        }

        const QImage frame = frameAt(shown());
        if (!frame.isNull()) {
            const QSizeF size = QSizeF(frame.size()) / frame.devicePixelRatioF();
            QRectF target(QPointF(), size.scaled(tile.size(), Qt::KeepAspectRatio));
            target.moveCenter(tile.center());
            painter.save();
            painter.setClipPath(shape);
            painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
            painter.drawImage(target, frame);
            painter.restore();
        }

        if (m_frames.size() < 2) return;
        // where in the recording this is, along the bottom
        const QRectF track(tile.left() + 10, tile.bottom() - 7, tile.width() - 20, 3);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0, 0, 0, 90));
        painter.drawRoundedRect(track.adjusted(-1, -1, 1, 1), 2.5, 2.5);
        QRectF done = track;
        done.setWidth(track.width() * (shown() + 1) / m_frames.size());
        painter.setBrush(QColor(255, 255, 255, 220));
        painter.drawRoundedRect(done, 1.5, 1.5);

        if (underMouse() || m_play.isActive() || m_scrubbing) {
            const QRectF button = playButton();
            painter.setBrush(QColor(0, 0, 0, 120));
            painter.drawEllipse(button);
            painter.setBrush(Qt::white);
            const QPointF c = button.center();
            if (m_play.isActive()) {
                painter.drawRoundedRect(QRectF(c.x() - 5, c.y() - 6, 3.5, 12), 1, 1);
                painter.drawRoundedRect(QRectF(c.x() + 1.5, c.y() - 6, 3.5, 12), 1, 1);
            } else {
                painter.drawPolygon(QPolygonF({c + QPointF(-4, -6.5), c + QPointF(7, 0), c + QPointF(-4, 6.5)}));
            }
            // the time in the video, while looking back
            if (m_index >= 0) {
                const QString time = clock(m_index / videoFps) + QStringLiteral(" / ") + clock(m_frames.size() / videoFps);
                QFont font = KisModernStyle::textFont(this->font(), 0.85);
                painter.setFont(font);
                const QFontMetricsF metrics(font);
                QRectF pill(0, 0, metrics.horizontalAdvance(time) + 14, metrics.height() + 4);
                pill.moveTopRight(tile.topRight() + QPointF(-8, 8));
                painter.setBrush(QColor(0, 0, 0, 120));
                painter.drawPath(KisModernStyle::squircle(pill, pill.height() / 2));
                painter.setPen(Qt::white);
                painter.drawText(pill, Qt::AlignCenter, time);
            }
        }
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::RightButton) {
            showLatest();
            return;
        }
        if (event->button() != Qt::LeftButton || m_frames.isEmpty()) return;
        if (m_frames.size() > 1 && playButton().contains(event->pos())) {
            if (m_play.isActive()) {
                m_play.stop();
            } else {
                if (m_index < 0 || m_index >= m_frames.size() - 1) m_index = 0;
                m_play.start();
            }
            update();
            return;
        }
        m_play.stop();
        m_scrubbing = true;
        scrubTo(event->pos().x());
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (m_scrubbing) scrubTo(event->pos().x());
        else update();
    }

    void mouseReleaseEvent(QMouseEvent *) override
    {
        m_scrubbing = false;
        update();
    }

    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        if (!playButton().contains(event->pos())) showLatest();
    }

    void leaveEvent(QEvent *) override { update(); }
    void resizeEvent(QResizeEvent *) override { m_cache.clear(); }

private:
    int shown() const { return m_index < 0 ? m_frames.size() - 1 : m_index; }

    QRectF playButton() const { return QRectF(10, height() - 46, 32, 32); }

    void scrubTo(int x)
    {
        const qreal share = qBound<qreal>(0, (x - 10.0) / qMax(1, width() - 20), 1);
        m_index = qRound(share * (m_frames.size() - 1));
        update();
    }

    void showLatest()
    {
        m_play.stop();
        m_index = -1;
        update();
    }

    /// The frame, decoded at the size it is shown (the last few kept).
    QImage frameAt(int index)
    {
        auto it = m_cache.constFind(index);
        if (it != m_cache.constEnd()) return *it;
        QImageReader reader(m_frames[index]);
        const QSize full = reader.size();
        if (full.isEmpty()) return QImage();
        if (m_aspect != full) {
            m_aspect = full;
            updateGeometry();
        }
        const qreal ratio = devicePixelRatioF();
        reader.setScaledSize(full.scaled((QSizeF(size()) * ratio).toSize(), Qt::KeepAspectRatio).boundedTo(full));
        QImage image = reader.read();
        image.setDevicePixelRatio(ratio);
        if (m_cache.size() > 6) m_cache.clear();
        m_cache.insert(index, image);
        return image;
    }

    QStringList m_frames;
    /// The frame shown; -1 follows the latest.
    int m_index {-1};
    bool m_scrubbing {false};
    QTimer m_play;
    QHash<int, QImage> m_cache;
    QSize m_aspect;
};

/// The round record button: a red dot to record, a red square to stop; while frames are taken, it glows.
class RecordButton : public QAbstractButton
{
public:
    explicit RecordButton(QWidget *parent)
        : QAbstractButton(parent)
    {
        setCheckable(true);
        setFixedSize(44, 44);
        setCursor(Qt::PointingHandCursor);
    }

    void setActive(bool active)
    {
        m_active = active;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QRectF ring = QRectF(rect()).adjusted(2, 2, -2, -2);
        QColor rim = isChecked() ? recordRed : inkOf(this, underMouse() ? 0.45 : 0.28);
        if (!isEnabled()) rim = inkOf(this, 0.12);
        if (isChecked() && m_active) {
            QColor glow = recordRed;
            glow.setAlphaF(0.25);
            painter.setPen(Qt::NoPen);
            painter.setBrush(glow);
            painter.drawEllipse(ring);
        }
        painter.setPen(QPen(rim, 2));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(ring.adjusted(1, 1, -1, -1));
        painter.setPen(Qt::NoPen);
        painter.setBrush(isEnabled() ? recordRed : inkOf(this, 0.18));
        const QPointF c = ring.center();
        if (isChecked()) {
            painter.drawPath(KisModernStyle::squircle(QRectF(c - QPointF(7, 7), QSizeF(14, 14)), 4));
        } else {
            painter.drawEllipse(c, 9, 9);
        }
    }

    void enterEvent(QEvent *) override { update(); }
    void leaveEvent(QEvent *) override { update(); }

private:
    bool m_active {false};
};

/// The video's length, dragged sideways; right click goes back to the default.
class LengthValue : public QWidget
{
public:
    std::function<void(int)> changed;

    explicit LengthValue(QWidget *parent)
        : QWidget(parent)
    {
        setCursor(Qt::SizeHorCursor);
        setToolTip(i18nc("@info:tooltip", "How long the whole time-lapse plays"));
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    }

    void setSeconds(int seconds)
    {
        m_seconds = qBound(minimum, seconds, maximum);
        update();
    }

    int seconds() const { return m_seconds; }

    QSize sizeHint() const override
    {
        const QFontMetrics metrics(font());
        return QSize(metrics.horizontalAdvance(QStringLiteral("888 s")) + 2 * KisModernStyle::space2, 28);
    }

    static constexpr int minimum = 5;
    static constexpr int maximum = 300;
    static constexpr int standard = 30;

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QRectF box = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        painter.setPen(Qt::NoPen);
        painter.setBrush(inkOf(this, underMouse() || m_pressed ? 0.12 : 0.07));
        painter.drawPath(KisModernStyle::squircle(box, KisModernStyle::cornerRadius));
        QFont font = this->font();
        painter.setFont(font);
        painter.setPen(palette().color(QPalette::WindowText));
        painter.drawText(box, Qt::AlignCenter, i18nc("a length in seconds, as short as possible", "%1 s", m_seconds));
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::RightButton) {
            set(standard);
            return;
        }
        m_pressed = event->button() == Qt::LeftButton;
        m_pressX = event->pos().x();
        m_pressSeconds = m_seconds;
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (!m_pressed) return;
        // a second for every few pixels; Shift is finer
        const int step = event->modifiers() & Qt::ShiftModifier ? 12 : 4;
        set(m_pressSeconds + (event->pos().x() - m_pressX) / step);
    }

    void mouseReleaseEvent(QMouseEvent *) override
    {
        m_pressed = false;
        update();
    }

    void wheelEvent(QWheelEvent *event) override
    {
        set(m_seconds + KisModernStyle::wheelSteps(event, this));
        event->accept();
    }

    void enterEvent(QEvent *) override { update(); }
    void leaveEvent(QEvent *) override { update(); }

private:
    void set(int seconds)
    {
        seconds = qBound(minimum, seconds, maximum);
        if (seconds == m_seconds) return;
        m_seconds = seconds;
        update();
        if (changed) changed(seconds);
    }

    int m_seconds {standard};
    bool m_pressed {false};
    int m_pressX {0};
    int m_pressSeconds {0};
};

/// The export button, filling as the video is encoded.
class ExportButton : public QPushButton
{
public:
    using QPushButton::QPushButton;

    void setProgress(qreal progress)
    {
        m_progress = progress;
        update();
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        QPushButton::paintEvent(event);
        if (m_progress <= 0) return;
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        QRectF bar = QRectF(rect()).adjusted(3, height() - 5, -3, -2);
        bar.setWidth(bar.width() * qMin<qreal>(1, m_progress));
        painter.setPen(Qt::NoPen);
        painter.setBrush(palette().color(QPalette::Highlight));
        painter.drawRoundedRect(bar, 1.5, 1.5);
    }

private:
    qreal m_progress {0};
};

/// Where the profiles of RecorderExportConfig keep each format.
struct VideoFormat {
    QString name;
    int profile;
    /// What it is good for
    QString toolTip;
};

const QVector<VideoFormat> &videoFormats()
{
    static const QVector<VideoFormat> formats = {
        {QStringLiteral("MP4"), 0, i18nc("@info:tooltip video format", "Plays anywhere")},
        {QStringLiteral("GIF"), 1, i18nc("@info:tooltip video format", "Loops in chats. Big files")},
        {QStringLiteral("WebM"), 3, i18nc("@info:tooltip video format", "Small, for the web")}};
    return formats;
}

#ifndef Q_OS_ANDROID
/// Splits ffmpeg arguments as the export dialog does: quotes group, three quotes are a quote.
QStringList splitCommand(const QString &command)
{
    QStringList args;
    QString token;
    int quotes = 0;
    bool inQuote = false;
    for (const QChar ch : command) {
        if (ch == QLatin1Char('"')) {
            if (++quotes == 3) {
                quotes = 0;
                token += ch;
            }
            continue;
        }
        if (quotes) {
            if (quotes == 1) inQuote = !inQuote;
            quotes = 0;
        }
        if (!inQuote && ch.isSpace()) {
            if (!token.isEmpty()) {
                args += token;
                token.clear();
            }
        } else {
            token += ch;
        }
    }
    if (!token.isEmpty()) args += token;
    return args;
}
#endif

} // namespace


class RecorderDockerDock::Private
{
public:
    RecorderDockerDock *const q;
    QPointer<KisCanvas2> canvas;
    RecorderWriterManager writer;

    QAction *recordToggleAction = nullptr;
    QAction *exportAction = nullptr;

    TimelapseView *view = nullptr;
    RecordButton *recordButton = nullptr;
    QLabel *lengthLabel = nullptr;
    QList<QToolButton *> formatButtons;
    LengthValue *lengthValue = nullptr;
    ExportButton *exportButton = nullptr;
    QToolButton *menuButton = nullptr;
    QAction *autoRecordAction = nullptr;
    QActionGroup *intervalGroup = nullptr;
    QActionGroup *resolutionGroup = nullptr;
    QTimer framesTimer;
    QTimer showTimer;
    int frameCount = 0;

    QString snapshotDirectory;
    QString prefix;
    QString outputDirectory;
    double captureInterval = 0.;
    RecorderFormat format = RecorderFormat::JPEG;
    int quality = 0;
    int compression = 0;
    int resolution = 0;
    bool realTimeCaptureMode = false;
    bool recordIsolateLayerMode = false;
    bool recordAutomatically = false;
    bool paused = true;
#ifdef Q_OS_ANDROID
    bool internalMoveInProgress{false};
#else
    QScopedPointer<KisFFMpegWrapper> exporter;
    QString exportedFile;
#endif
    QTimer pausedTimer;
    QTimer warningTimer;

    QLabel* statusBarLabel;
    QLabel* statusBarWarningLabel;

    QMap<QString, bool> enabledIds;

    Private(const RecorderExportSettings &es, RecorderDockerDock *q_ptr)
        : q(q_ptr)
        , writer(es)
        , statusBarLabel(new QLabel())
        , statusBarWarningLabel(new QLabel())
    {
        updateRecIndicator();
        statusBarWarningLabel->setPixmap(KisIconUtils::loadIcon("warning").pixmap(16, 16));
        statusBarWarningLabel->hide();
        warningTimer.setInterval(10000);
        warningTimer.setSingleShot(true);
        pausedTimer.setSingleShot(true);
        connect(&warningTimer, SIGNAL(timeout()), q, SLOT(onWarningTimeout()));
        connect(&pausedTimer, SIGNAL(timeout()), q, SLOT(onPausedTimeout()));
    }

    // For technical reasons recorder can not capture an image that is larger than 2^31 - 1 bytes in size, It is also
    // kinda absurd that you'd even want to do it, so we just shouldn't support it
    bool canRecord()
    {
        QSize imageSize = canvas->image()->size();

        // We always convert to RGBA8, so can assume 4 bytes/pixel
        quint64 totalSize = (quint64)imageSize.width() * (quint64)imageSize.height() * 4;

        return totalSize <= 2147483647;
    }

    void loadSettings()
    {
        RecorderConfig config(true);
        snapshotDirectory = config.snapshotDirectory();
#ifdef Q_OS_ANDROID
        fixInternalSnapshotDirectory();
#endif
        captureInterval = config.captureInterval();
        format = config.format();
        quality = config.quality();
        compression = config.compression();
        resolution = config.resolution();
        writer.recorderThreads.set(config.threads());
        realTimeCaptureMode = config.realTimeCaptureMode();
        if (realTimeCaptureMode) {
            q->exportSettings->lockFps = true;
            q->exportSettings->realTimeCaptureModeWasSet = true;
        }
        recordIsolateLayerMode = config.recordIsolateLayerMode();
        recordAutomatically = config.recordAutomatically();
    }

    void loadRelevantExportSettings()
    {
        RecorderExportConfig config(true);
        q->exportSettings->fps = config.fps();
    }

    void updateWriterSettings()
    {
        outputDirectory = snapshotDirectory % QDir::separator() % prefix % QDir::separator();
        writer.setup({
            outputDirectory,
            format,
            quality,
            compression,
            resolution,
            captureInterval,
            recordIsolateLayerMode,
            realTimeCaptureMode});
    }

    QString getPrefix()
    {
        return !canvas ? ""
               : canvas->imageView()->document()->documentInfo()->aboutInfo("creation-date").remove(QRegularExpression("[^0-9]"));
    }

    /// This image's frames so far, oldest first.
    QStringList frames() const
    {
        if (!canvas || outputDirectory.isEmpty()) return QStringList();
        const QDir dir(outputDirectory, QStringLiteral("*.") + RecorderFormatInfo::fileExtension(format), QDir::Name,
                       QDir::Files | QDir::NoDotAndDotDot);
        QStringList paths;
        for (const QString &name : dir.entryList()) paths << dir.filePath(name);
        return paths;
    }

    void refreshFrames()
    {
        const QStringList all = frames();
        frameCount = all.size();
        view->setFrames(all);
        lengthLabel->setText(clock(frameCount / videoFps));
        lengthLabel->setVisible(frameCount > 0);
        updateExportEnabled();
    }

    void updateExportEnabled()
    {
#ifndef Q_OS_ANDROID
        const bool exporting = exporter;
#else
        const bool exporting = false;
#endif
        const bool recording = recordButton->isChecked();
        exportButton->setEnabled(canvas && !recording && !exporting && frameCount > 0);
        exportButton->setToolTip(recording ? i18nc("@info:tooltip", "Stop recording to save the video")
                                           : i18nc("@info:tooltip", "Save the video beside the image"));
    }

    void updateRecordStatus(bool isRecording)
    {
        recordToggleAction->setChecked(isRecording);
        recordToggleAction->setEnabled(true);

        QSignalBlocker blocker(recordButton);
        recordButton->setChecked(isRecording);
        if (!isRecording) recordButton->setActive(false);
        recordButton->setToolTip(isRecording ? i18nc("Stop recording the canvas", "Stop") : i18nc("Start recording the canvas", "Record"));
        if (canvas && !canRecord()) {
            recordButton->setEnabled(false);
            recordButton->setToolTip(i18n("Image too large to be recorded"));
        } else {
            recordButton->setEnabled(!canvas.isNull());
        }
        if (isRecording) framesTimer.start();
        else framesTimer.stop();
        if (canvas) {
            refreshFrames();
        } else {
            frameCount = 0;
            updateExportEnabled();
        }

        statusBarLabel->setVisible(isRecording);

        if (!canvas)
            return;

        KisStatusBar *statusBar = canvas->viewManager()->statusBar();
        if (isRecording) {
            statusBar->addExtraWidget(statusBarWarningLabel);
            statusBar->addExtraWidget(statusBarLabel);
            updateRecIndicator();
        } else {
            statusBar->removeExtraWidget(statusBarWarningLabel);
            statusBar->removeExtraWidget(statusBarLabel);
        }
    }

    void updateRecIndicator()
    {
        auto threads = writer.recorderThreads.get();
        auto threadsInUse = writer.recorderThreads.getUsed();
        QString label("<font style='letter-spacing:-4px'>");
        QString activeColor;
        QString inactiveColor;
        for (unsigned int threadNr = 1; threadNr <= ThreadSystemValue::MaxThreadCount ; threadNr++)
        {
            if (threadNr > threads) {
                activeColor = inactiveColorGray;
                inactiveColor = inactiveColorGray;
            } else if (threadNr > ThreadSystemValue::MaxRecordThreadCount) {
                activeColor = activeColorRed;
                inactiveColor = inactiveColorRed;
            } else if (threadNr > ThreadSystemValue::IdealRecordThreadCount) {
                activeColor = activeColorOrange;
                inactiveColor = inactiveColorOrange;
            } else {
                activeColor = activeColorGreen;
                inactiveColor = inactiveColorGreen;
            }
            label.append(QString("<font%1>▍</font>")
                             .arg(threadNr <= threadsInUse ? activeColor : inactiveColor));
        }
        // don't remove empty <font></font> tag else label will jump a few pixels around
        label.append(QString("</font><font> %1 </font><font%2>●</font>")
                         .arg(i18nc("Recording symbol", "REC"))
                         .arg(paused ? "" : activeColorRed));
        statusBarLabel->setText(label);
        statusBarLabel->setToolTip(paused ? i18n("Recorder is paused") : QString(i18n("Active recording with %1 of %2 available threads")).arg(threadsInUse).arg(threads));
        if (recordButton) recordButton->setActive(!paused);
    }

    void showWarning(const QString &hint) {
        if (statusBarWarningLabel->isHidden()) {
            statusBarWarningLabel->setToolTip(hint);
            statusBarWarningLabel->show();
            warningTimer.start();
        }
    }

    void buildUi()
    {
        QWidget *page = new QWidget(q);
        auto *layout = new QVBoxLayout(page);
        layout->setContentsMargins(KisModernStyle::space2, KisModernStyle::space2, KisModernStyle::space2, KisModernStyle::space2);
        layout->setSpacing(KisModernStyle::space2);

        view = new TimelapseView(page);
        layout->addWidget(view);

        auto *recordRow = new QHBoxLayout();
        recordRow->setSpacing(KisModernStyle::space2);
        recordButton = new RecordButton(page);
        recordRow->addWidget(recordButton);
        lengthLabel = new QLabel(page);
        QFont font = KisModernStyle::textFont(lengthLabel->font(), 1.15, QFont::DemiBold);
        lengthLabel->setFont(font);
        lengthLabel->setToolTip(i18nc("@info:tooltip", "Time-lapse so far"));
        recordRow->addWidget(lengthLabel);
        recordRow->addStretch(1);
        menuButton = new QToolButton(page);
        menuButton->setAutoRaise(true);
        menuButton->setIcon(KisIconUtils::loadIcon(QStringLiteral("studio_more")));
        menuButton->setPopupMode(QToolButton::InstantPopup);
        menuButton->setToolTip(i18nc("@info:tooltip", "More"));
        menuButton->setStyleSheet(QStringLiteral("QToolButton::menu-indicator { image: none; }"));
        recordRow->addWidget(menuButton);
        layout->addLayout(recordRow);

        auto *exportRow = new QHBoxLayout();
        exportRow->setSpacing(KisModernStyle::space1);
        RecorderExportConfig exportConfig(true);
        const int profile = exportConfig.profileIndex();
        for (const VideoFormat &videoFormat : videoFormats()) {
            auto *button = new QToolButton(page);
            button->setText(videoFormat.name);
            button->setToolTip(videoFormat.toolTip);
            button->setCheckable(true);
            button->setAutoExclusive(true);
            button->setAutoRaise(true);
            button->setProperty(KisModernStyle::tabButtonProperty, true);
            button->setChecked(videoFormat.profile == profile);
            QObject::connect(button, &QToolButton::toggled, q, [videoFormat](bool on) {
                if (on) RecorderExportConfig(false).setProfileIndex(videoFormat.profile);
            });
            formatButtons << button;
            exportRow->addWidget(button);
        }
        // a format of the classic dialog's: shown as the first, kept until one is picked
        if (std::none_of(formatButtons.begin(), formatButtons.end(), [](QToolButton *b) { return b->isChecked(); })) {
            const QSignalBlocker blocker(formatButtons.first());
            formatButtons.first()->setChecked(true);
        }
        exportRow->addSpacing(KisModernStyle::space1);
        lengthValue = new LengthValue(page);
        lengthValue->setSeconds(KisConfig(true).readEntry<int>("recorder_export/lengthSec", LengthValue::standard));
        lengthValue->changed = [](int seconds) { KisConfig(false).writeEntry("recorder_export/lengthSec", seconds); };
        exportRow->addWidget(lengthValue);
        exportRow->addStretch(1);
        exportButton = new ExportButton(i18nc("Save the time-lapse video", "Save"), page);
        exportRow->addWidget(exportButton);
        layout->addLayout(exportRow);
        layout->addStretch(1);

        buildMenu();
        q->setWidget(page);

        framesTimer.setInterval(1500);
        // the folder listed only while the panel is seen: it holds thousands of frames after a long session
        QObject::connect(&framesTimer, &QTimer::timeout, q, [this]() {
            if (q->isVisible()) refreshFrames();
        });
        QObject::connect(q, &QDockWidget::visibilityChanged, q, [this](bool visible) {
            if (visible && framesTimer.isActive()) refreshFrames();
        });
        showTimer.setSingleShot(true);
        showTimer.setInterval(8000);
        QObject::connect(&showTimer, &QTimer::timeout, q, [this]() {
            exportButton->setText(i18nc("Save the time-lapse video", "Save"));
#ifndef Q_OS_ANDROID
            exportedFile.clear();
#endif
        });
    }

    void buildMenu()
    {
        auto *menu = new QMenu(menuButton);
        autoRecordAction = menu->addAction(i18nc("@action:inmenu record every image from its start", "Record every image"));
        autoRecordAction->setCheckable(true);
        autoRecordAction->setChecked(recordAutomatically);
        QObject::connect(autoRecordAction, &QAction::toggled, q, [this](bool on) {
            RecorderConfig(false).setRecordAutomatically(on);
            loadSettings();
        });

        menu->addSection(i18nc("@title:menu how often a frame is taken", "A frame every"));
        intervalGroup = new QActionGroup(menu);
        for (const double seconds : {0.5, 1.0, 2.0, 5.0}) {
            QAction *action = menu->addAction(i18nc("a time in seconds, as short as possible", "%1 s", QLocale().toString(seconds)));
            action->setCheckable(true);
            action->setChecked(qFuzzyCompare(seconds, captureInterval));
            intervalGroup->addAction(action);
            QObject::connect(action, &QAction::triggered, q, [this, seconds]() {
                RecorderConfig(false).setCaptureInterval(seconds);
                loadSettings();
                updateWriterSettings();
            });
        }

        menu->addSection(i18nc("@title:menu the size of recorded frames", "Frame size"));
        resolutionGroup = new QActionGroup(menu);
        const QStringList sizes = {i18nc("frames at the image's size", "Full"), i18nc("frames at half the image's size", "Half"),
                                   i18nc("frames at a quarter of the image's size", "Quarter")};
        for (int index = 0; index < sizes.size(); index++) {
            QAction *action = menu->addAction(sizes[index]);
            action->setCheckable(true);
            action->setChecked(index == resolution);
            resolutionGroup->addAction(action);
            QObject::connect(action, &QAction::triggered, q, [this, index]() {
                RecorderConfig(false).setResolution(index);
                loadSettings();
                updateWriterSettings();
            });
        }

        menu->addSeparator();
        menu->addAction(KisIconUtils::loadIcon("folder"), i18nc("@action:inmenu where recordings are kept", "Recordings folder…"), q,
                        SLOT(slotSelectSnapshotDirectory()));
        menu->addAction(i18nc("@action:inmenu", "Manage recordings…"), q, [this]() {
            RecorderSnapshotsManager snapshotsManager(q);
            snapshotsManager.execFor(snapshotDirectory);
            refreshFrames();
        });
        menu->addAction(i18nc("@action:inmenu the full time-lapse export dialog", "Export options…"), q, SLOT(onExportOptionsClicked()));
        QObject::connect(menu, &QMenu::aboutToShow, q, [this]() {
            // recording settings take effect from the next recording
            const bool recording = recordButton->isChecked();
            intervalGroup->setEnabled(!recording);
            resolutionGroup->setEnabled(!recording);
            autoRecordAction->setChecked(recordAutomatically);
        });
        menuButton->setMenu(menu);
    }

#ifndef Q_OS_ANDROID
    /// A file beside the image (or in the videos folder for one not saved yet) that is not taken.
    QString videoPath(const QString &extension) const
    {
        KisDocument *document = canvas->imageView()->document();
        const QFileInfo image(document->localFilePath());
        QString directory = image.exists() ? image.absolutePath() : RecorderExportConfig(true).videoDirectory();
        if (directory.isEmpty()) directory = QDir::homePath();
        const QString base = QFileInfo(document->caption().trimmed()).completeBaseName();
        const QString name = (base.isEmpty() ? i18nc("file name of a time-lapse video", "Time-lapse") : base + QStringLiteral(" ")
                                                                                                         + i18nc("file name suffix of a time-lapse video", "time-lapse"));
        QString path = QDir(directory).filePath(name + QStringLiteral(".") + extension);
        for (int n = 2; QFileInfo::exists(path); n++) {
            path = QDir(directory).filePath(QStringLiteral("%1 %2.%3").arg(name).arg(n).arg(extension));
        }
        return path;
    }

    QString applyVariables(const QString &templateArguments) const
    {
        const RecorderExportSettings *settings = q->exportSettings;
        const QSize &outSize = settings->resize ? settings->size : settings->imageSize;
        const int previewLength = settings->resultPreview ? settings->firstFrameSec : 0;
        const int resultLength = settings->extendResult ? settings->lastFrameSec : 0;
        const float transitionLength = settings->resultPreview ? 0.7 : 0;
        return QString(templateArguments)
            .replace("$IN_FPS", QString::number(settings->inputFps))
            .replace("$OUT_FPS", QString::number(settings->fps))
            .replace("$WIDTH", QString::number(outSize.width()))
            .replace("$HEIGHT", QString::number(outSize.height()))
            .replace("$FRAMES", QString::number(settings->framesCount))
            .replace("$INPUT_DIR", settings->inputDirectory)
            .replace("$FIRST_FRAME_SEC", QString::number(previewLength))
            .replace("$TRANSITION_LENGTH", QString::number(transitionLength))
            .replace("$H264_ENCODER", settings->h264Encoder)
            .replace("$LAST_FRAME_SEC", QString::number(resultLength))
            .replace("$EXT", RecorderFormatInfo::fileExtension(settings->format));
    }

    /// Encodes the frames into the chosen format and length, with the export dialog's other settings.
    void exportVideo()
    {
        RecorderExportSettings *settings = q->exportSettings;
        RecorderExportConfig(true).loadConfiguration(settings);
        const QJsonObject ffmpeg = KisFFMpegWrapper::findFFMpeg(settings->ffmpegPath);
        if (!ffmpeg["enabled"].toBool()) {
            QMessageBox::warning(q, i18nc("@title:window", "Time-lapse"),
                                 i18n("FFmpeg, which makes the video, was not found. Choose where it is in Export Options."));
            return;
        }
        settings->ffmpegPath = ffmpeg["path"].toString();
        const QJsonArray encoders = ffmpeg["codecs"].toObject()["h264"].toObject()["encoders"].toArray();
        settings->h264Encoder = encoders.contains("libopenh264") ? "libopenh264" : "libx264";

        settings->inputDirectory = outputDirectory;
        settings->format = format;
        settings->realTimeCaptureMode = realTimeCaptureMode;
        RecorderExportPreprocessor preprocessor(settings->inputDirectory, settings->format);
        preprocessor.doPreprocessing();
        // it renames the frames: the preview reads them by their new names
        refreshFrames();
        const QStringList all = frames();
        settings->framesCount = all.size();
        if (all.isEmpty()) return;
        settings->imageSize = QImageReader(all.last()).size();
        settings->imageSize.rwidth() &= ~1;
        settings->imageSize.rheight() &= ~1;
        // videos over 1920 pixels often fail to encode or upload
        constexpr int limit = 1920;
        settings->resize = true;
        settings->size = settings->imageSize.scaled(QSize(limit, limit), Qt::KeepAspectRatio).boundedTo(settings->imageSize);
        settings->size.rwidth() &= ~1;
        settings->size.rheight() &= ~1;
        // as many frames a second as fit the chosen length, after the stills at the start and the end
        const int stills = (settings->resultPreview ? settings->firstFrameSec : 0) + (settings->extendResult ? settings->lastFrameSec : 0);
        const int painting = qMax(1, lengthValue->seconds() - stills);
        settings->inputFps = qBound(8, qRound(qreal(settings->framesCount) / painting), 240);
        settings->profileIndex = RecorderExportConfig(true).profileIndex();
        const RecorderProfile &profile = settings->profiles[qBound(0, settings->profileIndex, int(settings->profiles.size()) - 1)];
        settings->videoFilePath = videoPath(profile.extension);

        exporter.reset(new KisFFMpegWrapper(q));
        connect(exporter.data(), SIGNAL(sigStarted()), q, SLOT(onExporterStarted()));
        connect(exporter.data(), SIGNAL(sigFinished()), q, SLOT(onExporterFinished()));
        connect(exporter.data(), SIGNAL(sigFinishedWithError(QString)), q, SLOT(onExporterFinishedWithError(QString)));
        connect(exporter.data(), SIGNAL(sigProgressUpdated(int)), q, SLOT(onExporterProgressUpdated(int)));
        KisFFMpegWrapperSettings run;
        run.processPath = settings->ffmpegPath;
        run.args = splitCommand(applyVariables(profile.arguments));
        run.outputFile = settings->videoFilePath;
        run.batchMode = true;
        exportButton->setText(i18nc("the time-lapse video is being made", "Saving…"));
        exportButton->setProgress(0.01);
        updateExportEnabled();
        exporter->startNonBlocking(run);
    }

    void finishExport()
    {
        if (exporter) {
            exporter->reset();
            exporter.reset();
        }
        exportButton->setProgress(0);
        updateExportEnabled();
    }
#endif

#ifdef Q_OS_ANDROID
    void fixInternalSnapshotDirectory()
    {
        // Older versions of Krita used an internal directory as the snapshots
        // directory by default, which is a bogus place to save stuff to because
        // the user can't access it. That means the files stored there are stuck
        // inaccessible and once the user picks a "real" directory, they can no
        // longer even delete the files. So here we're rectifying the situation.
        if (snapshotDirectory == RecorderConfig::defaultInternalSnapshotDirectory()) {
            // Internal path got persisted to settings. Clear that out, replace
            // it with the default of nothing.
            snapshotDirectory = QString();
        } else {
            q->moveFilesFromInternalSnapshotDirectory();
        }
    }
#endif
};

RecorderDockerDock::RecorderDockerDock()
    : QDockWidget(i18nc("Title of the docker", "Time-lapse"))
    , exportSettings(new RecorderExportSettings())
    , d(new Private(*exportSettings, this))
{
    setWindowIcon(KisIconUtils::loadIcon(QStringLiteral("studio_timelapse")));
    d->loadSettings();
    d->loadRelevantExportSettings();
    d->buildUi();

    KisActionRegistry *actionRegistry = KisActionRegistry::instance();
    d->recordToggleAction = actionRegistry->makeQAction(keyActionRecordToggle, this);
    d->exportAction = actionRegistry->makeQAction(keyActionExport, this);

    connect(d->recordToggleAction, SIGNAL(toggled(bool)), d->recordButton, SLOT(setChecked(bool)));
    // the shortcut does what the button would, and nothing while the button can't (recording, saving)
    connect(d->exportAction, &QAction::triggered, this, [this]() {
        if (d->exportButton->isEnabled()) onExportButtonClicked();
    });

    // Need to register toolbar actions before attaching canvas else it wont appear after restart.
    // Is there any better way to do this?
    connect(KisPart::instance(), SIGNAL(sigMainWindowIsBeingCreated(KisMainWindow *)),
            this, SLOT(onMainWindowIsBeingCreated(KisMainWindow *)));

    connect(d->recordButton, SIGNAL(toggled(bool)), this, SLOT(onRecordButtonToggled(bool)));
    connect(d->exportButton, SIGNAL(clicked()), this, SLOT(onExportButtonClicked()));

    connect(&d->writer.recorderThreads, SIGNAL(notifyInUseChange(bool)), this, SLOT(onActiveRecording(bool)));
    connect(&d->writer.recorderThreads, SIGNAL(notifyInUseChange(bool)), this, SLOT(onUpdateRecIndicator()));
    connect(&d->writer, SIGNAL(started()), this, SLOT(onWriterStarted()));
    connect(&d->writer, SIGNAL(stopped()), this, SLOT(onWriterStopped()));
    connect(&d->writer, SIGNAL(frameWriteFailed()), this, SLOT(onWriterFrameWriteFailed()));
    connect(&d->writer, SIGNAL(recorderStopWarning()), this, SLOT(onRecorderStopWarning()));
    connect(&d->writer, SIGNAL(lowPerformanceWarning()), this, SLOT(onLowPerformanceWarning()));

    d->updateRecordStatus(false);
}

RecorderDockerDock::~RecorderDockerDock()
{
    delete d;
    delete exportSettings;
}

void RecorderDockerDock::setCanvas(KoCanvasBase* canvas)
{
    setEnabled(canvas != nullptr);

    if (d->canvas == canvas)
        return;

    d->canvas = dynamic_cast<KisCanvas2*>(canvas);
    d->writer.setCanvas(d->canvas);

    if (!d->canvas)
        return;

    KisDocument *document = d->canvas->imageView()->document();

    d->prefix = d->getPrefix();
    bool wasToggled = false;
    if (d->recordAutomatically && !d->snapshotDirectory.isEmpty()
        && !d->enabledIds.contains(document->linkedResourcesStorageId()) && d->canRecord()) {
        wasToggled = onRecordButtonToggled(true);
    }
    if (!wasToggled) { // onRecordButtonToggled(true) may call it, don't call it twice.
        d->updateWriterSettings();
    }

    bool enabled = d->enabledIds.value(document->linkedResourcesStorageId(), false);
    d->writer.setEnabled(enabled);
    d->updateRecordStatus(enabled);
}

void RecorderDockerDock::unsetCanvas()
{
    d->updateRecordStatus(false);
    d->recordToggleAction->setChecked(false);
    setEnabled(false);
    d->writer.stop();
    d->writer.setCanvas(nullptr);
    d->canvas = nullptr;
    d->enabledIds.clear();
    d->view->setFrames(QStringList());
}

void RecorderDockerDock::onMainWindowIsBeingCreated(KisMainWindow *window)
{
    KisKActionCollection *actionCollection = window->viewManager()->actionCollection();
    actionCollection->addAction(keyActionRecordToggle, d->recordToggleAction);
    actionCollection->addAction(keyActionExport, d->exportAction);
}

bool RecorderDockerDock::onRecordButtonToggled(bool checked)
{
    QSignalBlocker blocker(d->recordButton);

    // Ask the user to pick a directory if we don't have one. This should only
    // happen on Android, other operating systems have a non-empty default that
    // the user is not able to clear out via the user interface.
    if (checked && d->snapshotDirectory.isEmpty()) {
        slotSelectSnapshotDirectory();
        if (d->snapshotDirectory.isEmpty()) {
            d->recordButton->setChecked(false);
            d->recordToggleAction->setChecked(false);
            return false;
        }
    }

    d->recordToggleAction->setChecked(checked);

    if (!d->canvas)
        return false;

    const QString &id = d->canvas->imageView()->document()->linkedResourcesStorageId();

    bool wasEmpty = !d->enabledIds.values().contains(true);

    d->enabledIds[id] = checked;

    bool isEmpty = !d->enabledIds.values().contains(true);

    d->writer.setEnabled(checked);

    if (isEmpty == wasEmpty) {
        d->updateRecordStatus(checked);
        return false;
    }


    d->recordButton->setEnabled(false);

    if (checked) {
        d->updateWriterSettings();
        d->writer.start();

        // Calculate Rec symbol activity timeout depending on the capture interval
        // The pausedTimer interval is set to a slightly greater value than the capture interval
        // to avoid flickering for ongoing painting. This is also the reason for the min and max
        // values 305 and 2005 (instead of 300 and 2000, respectively).
        if (d->realTimeCaptureMode) {
            d->pausedTimer.setInterval(qBound(305, static_cast<int>(1000.0/static_cast<double>(exportSettings->fps)) + 5,2005));
        } else {
            d->pausedTimer.setInterval(qBound(305, static_cast<int>(qMax(d->captureInterval, .1) * 1000.0) + 5, 2005));
        }
    } else {
        d->writer.stop();
        d->warningTimer.stop();
        d->pausedTimer.stop();
        d->statusBarWarningLabel->hide();
        d->paused = true;
    }

    return true;
}

void RecorderDockerDock::onExportButtonClicked()
{
    if (!d->canvas)
        return;
#ifdef Q_OS_ANDROID
    onExportOptionsClicked();
#else
    if (!d->exportedFile.isEmpty()) {
        // after saving, the button shows the video
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(d->exportedFile).absolutePath()));
        return;
    }
    if (!d->exporter) d->exportVideo();
#endif
}

void RecorderDockerDock::onExportOptionsClicked()
{
    if (!d->canvas)
        return;

    KisDocument *document = d->canvas->imageView()->document();

#ifndef Q_OS_ANDROID
    exportSettings->videoFileName = QFileInfo(document->caption().trimmed()).completeBaseName();
#endif
    exportSettings->inputDirectory = d->outputDirectory;
    exportSettings->format = d->format;
    exportSettings->realTimeCaptureMode = d->realTimeCaptureMode;

    RecorderExport exportDialog(exportSettings, this);
    exportDialog.setup();
    exportDialog.exec();
    d->refreshFrames();
}

void RecorderDockerDock::slotSelectSnapshotDirectory()
{
    KoFileDialog dialog(this, KoFileDialog::OpenDirectory, "SelectRecordingsDirectory");
    dialog.setCaption(i18n("Select a Directory for Recordings"));
    dialog.setDefaultDir(d->snapshotDirectory);
    QString directory = dialog.filename();
    if (!directory.isEmpty()) {
        RecorderConfig(false).setSnapshotDirectory(directory);
        d->loadSettings();
        d->updateWriterSettings();
        d->refreshFrames();
    }
}

void RecorderDockerDock::onWriterStarted()
{
    d->updateRecordStatus(true);
}

void RecorderDockerDock::onWriterStopped()
{
    d->updateRecordStatus(false);
}

void RecorderDockerDock::onUpdateRecIndicator()
{
    d->updateRecIndicator();
}

void RecorderDockerDock::onActiveRecording(bool valueWasIncreased)
{
    if (!valueWasIncreased)
        return;

    d->paused = false;
    d->pausedTimer.start();
}

void RecorderDockerDock::onPausedTimeout()
{
    d->paused = true;
    d->updateRecIndicator();
}

void RecorderDockerDock::onWriterFrameWriteFailed()
{
    QMessageBox::warning(this, i18nc("@title:window", "Time-lapse"),
                         i18n("The recorder has been stopped due to failure while writing a frame. Please check free disk space and start the recorder again."));
}

void RecorderDockerDock::onRecorderStopWarning()
{
    QMessageBox::warning(this, i18nc("@title:window", "Time-lapse"),
                         i18n("The recorder could not be stopped. Please restart the application."));
}
void RecorderDockerDock::onLowPerformanceWarning()
{
    if (d->realTimeCaptureMode) {
        d->showWarning(i18n("Low performance warning. The recorder is not able to write all the frames in time during Real Time Capture mode.\nTry to reduce the frame rate for the ffmpeg export or reduce the scaling filtering in the canvas acceleration settings."));
    } else {
        d->showWarning(i18n("Low performance warning. The recorder is not able to write all the frames in time.\nTry to increase the capture interval or reduce the scaling filtering in the canvas acceleration settings."));
    }
}

void RecorderDockerDock::onWarningTimeout()
{
    d->statusBarWarningLabel->hide();
}

void RecorderDockerDock::onExporterStarted()
{
}

void RecorderDockerDock::onExporterFinished()
{
#ifndef Q_OS_ANDROID
    d->exportedFile = exportSettings->videoFilePath;
    d->finishExport();
    d->exportButton->setText(i18nc("show the saved time-lapse video in its folder", "Show"));
    d->exportButton->setToolTip(QFileInfo(d->exportedFile).fileName());
    d->showTimer.start();
    if (d->canvas) {
        d->canvas->viewManager()->showFloatingMessage(i18nc("@info:status", "Saved %1", QFileInfo(d->exportedFile).fileName()),
                                                      KisIconUtils::loadIcon("document-save"));
    }
#endif
}

void RecorderDockerDock::onExporterFinishedWithError(QString error)
{
#ifndef Q_OS_ANDROID
    d->finishExport();
    d->exportButton->setText(i18nc("Save the time-lapse video", "Save"));
    QMessageBox::critical(this, i18nc("@title:window", "Time-lapse"), i18n("The video could not be saved:") % "\n\n" % error);
#else
    Q_UNUSED(error);
#endif
}

void RecorderDockerDock::onExporterProgressUpdated(int frameNo)
{
    const qreal frames = exportSettings->framesCount * exportSettings->fps / qMax<qreal>(1, exportSettings->inputFps);
    d->exportButton->setProgress(qBound<qreal>(0.01, frameNo / qMax<qreal>(1, frames), 1));
}

#ifdef Q_OS_ANDROID
void RecorderDockerDock::moveFilesFromInternalSnapshotDirectory()
{
    if (!d->internalMoveInProgress) {
        const QString &internalPath = RecorderConfig::defaultInternalSnapshotDirectory();
        if (!d->snapshotDirectory.isEmpty() && QFileInfo::exists(internalPath)) {
            // The user has picked a directory to record to, but the nonsense
            // internal directory is present and may have stuff inside that
            // would become effectively inaccessible. To fix that, we move the
            // files over to the selected directory. Of course moving files on
            // Android is gobsmackingly slow, so we'll have to do it in the
            // background to not lock the UI for ages. The moving should be
            // re-entrant, so getting interrupted and continuing later is fine.
            qWarning().nospace() << "Moving recordings stuck in internal directory '" << internalPath
                                 << "' to selected directory '" << d->snapshotDirectory << "'";
            RecorderDockerInternalSnapshotsMover *mover =
                new RecorderDockerInternalSnapshotsMover(internalPath, d->snapshotDirectory);
            connect(mover,
                    &RecorderDockerInternalSnapshotsMover::sigMoveFinished,
                    this,
                    &RecorderDockerDock::slotInternalSnapshotMoveFinished,
                    Qt::QueuedConnection);
            d->internalMoveInProgress = true;
            QThreadPool::globalInstance()->start(mover);
        }
    }
}

void RecorderDockerDock::slotInternalSnapshotMoveFinished(const QString &srcRoot)
{
    d->internalMoveInProgress = false;
    if (srcRoot != d->snapshotDirectory) {
        // Directory changed meanwhile, trigger another move.
        moveFilesFromInternalSnapshotDirectory();
    }
}

RecorderDockerInternalSnapshotsMover::RecorderDockerInternalSnapshotsMover(const QString &srcRoot,
                                                                           const QString &dstRoot)
    : m_srcRoot(srcRoot)
    , m_dstRoot(dstRoot)
{
}

void RecorderDockerInternalSnapshotsMover::run()
{
    moveFromInternalSnapshotDirectory(QDir(m_srcRoot), QDir(m_dstRoot));
    if (!QDir().rmdir(m_srcRoot)) {
        qWarning().nospace() << "Failed to remove root directory '" << m_srcRoot << "'";
    }
    Q_EMIT sigMoveFinished(m_srcRoot);
}

void RecorderDockerInternalSnapshotsMover::moveFromInternalSnapshotDirectory(const QDir &src, const QDir &dst)
{
    for (const QFileInfo &srcInfo : src.entryInfoList(FILTERS)) {
        QString srcName = srcInfo.fileName();
        QString dstPath = dst.filePath(srcName);

        if (srcInfo.isDir()) {
            // Move the directory over recursively.
            if (dst.mkpath(dstPath)) {
                moveFromInternalSnapshotDirectory(QDir(srcInfo.filePath()), QDir(dstPath));
            } else {
                qWarning().nospace() << "Failed to create directory '" << dstPath << "' in '" << dst.path() << "'";
            }

            // Removal will fail if the directory is non-empty, so we
            // can just attempt it unconditionally.
            if (!src.rmdir(srcName)) {
                qWarning().nospace() << "Failed to remove directory '" << srcName << "' in '" << src.path() << "'";
            }

        } else {
            QFile srcFile(srcInfo.filePath());
            // Rename refuses to replace files in the destination, so
            // try to remove that first. The only reason it should
            // already exist is if a previous attempt to move the file
            // partially copied it and then got interrupted.
            QFile::remove(dstPath);
            if (!srcFile.rename(dstPath)) {
                qWarning().nospace() << "Error " << srcFile.error() << " moving '" << srcFile.fileName() << "' to '"
                                     << dstPath << "': " << srcFile.errorString();
            }
        }
    }
}
#endif
