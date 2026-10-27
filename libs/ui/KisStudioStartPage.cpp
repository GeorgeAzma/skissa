/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioStartPage.h"

#include <QAbstractButton>
#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QElapsedTimer>
#include <QDragEnterEvent>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHash>
#include <QImageReader>
#include <QLocale>
#include <QMdiArea>
#include <QMdiSubWindow>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QStandardItemModel>
#include <QThreadPool>
#include <QDesktopServices>
#include <QUrl>
#include <QVBoxLayout>
#include <QVariantAnimation>
#include <QtConcurrentRun>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <numeric>

#include <kactioncollection.h>
#include <klocalizedstring.h>

#include <KisMimeDatabase.h>
#include <KisModernStyle.h>
#include <KoColor.h>
#include <KoColorProfile.h>
#include <KoColorSpaceRegistry.h>
#include <KoStore.h>
#include <kis_icon_utils.h>
#include <kis_image.h>
#include <kis_paint_device.h>

#include "KisDocument.h"
#include "KisMainWindow.h"
#include "KisPart.h"
#include "KisView.h"
#include <KisProfile.h>
#include "kis_clipboard.h"
#include "kis_config.h"
#include "utils/KisRecentDocumentsModelWrapper.h"
#include "widgets/KisStudioChips.h"
#include "widgets/KisStudioInlineEdit.h"
#include "widgets/KisStudioValueRow.h"

namespace
{

/// The new image settings Krita's own dialog keeps for a custom image
const QLatin1String newImageSection("");

QColor withAlpha(QColor color, qreal alpha)
{
    color.setAlphaF(alpha);
    return color;
}

/// One box blur pass of \p radius pixels either way over a premultiplied image, along its rows or columns
void boxPass(QImage &image, int radius, bool rows)
{
    const int lines = rows ? image.height() : image.width();
    const int length = rows ? image.width() : image.height();
    QVector<QRgb> line(length);
    auto at = [&image, rows](int l, int i) -> QRgb & {
        return rows ? reinterpret_cast<QRgb *>(image.scanLine(l))[i] : reinterpret_cast<QRgb *>(image.scanLine(i))[l];
    };
    const int count = 2 * radius + 1;
    for (int l = 0; l < lines; l++) {
        for (int i = 0; i < length; i++) line[i] = at(l, i);
        int sum[4] = {0, 0, 0, 0};
        auto add = [&sum, &line, length](int i, int sign) {
            const QRgb c = line[qBound(0, i, length - 1)];
            sum[0] += sign * qRed(c);
            sum[1] += sign * qGreen(c);
            sum[2] += sign * qBlue(c);
            sum[3] += sign * qAlpha(c);
        };
        for (int i = -radius; i <= radius; i++) add(i, 1);
        for (int i = 0; i < length; i++) {
            at(l, i) = qRgba(sum[0] / count, sum[1] / count, sum[2] / count, sum[3] / count);
            add(i - radius, -1);
            add(i + radius + 1, 1);
        }
    }
}

/// \p image blurred by three box passes, close to a gaussian
QImage blurred(const QImage &image, int radius)
{
    QImage result = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    for (int pass = 0; pass < 3; pass++) {
        boxPass(result, radius, true);
        boxPass(result, radius, false);
    }
    return result;
}

/// The picture in \p path, at most \p longest pixels along its longer side; null if it can't be read
QImage readPicture(const QString &path, int longest)
{
    QImage image;
    const QString mime = KisMimeDatabase::mimeTypeForFile(path);
    if (mime == QLatin1String(KisDocument::nativeFormatMimeType()) || mime == QLatin1String("application/x-krita-archive")
        || mime == QLatin1String("image/openraster")) {
        QScopedPointer<KoStore> store(KoStore::createStore(path, KoStore::Read));
        if (store) {
            // the merged image is the whole picture; the thumbnail is a small one
            for (const QString &name : {QStringLiteral("mergedimage.png"), QStringLiteral("Thumbnails/thumbnail.png"), QStringLiteral("preview.png")}) {
                if (!store->hasFile(name) || !store->open(name)) continue;
                const QByteArray bytes = store->read(store->size());
                store->close();
                if (image.loadFromData(bytes)) break;
            }
        }
    } else {
        QImageReader reader(path);
        reader.setAutoTransform(true);
        const QSize size = reader.size();
        if (size.isValid() && qMax(size.width(), size.height()) > longest) {
            reader.setScaledSize(size.scaled(longest, longest, Qt::KeepAspectRatio));
        }
        image = reader.read();
        if (image.isNull()) {
            // what Qt can't read (TIFF, PSD, JPEG XL...), through Krita
            QScopedPointer<KisDocument> doc(KisPart::instance()->createTemporaryDocument());
            doc->setFileBatchMode(true);
            if (doc->openPath(path, KisDocument::DontAddToRecent)) {
                KisPaintDeviceSP projection = doc->image()->projection();
                const QRect bounds = projection->exactBounds();
                QSize scaled = bounds.size();
                if (qMax(scaled.width(), scaled.height()) > longest) scaled.scale(longest, longest, Qt::KeepAspectRatio);
                if (!scaled.isEmpty()) image = projection->createThumbnailUncached(scaled.width(), scaled.height(), bounds);
            }
        }
    }
    if (!image.isNull() && qMax(image.width(), image.height()) > longest) {
        image = image.scaled(longest, longest, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    return image;
}

/// The recent images' pictures, read on two threads and kept until their files change
class Pictures : public QObject
{
public:
    static Pictures *instance()
    {
        static Pictures *pictures = new Pictures(qApp);
        return pictures;
    }

    /// Calls \p loaded, while \p viewer lives, whenever a picture has been read
    void watch(QObject *viewer, std::function<void()> loaded) { m_viewers.append({QPointer<QObject>(viewer), loaded}); }

    /// The picture of \p path at most \p longest pixels long; null while it is being read, or if it can't be
    QImage get(const QString &path, int longest)
    {
        const QString key = path + QLatin1Char('|') + QString::number(longest);
        const QDateTime time = modified(path);
        const auto found = m_pictures.constFind(key);
        if ((found == m_pictures.constEnd() || found->time != time) && !m_reading.contains(key)) {
            m_reading.insert(key);
            auto *watcher = new QFutureWatcher<QImage>(this);
            connect(watcher, &QFutureWatcher<QImage>::finished, this, [this, watcher, key, time]() {
                m_reading.remove(key);
                m_pictures.insert(key, {watcher->result(), time});
                // the pictures asked for longest ago go: files dropped from the recent list aren't asked for again
                m_used.removeOne(key);
                m_used.append(key);
                while (m_used.size() > keptPictures) m_pictures.remove(m_used.takeFirst());
                watcher->deleteLater();
                for (int i = m_viewers.size() - 1; i >= 0; i--) {
                    if (m_viewers[i].first) m_viewers[i].second();
                    else m_viewers.removeAt(i);
                }
            });
            watcher->setFuture(QtConcurrent::run(&m_pool, readPicture, path, longest));
        }
        // a changed file shows its old picture until the new one is read
        if (found == m_pictures.constEnd()) return QImage();
        if (m_used.isEmpty() || m_used.last() != key) {
            m_used.removeOne(key);
            m_used.append(key);
        }
        return found->image;
    }

private:
    explicit Pictures(QObject *parent)
        : QObject(parent)
    {
        m_pool.setMaxThreadCount(2);
        m_clock.start();
        // quitting waits for the pictures being read, not for all those still queued
        connect(qApp, &QCoreApplication::aboutToQuit, this, [this]() { m_pool.clear(); });
    }

    /// When \p path last changed, looked at no more than every two seconds: asked at every paint and layout of
    /// the gallery, and a file on a network drive can take long to answer
    QDateTime modified(const QString &path)
    {
        auto it = m_modified.find(path);
        if (it == m_modified.end() || m_clock.elapsed() - it->second > 2000) {
            const QDateTime time = QFileInfo(path).lastModified();
            if (it == m_modified.end()) it = m_modified.insert(path, {time, m_clock.elapsed()});
            else *it = {time, m_clock.elapsed()};
        }
        return it->first;
    }

    struct Picture {
        QImage image;
        QDateTime time;
    };
    /// About a gallery's worth (at most 0.9 MB each, the large one 4 MB)
    static constexpr int keptPictures = 64;
    QHash<QString, Picture> m_pictures;
    /// Least recently asked for first
    QStringList m_used;
    QHash<QString, QPair<QDateTime, qint64>> m_modified;
    QElapsedTimer m_clock;
    QSet<QString> m_reading;
    QThreadPool m_pool;
    QList<QPair<QPointer<QObject>, std::function<void()>>> m_viewers;
};

/// A size for a new image, drawn as its shape to one scale with the others; or the image on the clipboard, Custom or Open
/// Calls a function each time the widget it watches is shown
class ShowWatch : public QObject
{
public:
    ShowWatch(QWidget *widget, std::function<void()> shown)
        : QObject(widget)
        , m_shown(std::move(shown))
    {
        widget->installEventFilter(this);
    }

protected:
    bool eventFilter(QObject *, QEvent *event) override
    {
        if (event->type() == QEvent::Show) m_shown();
        return false;
    }

private:
    std::function<void()> m_shown;
};

class NewTile : public QAbstractButton
{
public:
    static constexpr int tileWidth = 124;
    static constexpr int tileHeight = 150;
    /// Where the shape is drawn
    static QRectF shapeBox() { return QRectF(KisModernStyle::space3, KisModernStyle::space3, tileWidth - 2 * KisModernStyle::space3, 84); }

    NewTile(const QString &name, const QSize &pixels, QWidget *parent)
        : QAbstractButton(parent)
        , m_name(name)
        , m_pixels(pixels)
    {
        // not as its text, which popups would give a keyboard accelerator
        setAccessibleName(name);
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_Hover);
        setFixedSize(tileWidth, tileHeight);
        // typing on the start page filters the recent images
        setFocusPolicy(Qt::NoFocus);
    }

    /// Shows \p picture of \p pixels in place of a shape (the clipboard's image)
    void setPicture(const QImage &picture, const QSize &pixels)
    {
        m_picture = picture;
        m_pixels = pixels;
        update();
    }

    /// Shows \p icon in place of a shape
    void setGlyph(const QIcon &icon) { m_glyph = icon; }

    /// Logical pixels per image pixel, the same for every tile so the shapes compare
    void setScale(qreal scale) { m_scale = scale; }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        const QColor ink = palette().color(QPalette::WindowText);
        painter.setPen(Qt::NoPen);
        painter.setBrush(withAlpha(ink, isDown() || isChecked() ? 0.14 : underMouse() ? 0.1 : 0.05));
        painter.drawPath(KisModernStyle::squircle(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), KisModernStyle::tileRadius));

        const QRectF box = shapeBox();
        auto shapeRect = [this, &box]() {
            QSizeF size = QSizeF(m_pixels) * m_scale;
            if (size.width() > box.width() || size.height() > box.height() || m_scale <= 0) size.scale(box.size() * 0.86, Qt::KeepAspectRatio);
            return QRectF(box.center() - QPointF(size.width(), size.height()) / 2, size);
        };
        if (!m_picture.isNull()) {
            const QRectF at = shapeRect();
            painter.save();
            painter.setClipPath(KisModernStyle::squircle(at, 3));
            painter.drawImage(at, m_picture);
            painter.restore();
        } else if (!m_glyph.isNull()) {
            const int side = 32;
            const QRect at(box.center().toPoint() - QPoint(side / 2, side / 2), QSize(side, side));
            m_glyph.paint(&painter, at);
        } else if (m_pixels.isValid()) {
            KisConfig cfg(true);
            QColor paper = cfg.defaultBackgroundColor(newImageSection);
            paper.setAlpha(qMax(40, int(cfg.defaultBackgroundOpacity(newImageSection))));
            painter.setBrush(paper);
            painter.setPen(QPen(withAlpha(ink, 0.25), 1));
            painter.drawPath(KisModernStyle::squircle(shapeRect(), 3));
        } else {
            // Custom: an outline to fill in
            painter.setPen(QPen(withAlpha(ink, underMouse() ? 0.6 : 0.4), 1.5, Qt::DashLine));
            painter.setBrush(Qt::NoBrush);
            const QRectF at = QRectF(box.center() - QPointF(30, 30), QSizeF(60, 60));
            painter.drawPath(KisModernStyle::squircle(at, 4));
            painter.setPen(QPen(withAlpha(ink, 0.7), 1.5, Qt::SolidLine, Qt::RoundCap));
            painter.drawLine(at.center() - QPointF(8, 0), at.center() + QPointF(8, 0));
            painter.drawLine(at.center() - QPointF(0, 8), at.center() + QPointF(0, 8));
        }

        // the name, then the size quietly
        painter.setPen(ink);
        painter.setFont(font());
        const QRect nameRect(KisModernStyle::space2, int(box.bottom()) + KisModernStyle::space2, width() - 2 * KisModernStyle::space2, fontMetrics().height());
        painter.drawText(nameRect, Qt::AlignHCenter | Qt::AlignVCenter, fontMetrics().elidedText(m_name, Qt::ElideRight, nameRect.width()));
        if (m_pixels.isValid() && m_glyph.isNull()) {
            painter.setFont(KisModernStyle::textFont(font(), 0.88));
            painter.setPen(KisModernStyle::secondaryInk(ink));
            painter.drawText(nameRect.translated(0, nameRect.height() + 1), Qt::AlignHCenter | Qt::AlignVCenter,
                             i18nc("an image's size in pixels: width × height", "%1 × %2", m_pixels.width(), m_pixels.height()));
        }
    }

private:
    QString m_name;
    QSize m_pixels;
    QImage m_picture;
    QIcon m_glyph;
    qreal m_scale {0};
};

