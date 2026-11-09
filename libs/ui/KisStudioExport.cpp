/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "KisStudioExport.h"

#include <QBuffer>
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
#include <KoDocumentInfo.h>
#include <KisImportExportFilter.h>
#include <KisImportExportManager.h>
#include <KisMainWindow.h>
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

} // namespace

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
    QTimer measureLater;
};

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
    d->whole = image->projection()->convertToQImage(nullptr, image->bounds()).convertToFormat(QImage::Format_ARGB32);
    // sized as the files are written
    KisHiddenColors::clear(d->whole);

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
    for (int i = 0; i < count; i++) {
        if (formats()[i].mime == cfg.readEntry<QString>(formatKey, QString())) d->current = i;
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
    const QPoint middle = KisEffectPanels::busiestPoint(image->projection(), image->bounds());
    QRect partRect(middle - QPoint(partSize.width() / 2, partSize.height() / 2), partSize);
    partRect.moveLeft(qBound(0, partRect.left(), qMax(0, d->whole.width() - partRect.width())));
    partRect.moveTop(qBound(0, partRect.top(), qMax(0, d->whole.height() - partRect.height())));
    d->part = d->whole.copy(partRect & d->whole.rect());

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
    for (int i = 0; i < count; i++) measure(i);
}

KisStudioExport::~KisStudioExport()
{
}

void KisStudioExport::refresh()
{
    QVector<KisStudioChips::Chip> chips;
    for (int i = 0; i < formats().size(); i++) {
        const Format &format = formats()[i];
        if (d->pictures[i].isNull()) {
            d->pictures[i] = canEncode(format) ? QImage::fromData(encoded(d->part, format, d->quality[i])) : d->part;
            if (d->pictures[i].isNull()) d->pictures[i] = d->part;
        }
        KisStudioChips::Chip chip;
        chip.text = d->sizes[i] < 0 ? format.name
                                    : i18nc("export format and the size of its file", "%1 · %2", format.name,
                                            QLocale().formattedDataSize(d->sizes[i], 1, QLocale::DataSizeTraditionalFormat));
        chip.picture = KisEffectPanels::tilePicture(d->pictures[i], false, true);
        chip.pictureKey = d->pictures[i].cacheKey();
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
    if (!canEncode(format)) return;
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
    QPointer<KisMainWindow> window = d->window;
    QPointer<KisDocument> document = d->document;
    close();
    // asked where once the sheet is gone: the file dialog is a window of its own
    QTimer::singleShot(0, window.data(), [window, document, format, quality]() {
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
        KisPropertiesConfigurationSP settings = filter ? filter->lastSavedConfiguration("application/x-krita", format.mime.toLatin1())
                                                       : KisPropertiesConfigurationSP(new KisPropertiesConfiguration());
        if (format.lossy) {
            if (format.mime == QLatin1String("image/webp")) {
                settings->setProperty("quality", double(quality));
                settings->setProperty("lossless", quality >= 100);
            } else {
                settings->setProperty("quality", quality);
            }
        }
        settings->setProperty(chosenTag, true);
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
