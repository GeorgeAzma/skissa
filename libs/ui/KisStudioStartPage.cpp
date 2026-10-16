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
#include <QDragEnterEvent>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHash>
#include <QImageReader>
#include <QLocale>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QStandardItemModel>
#include <QThreadPool>
#include <QDesktopServices>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrentRun>

#include <cmath>

#include <klocalizedstring.h>

#include <KisMimeDatabase.h>
#include <KisModernStyle.h>
#include <KoColor.h>
#include <KoColorSpaceRegistry.h>
#include <KoStore.h>
#include <kis_icon_utils.h>
#include <kis_image.h>
#include <kis_paint_device.h>

#include "KisDocument.h"
#include "KisMainWindow.h"
#include "KisPart.h"
#include <KisProfile.h>
#include "dialogs/KisDlgCreateNewDocument.h"
#include "kis_clipboard.h"
#include "kis_config.h"
#include "utils/KisRecentDocumentsModelWrapper.h"
#include "widgets/KisStudioInlineEdit.h"

namespace
{

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
        const QDateTime time = QFileInfo(path).lastModified();
        const auto found = m_pictures.constFind(key);
        if ((found == m_pictures.constEnd() || found->time != time) && !m_reading.contains(key)) {
            m_reading.insert(key);
            auto *watcher = new QFutureWatcher<QImage>(this);
            connect(watcher, &QFutureWatcher<QImage>::finished, this, [this, watcher, key, time]() {
                m_reading.remove(key);
                m_pictures.insert(key, {watcher->result(), time});
                watcher->deleteLater();
                for (int i = m_viewers.size() - 1; i >= 0; i--) {
                    if (m_viewers[i].first) m_viewers[i].second();
                    else m_viewers.removeAt(i);
                }
            });
            watcher->setFuture(QtConcurrent::run(&m_pool, readPicture, path, longest));
        }
        // a changed file shows its old picture until the new one is read
        return found != m_pictures.constEnd() ? found->image : QImage();
    }

private:
    explicit Pictures(QObject *parent)
        : QObject(parent)
    {
        m_pool.setMaxThreadCount(2);
    }

    struct Picture {
        QImage image;
        QDateTime time;
    };
    QHash<QString, Picture> m_pictures;
    QSet<QString> m_reading;
    QThreadPool m_pool;
    QList<QPair<QPointer<QObject>, std::function<void()>>> m_viewers;
};

/// A size for a new image, drawn as its shape to one scale with the others; or the image on the clipboard, Custom or Open
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
        painter.setBrush(withAlpha(ink, isDown() ? 0.14 : underMouse() ? 0.1 : 0.05));
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
            QColor paper = cfg.defaultBackgroundColor();
            paper.setAlpha(qMax(40, int(cfg.defaultBackgroundOpacity())));
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

/**
 * Custom, opened: a new image's size made by dragging, wider as the pointer goes right and taller as it
 * goes down, by ratios. The numbers are typed on a double-click; Make makes it, ⋯ opens Krita's dialog.
 */
class SizeShaper : public QWidget
{
public:
    std::function<void(const QSize &)> made;
    std::function<void()> more;
    std::function<void()> closed;

    SizeShaper(const QSize &size, QWidget *parent)
        : QWidget(parent)
        , m_size(size)
        , m_initial(size)
    {
        setObjectName(QStringLiteral("studioSizeShaper"));
        setFixedSize(2 * NewTile::tileWidth + KisModernStyle::space3, NewTile::tileHeight);
        setMouseTracking(true);
        setAttribute(Qt::WA_Hover);
        setFocusPolicy(Qt::StrongFocus);
        setToolTip(i18nc("@info:tooltip a new image's size", "Size\nDrag: change\nDouble-click: type\nRight-click: reset"));
    }

    void reset(const QSize &size)
    {
        m_size = size;
        m_initial = size;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QColor ink = palette().color(QPalette::WindowText);
        painter.setPen(Qt::NoPen);
        painter.setBrush(withAlpha(ink, 0.1));
        painter.drawPath(KisModernStyle::squircle(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), KisModernStyle::tileRadius));

        // the shape, with a handle at its corner
        const QRectF box = shapeBox();
        const QSizeF fitted = QSizeF(m_size).scaled(box.size(), Qt::KeepAspectRatio);
        const QRectF shape(box.center() - QPointF(fitted.width(), fitted.height()) / 2, fitted);
        KisConfig cfg(true);
        QColor paper = cfg.defaultBackgroundColor();
        paper.setAlpha(qMax(40, int(cfg.defaultBackgroundOpacity())));
        painter.setBrush(paper);
        painter.setPen(QPen(withAlpha(ink, m_dragging ? 0.6 : 0.3), 1));
        painter.drawPath(KisModernStyle::squircle(shape, 3));
        painter.setPen(Qt::NoPen);
        painter.setBrush(withAlpha(ink, 0.85));
        painter.drawEllipse(shape.bottomRight(), 3.5, 3.5);