constexpr int maxSide = 16384;

/// A new image as the Custom panel makes it
struct NewImage {
    QSize size;
    qreal ratio {0}; ///< width over height kept, the wide way (≥ 1); 0: free
    qreal dpi {72};
    QString model;
    QString depth;
    QString profile;
    QColor paper;

    /// The kept ratio turned the way the image is; 0: free
    qreal kept() const
    {
        if (ratio <= 0) return 0;
        return size.width() >= size.height() ? ratio : 1 / ratio;
    }
    /// Sets the width, the height following a kept ratio
    void setWidth(int width)
    {
        const qreal r = kept();
        size.setWidth(width);
        if (r > 0) size.setHeight(qMax(1, int(std::round(width / r))));
    }
    /// Sets the height, the width following a kept ratio
    void setHeight(int height)
    {
        const qreal r = kept();
        size.setHeight(height);
        if (r > 0) size.setWidth(qMax(1, int(std::round(height * r))));
    }

    bool paperShaped() const { return qFuzzyCompare(ratio, M_SQRT2); }

    struct Step {
        QString name;
        int longSide;
    };
    /// How big it can be, by its long side: a screen's sizes, or A paper's at the resolution
    QVector<Step> steps() const
    {
        if (paperShaped()) {
            // the ones that fit
            QVector<Step> papers;
            const std::pair<const char *, qreal> sizes[] = {{"A5", 210}, {"A4", 297}, {"A3", 420}, {"A2", 594}};
            for (const auto &paper : sizes) {
                const int longSide = int(std::round(paper.second / 25.4 * dpi));
                if (longSide <= maxSide) papers.append({QString::fromLatin1(paper.first), longSide});
            }
            return papers;
        }
        return {{QStringLiteral("HD"), 1920}, {QStringLiteral("2K"), 2560}, {QStringLiteral("4K"), 3840}, {QStringLiteral("8K"), 7680}};
    }
    /// \p longSide on the longer side, the shorter one by the kept ratio (or as the shape is), turned as it is
    QSize sized(int longSide) const
    {
        const bool wide = size.width() >= size.height();
        const qreal r = ratio > 0 ? ratio : qreal(qMax(size.width(), size.height())) / qMax(1, qMin(size.width(), size.height()));
        const int shortSide = qBound(1, int(std::round(longSide / r)), maxSide);
        return wide ? QSize(longSide, shortSide) : QSize(shortSide, longSide);
    }
    /// The step it is at, -1: none
    int step() const
    {
        const QVector<Step> all = steps();
        for (int i = 0; i < all.size(); i++) {
            if (size == sized(all[i].longSide)) return i;
        }
        return -1;
    }
};

/**
 * The new image as a sheet pulled by its corner (a press anywhere puts the corner there, a side pulls one
 * way), among the other sizes drawn as outlines at the same scale from the same corner. A kept ratio holds
 * while it is pulled.
 */
class SizeStage : public QWidget
{
public:
    struct Outline {
        QString name;
        QSize pixels;
    };
    std::function<void()> changed;
    std::function<void()> made;
    std::function<void()> reset;

    SizeStage(NewImage *image, const QList<Outline> &outlines, const QSize &screen, QWidget *parent)
        : QWidget(parent)
        , m_image(image)
        , m_outlines(outlines)
        , m_screen(screen)
    {
        setMinimumWidth(360);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setMouseTracking(true);
        setToolTip(i18nc("@info:tooltip a new image's size, pulled by its corner", "Drag: change\nShift+drag: free\nDouble-click: make\nRight-click: reset"));
        m_zoom.setEasingCurve(KisModernStyle::motionCurve());
        m_zoom.setDuration(KisModernStyle::changeDuration);
        connect(&m_zoom, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
            m_scale = value.toReal();
            update();
        });
    }

    /// The sizes drawn and caught on
    void setOutlines(const QList<Outline> &outlines)
    {
        bool same = outlines.size() == m_outlines.size();
        for (int i = 0; same && i < outlines.size(); i++) same = outlines[i].name == m_outlines[i].name && outlines[i].pixels == m_outlines[i].pixels;
        if (same) return;
        m_outlines = outlines;
        update();
    }

    /// Zooms to show the size as it is now
    void settle()
    {
        if (m_dragging) return;
        m_zoom.stop();
        m_zoom.setStartValue(m_scale);
        m_zoom.setEndValue(scaleFor(m_image->size));
        m_zoom.start();
        update();
    }

protected:
    void resizeEvent(QResizeEvent *) override
    {
        m_zoom.stop();
        m_scale = scaleFor(m_image->size);
    }

    void paintEvent(QPaintEvent *) override
    {
        if (m_scale <= 0) m_scale = scaleFor(m_image->size);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QColor ink = palette().color(QPalette::WindowText);
        painter.setPen(Qt::NoPen);
        painter.setBrush(withAlpha(ink, 0.05));
        painter.drawPath(KisModernStyle::squircle(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), KisModernStyle::tileRadius));

        // the sheet, and the other sizes over it: in ink around it, in the paper's own ink on it
        const QRectF stage = rect();
        const QRectF sheet = sheetAt(m_image->size);
        const QPainterPath sheetPath = KisModernStyle::squircle(sheet, 3);
        const QColor paper = m_image->paper;
        painter.save();
        painter.setClipPath(KisModernStyle::squircle(stage, KisModernStyle::tileRadius));
        if (paper.alpha() < 255) {
            painter.save();
            painter.setClipPath(sheetPath, Qt::IntersectClip);
            const int cell = 8;
            for (int y = 0; y * cell < sheet.height(); y++) {
                for (int x = 0; x * cell < sheet.width(); x++) {
                    painter.fillRect(QRectF(sheet.left() + x * cell, sheet.bottom() - (y + 1) * cell, cell, cell),
                                     (x + y) % 2 ? QColor(204, 204, 204) : QColor(255, 255, 255));
                }
            }
            painter.restore();
        }
        painter.setBrush(paper);
        painter.setPen(QPen(withAlpha(ink, m_dragging ? 0.6 : 0.35), 1));
        painter.drawPath(sheetPath);

        const QColor paperInk = paper.alpha() < 128 || paper.lightnessF() > 0.5 ? QColor(Qt::black) : QColor(Qt::white);
        QPainterPath around;
        around.addRect(stage);
        painter.setFont(KisModernStyle::textFont(font(), 0.82));
        for (bool onSheet : {false, true}) {
            const QColor color = onSheet ? paperInk : ink;
            painter.save();
            painter.setClipPath(onSheet ? sheetPath : around.subtracted(sheetPath), Qt::IntersectClip);
            for (const Outline &outline : m_outlines) {
                const bool same = outline.pixels == m_image->size || outline.name == m_caught;
                const QRectF at = sheetAt(outline.pixels);
                painter.setPen(QPen(withAlpha(color, same ? 0.75 : 0.22), same ? 1.5 : 1));
                painter.setBrush(Qt::NoBrush);
                painter.drawPath(KisModernStyle::squircle(at, 3));
                painter.setPen(withAlpha(color, same ? 0.9 : 0.4));
                painter.drawText(at.adjusted(0, KisModernStyle::space1, -KisModernStyle::space1 - 1, 0), Qt::AlignRight | Qt::AlignTop, outline.name);
            }
            painter.restore();
        }
        painter.restore();

        // the corner it is pulled by, and the ratio it caught on
        const QPointF corner = sheet.topRight();
        if (stage.adjusted(-6, -6, 6, 6).contains(corner)) {
            painter.setPen(QPen(QColor(32, 32, 32), 1.5));
            painter.setBrush(Qt::white);
            painter.drawEllipse(corner, 5.5, 5.5);
        }
        if (!m_caught.isEmpty() && std::none_of(m_outlines.cbegin(), m_outlines.cend(), [this](const Outline &o) { return o.name == m_caught; })) {
            painter.setFont(KisModernStyle::textFont(font(), 0.88));
            painter.setPen(ink);
            const QFontMetrics metrics(painter.font());
            QRectF label(corner + QPointF(KisModernStyle::space2, -KisModernStyle::space2 - metrics.height()),
                         QSizeF(metrics.horizontalAdvance(m_caught) + 2, metrics.height()));
            if (label.right() > width() - KisModernStyle::space1) label.moveRight(corner.x() - KisModernStyle::space2);
            if (label.top() < KisModernStyle::space1) label.moveTop(corner.y() + KisModernStyle::space2);
            painter.drawText(label, Qt::AlignCenter, m_caught);
        }
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton) return;
        m_zoom.stop();
        m_dragging = true;
        m_free = event->modifiers() & Qt::ShiftModifier;
        m_pull = pull(event->pos());
        follow(event->pos());
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (!m_dragging) {
            const Pull pulled = pull(event->pos());
            setCursor(pulled == Across ? Qt::SizeHorCursor : pulled == Up ? Qt::SizeVerCursor : Qt::SizeBDiagCursor);
            return;
        }
        m_free = event->modifiers() & Qt::ShiftModifier;
        follow(event->pos());
    }

    void mouseReleaseEvent(QMouseEvent *) override
    {
        if (!m_dragging) return;
        m_dragging = false;
        m_caught.clear();
        settle();
    }

    void mouseDoubleClickEvent(QMouseEvent *) override
    {
        if (made) made();
    }

    void contextMenuEvent(QContextMenuEvent *event) override
    {
        event->accept();
        if (reset) reset();
    }

