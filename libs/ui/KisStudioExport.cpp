/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "KisStudioExport.h"

#include <QBuffer>
#include <QCheckBox>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QImageWriter>
#include <QKeyEvent>
#include <QLabel>
#include <QLocale>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QStandardPaths>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent>

#include <klocalizedstring.h>

#include <KisDocument.h>
#include <KoColorModelStandardIds.h>
#include <KoColorSpace.h>
#include <KoColorSpaceRegistry.h>
#include <KoDocumentInfo.h>
#include <KisImportExportFilter.h>
#include <KisImportExportManager.h>
#include <KisMainWindow.h>
#include <KisPart.h>
#include <KisViewManager.h>
#include <kis_floating_message.h>
#include <KisModernStyle.h>
#include <KisView.h>
#include <KoFileDialog.h>
#include <kis_config.h>
#include <kis_image.h>
#include <kis_paint_device.h>

#include "KisEffectPanels.h"
#include "KisHiddenColors.h"
#include "KisSettingsRegistry.h"
#include "KisStudioLayout.h"
#include "widgets/KisStudioChips.h"
#include "widgets/KisStudioValueRow.h"

namespace
{

struct Format {
    QString name;
    QString mime;
    const char *qtFormat;
    /// Loses some of the picture, by a quality
    bool lossy;
    int defaultQuality;
};

const QVector<Format> &formats()
{
    static const QVector<Format> list = {
        {QStringLiteral("PNG"), QStringLiteral("image/png"), "png", false, 100},
        {QStringLiteral("JPEG"), QStringLiteral("image/jpeg"), "jpg", true, 80},
        {QStringLiteral("WebP"), QStringLiteral("image/webp"), "webp", true, 75},
    };
    return list;
}

const QString formatKey = QStringLiteral("StudioExportFormat");
const QString folderKey = QStringLiteral("StudioExportFolder");

/// Set on the settings the sheet chose, so Krita's export asks nothing more
const QString chosenTag = QStringLiteral("skissaChosen");

/// While set, the export action runs Krita's own export
bool classic = false;

/// \p image as \p format would save it at \p quality (JPEG has no transparency: it is laid on white, as Krita's
/// export does by default)
QByteArray encoded(const QImage &image, const Format &format, int quality)
{
    QImage source = image;
    if (qstrcmp(format.qtFormat, "jpg") == 0 && image.hasAlphaChannel()) {
        source = QImage(image.size(), QImage::Format_RGB32);
        source.fill(Qt::white);
        QPainter painter(&source);
        painter.drawImage(0, 0, image);
    }
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    QImageWriter writer(&buffer, format.qtFormat);
    if (format.lossy) writer.setQuality(quality);
    writer.write(source);
    return bytes;
}

bool canEncode(const Format &format)
{
    return QImageWriter::supportedImageFormats().contains(QByteArray(format.qtFormat));
}

/**
 * An image exported again each time it is saved: the image, the exported file, its format and settings, and the
 * exported file's time and size as the last export left them. A file changed since by anything else is left alone.
 */
struct Link {
    QString document;
    QString target;
    QString mime;
    QString settings;
    qint64 time {0};
    qint64 size {-1};
};

const QString linksKey = QStringLiteral("StudioExportOnSave");

QVector<Link> links()
{
    QVector<Link> all;
    Q_FOREACH (const QString &line, KisConfig(true).readEntry<QStringList>(linksKey, QStringList())) {
        const QStringList parts = line.split(QLatin1Char('\t'));
        if (parts.size() < 6) continue;
        all.append({parts[0], parts[1], parts[2], QString::fromUtf8(QByteArray::fromBase64(parts[3].toLatin1())),
                    parts[4].toLongLong(), parts[5].toLongLong()});
    }
    return all;
}

void setLinks(const QVector<Link> &all)
{
    QStringList lines;
    for (const Link &link : all) {
        lines << QStringList{link.document, link.target, link.mime, QString::fromLatin1(link.settings.toUtf8().toBase64()),
                             QString::number(link.time), QString::number(link.size)}
                     .join(QLatin1Char('\t'));
    }
    KisSettingsRegistry::writeLater(linksKey, lines);
}

/// The same file however its path is written
bool sameFile(const QString &a, const QString &b)
{
    if (a.isEmpty() || b.isEmpty()) return false;
#ifdef Q_OS_WIN
    const Qt::CaseSensitivity sensitivity = Qt::CaseInsensitive;
#else
    const Qt::CaseSensitivity sensitivity = Qt::CaseSensitive;
#endif
    return QDir::cleanPath(QFileInfo(a).absoluteFilePath()).compare(QDir::cleanPath(QFileInfo(b).absoluteFilePath()), sensitivity) == 0;
}

int linkOf(const QVector<Link> &all, const QString &document)
{
    for (int i = 0; i < all.size(); i++) {
        if (sameFile(all[i].document, document)) return i;
    }
    return -1;
}

void tell(KisDocument *document, const QString &message)
{
    KisMainWindow *window = KisPart::instance()->currentMainwindow();
    if (window && window->viewManager() && window->activeView() && window->activeView()->document() == document) {
        window->viewManager()->showFloatingMessage(message, QIcon(), 2500, KisFloatingMessage::Low);
    }
}

/// Exports \p link's file again; a save still finishing holds the image a moment, so it tries again
void exportAgain(QPointer<KisDocument> document, const Link &link, int tries = 10)
{
    if (!document) return;
    KisPropertiesConfigurationSP settings = new KisPropertiesConfiguration();
    settings->fromXML(link.settings);
    settings->setProperty(chosenTag, true);
    if (document->exportDocument(link.target, link.mime.toLatin1(), false, false, settings)) {
        tell(document, i18nc("@info floating message, %1 a file name", "Exported %1 too", QFileInfo(link.target).fileName()));
    } else if (tries > 0) {
        QTimer::singleShot(300, document.data(), [document, link, tries]() { exportAgain(document, link, tries - 1); });
    }
}

/// After a save, its linked export; after that export, the file as it left it
void savingFinished(KisDocument *document, const QString &path)
{
    QVector<Link> all = links();
    for (int i = 0; i < all.size(); i++) {
        Link &link = all[i];
        if (sameFile(path, link.target)) {
            const QFileInfo file(link.target);
            link.time = file.lastModified().toMSecsSinceEpoch();
            link.size = file.size();
            setLinks(all);
            return;
        }
    }
    const int index = linkOf(all, document->path());
    if (index < 0 || !sameFile(path, document->path())) return;
    const Link link = all[index];
    const QFileInfo file(link.target);
    if (file.exists() && (file.lastModified().toMSecsSinceEpoch() != link.time || file.size() != link.size)) {
        // changed by something else since: never written over; exporting again is the user's to ask
        all.remove(index);
        setLinks(all);
        tell(document, i18nc("@info floating message, %1 a file name", "%1 was changed elsewhere, so it wasn't exported again", file.fileName()));
        return;
    }
    exportAgain(document, link);
}

void watch(KisDocument *document)
{
    if (!document || document->property("skissaExportWatched").toBool()) return;
    document->setProperty("skissaExportWatched", true);
    QObject::connect(document, &KisDocument::sigSavingFinished, document, [document](const QString &path) { savingFinished(document, path); });
}

} // namespace

