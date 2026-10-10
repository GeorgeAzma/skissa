/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioPicker.h"

#include "KisPopupCloser.h"
#include "KisStudioMotion.h"
#include "KisTouchMode.h"
#include "kis_config.h"

#include <QAbstractButton>
#include <QApplication>
#include <QGuiApplication>
#include <QContextMenuEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QRadialGradient>
#include <QLinearGradient>
#include <QPainterPath>
#include <QPointer>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QTimer>
#include <QVBoxLayout>

#include <KSharedConfig>
#include <kconfiggroup.h>
#include <kfts_fuzzy_match.h>
#include <klocalizedstring.h>

#include <algorithm>
#include <cmath>

namespace {

constexpr int shadowMargin = 16;
constexpr int headerHeight = 26;

/// The size of one item and how many go in a row
struct Metrics {
    int columns;
    int cellWidth;
    int cellHeight;
};

Metrics metricsFor(bool tiles, const QSize &tileSize = QSize())
{
    if (tiles && tileSize.isValid()) {
        // as many across as fit where four of the usual ones do
        return Metrics{qMax(2, 4 * 124 / tileSize.width()), tileSize.width(), tileSize.height()};
    }
    return tiles ? Metrics{4, 124, 96} : Metrics{3, 196, 32};
}

struct Item {
    QString id;
    QString name;
    QRect rect;
    int order {-1}; ///< the place in your list, -1 for the others
};

struct Header {
    QString title;
    QRect rect;
};

/// Where an item's picture goes
QRect previewRect(const QRect &cell, bool tiles, bool numbered)
{
    if (tiles) {
        return QRect(cell.left() + 6, cell.top() + 6, cell.width() - 12, cell.height() - 32);
    }
    return QRect(cell.left() + (numbered ? 22 : 8), cell.center().y() - 10, 30, 20);
}

QRectF starRect(const QRect &cell, bool tiles)
{
    if (tiles) {
        const QRect preview = previewRect(cell, true, false);
        return QRectF(preview.right() - 18, preview.top() + 5, 13, 13);
    }
    return QRectF(cell.right() - 20, cell.center().y() - 6.5, 13, 13);
}

QPolygonF starShape(const QRectF &r)
{
    QPolygonF star;
    for (int i = 0; i < 10; i++) {
        const qreal radius = (i % 2 ? 0.42 : 1.0) * r.width() / 2;
        const qreal angle = M_PI / 2 + i * M_PI / 5;
        star << r.center() + QPointF(radius * std::cos(angle), -radius * std::sin(angle));
    }
    return star;
}

} // namespace

struct KisStudioPicker::Private
{
    KisStudioPicker *q {nullptr};
    Source source;
    Metrics m {metricsFor(false)};
    QString original;
    QSet<QString> available;
    QStringList pinned;
    std::function<void()> relayout;
    bool showAll {false};
    bool done {false};
    int highlighted {-1};
    QTimer previewTimer;
    QString pendingPreview;

    QLineEdit *search {nullptr};
    QWidget *grid {nullptr};
    QScrollArea *scroll {nullptr};
    QAbstractButton *allButton {nullptr};
    QHBoxLayout *footer {nullptr};
    /// Another way to see them, and the link that switches to it
    QPointer<QWidget> alternate;
    QAbstractButton *alternateButton {nullptr};
    QLabel *hint {nullptr};

    QVector<Item> items;
    QVector<Header> headers;
    /// Where your list is, a drop there puts an item in it; empty while searching
    QRect pinnedArea;

    bool isAvailable(const QString &id) const { return available.isEmpty() || available.contains(id); }
    bool searching() const { return !search->text().trimmed().isEmpty(); }

    void setPinned(const QStringList &ids)
    {
        pinned = ids;
        KisStudioPicker::setPinnedIds(source, ids);
        if (relayout) relayout();
    }

    /// The item pointed at or moved to with the keys: highlighted, and previewed
    void point(int index)
    {
        if (index == highlighted) return;
        highlighted = index;
        grid->update();
        previewTimer.stop();
        if (index < 0 || !isAvailable(items[index].id)) return;
        if (source.previewDelay <= 0) {
            Q_EMIT q->sigPreview(items[index].id);
        } else {
            // the slow kinds wait for the pointer to rest
            pendingPreview = items[index].id;
            previewTimer.start(source.previewDelay);
        }
    }

    void pick(int index)
    {
        if (index < 0 || !isAvailable(items[index].id)) return;
        done = true;
        previewTimer.stop();
        Q_EMIT q->sigPreview(QString());
        Q_EMIT q->sigPicked(items[index].id);
        q->close();
    }
};