private:
    enum Pull { Corner, Across, Up };

    /// The corner every sheet grows from, the other sizes centred across
    QPointF origin() const
    {
        int widest = 0;
        for (const Outline &outline : m_outlines) widest = qMax(widest, outline.pixels.width());
        const qreal left = qMax(qreal(KisModernStyle::space4), width() / 2.0 - widest * m_scale / 2);
        return QPointF(left, height() - KisModernStyle::space4);
    }
    QRectF sheetAt(const QSize &pixels) const
    {
        const QPointF from = origin();
        return QRectF(from.x(), from.y() - pixels.height() * m_scale, pixels.width() * m_scale, pixels.height() * m_scale);
    }
    /// Screen pixels per image pixel that show \p pixels with room to grow, and every other size
    qreal scaleFor(const QSize &pixels) const
    {
        const QSizeF room(width() - 2 * KisModernStyle::space4, height() - 2 * KisModernStyle::space4);
        QSizeF shown = (QSizeF(pixels) * 1.1).expandedTo(QSizeF(m_screen) * 0.5);
        for (const Outline &outline : m_outlines) shown = shown.expandedTo(QSizeF(outline.pixels));
        return qMax(1e-4, qMin(room.width() / shown.width(), room.height() / shown.height()));
    }
    /// What a press at \p at pulls: a side near it, else the corner
    Pull pull(const QPoint &at) const
    {
        const QRectF sheet = sheetAt(m_image->size);
        const qreal reach = 6;
        const bool nearCorner = QLineF(at, sheet.topRight()).length() < 3 * reach;
        if (!nearCorner && std::abs(at.x() - sheet.right()) < reach && at.y() > sheet.top() && at.y() < sheet.bottom()) return Across;
        if (!nearCorner && std::abs(at.y() - sheet.top()) < reach && at.x() > sheet.left() && at.x() < sheet.right()) return Up;
        return Corner;
    }
    /// The roundest step at least a screen pixel long, so dragged numbers stay round
    int step() const
    {
        const qreal span = 1 / m_scale;
        for (int candidate : {1, 2, 5, 10, 20, 50, 100, 200}) {
            if (candidate >= span) return candidate;
        }
        return 500;
    }
    int rounded(qreal value) const
    {
        return qBound(1, int(std::round(value / step())) * step(), maxSide);
    }

    void follow(const QPoint &at)
    {
        m_caught.clear();
        const QPointF from = origin();
        const qreal across = qMax(1.0, (at.x() - from.x()) / m_scale);
        const qreal up = qMax(1.0, (from.y() - at.y()) / m_scale);
        const qreal kept = m_image->kept();
        if (m_pull == Across) {
            m_image->setWidth(rounded(across));
        } else if (m_pull == Up) {
            m_image->setHeight(rounded(up));
        } else if (kept > 0) {
            // the nearest point on the line of the kept ratio, at a width whose height is whole and round too
            const qreal t = (across * kept + up) / (kept * kept + 1);
            int width = rounded(t * kept);
            for (int below = 1; below <= 16; below++) {
                const qreal above = kept * below;
                if (std::abs(above - std::round(above)) > 1e-6) continue;
                const int unit = std::lcm(step(), int(std::round(above)));
                width = qBound(unit, int(std::round(t * kept / unit)) * unit, maxSide);
                break;
            }
            m_image->setWidth(width);
            // caught on the shape's sizes a few screen pixels off
            for (const Outline &outline : m_outlines) {
                if (m_free || std::abs(t * kept - outline.pixels.width()) >= 6 / m_scale) continue;
                m_image->size = outline.pixels;
                m_caught = outline.name;
                break;
            }
        } else if (m_free) {
            m_image->size = QSize(rounded(across), rounded(up));
        } else {
            m_image->size = caught(across, up);
        }
        update();
        if (changed) changed();
    }

    /// The corner at (\p across, \p up) image pixels, caught on another size's corner or a usual ratio a few
    /// screen pixels from it
    QSize caught(qreal across, qreal up)
    {
        const qreal reach = 6 / m_scale;
        for (const Outline &outline : m_outlines) {
            if (std::abs(across - outline.pixels.width()) < reach && std::abs(up - outline.pixels.height()) < reach) {
                m_caught = outline.name;
                return outline.pixels;
            }
        }
        struct Ratio {
            int wide;
            int high;
            qreal value;
            QString name;
        };
        static const QVector<Ratio> ratios = {
            {1, 1, 1.0, QStringLiteral("1:1")}, {4, 3, 4.0 / 3, QStringLiteral("4:3")}, {3, 4, 3.0 / 4, QStringLiteral("3:4")},
            {3, 2, 3.0 / 2, QStringLiteral("3:2")}, {2, 3, 2.0 / 3, QStringLiteral("2:3")}, {16, 9, 16.0 / 9, QStringLiteral("16:9")},
            {9, 16, 9.0 / 16, QStringLiteral("9:16")}, {0, 0, M_SQRT2, QStringLiteral("A4")}, {0, 0, M_SQRT1_2, QStringLiteral("A4")},
        };
        qreal nearest = 4 / m_scale;
        const Ratio *best = nullptr;
        for (const Ratio &ratio : ratios) {
            // the nearest point on the line of this ratio
            const qreal t = (across * ratio.value + up) / (ratio.value * ratio.value + 1);
            const qreal distance = std::hypot(across - t * ratio.value, up - t);
            if (distance < nearest) {
                nearest = distance;
                best = &ratio;
            }
        }
        if (!best) return QSize(rounded(across), rounded(up));
        m_caught = best->name;
        const qreal t = (across * best->value + up) / (best->value * best->value + 1);
        // a width whose height is whole and round too
        const int unit = best->wide > 0 ? std::lcm(step(), best->wide) : step();
        const int width = qBound(unit, int(std::round(t * best->value / unit)) * unit, maxSide);
        return QSize(width, qBound(1, int(std::round(width / best->value)), maxSide));
    }

    NewImage *m_image;
    QList<Outline> m_outlines;
    QSize m_screen;
    qreal m_scale {0};
    QVariantAnimation m_zoom;
    bool m_dragging {false};
    bool m_free {false};
    QString m_caught; ///< what the corner caught on while dragged
    Pull m_pull {Corner};
};

/// Width × height: each dragged by ratios, typed on a double-click (pixels, or cm, mm, in), reset by a right click
class SizeNumbers : public QWidget
{
public:
    std::function<void()> changed;

    SizeNumbers(NewImage *image, const QSize &initial, QWidget *parent)
        : QWidget(parent)
        , m_image(image)
        , m_initial(initial)
    {
        setMouseTracking(true);
        setFixedHeight(QFontMetrics(numberFont()).height() + fontMetrics().height() + KisModernStyle::space1);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        const QColor ink = palette().color(QPalette::WindowText);
        painter.setFont(numberFont());
        painter.setPen(ink);
        painter.drawText(widthRect(), Qt::AlignRight | Qt::AlignVCenter, QLocale().toString(m_image->size.width()));
        painter.drawText(heightRect(), Qt::AlignLeft | Qt::AlignVCenter, QLocale().toString(m_image->size.height()));
        painter.setPen(withAlpha(ink, 0.55));
        painter.drawText(timesRect(), Qt::AlignCenter, QStringLiteral("×"));

        // quietly, its size on paper when it is made for print
        if (m_image->dpi >= 150) {
            painter.setFont(KisModernStyle::textFont(font(), 0.88));
            painter.setPen(KisModernStyle::secondaryInk(ink));
            const QRect under(0, timesRect().bottom() + KisModernStyle::space1, width(), fontMetrics().height());
            painter.drawText(under, Qt::AlignHCenter | Qt::AlignTop,
                             i18nc("a new image's size on paper: width × height in cm", "%1 × %2 cm",
                                   QLocale().toString(m_image->size.width() * 2.54 / m_image->dpi, 'f', 1),
                                   QLocale().toString(m_image->size.height() * 2.54 / m_image->dpi, 'f', 1)));
        }
    }

    bool event(QEvent *event) override
    {
        if (event->type() == QEvent::ToolTip) {
            const QPoint at = static_cast<QHelpEvent *>(event)->pos();
            setToolTip(widthRect().contains(at) || heightRect().contains(at)
                           ? i18nc("@info:tooltip a new image's width or height", "Pixels, cm, mm or in\nDrag: change\nShift+drag: finer\nDouble-click: type\nRight-click: reset")
                           : QString());
        }
        return QWidget::event(event);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton) return;
        m_dragging = widthRect().contains(event->pos()) ? Width : heightRect().contains(event->pos()) ? Height : None;
        m_from = event->pos();
        m_fromSize = m_image->size;
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (m_dragging == None) {
            setCursor(widthRect().contains(event->pos()) || heightRect().contains(event->pos()) ? Qt::SizeHorCursor : Qt::ArrowCursor);
            return;
        }
        // by ratios, a tenth as fast with Shift
        const bool fine = event->modifiers() & Qt::ShiftModifier;
        const qreal factor = std::exp((event->pos().x() - m_from.x()) / (fine ? 1200.0 : 120.0));
        m_image->size = m_fromSize;
        if (m_dragging == Width) m_image->setWidth(qBound(1, int(std::round(m_fromSize.width() * factor)), maxSide));
        else m_image->setHeight(qBound(1, int(std::round(m_fromSize.height() * factor)), maxSide));
        if (changed) changed();
    }

    void mouseReleaseEvent(QMouseEvent *) override
    {
        m_dragging = None;
    }

    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        const bool across = widthRect().contains(event->pos());
        if (!across && !heightRect().contains(event->pos())) return;
        KisStudioInlineEdit::open(this, across ? widthRect() : heightRect(), QString::number(across ? m_image->size.width() : m_image->size.height()),
                                  (across ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter, numberFont(), [this, across](const QString &typed) {
                                      const int value = pixelsOf(typed);
                                      if (value <= 0) return;
                                      if (across) m_image->setWidth(value);
                                      else m_image->setHeight(value);
                                      if (changed) changed();
                                  });
    }

    void contextMenuEvent(QContextMenuEvent *event) override
    {
        event->accept();
        if (widthRect().contains(event->pos())) m_image->setWidth(m_initial.width());
        else if (heightRect().contains(event->pos())) m_image->setHeight(m_initial.height());
        else return;
        if (changed) changed();
    }