void KisStudioExport::watchSaves()
{
    static bool watching = false;
    if (watching) return;
    watching = true;
    Q_FOREACH (QPointer<KisDocument> document, KisPart::instance()->documents()) watch(document);
    QObject::connect(KisPart::instance(), &KisPart::sigDocumentAdded, KisPart::instance(), &watch);
}

struct KisStudioExport::Private {
    QPointer<KisMainWindow> window;
    QPointer<KisDocument> document;
    /// The image as it is seen, 8-bit sRGB
    QImage whole;
    /// A busy part of it at its own size, as each tile shows it
    QImage part;
    int current {0};
    QVector<int> quality;
    QVector<qint64> sizes;
    QVector<int> generation;
    QVector<QImage> pictures;
    KisStudioChips *tiles {nullptr};
    KisStudioValueRow *qualityRow {nullptr};
    QCheckBox *onSave {nullptr};
    QTimer measureLater;
    /// The lossless file measured at the image's own depth (16-bit PNG for a deeper image): not measured again
    bool losslessMeasured {false};
};

namespace
{

/// What the sheet shows, made off the window's thread
struct Prepared {
    QImage whole;
    QImage part;
    /// The PNG's size, when the image is deeper than 8 bits; -1 otherwise
    qint64 deepPngSize {-1};
};

/// \p device (the image's picture) as a 16-bit sRGB PNG would hold it, erased colors cleared
QImage deepImage(KisPaintDeviceSP device, const QRect &bounds)
{
    KisPaintDeviceSP copy = new KisPaintDevice(*device);
    KoColorSpaceRegistry *registry = KoColorSpaceRegistry::instance();
    copy->convertTo(registry->rgb16(registry->p709SRGBProfile()));
    QImage deep(bounds.size(), QImage::Format_RGBA64);
    QVector<quint16> row(bounds.width() * 4);
    for (int y = 0; y < bounds.height(); y++) {
        copy->readBytes(reinterpret_cast<quint8 *>(row.data()), bounds.left(), bounds.top() + y, bounds.width(), 1);
        QRgba64 *line = reinterpret_cast<QRgba64 *>(deep.scanLine(y));
        for (int x = 0; x < bounds.width(); x++) {
            // Krita keeps blue first
            const quint16 *pixel = row.constData() + 4 * x;
            line[x] = pixel[3] ? QRgba64::fromRgba64(pixel[2], pixel[1], pixel[0], pixel[3]) : QRgba64::fromRgba64(0);
        }
    }
    return deep;
}

Prepared prepare(KisPaintDeviceSP device, const QRect &bounds, const QSize &partSize, bool deeper)
{
    Prepared prepared;
    prepared.whole = device->convertToQImage(nullptr, bounds).convertToFormat(QImage::Format_ARGB32);
    // sized as the files are written
    KisHiddenColors::clear(prepared.whole);
    const QPoint middle = KisEffectPanels::busiestPoint(device, bounds);
    QRect partRect(middle - bounds.topLeft() - QPoint(partSize.width() / 2, partSize.height() / 2), partSize);
    partRect.moveLeft(qBound(0, partRect.left(), qMax(0, prepared.whole.width() - partRect.width())));
    partRect.moveTop(qBound(0, partRect.top(), qMax(0, prepared.whole.height() - partRect.height())));
    prepared.part = prepared.whole.copy(partRect & prepared.whole.rect());
    if (deeper) prepared.deepPngSize = encoded(deepImage(device, bounds), formats().first(), 100).size();
    return prepared;
}

} // namespace