        // the numbers, then Make and ⋯
        painter.setPen(ink);
        painter.setFont(numberFont());
        painter.drawText(widthRect(), Qt::AlignRight | Qt::AlignVCenter, QString::number(m_size.width()));
        painter.drawText(timesRect(), Qt::AlignCenter, QStringLiteral("×"));
        painter.drawText(heightRect(), Qt::AlignLeft | Qt::AlignVCenter, QString::number(m_size.height()));

        painter.setFont(font());
        painter.setBrush(withAlpha(ink, m_hovered == Make ? 0.95 : 0.85));
        painter.drawPath(KisModernStyle::squircle(makeRect(), KisModernStyle::cornerRadius));
        painter.setPen(palette().color(QPalette::Window));
        painter.drawText(makeRect(), Qt::AlignCenter, i18nc("@action:button make the new image", "Make"));
        painter.setPen(Qt::NoPen);
        painter.setBrush(withAlpha(ink, m_hovered == More ? 0.16 : 0.08));
        painter.drawPath(KisModernStyle::squircle(moreRect(), KisModernStyle::cornerRadius));
        painter.setPen(ink);
        painter.drawText(moreRect(), Qt::AlignCenter, QStringLiteral("⋯"));
    }

    bool event(QEvent *event) override
    {
        if (event->type() == QEvent::ToolTip) {
            const QPoint at = static_cast<QHelpEvent *>(event)->pos();
            if (moreRect().contains(at)) setToolTip(i18nc("@info:tooltip", "Resolution, color, templates"));
            else if (makeRect().contains(at)) setToolTip(QString());
            else setToolTip(i18nc("@info:tooltip a new image's size", "Size\nDrag: change\nDouble-click: type\nRight-click: reset"));
        }
        return QWidget::event(event);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton) return;
        if (makeRect().contains(event->pos())) {
            if (made) made(m_size);
        } else if (moreRect().contains(event->pos())) {
            if (more) more();
        } else if (!widthRect().contains(event->pos()) && !heightRect().contains(event->pos())) {
            m_dragging = true;
            m_from = event->pos();
            m_fromSize = m_size;
            update();
        }
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (!m_dragging) {
            const Part hovered = makeRect().contains(event->pos()) ? Make : moreRect().contains(event->pos()) ? More : None;
            setCursor(hovered != None ? Qt::PointingHandCursor
                      : widthRect().contains(event->pos()) || heightRect().contains(event->pos()) ? Qt::IBeamCursor
                                                                                                   : Qt::SizeFDiagCursor);
            if (hovered != m_hovered) {
                m_hovered = hovered;
                update();
            }
            return;
        }
        // by ratios, a tenth as fast with Shift; even sizes otherwise
        const bool fine = event->modifiers() & Qt::ShiftModifier;
        const qreal pace = fine ? 1200 : 120;
        const QPoint moved = event->pos() - m_from;
        auto scaled = [fine, pace](int value, int by) {
            const qreal v = value * std::exp(by / pace);
            const int step = fine ? 1 : 8;
            return qBound(16, int(std::round(v / step)) * step, 16384);
        };
        m_size = QSize(scaled(m_fromSize.width(), moved.x()), scaled(m_fromSize.height(), moved.y()));
        update();
    }

    void mouseReleaseEvent(QMouseEvent *) override
    {
        m_dragging = false;
        update();
    }

    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        const bool across = widthRect().contains(event->pos());
        if (!across && !heightRect().contains(event->pos())) return;
        const QRect at = across ? widthRect() : heightRect();
        KisStudioInlineEdit::open(this, at, QString::number(across ? m_size.width() : m_size.height()),
                                  (across ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter, numberFont(), [this, across](const QString &typed) {
                                      bool ok = false;
                                      const int value = qBound(1, typed.trimmed().toInt(&ok), 16384);
                                      if (!ok) return;
                                      if (across) m_size.setWidth(value);
                                      else m_size.setHeight(value);
                                      update();
                                  });
    }

    void contextMenuEvent(QContextMenuEvent *event) override
    {
        event->accept();
        m_size = m_initial;
        update();
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
            if (made) made(m_size);
        } else if (event->key() == Qt::Key_Escape) {
            if (closed) closed();
        } else {
            QWidget::keyPressEvent(event);
        }
    }

    void leaveEvent(QEvent *) override
    {
        m_hovered = None;
        update();
    }