private:
    enum Part { None, Width, Height };

    QFont numberFont() const { return KisModernStyle::textFont(font(), 1.3, QFont::DemiBold); }
    QRect timesRect() const
    {
        const QFontMetrics metrics(numberFont());
        const int side = metrics.horizontalAdvance(QStringLiteral(" × "));
        return QRect((width() - side) / 2, 0, side, metrics.height());
    }
    QRect widthRect() const { return QRect(0, 0, timesRect().left(), timesRect().height()); }
    QRect heightRect() const { return QRect(timesRect().right() + 1, 0, width() - timesRect().right() - 1, timesRect().height()); }

    /// Pixels from typed text: a number, or one in cm, mm or in at the image's resolution (300 dpi when it is a screen's)
    int pixelsOf(const QString &typed)
    {
        static const QRegularExpression pattern(QStringLiteral("^\\s*([0-9]+(?:[.,][0-9]+)?)\\s*(px|cm|mm|in|\")?\\s*$"),
                                                QRegularExpression::CaseInsensitiveOption);
        const QRegularExpressionMatch match = pattern.match(typed);
        if (!match.hasMatch()) return 0;
        const qreal number = match.captured(1).replace(QLatin1Char(','), QLatin1Char('.')).toDouble();
        const QString unit = match.captured(2).toLower();
        qreal inches = -1;
        if (unit == QLatin1String("cm")) inches = number / 2.54;
        else if (unit == QLatin1String("mm")) inches = number / 25.4;
        else if (unit == QLatin1String("in") || unit == QLatin1String("\"")) inches = number;
        if (inches < 0) return qBound(1, int(std::round(number)), maxSide);
        if (m_image->dpi < 150) m_image->dpi = 300;
        return qBound(1, int(std::round(inches * m_image->dpi)), maxSide);
    }

    NewImage *m_image;
    QSize m_initial;
    QSize m_fromSize;
    QPoint m_from;
    Part m_dragging {None};
};

/**
 * Custom, opened under the tiles: the sheet on the left; beside it the numbers, its shape (1:1, …, A paper; the lit
 * one again frees it) and a turn, how big (HD to 8K, or A5 to A2), its resolution and color as rows, the paper and Make.
 */
class CustomImage : public QWidget
{
public:
    std::function<void(const NewImage &)> made;
    std::function<void()> closed;

    CustomImage(const QSize &size, const QList<SizeStage::Outline> &outlines, const QSize &screen, QWidget *parent)
        : QWidget(parent)
    {
        setObjectName(QStringLiteral("studioCustomImage"));
        setFocusPolicy(Qt::StrongFocus);
        setFixedHeight(300);

        KisConfig cfg(true);
        m_image.size = size;
        m_image.dpi = qBound(30.0, std::round(cfg.defImageResolution() * 72.0), 1200.0);
        m_image.model = cfg.defColorModel();
        m_image.depth = cfg.defaultColorDepth();
        m_image.profile = cfg.defColorProfile();
        m_image.paper = cfg.defaultBackgroundColor(newImageSection);
        m_image.paper.setAlpha(cfg.defaultBackgroundOpacity(newImageSection));
        m_initial = m_image;

        auto *layout = new QHBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(KisModernStyle::space3);
        m_presets = outlines;
        m_stage = new SizeStage(&m_image, outlines, screen, this);
        layout->addWidget(m_stage, 1);

        auto *side = new QWidget(this);
        side->setFixedWidth(260);
        auto *column = new QVBoxLayout(side);
        column->setContentsMargins(0, KisModernStyle::space2, 0, 0);
        column->setSpacing(KisModernStyle::space2);
        layout->addWidget(side);

        m_numbers = new SizeNumbers(&m_image, size, side);
        column->addWidget(m_numbers);

        m_ratios = new KisStudioChips(side);
        m_ratios->setTabs(true);
        column->addWidget(m_ratios);
        connect(m_ratios, &KisStudioChips::clicked, this, [this](int index) {
            if (index == ratios().size()) {
                m_image.size.transpose();
            } else {
                // the lit one again frees the size
                const qreal picked = ratios()[index].value;
                m_image.ratio = qFuzzyCompare(picked, m_image.ratio) ? 0 : picked;
                // paper is for print
                if (m_image.paperShaped() && m_image.dpi < 150) m_image.dpi = 300;
                // the longer side stays
                if (m_image.size.width() >= m_image.size.height()) m_image.setWidth(m_image.size.width());
                else m_image.setHeight(m_image.size.height());
            }
            refresh();
        });

        m_steps = new KisStudioChips(side);
        m_steps->setTabs(true);
        column->addWidget(m_steps);
        connect(m_steps, &KisStudioChips::clicked, this, [this](int index) {
            const QVector<NewImage::Step> steps = m_image.steps();
            if (index < steps.size()) m_image.size = m_image.sized(steps[index].longSide);
            refresh();
        });

        column->addSpacing(KisModernStyle::space1);
        m_dpi = new KisStudioValueRow(i18nc("a new image's resolution", "Resolution"), side);
        m_dpi->setCompact(true);
        KisStudioValueRow::Range dpiRange;
        dpiRange.minimum = 30;
        dpiRange.maximum = 1200;
        dpiRange.size = true;
        dpiRange.suffix = i18nc("dots per inch, after a number", " dpi");
        m_dpi->setRange(dpiRange);
        m_dpi->setDefaultValue(m_image.dpi);
        column->addWidget(m_dpi);
        connect(m_dpi, &KisStudioValueRow::valueChanged, this, [this](qreal dpi) {
            // a paper size stays that paper
            const int step = m_image.paperShaped() ? m_image.step() : -1;
            m_image.dpi = roundDpi(dpi);
            if (step >= 0) m_image.size = m_image.sized(m_image.steps()[step].longSide);
            refresh();
        });

        m_color = new KisStudioValueRow(i18nc("a new image's color model and depth", "Color"), side);
        m_color->setCompact(true);
        m_color->setDraggable(false);
        column->addWidget(m_color);
        connect(m_color, &KisStudioValueRow::clicked, this, [this]() { colorMenu(); });

        m_papers = {QColor(Qt::white), QColor(Qt::black), QColor(255, 255, 255, 0)};
        if (!m_papers.contains(m_image.paper) && m_image.paper.alpha() > 0) m_papers.insert(1, m_image.paper);
        if (m_image.paper.alpha() == 0) m_image.paper = m_papers.last();
        m_paper = new KisStudioChips(side);
        m_paper->setTileSize(28);
        column->addWidget(m_paper, 0, Qt::AlignHCenter);
        connect(m_paper, &KisStudioChips::clicked, this, [this](int index) {
            m_image.paper = m_papers[index];
            refresh();
        });

        column->addStretch(1);
        auto *make = new QPushButton(i18nc("@action:button make the new image", "Make"), side);
        column->addWidget(make);
        connect(make, &QPushButton::clicked, this, [this]() { makeIt(); });

        m_stage->changed = [this]() { refresh(false); };
        m_stage->made = [this]() { makeIt(); };
        m_stage->reset = [this]() {
            m_image = m_initial;
            refresh();
        };
        m_numbers->changed = [this]() { refresh(); };
        refresh();
    }

protected:
    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
            makeIt();
        } else if (event->key() == Qt::Key_Escape) {
            if (closed) closed();
        } else {
            QWidget::keyPressEvent(event);
        }
    }