bool KisStudioExport::open(KisMainWindow *window)
{
    if (classic || !window || !KisStudioLayout::isEnabledInConfig()) return false;
    KisView *view = window->activeView();
    if (!view || !view->document() || !view->document()->image()) return false;
    auto *sheet = new KisStudioExport(window);
    sheet->adjustSize();
    const QRect frame = window->geometry();
    sheet->popupAt(frame.center() - QPoint(sheet->width() / 2, sheet->height() / 2));
    return true;
}

KisStudioExport::KisStudioExport(KisMainWindow *window)
    : KisStudioPopup(window)
    , d(new Private)
{
    setAttribute(Qt::WA_DeleteOnClose);
    d->window = window;
    d->document = window->activeView()->document();
    KisImageSP image = d->document->image();
    image->waitForDone();

    const int count = formats().size();
    d->sizes.fill(-1, count);
    d->generation.fill(0, count);
    d->pictures.resize(count);
    KisConfig cfg(true);
    for (int i = 0; i < count; i++) {
        // the quality last exported with, as Krita's export keeps it
        const KisPropertiesConfigurationSP last = cfg.exportConfiguration(formats()[i].mime);
        d->quality << qBound(1, qRound(last ? last->getDouble("quality", formats()[i].defaultQuality) : formats()[i].defaultQuality), 100);
    }
    // an image exported on each save opens on the format it is exported as
    const QVector<Link> all = links();
    const int linked = d->document->path().isEmpty() ? -1 : linkOf(all, d->document->path());
    const QString chosen = linked >= 0 ? all[linked].mime : cfg.readEntry<QString>(formatKey, QString());
    for (int i = 0; i < count; i++) {
        if (formats()[i].mime == chosen) d->current = i;
    }

    auto *layout = new QVBoxLayout(this);
    fitLayout(layout, KisModernStyle::space3);
    layout->setSpacing(KisModernStyle::space2 + KisModernStyle::hairline);

    auto *title = new QLabel(i18nc("@title the export sheet", "Export"), this);
    title->setFont(KisModernStyle::textFont(font(), 1.0, QFont::DemiBold));
    layout->addWidget(title);

    // a busy part, each pixel shown twice as wide as the screen's, where what a format loses shows
    const QSize tile(156, qMax(124, 2 * fontMetrics().height() + 76));
    setMinimumWidth(count * tile.width() + 4 * KisModernStyle::space3 + 2 * shadowMargin());
    const qreal dpr = devicePixelRatioF();
    const QSize partSize = (QSizeF(tile.width(), tile.height() - fontMetrics().height() - KisModernStyle::space2) * dpr / 2).toSize();
    // the picture read and its busy part found off the window's thread, from the image as it is now
    {
        const KisPaintDeviceSP snapshot = KisEffectPanels::fullSizeCopy(image->projection());
        const QRect bounds = image->bounds();
        const bool deeper = image->colorSpace()->colorDepthId() != Integer8BitsColorDepthID;
        auto *watcher = new QFutureWatcher<Prepared>(this);
        connect(watcher, &QFutureWatcher<Prepared>::finished, this, [this, watcher]() {
            watcher->deleteLater();
            const Prepared prepared = watcher->result();
            d->whole = prepared.whole;
            d->part = prepared.part;
            if (prepared.deepPngSize >= 0) {
                d->sizes[0] = prepared.deepPngSize;
                d->losslessMeasured = true;
            }
            d->pictures.fill(QImage());
            refresh();
            for (int i = 0; i < formats().size(); i++) measure(i);
        });
        watcher->setFuture(QtConcurrent::run([snapshot, bounds, partSize, deeper]() { return prepare(snapshot, bounds, partSize, deeper); }));
    }

    d->tiles = new KisStudioChips(this);
    d->tiles->setTileColumns(count, tile.height());
    layout->addWidget(d->tiles);
    connect(d->tiles, &KisStudioChips::clicked, this, [this](int index) {
        d->current = index;
        KisSettingsRegistry::writeLater(formatKey, formats()[index].mime);
        refresh();
    });

    d->qualityRow = new KisStudioValueRow(i18nc("export: how much of the picture a lossy format keeps", "Quality"), this);
    KisStudioValueRow::Range range;
    range.minimum = 1;
    range.maximum = 100;
    range.suffix = QStringLiteral("%");
    d->qualityRow->setRange(range);
    d->qualityRow->setClickSetsValue(true);
    layout->addWidget(d->qualityRow);
    connect(d->qualityRow, &KisStudioValueRow::valueChanged, this, [this](qreal value) {
        const int index = d->current;
        d->quality[index] = qBound(1, qRound(value), 100);
        d->pictures[index] = QImage();
        d->sizes[index] = -1;
        refresh();
        // the file's size once the drag rests: the whole image is written for it
        d->measureLater.start();
    });
    d->measureLater.setSingleShot(true);
    d->measureLater.setInterval(250);
    connect(&d->measureLater, &QTimer::timeout, this, [this]() { measure(d->current); });

    // an image saved as .kra only: the link is kept by its file, and a picture saved as itself is already its export
    d->onSave = new QCheckBox(i18nc("@option:check export sheet", "Export again on each save"), this);
    d->onSave->setFocusPolicy(Qt::NoFocus);
    d->onSave->setChecked(linked >= 0);
    const bool native = !d->document->path().isEmpty() && d->document->mimeType() == QByteArrayLiteral("application/x-krita");
    d->onSave->setVisible(native || linked >= 0);
    if (linked >= 0) d->onSave->setToolTip(all[linked].target);
    connect(d->onSave, &QCheckBox::toggled, this, [this](bool on) {
        if (on) return;
        // off at once; on only once exported, which says where
        QVector<Link> all = links();
        const int index = linkOf(all, d->document->path());
        if (index < 0) return;
        all.remove(index);
        setLinks(all);
    });
    layout->addWidget(d->onSave);

    auto *buttons = new QHBoxLayout();
    buttons->setSpacing(KisModernStyle::space2);
    auto *other = new QPushButton(i18nc("@action:button export in a format not shown", "Other formats…"), this);
    other->setFlat(true);
    other->setFocusPolicy(Qt::NoFocus);
    connect(other, &QPushButton::clicked, this, [this]() {
        QPointer<KisMainWindow> window = d->window;
        close();
        QTimer::singleShot(0, window.data(), [window]() {
            if (!window) return;
            classic = true;
            QMetaObject::invokeMethod(window, "slotExportFile");
            classic = false;
        });
    });
    buttons->addWidget(other);
    buttons->addStretch(1);
    auto *go = new QPushButton(i18nc("@action:button", "Export"), this);
    go->setDefault(true);
    go->setFocusPolicy(Qt::NoFocus);
    connect(go, &QPushButton::clicked, this, &KisStudioExport::exportNow);
    buttons->addWidget(go);
    layout->addLayout(buttons);

    refresh();
}