namespace {

/**
 * The painted items inside the picker. With a mouse or a pen: a click picks,
 * a drag moves the item (within your list to reorder it, into it to add it,
 * out of it to take it off), a right click adds it to your list or takes it
 * off. With a finger (KisTouchMode), which does not hover: a tap picks, a
 * swipe scrolls, and holding previews the item; let go to pick it, or move
 * to carry it like a drag. Every item shows its star then.
 */
class Grid : public QWidget
{
public:
    explicit Grid(KisStudioPicker::Private *d) : QWidget(nullptr), m_d(d)
    {
        setMouseTracking(true);
        // holding means more than a right click here, see above
        setProperty("ownLongPress", true);
        m_hold.setSingleShot(true);
        m_hold.setInterval(KisTouchMode::longPressDelay);
        QObject::connect(&m_hold, &QTimer::timeout, this, [this]() {
            if (m_press < 0) return;
            m_lifted = true;
            m_d->point(m_press);
        });
        QObject::connect(KisTouchMode::instance(), &KisTouchMode::changed, this, [this]() { update(); });
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QPalette &pal = palette();
        const bool tiles = m_d->source.tiles;
        QColor dim = pal.color(QPalette::WindowText);
        dim.setAlphaF(0.5);

        QFont headerFont = font();
        headerFont.setPointSizeF(headerFont.pointSizeF() * 0.8);
        headerFont.setBold(true);
        headerFont.setCapitalization(QFont::AllUppercase);
        headerFont.setLetterSpacing(QFont::PercentageSpacing, 106);
        painter.setFont(headerFont);
        painter.setPen(dim);
        Q_FOREACH (const Header &header, m_d->headers) {
            painter.drawText(header.rect.adjusted(8, 0, 0, -2), Qt::AlignLeft | Qt::AlignBottom, header.title);
        }

        QFont numberFont = font();
        numberFont.setPointSizeF(numberFont.pointSizeF() * 0.8);

        for (int i = 0; i < m_d->items.size(); i++) {
            const Item &item = m_d->items[i];
            const bool current = item.id == m_d->original;
            const bool highlighted = i == m_d->highlighted && m_dragging < 0;
            const bool enabled = m_d->isAvailable(item.id);
            painter.setOpacity(i == m_dragging ? 0.3 : 1.0);

            if (highlighted || current) {
                const QColor base = pal.color(QPalette::Window);
                const QColor select = pal.color(QPalette::Highlight);
                const qreal t = highlighted ? 0.55 : 0.28;
                painter.setPen(Qt::NoPen);
                painter.setBrush(QColor::fromRgbF(base.redF() + (select.redF() - base.redF()) * t,
                                                  base.greenF() + (select.greenF() - base.greenF()) * t,
                                                  base.blueF() + (select.blueF() - base.blueF()) * t));
                painter.drawRoundedRect(QRectF(item.rect).adjusted(2, 1, -2, -1), tiles ? 8 : 6, tiles ? 8 : 6);
            }

            const QRect preview = previewRect(item.rect, tiles, item.order >= 0);
            QPainterPath clip;
            clip.addRoundedRect(QRectF(preview), tiles ? 6 : 4, tiles ? 6 : 4);
            painter.save();
            painter.setClipPath(clip);
            painter.setOpacity(painter.opacity() * (enabled ? 1.0 : 0.35));
            if (m_d->source.paintPreview) m_d->source.paintPreview(painter, preview, item.id);
            painter.restore();

            // yours are numbered: the order they come in
            if (item.order >= 0) {
                painter.setFont(numberFont);
                const QString number = QString::number(item.order + 1);
                if (tiles) {
                    const QRect badge(preview.left() + 4, preview.top() + 4, painter.fontMetrics().horizontalAdvance(number) + 10, 16);
                    painter.setPen(Qt::NoPen);
                    painter.setBrush(QColor(0, 0, 0, 140));
                    painter.drawRoundedRect(QRectF(badge), 8, 8);
                    painter.setPen(Qt::white);
                    painter.drawText(badge, Qt::AlignCenter, number);
                } else {
                    QColor numberInk = pal.color(QPalette::WindowText);
                    numberInk.setAlphaF(0.35);
                    painter.setPen(numberInk);
                    painter.drawText(QRect(item.rect.left() + 2, item.rect.top(), 18, item.rect.height()),
                                     Qt::AlignRight | Qt::AlignVCenter, number);
                }
            }

            painter.setFont(font());
            painter.setPen(enabled ? pal.color(QPalette::WindowText) : dim);
            if (tiles) {
                const QRect text(item.rect.left() + 8, preview.bottom() + 2, item.rect.width() - 16, item.rect.bottom() - preview.bottom() - 4);
                painter.drawText(text, Qt::AlignLeft | Qt::AlignVCenter, painter.fontMetrics().elidedText(item.name, Qt::ElideRight, text.width()));
            } else {
                const int textLeft = preview.right() + 9 - item.rect.left();
                painter.drawText(item.rect.adjusted(textLeft, 0, -24, 0), Qt::AlignVCenter | Qt::AlignLeft,
                                 painter.fontMetrics().elidedText(item.name, Qt::ElideRight, item.rect.width() - textLeft - 26));
            }

            // the star of the one pointed at: full for yours, empty for the others; among search
            // matches and recent ones yours always show theirs, as there is no other way to tell
            const bool pinned = item.order >= 0 || m_d->pinned.contains(item.id);
            if (highlighted || (pinned && item.order < 0) || KisTouchMode::isActive()) {
                const QRectF star = starRect(item.rect, tiles);
                if (tiles) {
                    painter.setPen(Qt::NoPen);
                    painter.setBrush(QColor(0, 0, 0, 120));
                    painter.drawEllipse(star.center(), 10, 10);
                }
                QColor ink = pinned ? pal.color(QPalette::Link) : (tiles ? QColor(Qt::white) : pal.color(QPalette::WindowText));
                ink.setAlphaF(pinned ? 0.95 : (tiles ? 0.8 : 0.45));
                painter.setPen(QPen(ink, 1.1));
                painter.setBrush(pinned ? ink : Qt::NoBrush);
                painter.drawPolygon(starShape(star));
            }
        }
        painter.setOpacity(1.0);

        // where a dragged item would go
        if (m_dragging >= 0) {
            if (!m_dropLine.isNull()) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(pal.color(QPalette::Highlight));
                painter.drawRoundedRect(QRectF(m_dropLine), 1.5, 1.5);
            } else if (m_dropUnpins) {
                QColor wash = pal.color(QPalette::WindowText);
                wash.setAlphaF(0.06);
                painter.setPen(Qt::NoPen);
                painter.setBrush(wash);
                painter.drawRoundedRect(QRectF(0, m_d->pinnedArea.bottom() + 4, width(), height() - m_d->pinnedArea.bottom() - 4), 8, 8);
            }
            // the item itself, beside the pointer, where it does not hide the line
            const Item &dragged = m_d->items[m_dragging];
            const QSize size = tiles ? QSize(m_d->m.cellWidth - 8, m_d->m.cellHeight - 8) : QSize(m_d->m.cellWidth - 8, m_d->m.cellHeight - 2);
            const QRect ghost(m_pointer + QPoint(14, 10), size);
            painter.setPen(QPen(pal.color(QPalette::Highlight), 1));
            painter.setBrush(pal.color(QPalette::Window).lighter(125));
            painter.drawRoundedRect(QRectF(ghost), 6, 6);
            const QRect preview = previewRect(ghost, tiles, false);
            painter.save();
            QPainterPath clip;
            clip.addRoundedRect(QRectF(preview), 4, 4);
            painter.setClipPath(clip);
            if (m_d->source.paintPreview) m_d->source.paintPreview(painter, preview, dragged.id);
            painter.restore();
            painter.setPen(pal.color(QPalette::WindowText));
            const QRect text = tiles ? QRect(ghost.left() + 8, preview.bottom() + 2, ghost.width() - 16, ghost.bottom() - preview.bottom() - 2)
                                     : ghost.adjusted(preview.right() + 9 - ghost.left(), 0, -6, 0);
            painter.drawText(text, Qt::AlignVCenter | Qt::AlignLeft, painter.fontMetrics().elidedText(dragged.name, Qt::ElideRight, text.width()));
        }
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        m_press = event->button() == Qt::LeftButton ? itemAt(event->pos()) : -1;
        m_pressPos = event->pos();
        m_pressGlobal = event->globalPos();
        m_touch = KisTouchMode::isActive() && event->source() != Qt::MouseEventNotSynthesized && event->button() == Qt::LeftButton;
        m_lifted = false;
        m_scrolling = false;
        if (m_touch) {
            m_pressScroll = m_d->scroll->verticalScrollBar()->value();
            if (m_press >= 0 && !m_d->searching()) m_hold.start();
        }
    }