private:
    struct Ratio {
        qreal value;
        QString name;
    };
    static const QVector<Ratio> &ratios()
    {
        static const QVector<Ratio> all = {
            {1.0, QStringLiteral("1:1")}, {4.0 / 3, QStringLiteral("4:3")},
            {3.0 / 2, QStringLiteral("3:2")}, {16.0 / 9, QStringLiteral("16:9")}, {M_SQRT2, i18nc("a new image's shape: as A paper (A4, A3…)", "A")},
        };
        return all;
    }

    /// Everything shown again; \p settle: the stage zooms to the size
    void refresh(bool settle = true)
    {
        QVector<KisStudioChips::Chip> chips;
        for (const Ratio &ratio : ratios()) {
            KisStudioChips::Chip chip;
            chip.text = ratio.name;
            chip.on = m_image.ratio > 0 && qFuzzyCompare(ratio.value, m_image.ratio);
            chip.toolTip = ratio.value == M_SQRT2 ? i18nc("@info:tooltip", "A paper's shape") : QString();
            chips << chip;
        }
        KisStudioChips::Chip turn;
        turn.text = QStringLiteral("↻");
        turn.toolTip = i18nc("@info:tooltip turn a new image: swap its width and height", "Turn");
        chips << turn;
        m_ratios->setChips(chips);

        const QVector<NewImage::Step> steps = m_image.steps();
        const int at = m_image.step();
        QVector<KisStudioChips::Chip> sizes;
        QList<SizeStage::Outline> outlines;
        for (int i = 0; i < steps.size(); i++) {
            KisStudioChips::Chip chip;
            chip.text = steps[i].name;
            chip.on = i == at;
            const QSize pixels = m_image.sized(steps[i].longSide);
            chip.toolTip = i18nc("an image's size in pixels: width × height", "%1 × %2", QLocale().toString(pixels.width()), QLocale().toString(pixels.height()));
            sizes << chip;
            outlines.append({steps[i].name, pixels});
        }
        m_steps->setChips(sizes);
        // the sheet among this shape's sizes, or the usual images while it is free
        m_stage->setOutlines(m_image.ratio > 0 ? outlines : m_presets);

        // written as caught, since a row being dragged keeps its own value
        m_dpi->setValue(m_image.dpi);
        m_dpi->setValueText(i18nc("a resolution in dots per inch", "%1 dpi", QLocale().toString(int(m_image.dpi))));
        m_color->setValueText(colorText());

        QVector<KisStudioChips::Chip> papers;
        for (const QColor &paper : m_papers) {
            KisStudioChips::Chip chip;
            chip.on = paper == m_image.paper;
            chip.toolTip = paper.alpha() == 0 ? i18nc("@info:tooltip a new image's paper", "Clear") : QString();
            chip.picture = [paper](QPainter &painter, const QRectF &at) {
                const qreal side = qMin(at.width(), at.height()) * 0.8;
                const QRectF dot(at.center() - QPointF(side, side) / 2, QSizeF(side, side));
                painter.setRenderHint(QPainter::Antialiasing, true);
                painter.setPen(QPen(QColor(128, 128, 128, 120), 1));
                if (paper.alpha() == 0) {
                    painter.setBrush(Qt::white);
                    painter.drawEllipse(dot);
                    QPainterPath half;
                    half.moveTo(dot.center());
                    half.arcTo(dot, 90, 90);
                    half.closeSubpath();
                    half.moveTo(dot.center());
                    half.arcTo(dot, 270, 90);
                    half.closeSubpath();
                    painter.setPen(Qt::NoPen);
                    painter.setBrush(QColor(190, 190, 190));
                    painter.drawPath(half);
                } else {
                    painter.setBrush(paper);
                    painter.drawEllipse(dot);
                }
            };
            papers << chip;
        }
        m_paper->setChips(papers);

        m_numbers->update();
        if (settle) m_stage->settle();
        else m_stage->update();
    }

    QString colorText() const
    {
        // "RGB · 8-bit": the first part of each name
        const KoColorSpaceRegistry *registry = KoColorSpaceRegistry::instance();
        QString model = m_image.model;
        QString depth = m_image.depth;
        Q_FOREACH (const KoID &id, registry->colorModelsList(KoColorSpaceRegistry::OnlyUserVisible)) {
            if (id.id() == m_image.model) model = id.name().section(QLatin1Char('/'), 0, 0);
        }
        Q_FOREACH (const KoID &id, registry->colorDepthList(m_image.model, KoColorSpaceRegistry::OnlyUserVisible)) {
            if (id.id() == m_image.depth) depth = id.name().section(QLatin1Char('/'), 0, 0);
        }
        return model + QStringLiteral(" · ") + depth;
    }

    void colorMenu()
    {
        const KoColorSpaceRegistry *registry = KoColorSpaceRegistry::instance();
        QMenu menu(this);
        Q_FOREACH (const KoID &id, registry->colorModelsList(KoColorSpaceRegistry::OnlyUserVisible)) {
            QAction *action = menu.addAction(id.name());
            action->setCheckable(true);
            action->setChecked(id.id() == m_image.model);
            connect(action, &QAction::triggered, this, [this, id]() {
                m_image.model = id.id();
                // the depth kept where the model has it, the profile its own
                const QList<KoID> depths = KoColorSpaceRegistry::instance()->colorDepthList(id.id(), KoColorSpaceRegistry::OnlyUserVisible);
                bool has = false;
                for (const KoID &depth : depths) has = has || depth.id() == m_image.depth;
                if (!has && !depths.isEmpty()) m_image.depth = depths.first().id();
                m_image.profile = defaultProfile();
                refresh(false);
            });
        }
        menu.addSeparator();
        Q_FOREACH (const KoID &id, registry->colorDepthList(m_image.model, KoColorSpaceRegistry::OnlyUserVisible)) {
            QAction *action = menu.addAction(id.name());
            action->setCheckable(true);
            action->setChecked(id.id() == m_image.depth);
            connect(action, &QAction::triggered, this, [this, id]() {
                m_image.depth = id.id();
                bool has = false;
                Q_FOREACH (const KoColorProfile *profile, KoColorSpaceRegistry::instance()->profilesFor(colorSpace())) has = has || profile->name() == m_image.profile;
                if (!has) m_image.profile = defaultProfile();
                refresh(false);
            });
        }
        menu.addSeparator();
        QMenu *profiles = menu.addMenu(i18nc("a new image's color profile", "Profile"));
        QList<const KoColorProfile *> all = registry->profilesFor(colorSpace());
        std::sort(all.begin(), all.end(), [](const KoColorProfile *a, const KoColorProfile *b) { return a->name() < b->name(); });
        for (const KoColorProfile *profile : all) {
            const QString name = profile->name();
            QAction *action = profiles->addAction(name);
            action->setCheckable(true);
            action->setChecked(name == m_image.profile);
            connect(action, &QAction::triggered, this, [this, name]() { m_image.profile = name; });
        }
        menu.exec(m_color->mapToGlobal(QPoint(0, m_color->height())));
    }

    /// The resolutions people use, caught from a little way off; others by tens
    static qreal roundDpi(qreal dpi)
    {
        qreal nearest = 0.06;
        qreal caught = std::round(dpi / 10) * 10;
        for (qreal usual : {72.0, 96.0, 100.0, 150.0, 200.0, 240.0, 250.0, 300.0, 350.0, 400.0, 600.0, 1200.0}) {
            const qreal off = std::abs(dpi / usual - 1);
            if (off < nearest) {
                nearest = off;
                caught = usual;
            }
        }
        return caught;
    }

    QString colorSpace() const { return KoColorSpaceRegistry::instance()->colorSpaceId(m_image.model, m_image.depth); }
    QString defaultProfile() const { return KoColorSpaceRegistry::instance()->defaultProfileForColorSpace(colorSpace()); }

    void makeIt()
    {
        if (made) made(m_image);
    }

    NewImage m_image;
    NewImage m_initial;
    QList<QColor> m_papers;
    QList<SizeStage::Outline> m_presets;
    SizeStage *m_stage {nullptr};
    SizeNumbers *m_numbers {nullptr};
    KisStudioChips *m_ratios {nullptr};
    KisStudioChips *m_steps {nullptr};
    KisStudioValueRow *m_dpi {nullptr};
    KisStudioValueRow *m_color {nullptr};
    KisStudioChips *m_paper {nullptr};
};

/// When \p time was: today or yesterday at a time, or else the day
QString whenText(const QDateTime &time)
{
    const QDate day = time.date();
    const QDate today = QDate::currentDate();
    const QString clock = QLocale().toString(time.time(), QLocale::ShortFormat);
    if (day == today) return i18nc("when an image was changed: today at a time", "Today %1", clock);
    if (day == today.addDays(-1)) return i18nc("when an image was changed: yesterday at a time", "Yesterday %1", clock);
    return QLocale().toString(day, day.year() == today.year() ? QStringLiteral("d MMMM") : QStringLiteral("d MMMM yyyy"));
}

/**
 * The recent images: the last one large, then the others in rows that keep their shapes, under Today,
 * This week and Earlier. A click opens one; typing filters them by name; the arrow keys and Enter pick.
 */
class Gallery : public QWidget
{
public:
    static constexpr int rowHeight = 190;
    static constexpr int gap = KisModernStyle::space3;
    static constexpr int pictureLength = 480;
    static constexpr int heroLength = 1024;

    /// by value: opening refreshes the items it would refer into
    std::function<void(QString path)> opened;
    std::function<void(const QString &path)> forgotten;
    /// Esc with nothing typed: whether there is somewhere to go back to, and going there
    std::function<bool()> canLeave;
    std::function<void()> left;

    explicit Gallery(QWidget *parent)
        : QWidget(parent)
    {
        setObjectName(QStringLiteral("studioRecentImages"));
        setMouseTracking(true);
        setAttribute(Qt::WA_Hover);
        setFocusPolicy(Qt::StrongFocus);
        QSizePolicy policy(QSizePolicy::Preferred, QSizePolicy::Preferred);
        policy.setHeightForWidth(true);
        setSizePolicy(policy);
        m_model = &KisRecentDocumentsModelWrapper::instance()->model();
        auto refresh = [this]() { reload(); };
        connect(m_model, &QAbstractItemModel::rowsInserted, this, refresh);
        connect(m_model, &QAbstractItemModel::rowsRemoved, this, refresh);
        connect(m_model, &QAbstractItemModel::modelReset, this, refresh);
        Pictures::instance()->watch(this, [this]() { relayout(); });
        reload();
    }

    /// The image last worked on, if any
    QString latest() const { return m_items.isEmpty() ? QString() : m_items.first().path; }

    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override { return arrange(width).height; }
    QSize sizeHint() const override { return QSize(960, heightForWidth(960)); }

protected:
    void hideEvent(QHideEvent *event) override
    {
        // the page is away most of the time: its tiles are made again when it shows
        m_tiles.clear();
        QWidget::hideEvent(event);
    }

    /// \p picture scaled into \p rect's shape once, so scrolling only copies it
    QPixmap tile(const QImage &picture, const QRectF &rect, qreal radius)
    {
        const qreal ratio = devicePixelRatioF();
        const QString key = QStringLiteral("%1|%2x%3|%4|%5").arg(picture.cacheKey()).arg(rect.width()).arg(rect.height()).arg(radius).arg(ratio);
        const auto found = m_tiles.constFind(key);
        if (found != m_tiles.constEnd()) return *found;
        QPixmap made((rect.size() * ratio).toSize());
        made.setDevicePixelRatio(ratio);
        made.fill(Qt::transparent);
        {
            QPainter painter(&made);
            painter.setRenderHint(QPainter::Antialiasing, true);
            painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
            const QRectF local(QPointF(), rect.size());
            painter.fillPath(KisModernStyle::squircle(local, radius), Qt::white);
            painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
            painter.drawImage(local, picture);
        }
        if (m_tiles.size() > 64) m_tiles.clear();
        m_tiles.insert(key, made);
        return made;
    }

    void paintEvent(QPaintEvent *event) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        const QColor ink = palette().color(QPalette::WindowText);
        const Arrangement &arranged = arrangement();