private:
    enum Part { None, Make, More };

    QFont numberFont() const { return KisModernStyle::textFont(font(), 1.15, QFont::DemiBold); }
    QRectF shapeBox() const { return QRectF(KisModernStyle::space3, KisModernStyle::space3, NewTile::tileWidth - KisModernStyle::space3, height() - 2 * KisModernStyle::space3); }
    int columnLeft() const { return NewTile::tileWidth + KisModernStyle::space3; }
    int columnWidth() const { return width() - columnLeft() - KisModernStyle::space3; }
    QRect timesRect() const
    {
        const int side = QFontMetrics(numberFont()).horizontalAdvance(QStringLiteral(" × "));
        return QRect(columnLeft() + (columnWidth() - side) / 2, KisModernStyle::space4 * 2, side, QFontMetrics(numberFont()).height());
    }
    QRect widthRect() const
    {
        const QRect times = timesRect();
        return QRect(columnLeft(), times.top(), times.left() - columnLeft(), times.height());
    }
    QRect heightRect() const
    {
        const QRect times = timesRect();
        return QRect(times.right() + 1, times.top(), columnLeft() + columnWidth() - times.right() - 1, times.height());
    }
    QRect makeRect() const
    {
        const int h = fontMetrics().height() + 2 * KisModernStyle::space2;
        return QRect(columnLeft(), height() - KisModernStyle::space3 - h, columnWidth() - h - KisModernStyle::space1, h);
    }
    QRect moreRect() const
    {
        const QRect make = makeRect();
        return QRect(make.right() + 1 + KisModernStyle::space1, make.top(), make.height(), make.height());
    }

    QSize m_size;
    QSize m_initial;
    QSize m_fromSize;
    QPoint m_from;
    bool m_dragging {false};
    Part m_hovered {None};
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

    std::function<void(const QString &path)> opened;
    std::function<void(const QString &path)> forgotten;

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
    void paintEvent(QPaintEvent *) override
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
            const Item &item = m_items[placed.item];
            const qreal radius = placed.hero ? KisModernStyle::panelRadius : KisModernStyle::cardRadius;
            const QPainterPath shape = KisModernStyle::squircle(placed.rect, radius);
            const QImage picture = Pictures::instance()->get(item.path, placed.hero ? heroLength : pictureLength);
            if (picture.isNull()) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(withAlpha(ink, 0.06));
                painter.drawPath(shape);
            } else {
                painter.save();
                painter.setClipPath(shape);
                painter.drawImage(placed.rect, picture);
                painter.restore();
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
                || key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter || (key->key() == Qt::Key_Escape && !m_filter.isEmpty());
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
            setFilter(QString());
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

/// A new image of \p pixels at \p pixelsPerPoint, otherwise as the last image made
void createImage(const QSize &pixels, qreal pixelsPerPoint)
{
    KisConfig cfg(true);
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->colorSpace(cfg.defColorModel(), cfg.defaultColorDepth(), cfg.defColorProfile());
    if (!cs) cs = KoColorSpaceRegistry::instance()->rgb8();
    QColor background = cfg.defaultBackgroundColor();
    background.setAlpha(cfg.defaultBackgroundOpacity());
    KisDocument *doc = KisPart::instance()->createDocument();
    if (!doc->newImage(i18n("Unnamed"), pixels.width(), pixels.height(), cs, KoColor(background, cs), cfg.defaultBackgroundStyle(),
                       cfg.numDefaultLayers(), QString(), pixelsPerPoint)) {
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
    d->gallery->opened = [this](const QString &path) {
        if (d->mainWindow) d->mainWindow->openDocument(path, KisMainWindow::None);
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
    auto *tiles = new QHBoxLayout(row);
    tiles->setContentsMargins(0, 0, 0, 0);
    tiles->setSpacing(KisModernStyle::space3);
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
        auto *dialog = new KisDlgCreateNewDocument(window);
        dialog->SelectPage(KisDlgCreateNewDocument::Page::CreateFromClipboard);
        dialog->exec();
        dialog->deleteLater();
    });
    auto updateClipboard = [clipboard]() {
        if (!clipboard) return;
        const QImage image = QGuiApplication::clipboard() ? QGuiApplication::clipboard()->image() : QImage();
        clipboard->setVisible(!image.isNull());
        if (!image.isNull()) clipboard->setPicture(image.scaled(240, 240, Qt::KeepAspectRatio, Qt::SmoothTransformation), image.size());
    };
    QObject::connect(KisClipboard::instance(), &KisClipboard::clipChanged, row, updateClipboard);
    updateClipboard();
    tiles->addWidget(clipboard);

    // Custom opens into a size to drag, in its place
    auto *custom = new NewTile(i18nc("a new image: of a size of your own", "Custom"), QSize(), row);
    auto *shaper = new SizeShaper(last.isValid() ? last : QSize(2000, 1400), row);
    shaper->hide();
    tiles->addWidget(custom);
    tiles->addWidget(shaper);
    // a popup holding the tiles grows and shrinks with them
    auto refit = [row, window]() {
        QWidget *top = row->window();
        if (top && top != window) top->adjustSize();
    };
    QObject::connect(custom, &QAbstractButton::clicked, row, [custom, shaper, refit]() {
        custom->hide();
        shaper->show();
        shaper->setFocus(Qt::OtherFocusReason);
        refit();
    });
    shaper->closed = [custom, shaper, refit]() {
        shaper->hide();
        custom->show();
        refit();
    };
    shaper->made = [picked, pixelsPerPoint](const QSize &size) {
        if (picked) picked();
        // the next Last
        KisConfig cfg(false);
        cfg.defImageWidth(size.width());
        cfg.defImageHeight(size.height());
        createImage(size, pixelsPerPoint);
    };
    shaper->more = [window, picked]() {
        if (picked) picked();
        if (window) window->showNewImageDialog();
    };
    tiles->addStretch(1);
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