    // a right click: into your list, or off it
    void contextMenuEvent(QContextMenuEvent *event) override
    {
        event->accept();
        const int index = itemAt(event->pos());
        if (index >= 0) togglePinned(m_d->items[index].id);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (m_touch && (event->buttons() & Qt::LeftButton)) {
            const QPoint moved = event->globalPos() - m_pressGlobal;
            if (m_dragging < 0 && moved.manhattanLength() > QApplication::startDragDistance()) {
                m_hold.stop();
                if (m_lifted && m_press >= 0) {
                    // held, then moved: carried like a drag
                    m_dragging = m_press;
                    m_d->point(-1);
                } else {
                    m_scrolling = true;
                }
            }
            if (m_dragging >= 0) {
                m_pointer = event->pos();
                updateDrop();
                update();
            } else if (m_scrolling) {
                m_d->scroll->verticalScrollBar()->setValue(m_pressScroll - moved.y());
            }
            return;
        }
        if (m_press >= 0 && (event->buttons() & Qt::LeftButton)) {
            if (m_dragging < 0 && (event->pos() - m_pressPos).manhattanLength() >= QApplication::startDragDistance()
                && !m_d->searching()) {
                m_dragging = m_press;
                m_d->point(-1);
            }
            if (m_dragging >= 0) {
                m_pointer = event->pos();
                updateDrop();
                update();
                return;
            }
        }
        m_d->point(itemAt(event->pos()));
    }