        for (const Heading &heading : arranged.headings) {
            painter.setPen(ink);
            painter.setFont(heading.filter ? KisModernStyle::textFont(font(), 1.35, QFont::DemiBold) : KisModernStyle::textFont(font(), 1.0, QFont::DemiBold));
            painter.drawText(heading.rect, Qt::AlignLeft | Qt::AlignVCenter, heading.text);
        }
        if (!m_filter.isEmpty()) {
            // clears the filter, as Escape does
            painter.setPen(Qt::NoPen);
            painter.setBrush(withAlpha(ink, 0.1));
            painter.drawEllipse(QRectF(m_clearRect));
            painter.setPen(QPen(withAlpha(ink, 0.8), 1.5, Qt::SolidLine, Qt::RoundCap));
            const QPointF c = QRectF(m_clearRect).center();
            painter.drawLine(c + QPointF(-4, -4), c + QPointF(4, 4));
            painter.drawLine(c + QPointF(-4, 4), c + QPointF(4, -4));
        }
        if (arranged.placed.isEmpty()) {
            paintEmpty(painter, arranged);
            return;
        }
        for (int i = 0; i < arranged.placed.size(); i++) {
            const Placed &placed = arranged.placed[i];
            if (!event->rect().intersects(placed.rect.toAlignedRect())) continue;
            const Item &item = m_items[placed.item];
            const qreal radius = placed.hero ? KisModernStyle::panelRadius : KisModernStyle::cardRadius;
            const QPainterPath shape = KisModernStyle::squircle(placed.rect, radius);
            const QImage picture = Pictures::instance()->get(item.path, placed.hero ? heroLength : pictureLength);
            if (picture.isNull()) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(withAlpha(ink, 0.06));
                painter.drawPath(shape);
            } else {
                painter.drawPixmap(placed.rect.topLeft(), tile(picture, placed.rect, radius));
            }
            const bool hovered = i == m_hovered;
            const bool selected = i == m_selected && m_keyboard && hasFocus();
            if (!placed.hero && (hovered || selected || picture.isNull())) {
                // the name over the bottom of the picture
                const QRectF band(placed.rect.left(), placed.rect.bottom() - 34, placed.rect.width(), 34);
                if (!picture.isNull()) {
                    QLinearGradient shade(band.topLeft(), band.bottomLeft());
                    shade.setColorAt(0, QColor(0, 0, 0, 0));
                    shade.setColorAt(1, QColor(0, 0, 0, 150));
                    painter.save();
                    painter.setClipPath(shape);
                    painter.fillRect(band, shade);
                    painter.restore();
                }
                painter.setPen(picture.isNull() ? ink : QColor(Qt::white));
                painter.setFont(font());
                const QRectF text = band.adjusted(KisModernStyle::space2, 0, -KisModernStyle::space2, -KisModernStyle::space1);
                painter.drawText(text, Qt::AlignLeft | Qt::AlignBottom, fontMetrics().elidedText(item.name, Qt::ElideMiddle, int(text.width())));
            }
            painter.setBrush(Qt::NoBrush);
            if (selected) {
                painter.setPen(QPen(palette().color(QPalette::Highlight), 2));
                painter.drawPath(KisModernStyle::squircle(placed.rect.adjusted(-3, -3, 3, 3), radius + 2));
            } else {
                painter.setPen(QPen(withAlpha(ink, hovered ? 0.45 : 0.1), 1));
                painter.drawPath(KisModernStyle::squircle(placed.rect.adjusted(0.5, 0.5, -0.5, -0.5), radius));
            }
            if (placed.hero && (hovered || selected)) {
                // what a click does, on the picture
                const QString label = i18nc("@action open the image last worked on again", "Continue");
                const QFont strong = KisModernStyle::textFont(font(), 1.1, QFont::DemiBold);
                const QFontMetrics metrics(strong);
                const QSizeF chip(metrics.horizontalAdvance(label) + 2 * KisModernStyle::space4, metrics.height() + 2 * KisModernStyle::space2);
                const QRectF at(placed.rect.center().x() - chip.width() / 2, placed.rect.bottom() - chip.height() - KisModernStyle::space4, chip.width(), chip.height());
                painter.setPen(Qt::NoPen);
                painter.setBrush(QColor(0, 0, 0, 150));
                painter.drawPath(KisModernStyle::squircle(at, at.height() / 2));
                painter.setPen(Qt::white);
                painter.setFont(strong);
                painter.drawText(at, Qt::AlignCenter, label);
            }
            if (placed.hero) {
                // its name and when it was changed, under it
                const QRectF under(placed.rect.left() - 200, placed.rect.bottom() + KisModernStyle::space3, placed.rect.width() + 400, heroCaption());
                const QFont title = KisModernStyle::textFont(font(), 1.2, QFont::DemiBold);
                painter.setFont(title);
                painter.setPen(ink);
                const int titleHeight = QFontMetrics(title).height();
                painter.drawText(QRectF(under.left(), under.top(), under.width(), titleHeight), Qt::AlignHCenter | Qt::AlignVCenter, item.name);
                painter.setFont(font());
                painter.setPen(KisModernStyle::secondaryInk(ink));
                painter.drawText(QRectF(under.left(), under.top() + titleHeight, under.width(), fontMetrics().height() + 2), Qt::AlignHCenter | Qt::AlignVCenter,
                                 whenText(item.time));
            }
        }
    }

    bool event(QEvent *event) override
    {
        if (event->type() == QEvent::ShortcutOverride) {
            // typing goes to the filter, not to the window's shortcuts
            auto *key = static_cast<QKeyEvent *>(event);
            const bool plain = !(key->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier));
            const bool ours = (!key->text().isEmpty() && key->text().at(0).isPrint()) || key->key() == Qt::Key_Backspace
                || key->key() == Qt::Key_Left || key->key() == Qt::Key_Right || key->key() == Qt::Key_Up || key->key() == Qt::Key_Down
                || key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter
                || (key->key() == Qt::Key_Escape && (!m_filter.isEmpty() || (canLeave && canLeave())));
            if (plain && ours) {
                event->accept();
                return true;
            }
        }
        if (event->type() == QEvent::ToolTip) {
            const int at = placedAt(static_cast<QHelpEvent *>(event)->pos());
            setToolTip(at >= 0 ? m_items[arrangement().placed[at].item].path : QString());
        }
        return QWidget::event(event);
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        const Arrangement &arranged = arrangement();
        const int count = arranged.placed.size();
        switch (event->key()) {
        case Qt::Key_Left:
        case Qt::Key_Right:
            if (count) select(qBound(0, m_selected + (event->key() == Qt::Key_Left ? -1 : 1), count - 1));
            return;
        case Qt::Key_Up:
        case Qt::Key_Down:
            if (count) select(nextRow(event->key() == Qt::Key_Down));
            return;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            if (m_selected >= 0 && m_selected < count && opened) opened(m_items[arranged.placed[m_selected].item].path);
            return;
        case Qt::Key_Escape:
            if (m_filter.isEmpty() && canLeave && canLeave() && left) left();
            else setFilter(QString());
            return;
        case Qt::Key_Backspace:
            setFilter(m_filter.left(m_filter.size() - 1));
            return;
        default:
            break;
        }
        const QString text = event->text();
        if (!text.isEmpty() && text.at(0).isPrint() && !(event->modifiers() & (Qt::ControlModifier | Qt::AltModifier))) {
            setFilter(m_filter + text);
            return;
        }
        QWidget::keyPressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        m_keyboard = false;
        const int hovered = placedAt(event->pos());
        const bool clear = !m_filter.isEmpty() && m_clearRect.contains(event->pos());
        setCursor(hovered >= 0 || clear ? Qt::PointingHandCursor : Qt::ArrowCursor);
        if (hovered != m_hovered) {
            m_hovered = hovered;
            update();
        }
    }

    void leaveEvent(QEvent *) override
    {
        m_hovered = -1;
        update();
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        m_pressed = event->button() == Qt::LeftButton ? placedAt(event->pos()) : -1;
        setFocus(Qt::MouseFocusReason);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton) return;
        if (!m_filter.isEmpty() && m_clearRect.contains(event->pos())) {
            setFilter(QString());
            return;
        }
        const int at = placedAt(event->pos());
        if (at >= 0 && at == m_pressed && opened) opened(m_items[arrangement().placed[at].item].path);
        m_pressed = -1;
    }

    void contextMenuEvent(QContextMenuEvent *event) override
    {
        const int at = placedAt(event->pos());
        if (at < 0) return;
        const QString path = m_items[arrangement().placed[at].item].path;
        QMenu menu;
        QAction *forget = menu.addAction(i18nc("@action:inmenu take an image off the recent images", "Forget"));
        if (menu.exec(event->globalPos()) == forget && forgotten) forgotten(path);
    }

    void resizeEvent(QResizeEvent *event) override
    {
        QWidget::resizeEvent(event);
        m_arranged = false;
    }

private:
    struct Item {
        QString path;
        QString name;
        QDateTime time;
    };
    struct Placed {
        int item;
        QRectF rect;
        bool hero;
    };
    struct Heading {
        QString text;
        QRectF rect;
        bool filter;
    };
    struct Arrangement {
        QVector<Placed> placed;
        QVector<Heading> headings;
        int height {0};
    };

    void reload()
    {
        m_items.clear();
        for (int i = 0; i < m_model->rowCount(); i++) {
            const QString path = m_model->index(i, 0).data(Qt::ToolTipRole).toString();
            if (path.isEmpty()) continue;
            const QFileInfo info(path);
            m_items.append({path, info.completeBaseName(), info.lastModified()});
        }
        relayout();
    }

    void relayout()
    {
        m_arranged = false;
        updateGeometry();
        update();
    }

    void setFilter(const QString &filter)
    {
        if (filter == m_filter) return;
        m_filter = filter;
        m_selected = 0;
        m_keyboard = true;
        relayout();
        if (QScrollArea *scroller = scrollArea()) scroller->verticalScrollBar()->setValue(0);
    }

    QScrollArea *scrollArea() const
    {
        for (QWidget *w = parentWidget(); w; w = w->parentWidget()) {
            if (auto *scroller = qobject_cast<QScrollArea *>(w)) return scroller;
        }
        return nullptr;
    }

    void select(int index)
    {
        m_selected = index;
        m_keyboard = true;
        update();
        QScrollArea *scroller = scrollArea();
        const Arrangement &arranged = arrangement();
        if (!scroller || !scroller->widget() || index < 0 || index >= arranged.placed.size()) return;
        const QRect rect = arranged.placed[index].rect.toAlignedRect();
        const QPoint center = mapTo(scroller->widget(), rect.center());
        scroller->ensureVisible(center.x(), center.y(), rect.width() / 2 + gap, rect.height() / 2 + 3 * gap);
    }

    /// The picture in the next row up or down nearest across to the one selected
    int nextRow(bool down) const
    {
        const Arrangement &arranged = arrangement();
        if (m_selected < 0 || m_selected >= arranged.placed.size()) return 0;
        const QRectF from = arranged.placed[m_selected].rect;
        int best = m_selected;
        qreal bestRow = 0;
        qreal bestAcross = 0;
        for (int i = 0; i < arranged.placed.size(); i++) {
            const QRectF r = arranged.placed[i].rect;
            const qreal row = down ? r.top() - from.bottom() : from.top() - r.bottom();
            if (row < 0) continue;
            const qreal across = std::abs(r.center().x() - from.center().x());
            if (best == m_selected || row < bestRow - 1 || (std::abs(row - bestRow) <= 1 && across < bestAcross)) {
                best = i;
                bestRow = row;
                bestAcross = across;
            }
        }
        return best;
    }

    int placedAt(const QPoint &pos) const
    {
        const Arrangement &arranged = arrangement();
        for (int i = 0; i < arranged.placed.size(); i++) {
            if (arranged.placed[i].rect.contains(pos)) return i;
        }
        return -1;
    }

    qreal aspectOf(int item) const
    {
        const QImage picture = Pictures::instance()->get(m_items[item].path, pictureLength);
        if (picture.isNull() || picture.height() <= 0) return 1.5;
        return qBound(0.4, qreal(picture.width()) / picture.height(), 3.0);
    }

    int heroCaption() const
    {
        return QFontMetrics(KisModernStyle::textFont(font(), 1.2, QFont::DemiBold)).height() + fontMetrics().height() + 2;
    }

    int headingHeight() const { return fontMetrics().height() + 2 * KisModernStyle::space2; }

    const Arrangement &arrangement() const
    {
        if (!m_arranged || m_arrangedWidth != width()) {
            m_arrangement = arrange(width());
            m_arranged = true;
            m_arrangedWidth = width();
        }
        return m_arrangement;
    }

    Arrangement arrange(int width) const
    {
        Arrangement out;
        if (width <= 0) return out;
        QVector<int> shown;
        for (int i = 0; i < m_items.size(); i++) {
            if (m_filter.isEmpty() || m_items[i].name.contains(m_filter, Qt::CaseInsensitive)) shown << i;
        }
        qreal y = 0;
        if (!m_filter.isEmpty()) {
            const int height = QFontMetrics(KisModernStyle::textFont(font(), 1.35, QFont::DemiBold)).height() + 2 * KisModernStyle::space2;
            out.headings.append({QStringLiteral("“%1”").arg(m_filter), QRectF(0, y, width, height), true});
            const int textWidth = QFontMetrics(KisModernStyle::textFont(font(), 1.35, QFont::DemiBold)).horizontalAdvance(QStringLiteral("“%1”").arg(m_filter));
            m_clearRect = QRect(textWidth + KisModernStyle::space3, int(y) + (height - 24) / 2, 24, 24);
            y += height + KisModernStyle::space2;
        }
        if (shown.isEmpty()) {
            out.height = int(y) + emptyHeight();
            return out;
        }
        // the image last worked on, large
        if (m_filter.isEmpty()) {
            const int window = this->window() ? this->window()->height() : 900;
            const qreal heroHeight = qBound<qreal>(200, window * 0.36, 440);
            const qreal aspect = [this, &shown]() {
                const QImage picture = Pictures::instance()->get(m_items[shown.first()].path, heroLength);
                return picture.isNull() || picture.height() <= 0 ? aspectOf(shown.first()) : qBound(0.4, qreal(picture.width()) / picture.height(), 3.0);
            }();
            QSizeF size(heroHeight * aspect, heroHeight);
            if (size.width() > width) size.scale(width, heroHeight, Qt::KeepAspectRatio);
            out.placed.append({shown.first(), QRectF((width - size.width()) / 2, y, size.width(), size.height()), true});
            y += size.height() + KisModernStyle::space3 + heroCaption() + KisModernStyle::space4 * 2;
            shown.removeFirst();
        }
        // the rest by when they were changed, unless filtered
        QVector<QPair<QString, QVector<int>>> groups;
        if (m_filter.isEmpty()) {
            const QDate today = QDate::currentDate();
            QVector<int> recent, week, earlier;
            for (int i : shown) {
                const QDate day = m_items[i].time.date();
                (day == today ? recent : day > today.addDays(-7) ? week : earlier) << i;
            }
            if (!recent.isEmpty()) groups.append({i18nc("@title recent images changed today", "Today"), recent});
            if (!week.isEmpty()) groups.append({i18nc("@title recent images changed in the last seven days", "This week"), week});
            if (!earlier.isEmpty()) groups.append({i18nc("@title recent images changed before this week", "Earlier"), earlier});
        } else {
            groups.append({QString(), shown});
        }
        for (const auto &group : groups) {
            if (!group.first.isEmpty()) {
                out.headings.append({group.first, QRectF(0, y, width, headingHeight()), false});
                y += headingHeight();
            }
            // rows as wide as the page at about the same height, keeping each picture's shape; the last isn't stretched
            QVector<int> row;
            qreal aspects = 0;
            auto place = [&](qreal height) {
                qreal x = 0;
                for (int i : row) {
                    const qreal w = height * aspectOf(i);
                    out.placed.append({i, QRectF(x, y, w, height), false});
                    x += w + gap;
                }
                y += height + gap;
                row.clear();
                aspects = 0;
            };
            for (int i : group.second) {
                row << i;
                aspects += aspectOf(i);
                const qreal room = width - gap * (row.size() - 1);
                if (aspects * rowHeight >= room) place(room / aspects);
            }
            if (!row.isEmpty()) place(rowHeight);
            y += KisModernStyle::space3;
        }
        out.height = int(std::ceil(y));
        return out;
    }

    int emptyHeight() const { return 180; }

    void paintEmpty(QPainter &painter, const Arrangement &arranged)
    {
        const QColor ink = palette().color(QPalette::WindowText);
        const qreal top = arranged.headings.isEmpty() ? 0 : arranged.headings.last().rect.bottom() + KisModernStyle::space2;
        const qreal w = qMin<qreal>(width(), 520);
        const QRectF area((width() - w) / 2, top, w, emptyHeight() - KisModernStyle::space3);
        painter.setPen(QPen(withAlpha(ink, 0.2), 1.5, Qt::DashLine));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(KisModernStyle::squircle(area.adjusted(0.75, 0.75, -0.75, -0.75), KisModernStyle::panelRadius));
        painter.setPen(KisModernStyle::secondaryInk(ink));
        painter.setFont(font());
        painter.drawText(area, Qt::AlignCenter,
                         m_filter.isEmpty() ? i18nc("@info the start page with no recent images", "No recent images")
                                            : i18nc("@info no recent image's name has the typed text", "No images named “%1”", m_filter));
    }

    QPointer<QStandardItemModel> m_model;
    QVector<Item> m_items;
    /// The pictures as drawn, by picture, size and shape
    QHash<QString, QPixmap> m_tiles;
    QString m_filter;
    mutable QRect m_clearRect;
    int m_hovered {-1};
    int m_pressed {-1};
    int m_selected {0};
    bool m_keyboard {false};
    mutable Arrangement m_arrangement;
    mutable bool m_arranged {false};
    mutable int m_arrangedWidth {-1};
};