KisStudioExport::~KisStudioExport()
{
}

void KisStudioExport::refresh()
{
    QVector<KisStudioChips::Chip> chips;
    for (int i = 0; i < formats().size(); i++) {
        const Format &format = formats()[i];
        if (d->pictures[i].isNull() && !d->part.isNull()) {
            d->pictures[i] = canEncode(format) ? QImage::fromData(encoded(d->part, format, d->quality[i])) : d->part;
            if (d->pictures[i].isNull()) d->pictures[i] = d->part;
        }
        KisStudioChips::Chip chip;
        chip.text = d->sizes[i] < 0 ? format.name
                                    : i18nc("export format and the size of its file", "%1 · %2", format.name,
                                            QLocale().formattedDataSize(d->sizes[i], 1, QLocale::DataSizeTraditionalFormat));
        // nothing drawn until the picture has been read
        if (!d->pictures[i].isNull()) {
            chip.picture = KisEffectPanels::tilePicture(d->pictures[i], false, true);
            chip.pictureKey = d->pictures[i].cacheKey();
        }
        chip.on = i == d->current;
        chips << chip;
    }
    d->tiles->setChips(chips);
    const bool lossy = formats()[d->current].lossy;
    d->qualityRow->setVisible(lossy);
    if (lossy) {
        d->qualityRow->setValue(d->quality[d->current]);
        d->qualityRow->setDefaultValue(formats()[d->current].defaultQuality);
    }
    adjustSize();
}