    void leaveEvent(QEvent *) override
    {
        if (m_dragging < 0) m_d->point(-1);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        const int pressed = m_press;
        const bool touch = m_touch;
        const bool scrolled = m_scrolling;
        m_press = -1;
        m_touch = false;
        m_scrolling = false;
        m_hold.stop();
        if (m_dragging >= 0) {
            drop();
            return;
        }
        if (scrolled) {
            return;
        }
        const int index = itemAt(event->pos());
        if (index < 0 || index != pressed || event->button() != Qt::LeftButton) {
            // a finger lifted elsewhere: whatever it was previewing is not wanted
            if (touch) {
                m_d->point(-1);
                Q_EMIT m_d->q->sigPreview(QString());
            }
            return;
        }
        // the star adds it to your list or takes it off, the rest picks it
        const Item &item = m_d->items[index];
        if (!m_lifted && starRect(item.rect, m_d->source.tiles).adjusted(-4, -6, 4, 6).contains(event->pos())) {
            togglePinned(item.id);
            return;
        }
        m_d->pick(index);
    }

private:
    void togglePinned(const QString &id)
    {
        QStringList pinned = m_d->pinned;
        if (pinned.contains(id)) pinned.removeAll(id);
        else pinned << id;
        m_d->setPinned(pinned);
    }

    int itemAt(const QPoint &pos) const
    {
        for (int i = 0; i < m_d->items.size(); i++) {
            if (m_d->items[i].rect.contains(pos)) return i;
        }
        return -1;
    }

    /// Where the dragged item goes: a place in your list, next to the one nearest the pointer, or out of it
    void updateDrop()
    {
        m_dropAt = -1;
        m_dropLine = QRect();
        m_dropUnpins = false;
        if (!m_d->pinnedArea.adjusted(0, 0, 0, m_d->m.cellHeight / 2).contains(m_pointer)) {
            m_dropUnpins = m_d->items[m_dragging].order >= 0;
            return;
        }
        int nearest = -1;
        qreal distance = 0;
        for (int i = 0; i < m_d->items.size(); i++) {
            if (m_d->items[i].order < 0) continue;
            const QPointF offset = QPointF(m_pointer) - QRectF(m_d->items[i].rect).center();
            const qreal d = offset.x() * offset.x() + offset.y() * offset.y();
            if (nearest < 0 || d < distance) {
                nearest = i;
                distance = d;
            }
        }
        if (nearest < 0) {
            m_dropAt = m_d->pinned.size();
            return;
        }
        // rows are read downwards, tiles across
        const QRect rect = m_d->items[nearest].rect;
        if (m_d->source.tiles) {
            const bool after = m_pointer.x() > rect.center().x();
            m_dropLine = QRect(after ? rect.right() - 1 : rect.left() - 1, rect.top() + 6, 3, rect.height() - 12);
            m_dropAt = m_d->items[nearest].order + (after ? 1 : 0);
        } else {
            const bool after = m_pointer.y() > rect.center().y();
            m_dropLine = QRect(rect.left() + 6, after ? rect.bottom() - 1 : rect.top() - 2, rect.width() - 12, 3);
            m_dropAt = m_d->items[nearest].order + (after ? 1 : 0);
        }
    }

    void drop()
    {
        const QString id = m_d->items[m_dragging].id;
        const int from = m_d->pinned.indexOf(id);
        QStringList pinned = m_d->pinned;
        if (m_dropAt >= 0) {
            int to = m_dropAt;
            if (from >= 0) {
                pinned.removeAt(from);
                if (to > from) to--;
            }
            pinned.insert(qBound(0, to, pinned.size()), id);
        } else if (m_dropUnpins) {
            pinned.removeAll(id);
        }
        m_dragging = -1;
        m_dropLine = QRect();
        m_dropUnpins = false;
        if (pinned != m_d->pinned) {
            m_d->setPinned(pinned);
        } else {
            update();
        }
    }

    KisStudioPicker::Private *m_d;
    int m_press {-1};
    QPoint m_pressPos;
    QPoint m_pressGlobal;
    int m_pressScroll {0};
    /// A finger, see the class comment
    bool m_touch {false};
    bool m_lifted {false};
    bool m_scrolling {false};
    QTimer m_hold;
    int m_dragging {-1};
    QPoint m_pointer;
    int m_dropAt {-1};       ///< the place in your list it would take
    bool m_dropUnpins {false};
    QRect m_dropLine;
};