/// The new images and Open, on a pane of their own over the backdrop
class Pill : public QWidget
{
public:
    using QWidget::QWidget;

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QColor ink = palette().color(QPalette::WindowText);
        const QPainterPath shape = KisModernStyle::squircle(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), KisModernStyle::panelRadius + KisModernStyle::space1);
        painter.fillPath(shape, withAlpha(palette().color(QPalette::Window), 0.55));
        painter.setPen(QPen(withAlpha(ink, 0.12), 1));
        painter.drawPath(shape);
    }
};

/// A quiet link at the foot of the page: a small mark and a word, full ink under the pointer
class FootLink : public QAbstractButton
{
public:
    enum Mark { Heart, Home, Issue };

    FootLink(const QString &text, Mark mark, const QUrl &url, QWidget *parent)
        : QAbstractButton(parent)
        , m_mark(mark)
    {
        setText(text);
        setToolTip(url.toString());
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_Hover);
        setFocusPolicy(Qt::NoFocus);
        connect(this, &QAbstractButton::clicked, this, [url]() { QDesktopServices::openUrl(url); });
    }

    QSize sizeHint() const override
    {
        return QSize(fontMetrics().horizontalAdvance(text()) + 14 + 2 * KisModernStyle::space2 + KisModernStyle::space1,
                     fontMetrics().height() + 2 * KisModernStyle::space1);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QColor ink = palette().color(QPalette::WindowText);
        if (underMouse()) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(withAlpha(ink, 0.08));
            painter.drawPath(KisModernStyle::squircle(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), KisModernStyle::cornerRadius));
        }
        const QColor shown = withAlpha(ink, underMouse() ? 1.0 : 0.7);
        const QRectF mark(KisModernStyle::space2, (height() - 12) / 2.0, 12, 12);
        if (m_mark == Heart) {
            // two lobes and a point, in the warm red of Krita's donation badge
            QPainterPath heart;
            const QPointF c = mark.center();
            heart.moveTo(c.x(), mark.bottom() - 0.5);
            heart.cubicTo(mark.left() - 1, c.y() + 0.5, mark.left() + 1.5, mark.top() - 1.5, c.x(), mark.top() + 2.6);
            heart.cubicTo(mark.right() - 1.5, mark.top() - 1.5, mark.right() + 1, c.y() + 0.5, c.x(), mark.bottom() - 0.5);
            painter.setPen(Qt::NoPen);
            painter.setBrush(withAlpha(QColor(229, 72, 77), underMouse() ? 1.0 : 0.85));
            painter.drawPath(heart);
        } else {
            painter.setPen(QPen(shown, 1.3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            painter.setBrush(Qt::NoBrush);
            if (m_mark == Home) {
                // the app's own icon
                qApp->windowIcon().paint(&painter, mark.adjusted(-1, -1, 1, 1).toRect());
            } else {
                // a speech bubble
                const QRectF bubble = mark.adjusted(0.5, 1, -0.5, -3);
                QPainterPath path = KisModernStyle::squircle(bubble, 3);
                path.moveTo(bubble.left() + 3, bubble.bottom());
                path.lineTo(bubble.left() + 2.5, mark.bottom() - 0.5);
                path.lineTo(bubble.left() + 6, bubble.bottom());
                painter.drawPath(path);
            }
        }
        painter.setPen(shown);
        painter.drawText(QRectF(mark.right() + KisModernStyle::space1, 0, width(), height()), Qt::AlignLeft | Qt::AlignVCenter, text());
    }

private:
    Mark m_mark;
};

/// A new image of \p pixels at \p pixelsPerPoint, one layer, otherwise as the last image made
void createImage(const QSize &pixels, qreal pixelsPerPoint)
{
    KisConfig cfg(true);
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->colorSpace(cfg.defColorModel(), cfg.defaultColorDepth(), cfg.defColorProfile());
    if (!cs) cs = KoColorSpaceRegistry::instance()->rgb8();
    QColor background = cfg.defaultBackgroundColor(newImageSection);
    background.setAlpha(cfg.defaultBackgroundOpacity(newImageSection));
    KisDocument *doc = KisPart::instance()->createDocument();
    if (!doc->newImage(i18n("Unnamed"), pixels.width(), pixels.height(), cs, KoColor(background, cs), cfg.defaultBackgroundStyle(newImageSection),
                       1, QString(), pixelsPerPoint)) {
        delete doc;
        return;
    }
    doc->setModified(false);
    KisPart::instance()->startCustomDocument(doc);
}

} // namespace

struct KisStudioStartPage::Private
{
    QPointer<KisMainWindow> mainWindow;
    Gallery *gallery {nullptr};
    QPointer<QScrollArea> scroller;
    bool dropping {false};
    /// The last image, blurred and veiled, as big as the page's view
    QPixmap backdrop;
    QString backdropKey;
};

KisStudioStartPage::KisStudioStartPage(KisMainWindow *mainWindow)
    : QWidget(nullptr)
    , d(new Private)
{
    d->mainWindow = mainWindow;
    setObjectName(QStringLiteral("studioStartPage"));
    setAcceptDrops(true);
    setAutoFillBackground(false);

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(KisModernStyle::space4 * 3, KisModernStyle::space4 * 3, KisModernStyle::space4 * 3, KisModernStyle::space4 * 2);
    outer->setSpacing(KisModernStyle::space4 * 2);

    // New and Open, centered
    auto *pill = new Pill(this);
    auto *pillLayout = new QHBoxLayout(pill);
    pillLayout->setContentsMargins(KisModernStyle::space3, KisModernStyle::space3, KisModernStyle::space3, KisModernStyle::space3);
    pillLayout->setSpacing(KisModernStyle::space3);
    pillLayout->addWidget(createNewTiles(mainWindow, pill));
    auto *open = new NewTile(i18nc("@action:button open an image", "Open"), QSize(), pill);
    open->setGlyph(KisIconUtils::loadIcon(QStringLiteral("document-open")));
    connect(open, &QAbstractButton::clicked, this, [this]() {
        if (d->mainWindow) d->mainWindow->slotFileOpen();
    });
    pillLayout->addWidget(open);
    auto *pillRow = new QHBoxLayout();
    pillRow->addStretch(1);
    pillRow->addWidget(pill);
    pillRow->addStretch(1);
    outer->addLayout(pillRow);

    // the recent images, no wider than reads well
    d->gallery = new Gallery(this);
    d->gallery->setMaximumWidth(1480);
    d->gallery->opened = [this](const QString path) {
        if (!d->mainWindow) return;
        // one already open is shown again, not opened twice
        Q_FOREACH (QPointer<KisView> view, KisPart::instance()->views()) {
            if (!view || view->mainWindow() != d->mainWindow || !view->document()) continue;
            const QString open = view->document()->localFilePath();
            if (open.isEmpty() || QFileInfo(open) != QFileInfo(path)) continue;
            if (auto *tab = qobject_cast<QMdiSubWindow *>(view->parentWidget())) {
                if (tab->mdiArea()) tab->mdiArea()->setActiveSubWindow(tab);
            }
            d->mainWindow->showWelcomeScreen(false);
            return;
        }
        d->mainWindow->openDocument(path, KisMainWindow::None);
    };
    // with images open, Esc goes back to them
    d->gallery->canLeave = [this]() { return d->mainWindow && d->mainWindow->activeView(); };
    d->gallery->left = [this]() {
        if (d->mainWindow) d->mainWindow->showWelcomeScreen(false);
    };
    d->gallery->forgotten = [this](const QString &path) {
        if (d->mainWindow) d->mainWindow->removeRecentFile(path);
    };
    auto *galleryRow = new QHBoxLayout();
    galleryRow->addStretch(0);
    galleryRow->addWidget(d->gallery, 1);
    galleryRow->addStretch(0);
    outer->addLayout(galleryRow);
    outer->addStretch(1);

    // at the foot, after your work: the painting app under all this, the project, and where to say what's wrong
    auto *foot = new QHBoxLayout();
    foot->setSpacing(KisModernStyle::space2);
    foot->addStretch(1);
    foot->addWidget(new FootLink(i18nc("@action:button on the start page", "Support Krita"), FootLink::Heart,
                                 QUrl(QStringLiteral("https://krita.org/support-us/donations")), this));
    foot->addWidget(new FootLink(KisProfile::name(), FootLink::Home, QUrl(KisProfile::homepage()), this));
    foot->addWidget(new FootLink(i18nc("@action:button on the start page", "Report a problem"), FootLink::Issue,
                                 QUrl(KisProfile::homepage() + QStringLiteral("/issues")), this));
    foot->addStretch(1);
    outer->addSpacing(KisModernStyle::space4 * 2);
    outer->addLayout(foot);

    Pictures::instance()->watch(this, [this]() { update(); });
    setFocusProxy(d->gallery);
}