void KisStudioExport::measure(int index)
{
    const Format format = formats()[index];
    if (!canEncode(format) || d->whole.isNull() || (!format.lossy && d->losslessMeasured)) return;
    const int generation = ++d->generation[index];
    auto *watcher = new QFutureWatcher<qint64>(this);
    connect(watcher, &QFutureWatcher<qint64>::finished, this, [this, watcher, index, generation]() {
        watcher->deleteLater();
        if (generation != d->generation[index]) return;
        d->sizes[index] = watcher->result();
        refresh();
    });
    const QImage whole = d->whole;
    const int quality = d->quality[index];
    watcher->setFuture(QtConcurrent::run([whole, format, quality]() { return qint64(encoded(whole, format, quality).size()); }));
}

void KisStudioExport::exportNow()
{
    const Format format = formats()[d->current];
    const int quality = d->quality[d->current];
    const bool onSave = d->onSave->isChecked() && !d->onSave->isHidden();
    QPointer<KisMainWindow> window = d->window;
    QPointer<KisDocument> document = d->document;
    close();
    // asked where once the sheet is gone: the file dialog is a window of its own
    QTimer::singleShot(0, window.data(), [window, document, format, quality, onSave]() {
        if (!window || !document) return;
        KisConfig cfg(true);
        QString folder = cfg.readEntry<QString>(folderKey, QString());
        if (folder.isEmpty()) folder = document->path().isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::PicturesLocation)
                                                                    : QFileInfo(document->path()).absolutePath();
        QString name = document->path().isEmpty() ? document->documentInfo()->aboutInfo(QStringLiteral("title"))
                                                  : QFileInfo(document->path()).completeBaseName();
        if (name.isEmpty()) name = i18nc("the name of an image never saved", "Untitled");
        KoFileDialog dialog(window, KoFileDialog::SaveFile, QStringLiteral("SkissaExport"));
        dialog.setCaption(i18nc("@title:window", "Export"));
        dialog.setDefaultDir(folder + QLatin1Char('/') + name + QLatin1Char('.') + QString::fromLatin1(format.qtFormat), true);
        dialog.setMimeTypeFilters({format.mime}, format.mime);
        const QString path = dialog.filename();
        if (path.isEmpty()) return;
        KisSettingsRegistry::writeLater(folderKey, QFileInfo(path).absolutePath());

        QScopedPointer<KisImportExportFilter> filter(KisImportExportManager::filterForMimeType(format.mime, KisImportExportManager::Export));
        // Krita's defaults, which the pictures show, not options last set in Krita's own dialog and not shown here
        KisPropertiesConfigurationSP settings = filter ? filter->defaultConfiguration("application/x-krita", format.mime.toLatin1())
                                                       : KisPropertiesConfigurationSP();
        if (!settings) settings = new KisPropertiesConfiguration();
        if (format.lossy) {
            if (format.mime == QLatin1String("image/webp")) {
                settings->setProperty("quality", double(quality));
                settings->setProperty("lossless", quality >= 100);
            } else {
                settings->setProperty("quality", quality);
            }
        }
        settings->setProperty(chosenTag, true);
        QVector<Link> all = links();
        const int index = linkOf(all, document->path());
        if (index >= 0) all.remove(index);
        if (onSave) {
            // its time and size are taken once the export is written
            all.append({document->path(), path, format.mime, settings->toXML(), 0, -1});
        }
        setLinks(all);
        document->exportDocument(path, format.mime.toLatin1(), false, false, settings);
    });
}

void KisStudioExport::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        exportNow();
        return;
    }
    KisStudioPopup::keyPressEvent(event);
}