/// A text button that looks like a link
class LinkButton : public QAbstractButton
{
public:
    using QAbstractButton::QAbstractButton;
    QSize sizeHint() const override
    {
        return QSize(fontMetrics().horizontalAdvance(QString(text()).remove(QLatin1Char('&'))) + 12, 24);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        QColor color = palette().color(QPalette::Link);
        if (!underMouse()) color.setAlphaF(0.85);
        painter.setPen(color);
        painter.drawText(rect(), Qt::AlignVCenter | Qt::AlignRight | Qt::TextHideMnemonic, text());
    }
};

/// How well \p query fits \p text: the start of it beats the start of a word beats the middle; 0 is not at all
int matchScore(const QString &query, const QString &text)
{
    const int at = text.indexOf(query, 0, Qt::CaseInsensitive);
    if (at < 0) return 0;
    const bool wordStart = at == 0 || !text.at(at - 1).isLetterOrNumber();
    return 1000 - (wordStart ? 0 : 200) - at;
}

} // namespace

QImage KisStudioPicker::samplePicture(const QSize &size)
{
    QImage image(size, QImage::Format_ARGB32);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QRectF r(QPointF(0, 0), QSizeF(size));
    QLinearGradient sky(r.topLeft(), r.bottomLeft());
    sky.setColorAt(0.0, QColor(72, 104, 168));
    sky.setColorAt(0.6, QColor(226, 160, 122));
    sky.setColorAt(1.0, QColor(244, 214, 168));
    painter.fillRect(r, sky);
    QRadialGradient sun(QPointF(r.width() * 0.68, r.height() * 0.38), r.height() * 0.3);
    sun.setColorAt(0.0, QColor(255, 246, 214));
    sun.setColorAt(0.45, QColor(255, 210, 120, 230));
    sun.setColorAt(1.0, QColor(255, 180, 90, 0));
    painter.fillRect(r, sun);
    QPainterPath hills;
    hills.moveTo(0, r.height() * 0.72);
    hills.cubicTo(r.width() * 0.25, r.height() * 0.5, r.width() * 0.45, r.height() * 0.86, r.width() * 0.7, r.height() * 0.66);
    hills.cubicTo(r.width() * 0.85, r.height() * 0.56, r.width() * 0.95, r.height() * 0.7, r.width(), r.height() * 0.64);
    hills.lineTo(r.width(), r.height());
    hills.lineTo(0, r.height());
    hills.closeSubpath();
    painter.fillPath(hills, QColor(46, 70, 62));
    // fine dark lines, for what works on edges and detail
    painter.setPen(QPen(QColor(24, 30, 36), qMax(1.0, r.height() / 48)));
    for (int i = 0; i < 5; i++) {
        const qreal x = r.width() * (0.12 + 0.06 * i);
        painter.drawLine(QPointF(x, r.height() * 0.62), QPointF(x + r.width() * 0.03, r.height() * (0.3 + 0.04 * i)));
    }
    return image;
}

bool KisStudioPicker::isFlat(const QImage &image)
{
    if (image.isNull()) {
        return true;
    }
    int lowest = 255 * 3, highest = 0, lowestAlpha = 255, highestAlpha = 0;
    for (int y = 0; y < image.height(); y += qMax(1, image.height() / 16)) {
        for (int x = 0; x < image.width(); x += qMax(1, image.width() / 16)) {
            const QRgb pixel = image.pixel(x, y);
            const int value = qRed(pixel) + qGreen(pixel) + qBlue(pixel);
            lowest = qMin(lowest, value);
            highest = qMax(highest, value);
            lowestAlpha = qMin(lowestAlpha, qAlpha(pixel));
            highestAlpha = qMax(highestAlpha, qAlpha(pixel));
        }
    }
    // all transparent is flat too, whatever the color channels hold
    return highestAlpha < 8 || (highest - lowest < 24 && highestAlpha - lowestAlpha < 10);
}

QStringList KisStudioPicker::pinnedIds(const Source &source)
{
    QStringList ids;
    // an empty list is a list too: the key says whether you made one
    if (KSharedConfig::openConfig()->group(QString()).hasKey(source.pinnedKey)) {
        ids = KisConfig(true).readEntry<QStringList>(source.pinnedKey, QStringList());
    } else if (source.initialPinned) {
        ids = source.initialPinned();
    }
    // ones this version does not have
    const QStringList all = source.allIds();
    const QSet<QString> known(all.begin(), all.end());
    ids.erase(std::remove_if(ids.begin(), ids.end(), [&known](const QString &id) { return !known.contains(id); }), ids.end());
    ids.removeDuplicates();
    return ids;
}

void KisStudioPicker::setPinnedIds(const Source &source, const QStringList &ids)
{
    KisConfig(false).writeEntry(source.pinnedKey, ids);
}

QStringList KisStudioPicker::scrollOrder(const Source &source)
{
    QStringList order = pinnedIds(source);
    Q_FOREACH (const Group &group, source.otherGroups(order)) {
        order << group.ids;
    }
    return order;
}