QWidget *KisStudioStartPage::createNewTiles(KisMainWindow *mainWindow, QWidget *parent, std::function<void()> picked)
{
    auto *row = new QWidget(parent);
    auto *column = new QVBoxLayout(row);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(KisModernStyle::space3);
    auto *tiles = new QHBoxLayout();
    tiles->setContentsMargins(0, 0, 0, 0);
    tiles->setSpacing(KisModernStyle::space3);
    column->addLayout(tiles);
    KisConfig cfg(true);
    const qreal pixelsPerPoint = cfg.defImageResolution();
    QScreen *screen = mainWindow && mainWindow->screen() ? mainWindow->screen() : QGuiApplication::primaryScreen();
    const QSize screenPixels = screen ? screen->size() * screen->devicePixelRatio() : QSize(1920, 1080);
    struct Preset {
        QString name;
        QSize pixels;
        qreal pixelsPerPoint;
    };
    QList<Preset> presets = {
        {i18nc("a new image's size: as big as the screen", "Screen"), screenPixels, pixelsPerPoint},
        {i18nc("a new image's size", "Square"), QSize(2048, 2048), pixelsPerPoint},
        {i18nc("a new image's size: the paper size, at 300 dpi", "A4"), QSize(2480, 3508), 300.0 / 72.0},
        {i18nc("a new image's size", "4K"), QSize(3840, 2160), pixelsPerPoint},
        {i18nc("a new image's size: a phone's screen, upright", "Portrait"), QSize(1080, 1920), pixelsPerPoint},
    };
    // the size made last, when it's none of these
    const QSize last(cfg.defImageWidth(), cfg.defImageHeight());
    bool known = false;
    for (const Preset &preset : presets) known = known || preset.pixels == last;
    if (!known && last.isValid()) presets.prepend({i18nc("a new image's size: the one made last", "Last"), last, pixelsPerPoint});
    // one scale for all, so the shapes compare: the largest fills its box
    const QRectF box = NewTile::shapeBox();
    qreal scale = 1;
    for (const Preset &preset : presets) {
        scale = qMin(scale, qMin(box.width() * 0.86 / preset.pixels.width(), box.height() * 0.86 / preset.pixels.height()));
    }
    for (const Preset &preset : presets) {
        auto *tile = new NewTile(preset.name, preset.pixels, row);
        tile->setScale(scale);
        const QSize pixels = preset.pixels;
        const qreal ppp = preset.pixelsPerPoint;
        QObject::connect(tile, &QAbstractButton::clicked, row, [pixels, ppp, picked]() {
            if (picked) picked();
            createImage(pixels, ppp);
        });
        tiles->addWidget(tile);
    }
    QPointer<KisMainWindow> window = mainWindow;
    QPointer<NewTile> clipboard = new NewTile(i18nc("a new image: from the image on the clipboard", "Clipboard"), QSize(), row);
    clipboard->setScale(scale);
    QObject::connect(clipboard, &QAbstractButton::clicked, row, [window, picked]() {
        if (picked) picked();
        QAction *paste = window ? window->actionCollection()->action(QStringLiteral("paste_new")) : nullptr;
        if (paste) paste->trigger();
    });
    auto updateClipboard = [clipboard]() {
        if (!clipboard) return;
        const QImage image = QGuiApplication::clipboard() ? QGuiApplication::clipboard()->image() : QImage();
        clipboard->setVisible(!image.isNull());
        if (!image.isNull()) clipboard->setPicture(image.scaled(240, 240, Qt::KeepAspectRatio, Qt::SmoothTransformation), image.size());
    };
    // the clipboard changes with every copy, in other apps too: its picture is made only when the tiles are seen
    auto stale = std::make_shared<bool>(true);
    QObject::connect(KisClipboard::instance(), &KisClipboard::clipChanged, row, [row, stale, updateClipboard]() {
        *stale = true;
        if (row->isVisible()) {
            *stale = false;
            updateClipboard();
        }
    });
    new ShowWatch(row, [stale, updateClipboard]() {
        if (!*stale) return;
        *stale = false;
        updateClipboard();
    });
    if (row->isVisible()) {
        *stale = false;
        updateClipboard();
    }
    tiles->addWidget(clipboard);

    // Custom opens its panel under the tiles, and closes it again
    auto *custom = new NewTile(i18nc("a new image: of a size of your own", "Custom"), QSize(), row);
    custom->setCheckable(true);
    tiles->addWidget(custom);
    QList<SizeStage::Outline> outlines;
    for (const Preset &preset : presets) outlines.append({preset.name, preset.pixels});
    auto *panel = new CustomImage(last.isValid() ? last : QSize(2000, 1400), outlines, screenPixels, row);
    panel->hide();
    column->addWidget(panel);
    // a popup holding the tiles grows and shrinks with them
    auto refit = [row, window]() {
        QWidget *top = row->window();
        if (top && top != window) top->adjustSize();
    };
    QObject::connect(custom, &QAbstractButton::toggled, row, [panel, refit](bool open) {
        panel->setVisible(open);
        if (open) panel->setFocus(Qt::OtherFocusReason);
        refit();
    });
    panel->closed = [custom]() { custom->setChecked(false); };
    panel->made = [picked](const NewImage &image) {
        if (picked) picked();
        // what the next new image starts from
        KisConfig cfg(false);
        cfg.defImageWidth(image.size.width());
        cfg.defImageHeight(image.size.height());
        cfg.defImageResolution(image.dpi / 72.0);
        cfg.defColorModel(image.model);
        cfg.setDefaultColorDepth(image.depth);
        cfg.defColorProfile(image.profile);
        QColor opaque = image.paper;
        opaque.setAlpha(255);
        cfg.setDefaultBackgroundColor(newImageSection, opaque);
        cfg.setDefaultBackgroundOpacity(newImageSection, quint8(image.paper.alpha()));
        createImage(image.size, image.dpi / 72.0);
    };
    return row;
}

KisStudioStartPage::~KisStudioStartPage()
{
    delete d;
}

void KisStudioStartPage::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.fillRect(rect(), palette().color(QPalette::Window));
    // the last image, blurred behind everything, staying put as the page scrolls
    QWidget *viewport = parentWidget();
    const QRect view = viewport ? QRect(mapFrom(viewport, QPoint()), viewport->size()) : rect();
    const QString latest = d->gallery ? d->gallery->latest() : QString();
    const QImage picture = latest.isEmpty() ? QImage() : Pictures::instance()->get(latest, Gallery::pictureLength);
    if (!picture.isNull()) {
        const QSize size = view.size() * devicePixelRatioF();
        const QString key = latest + QLatin1Char('|') + QString::number(picture.cacheKey()) + QLatin1Char('|') + QString::number(size.width())
            + QLatin1Char('x') + QString::number(size.height()) + palette().color(QPalette::Window).name();
        if (key != d->backdropKey) {
            d->backdropKey = key;
            // blurred at a third of the page's size, where stretching it the rest of the way leaves no creases
            const QSize third = (size / 3).expandedTo(QSize(1, 1));
            QImage soft = picture.scaled(third, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
            soft = blurred(soft, qMax(2, qMax(soft.width(), soft.height()) / 36));
            QImage cover = soft.scaled(size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
            cover = cover.copy((cover.width() - size.width()) / 2, (cover.height() - size.height()) / 2, size.width(), size.height());
            // veiled in the window's color, so what is over it reads
            QPainter veil(&cover);
            const QColor window = palette().color(QPalette::Window);
            veil.fillRect(cover.rect(), withAlpha(window, window.lightnessF() < 0.5 ? 0.62 : 0.72));
            veil.end();
            d->backdrop = QPixmap::fromImage(cover);
            d->backdrop.setDevicePixelRatio(devicePixelRatioF());
        }
        painter.drawPixmap(view.topLeft(), d->backdrop);
    }
    if (d->dropping) {
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(QPen(palette().color(QPalette::Highlight), 2));
        painter.setBrush(withAlpha(palette().color(QPalette::Highlight), 0.06));
        painter.drawPath(KisModernStyle::squircle(QRectF(view).adjusted(KisModernStyle::space3, KisModernStyle::space3, -KisModernStyle::space3, -KisModernStyle::space3),
                                                  KisModernStyle::panelRadius));
    }
}

void KisStudioStartPage::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    // the backdrop stays put as the page scrolls, so it is drawn again on each scroll
    QWidget *viewport = parentWidget();
    auto *scroller = viewport ? qobject_cast<QScrollArea *>(viewport->parentWidget()) : nullptr;
    if (scroller && scroller != d->scroller) {
        d->scroller = scroller;
        connect(scroller->verticalScrollBar(), &QScrollBar::valueChanged, this, [this]() { update(); });
    }
    d->gallery->setFocus(Qt::OtherFocusReason);
}

void KisStudioStartPage::dragEnterEvent(QDragEnterEvent *event)
{
    // only files it can open: not links from a browser
    bool files = false;
    Q_FOREACH (const QUrl &url, event->mimeData()->urls()) files = files || url.isLocalFile();
    if (!files) {
        event->ignore();
        return;
    }
    event->acceptProposedAction();
    d->dropping = true;
    update();
}

void KisStudioStartPage::dragLeaveEvent(QDragLeaveEvent *)
{
    d->dropping = false;
    update();
}

void KisStudioStartPage::dropEvent(QDropEvent *event)
{
    d->dropping = false;
    update();
    if (!d->mainWindow) return;
    Q_FOREACH (const QUrl &url, event->mimeData()->urls()) {
        if (!url.isLocalFile()) continue;
        const QString path = url.toLocalFile();
        if (path.endsWith(QLatin1String(".bundle"), Qt::CaseInsensitive)) {
            d->mainWindow->installBundle(path);
        } else {
            d->mainWindow->openDocument(path, KisMainWindow::None);
        }
    }
    event->acceptProposedAction();
}