KisStudioPicker::KisStudioPicker(const Source &source, const QString &current, const QSet<QString> &available, QWidget *parent)
    // not Qt::Popup: translucent popups render a black background on Windows
    : QFrame(parent, Qt::Tool | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint)
    , d(new Private)
{
    d->q = this;
    d->source = source;
    d->m = metricsFor(source.tiles, source.tileSize);
    d->original = current;
    d->available = available;
    d->pinned = pinnedIds(source);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_DeleteOnClose);

    d->previewTimer.setSingleShot(true);
    connect(&d->previewTimer, &QTimer::timeout, this, [this]() {
        if (!d->pendingPreview.isEmpty()) Q_EMIT sigPreview(d->pendingPreview);
    });

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(shadowMargin + 10, shadowMargin + 10, shadowMargin + 10, shadowMargin + 8);
    layout->setSpacing(4);

    d->search = new QLineEdit(this);
    d->search->setPlaceholderText(source.searchPlaceholder);
    d->search->setMinimumHeight(32);
    d->search->installEventFilter(this);
    layout->addWidget(d->search);

    d->grid = new Grid(d.get());
    d->scroll = new QScrollArea(this);
    d->scroll->setWidget(d->grid);
    d->scroll->setFrameShape(QFrame::NoFrame);
    d->scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    d->scroll->setWidgetResizable(false);
    d->scroll->viewport()->setAutoFillBackground(false);
    d->grid->setAutoFillBackground(false);
    layout->addWidget(d->scroll);

    auto *footer = new QHBoxLayout();
    d->footer = footer;
    footer->setContentsMargins(8, 2, 4, 0);
    d->hint = new QLabel(i18nc("@info", "Drag to reorder, the star adds or removes"), this);
    QPalette hintPalette = d->hint->palette();
    QColor hintInk = hintPalette.color(QPalette::WindowText);
    hintInk.setAlphaF(0.45);
    hintPalette.setColor(QPalette::WindowText, hintInk);
    d->hint->setPalette(hintPalette);
    footer->addWidget(d->hint, 1);
    auto *all = new LinkButton(this);
    all->setCursor(Qt::PointingHandCursor);
    d->allButton = all;
    footer->addWidget(all);
    layout->addLayout(footer);

    // recent ones, yours (numbered), then the others in groups; or the matches
    d->relayout = [this]() {
        d->items.clear();
        d->headers.clear();
        d->pinnedArea = QRect();
        const Metrics &m = d->m;
        const bool tiles = d->source.tiles;
        const int width = m.columns * m.cellWidth;
        const QString query = d->search->text().trimmed();
        auto nameOf = [this](const QString &id) { return d->source.name(id); };

        // left to right, then down
        auto placeAcross = [&](const QStringList &ids, int top, int firstOrder) {
            for (int i = 0; i < ids.size(); i++) {
                const QRect rect((i % m.columns) * m.cellWidth, top + (i / m.columns) * m.cellHeight, m.cellWidth, m.cellHeight);
                const int order = firstOrder >= 0 ? firstOrder + i : -1;
                const QString name = order >= 0 && d->source.pinnedName ? d->source.pinnedName(ids[i]) : nameOf(ids[i]);
                d->items << Item{ids[i], name, rect, order};
            }
            return top + std::max(1, int((ids.size() + m.columns - 1) / m.columns)) * m.cellHeight;
        };
        // down, then the next column: a list in columns
        auto placeDown = [&](const QStringList &ids, int top, int firstOrder) {
            const int perColumn = std::max(1, int((ids.size() + m.columns - 1) / m.columns));
            for (int i = 0; i < ids.size(); i++) {
                const QRect rect((i / perColumn) * m.cellWidth, top + (i % perColumn) * m.cellHeight, m.cellWidth, m.cellHeight);
                const int order = firstOrder >= 0 ? firstOrder + i : -1;
                const QString name = order >= 0 && d->source.pinnedName ? d->source.pinnedName(ids[i]) : nameOf(ids[i]);
                d->items << Item{ids[i], name, rect, order};
            }
            return top + perColumn * m.cellHeight;
        };
        auto header = [&](const QString &title, int x, int y, int w) {
            d->headers << Header{title, QRect(x, y, w, headerHeight)};
            return y + headerHeight;
        };

        int height = 0;
        if (!query.isEmpty()) {
            QVector<QPair<int, QString>> scored;
            Q_FOREACH (const QString &id, d->source.allIds()) {
                int score = matchScore(query, nameOf(id));
                if (d->source.aliases) {
                    Q_FOREACH (const QString &alias, d->source.aliases(id)) {
                        score = std::max(score, matchScore(query, alias) - 50);
                    }
                }
                if (score > 0) scored.append(qMakePair(score, id));
            }
            // a typo or a few letters of each word: only when the plain matches are few
            if (scored.size() < 6) {
                Q_FOREACH (const QString &id, d->source.allIds()) {
                    int score = 0;
                    const bool have = std::any_of(scored.begin(), scored.end(), [&id](const auto &s) { return s.second == id; });
                    if (!have && kfts::fuzzy_match(query, nameOf(id), score)) {
                        scored.append(qMakePair(score, id));
                    }
                }
            }
            std::stable_sort(scored.begin(), scored.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
            QStringList matches;
            for (const auto &s : scored) matches << s.second;
            const int top = header(i18nc("picker group", "Matches"), 0, 0, width);
            height = tiles ? placeAcross(matches, top, -1) : placeDown(matches, top, -1);
        } else {
            int y = 0;
            // recently used, not yours yet: the next ones to star
            if (d->source.recent) {
                QStringList recent;
                const QStringList all = d->source.allIds();
                Q_FOREACH (const QString &id, d->source.recent()) {
                    if (!d->pinned.contains(id) && all.contains(id) && recent.size() < m.columns) recent << id;
                }
                if (!recent.isEmpty()) {
                    y = placeAcross(recent, header(i18nc("picker group", "Recent"), 0, y, width), -1) + 8;
                }
            }
            const int yoursTop = y;
            y = header(d->source.yoursTitle, 0, y, width);
            y = tiles ? placeAcross(d->pinned, y, 0) : placeDown(d->pinned, y, 0);
            d->pinnedArea = QRect(0, yoursTop, width, y - yoursTop);
            height = y;

            if (d->showAll) {
                const QVector<Group> groups = d->source.otherGroups(d->pinned);
                if (tiles) {
                    // one group after another, across the whole width
                    y += 8;
                    Q_FOREACH (const Group &group, groups) {
                        y = placeAcross(group.ids, header(group.title, 0, y, width), -1) + 4;
                    }
                    height = y;
                } else {
                    // the groups over the columns, the shortest column next
                    QVector<int> heights(m.columns, y + 8);
                    Q_FOREACH (const Group &group, groups) {
                        const int c = int(std::min_element(heights.begin(), heights.end()) - heights.begin());
                        int top = header(group.title, c * m.cellWidth, heights[c], m.cellWidth);
                        Q_FOREACH (const QString &id, group.ids) {
                            d->items << Item{id, nameOf(id), QRect(c * m.cellWidth, top, m.cellWidth, m.cellHeight), -1};
                            top += m.cellHeight;
                        }
                        heights[c] = top;
                    }
                    height = *std::max_element(heights.begin(), heights.end());
                }
            }
        }

        d->grid->setFixedSize(width, std::max(height, m.cellHeight));
        const int maxHeight = tiles ? 540 : 460;
        d->scroll->setFixedSize(width + (height > maxHeight ? d->scroll->verticalScrollBar()->sizeHint().width() : 0),
                                std::min(height, maxHeight) + 2);
        d->scroll->setVerticalScrollBarPolicy(height > maxHeight ? Qt::ScrollBarAsNeeded : Qt::ScrollBarAlwaysOff);
        d->allButton->setText(d->showAll ? i18nc("@action:button", "Only yours")
                                         : d->source.allLabel.arg(d->source.allIds().size()));
        d->allButton->setVisible(query.isEmpty());
        d->hint->setVisible(query.isEmpty());

        d->highlighted = -1;
        for (int i = 0; i < d->items.size(); i++) {
            if (d->items[i].id == d->original) d->highlighted = i;
        }
        if (!query.isEmpty()) d->highlighted = d->items.isEmpty() ? -1 : 0;
        d->grid->update();
        adjustSize();
    };

    connect(d->search, &QLineEdit::textChanged, this, [this]() { d->relayout(); });
    connect(all, &QAbstractButton::clicked, this, [this]() {
        d->showAll = !d->showAll;
        d->relayout();
    });
    // a current one that is not yours: show it among the others
    d->showAll = !current.isEmpty() && !d->pinned.contains(current);
    d->relayout();
}

KisStudioPicker::~KisStudioPicker() = default;

void KisStudioPicker::updatePreviews()
{
    d->grid->update();
    if (d->alternate) d->alternate->update();
}

void KisStudioPicker::addAlternateView(QWidget *view, const QString &label, const QString &rememberedKey)
{
    d->alternate = view;
    view->setParent(this);
    view->hide();
    // where the list is, the list hidden while it shows
    if (auto *box = qobject_cast<QVBoxLayout *>(layout())) {
        box->insertWidget(box->indexOf(d->scroll) + 1, view);
    }
    auto *link = new LinkButton(this);
    link->setCursor(Qt::PointingHandCursor);
    d->alternateButton = link;
    d->footer->insertWidget(d->footer->count() - 1, link);
    d->footer->insertSpacing(d->footer->count() - 1, 14);
    auto apply = [this, label](bool on) {
        d->alternate->setVisible(on);
        d->scroll->setVisible(!on);
        d->search->setVisible(!on);
        d->allButton->setVisible(!on && d->search->text().trimmed().isEmpty());
        d->hint->setVisible(!on && d->search->text().trimmed().isEmpty());
        d->alternateButton->setText(on ? i18nc("@action:button back to the list", "List") : label);
        adjustSize();
    };
    connect(link, &QAbstractButton::clicked, this, [this, apply, rememberedKey]() {
        const bool on = !d->alternate->isVisible();
        KisConfig(false).writeEntry(rememberedKey, on);
        apply(on);
        if (!on) d->search->setFocus();
    });
    apply(KisConfig(true).readEntry<bool>(rememberedKey, false));
}

void KisStudioPicker::pickId(const QString &id)
{
    if (!d->isAvailable(id)) return;
    d->done = true;
    d->previewTimer.stop();
    Q_EMIT sigPreview(QString());
    Q_EMIT sigPicked(id);
    close();
}

void KisStudioPicker::popupBelow(QWidget *anchor)
{
    adjustSize();
    popupAt(anchor->mapToGlobal(QPoint(0, anchor->height())) - QPoint(shadowMargin, shadowMargin - 4), anchor);
}

void KisStudioPicker::popupNear(const QPoint &globalPos)
{
    adjustSize();
    // the search field just above the pointer, the items under it
    popupAt(globalPos - QPoint(width() / 2, shadowMargin + 26), nullptr);
}

void KisStudioPicker::popupLeftOf(QWidget *anchor)
{
    adjustSize();
    popupAt(anchor->mapToGlobal(QPoint(0, anchor->height())) - QPoint(width() - shadowMargin + 4, height() - shadowMargin), anchor);
}

void KisStudioPicker::popupAt(QPoint pos, QWidget *anchor)
{
    QScreen *screen = anchor ? anchor->screen() : QGuiApplication::screenAt(pos + QPoint(width() / 2, shadowMargin + 26));
    if (screen) {
        const QRect available = screen->availableGeometry();
        pos.setX(qBound(available.left(), pos.x(), available.right() - width()));
        pos.setY(qBound(available.top(), pos.y(), available.bottom() - height()));
    }
    move(pos);
    new KisPopupCloser(this, anchor);
    KisStudioMotion::showFaded(this);
    raise();
    activateWindow();
    d->search->setFocus();
    // the current one in view, also when it is far down among all the others
    if (d->highlighted >= 0) {
        const QRect rect = d->items[d->highlighted].rect;
        d->scroll->ensureVisible(rect.center().x(), rect.center().y(), 0, d->m.cellHeight * 2);
    }
}

bool KisStudioPicker::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == d->search && event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        const int count = d->items.size();
        auto move = [this, count](int delta) {
            if (!count) return;
            const int index = d->highlighted < 0 ? 0 : qBound(0, d->highlighted + delta, count - 1);
            d->point(index);
            d->scroll->ensureVisible(d->items[index].rect.center().x(), d->items[index].rect.center().y(), 0, d->m.cellHeight);
        };
        // tiles go across, so up and down are a row away
        const int vertical = d->source.tiles ? d->m.columns : 1;
        switch (key->key()) {
        case Qt::Key_Down: move(vertical); return true;
        case Qt::Key_Up: move(-vertical); return true;
        case Qt::Key_Right:
            if (d->source.tiles && d->search->cursorPosition() == d->search->text().size()) { move(1); return true; }
            break;
        case Qt::Key_Left:
            if (d->source.tiles && d->search->text().isEmpty()) { move(-1); return true; }
            break;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            d->pick(d->highlighted);
            return true;
        case Qt::Key_Escape:
            close();
            return true;
        default:
            break;
        }
    }
    return QFrame::eventFilter(watched, event);
}

void KisStudioPicker::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QRectF panel = QRectF(rect()).adjusted(shadowMargin, shadowMargin, -shadowMargin, -shadowMargin);
    const qreal radius = 12;
    for (int i = shadowMargin; i > 0; i -= 2) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0, 0, 0, int(34.0 * (1.0 - qreal(i) / shadowMargin) + 2)));
        painter.drawRoundedRect(panel.adjusted(-i, -i + 4, i, i + 4), radius + i, radius + i);
    }
    QColor border = palette().color(QPalette::WindowText);
    border.setAlphaF(0.08);
    painter.setPen(QPen(border, 1));
    painter.setBrush(palette().color(QPalette::Window));
    painter.drawRoundedRect(panel.adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
}

void KisStudioPicker::hideEvent(QHideEvent *event)
{
    // closed without picking: back to how it was
    if (!d->done) {
        d->done = true;
        d->previewTimer.stop();
        Q_EMIT sigPreview(QString());
    }
    QFrame::hideEvent(event);
}
