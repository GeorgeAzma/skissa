/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioPicker.h"
#include "KisStudioGlass.h"

#include "KisModernStyle.h"
#include "KisPopupCloser.h"
#include "KisPopupSelfActivatingLineEdit.h"
#include "KisStudioMotion.h"
#include "KisTouchMode.h"
#include "kis_config.h"

#include <QAbstractButton>
#include <QApplication>
#include <QGuiApplication>
#include <QContextMenuEvent>
#include <QHBoxLayout>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidgetAction>

#include <KSharedConfig>
#include <kconfiggroup.h>
#include <kfts_fuzzy_match.h>
#include <klocalizedstring.h>

#include <algorithm>
#include <cmath>

namespace {

constexpr int glassShadowMargin = 16;
constexpr int headerHeight = 26;


/// Moves \p pos so that a window of \p size placed there fits on \p screen
QPoint onScreen(QPoint pos, const QSize &size, const QScreen *screen)
{
    if (screen) {
        const QRect available = screen->availableGeometry();
        pos.setX(qBound(available.left(), pos.x(), available.right() - size.width()));
        pos.setY(qBound(available.top(), pos.y(), available.bottom() - size.height()));
    }
    return pos;
}

struct Metrics {
    int columns;
    int cellWidth;
    int cellHeight;
};

Metrics metricsFor(bool tiles, const QSize &tileSize = QSize())
{
    if (tiles && tileSize.isValid()) {
        // as many columns as fit in the width of four default tiles
        return Metrics{qMax(2, 4 * 124 / tileSize.width()), tileSize.width(), tileSize.height()};
    }
    return tiles ? Metrics{4, 124, 96} : Metrics{3, 196, 32};
}

struct Item {
    QString id;
    QString name;
    QRect rect;
    int order {-1}; ///< position in its list of the user's, or -1
    int list {-1};  ///< which of the user's lists it is in: 0 their own, then their groups; or -1
};

struct Header {
    QString title;
    QRect rect;
    int list {-1};  ///< which of the user's lists it heads: 0 their own, then their groups; or -1
};

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
    bool done {false};
    int highlighted {-1};
    QTimer previewTimer;
    QString pendingPreview;

    QLineEdit *search {nullptr};
    QWidget *grid {nullptr};
    QScrollArea *scroll {nullptr};
    QHBoxLayout *footer {nullptr};
    QPointer<QWidget> alternate;
    QAbstractButton *alternateButton {nullptr};

    QVector<Group> groups;
    /// The name of the user's own list, which they can rename when they can make groups
    QString yoursTitle;

    QVector<Item> items;
    QVector<Header> headers;
    /// Where each of the user's lists is, their own and then each group, where a drop puts an item; empty while searching
    QVector<QRect> listAreas;

    bool isAvailable(const QString &id) const { return available.isEmpty() || available.contains(id); }
    bool searching() const { return !search->text().trimmed().isEmpty(); }
    QStringList listIds(int list) const { return list == 0 ? pinned : groups.value(list - 1).ids; }

    QStringList grouped() const
    {
        QStringList ids;
        Q_FOREACH (const Group &group, groups) ids << group.ids;
        return ids;
    }

    /// Keeps the user's list and groups; a group left empty goes
    void setLists(const QStringList &newPinned, QVector<Group> newGroups)
    {
        newGroups.erase(std::remove_if(newGroups.begin(), newGroups.end(), [](const Group &group) { return group.ids.isEmpty(); }),
                        newGroups.end());
        if (newPinned != pinned) {
            pinned = newPinned;
            KisStudioPicker::setPinnedIds(source, pinned);
        }
        if (newGroups != groups) {
            groups = newGroups;
            KisStudioPicker::setUserGroups(source, groups);
        }
        if (relayout) relayout();
    }

    void setPinned(const QStringList &ids) { setLists(ids, groups); }

    void setYoursTitle(const QString &title)
    {
        yoursTitle = title;
        KisConfig(false).writeEntry(source.groupsKey + QStringLiteral("Yours"), title);
        if (relayout) relayout();
    }

    /// Highlights and previews the item under the pointer or selected with the keys
    void point(int index)
    {
        if (index >= items.size()) index = -1;
        if (index == highlighted) return;
        for (int i : {highlighted, index}) {
            if (i >= 0 && i < items.size()) grid->update(items[i].rect);
        }
        highlighted = index;
        previewTimer.stop();
        if (index < 0 || !isAvailable(items[index].id)) return;
        if (source.previewDelay <= 0) {
            Q_EMIT q->sigPreview(items[index].id);
        } else {
            pendingPreview = items[index].id;
            previewTimer.start(source.previewDelay);
        }
    }

    /// Ends the preview for good
    void finish()
    {
        done = true;
        previewTimer.stop();
        Q_EMIT q->sigPreview(QString());
    }

    void pickId(const QString &id)
    {
        if (!isAvailable(id)) return;
        finish();
        Q_EMIT q->sigPicked(id);
        q->close();
    }

    void pick(int index)
    {
        if (index >= 0 && index < items.size()) pickId(items[index].id);
    }
};

namespace {

int groupNamed(const QVector<KisStudioPicker::Group> &groups, const QString &title)
{
    for (int i = 0; i < groups.size(); i++) {
        if (groups[i].title.compare(title, Qt::CaseInsensitive) == 0) return i;
    }
    return -1;
}

/// One of the user's lists in an item's menu: its name, and ✕ to take the item out of it or + to put it in
class ListRow : public QWidget
{
public:
    ListRow(const QString &name, bool in, std::function<void()> toggle, QWidget *parent)
        : QWidget(parent)
        , m_name(name)
        , m_in(in)
        , m_toggle(std::move(toggle))
    {
        setMouseTracking(true);
        setAttribute(Qt::WA_Hover);
        setFixedHeight(28);
        setMinimumWidth(qMax(180, fontMetrics().horizontalAdvance(name) + 64));
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QColor ink = palette().color(QPalette::Text);
        const QRectF box = QRectF(rect()).adjusted(KisModernStyle::space1, 1, -KisModernStyle::space1, -1);
        if (underMouse()) {
            QColor hover = ink;
            hover.setAlphaF(0.08);
            painter.fillPath(KisModernStyle::squircle(box, KisModernStyle::cornerRadius), hover);
        }
        // the ones it is in in ink, the others quiet
        const QColor text = m_in ? ink : KisModernStyle::secondaryInk(ink);
        painter.setPen(text);
        const QRectF name = box.adjusted(KisModernStyle::space2, 0, -28, 0);
        painter.drawText(name, Qt::AlignLeft | Qt::AlignVCenter, fontMetrics().elidedText(m_name, Qt::ElideRight, int(name.width())));

        const QPointF mark(box.right() - 14, box.center().y());
        const qreal arm = m_in ? 3.5 : 4.5;
        painter.setPen(QPen(underMouse() ? ink : text, 1.5, Qt::SolidLine, Qt::RoundCap));
        if (m_in) {
            painter.drawLine(mark + QPointF(-arm, -arm), mark + QPointF(arm, arm));
            painter.drawLine(mark + QPointF(-arm, arm), mark + QPointF(arm, -arm));
        } else {
            painter.drawLine(mark + QPointF(-arm, 0), mark + QPointF(arm, 0));
            painter.drawLine(mark + QPointF(0, -arm), mark + QPointF(0, arm));
        }
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        // the menu would take it as a click on nothing
        event->accept();
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton || !rect().contains(event->pos())) return;
        m_toggle();
        m_in = !m_in;
        update();
    }

    bool event(QEvent *event) override
    {
        if (event->type() == QEvent::HoverEnter || event->type() == QEvent::HoverLeave) update();
        return QWidget::event(event);
    }

private:
    QString m_name;
    bool m_in {false};
    std::function<void()> m_toggle;
};

/**
 * The items. A click picks, a drag reorders, pins or unpins, or moves an item
 * between the user's groups, and a right click pins or unpins, or lists the
 * groups to put it in. A click on the title of one of the user's lists renames it. In touch mode a tap picks, a swipe scrolls and a long press
 * previews; releasing after it picks, moving drags.
 */
class Grid : public QWidget
{
public:
    explicit Grid(KisStudioPicker::Private *d) : QWidget(nullptr), m_d(d)
    {
        setMouseTracking(true);
        // a long press previews or drags here instead of opening a context menu
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

    /// Drops the pressed or dragged item, whose index means nothing once the items are laid out again
    void forgetPress()
    {
        m_press = -1;
        m_dragging = -1;
        m_lifted = false;
        m_hold.stop();
        m_dropList = -1;
        m_dropAt = -1;
        m_dropLine = QRect();
        m_dropRemoves = false;
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QPalette &pal = palette();
        const bool tiles = m_d->source.tiles;
        const QColor dim = KisModernStyle::secondaryInk(pal.color(QPalette::WindowText));

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
            if (!event->rect().intersects(item.rect)) continue;
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
                painter.drawPath(KisModernStyle::squircle(QRectF(item.rect).adjusted(2, 1, -2, -1), tiles ? 8 : 6));
            }

            const QRect preview = previewRect(item.rect, tiles, item.list == 0);
            QPainterPath clip;
            clip.addPath(KisModernStyle::squircle(QRectF(preview), tiles ? 6 : 4));
            painter.save();
            painter.setClipPath(clip);
            painter.setOpacity(painter.opacity() * (enabled ? 1.0 : 0.35));
            if (m_d->source.paintPreview) m_d->source.paintPreview(painter, preview, item.id);
            painter.restore();

            if (item.list == 0) {
                painter.setFont(numberFont);
                const QString number = QString::number(item.order + 1);
                if (tiles) {
                    const QRect badge(preview.left() + 4, preview.top() + 4, painter.fontMetrics().horizontalAdvance(number) + 10, 16);
                    painter.setPen(Qt::NoPen);
                    painter.setBrush(QColor(0, 0, 0, 140));
                    painter.drawPath(KisModernStyle::squircle(QRectF(badge), 8));
                    painter.setPen(Qt::white);
                    painter.drawText(badge, Qt::AlignCenter, number);
                } else {
                    QColor numberInk = pal.color(QPalette::WindowText);
                    numberInk.setAlphaF(0.5);
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

            // among matches and recent items, pinned ones always show their star: nothing else marks them
            const bool pinned = item.list == 0 || m_d->pinned.contains(item.id);
            if (highlighted || (pinned && item.list != 0) || KisTouchMode::isActive()) {
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

        if (m_dragging >= 0) {
            if (!m_dropLine.isNull()) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(pal.color(QPalette::Highlight));
                painter.drawPath(KisModernStyle::squircle(QRectF(m_dropLine), 1.5));
            } else if (m_dropRemoves) {
                // everywhere but the list it comes out of
                QColor wash = pal.color(QPalette::WindowText);
                wash.setAlphaF(0.06);
                painter.setPen(Qt::NoPen);
                painter.setBrush(wash);
                const QRect from = m_d->listAreas.value(m_d->items[m_dragging].list);
                if (from.top() > 8) painter.drawPath(KisModernStyle::squircle(QRectF(0, 0, width(), from.top() - 4), 8));
                if (from.bottom() + 8 < height()) {
                    painter.drawPath(KisModernStyle::squircle(QRectF(0, from.bottom() + 4, width(), height() - from.bottom() - 4), 8));
                }
            }
            // the dragged item, offset from the pointer so that it doesn't hide the drop line
            const Item &dragged = m_d->items[m_dragging];
            const QSize size = tiles ? QSize(m_d->m.cellWidth - 8, m_d->m.cellHeight - 8) : QSize(m_d->m.cellWidth - 8, m_d->m.cellHeight - 2);
            const QRect ghost(m_pointer + QPoint(14, 10), size);
            painter.setPen(QPen(pal.color(QPalette::Highlight), 1));
            painter.setBrush(pal.color(QPalette::Window).lighter(125));
            painter.drawPath(KisModernStyle::squircle(QRectF(ghost), 6));
            const QRect preview = previewRect(ghost, tiles, false);
            painter.save();
            QPainterPath clip;
            clip.addPath(KisModernStyle::squircle(QRectF(preview), 4));
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
        m_pressHeader = event->button() == Qt::LeftButton ? headerAt(event->pos()) : -1;
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

    void contextMenuEvent(QContextMenuEvent *event) override
    {
        event->accept();
        // pinning would re-lay out the items under the drag
        if (m_dragging >= 0) return;
        // inside popups Qt can give the position in the window's coordinates
        const QPoint pos = mapFromGlobal(event->globalPos());
        const int header = headerAt(pos);
        if (header >= 0) {
            groupMenu(m_d->headers[header].list, event->globalPos());
            return;
        }
        const int index = itemAt(pos);
        if (index < 0) return;
        if (m_d->source.groupsKey.isEmpty()) togglePinned(m_d->items[index].id);
        else listsMenu(m_d->items[index].id, event->globalPos());
    }

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == m_renaming && event->type() == QEvent::KeyPress && static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape) {
            m_finishRename(false);
            return true;
        }
        return QWidget::eventFilter(watched, event);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (m_touch && (event->buttons() & Qt::LeftButton)) {
            const QPoint moved = event->globalPos() - m_pressGlobal;
            if (m_dragging < 0 && moved.manhattanLength() > QApplication::startDragDistance()) {
                m_hold.stop();
                if (m_lifted && m_press >= 0) {
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
        // a title that is renamed by a click on it
        if (headerAt(event->pos()) >= 0) setCursor(Qt::IBeamCursor);
        else unsetCursor();
    }

    void leaveEvent(QEvent *) override
    {
        if (m_dragging < 0) m_d->point(-1);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        const int pressed = m_press;
        const int pressedHeader = m_pressHeader;
        const bool touch = m_touch;
        const bool scrolled = m_scrolling;
        m_press = -1;
        m_pressHeader = -1;
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
        if (pressedHeader >= 0 && event->button() == Qt::LeftButton && headerAt(event->pos()) == pressedHeader) {
            rename(m_d->headers[pressedHeader].list);
            return;
        }
        const int index = itemAt(event->pos());
        if (index < 0 || index != pressed || event->button() != Qt::LeftButton) {
            if (touch) {
                m_d->point(-1);
                Q_EMIT m_d->q->sigPreview(QString());
            }
            return;
        }
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

    /// Takes \p id out of the group named \p title or puts it in, making the group if there is none
    void toggleGrouped(const QString &id, const QString &title, bool onlyAdd = false)
    {
        QVector<KisStudioPicker::Group> groups = m_d->groups;
        const int at = groupNamed(groups, title);
        if (at < 0) {
            groups << KisStudioPicker::Group{title, {id}};
        } else if (!groups[at].ids.contains(id)) {
            groups[at].ids << id;
        } else if (!onlyAdd) {
            groups[at].ids.removeAll(id);
        }
        m_d->setLists(m_d->pinned, groups);
    }

    /// The user's list and groups, each with ✕ to take \p id out of it or + to put it in, then a new group typed
    void listsMenu(const QString &id, const QPoint &at)
    {
        QMenu menu(this);
        auto addRow = [&menu](const QString &name, bool in, std::function<void()> toggle) {
            auto *action = new QWidgetAction(&menu);
            action->setDefaultWidget(new ListRow(name, in, std::move(toggle), &menu));
            menu.addAction(action);
        };
        addRow(m_d->yoursTitle, m_d->pinned.contains(id), [this, id]() { togglePinned(id); });
        Q_FOREACH (const KisStudioPicker::Group &group, m_d->groups) {
            const QString title = group.title;
            addRow(title, group.ids.contains(id), [this, id, title]() { toggleGrouped(id, title); });
        }
        auto *box = new QWidget(&menu);
        auto *layout = new QHBoxLayout(box);
        layout->setContentsMargins(KisModernStyle::space1, KisModernStyle::space1, KisModernStyle::space1, KisModernStyle::space1);
        auto *field = new KisPopupSelfActivatingLineEdit(box);
        field->setPlaceholderText(i18nc("@info:placeholder", "New group"));
        layout->addWidget(field);
        auto *fieldAction = new QWidgetAction(&menu);
        fieldAction->setDefaultWidget(box);
        menu.addAction(fieldAction);
        QObject::connect(field, &QLineEdit::returnPressed, &menu, [this, &menu, field, id]() {
            const QString title = field->text().trimmed();
            if (title.isEmpty()) return;
            toggleGrouped(id, title, true);
            menu.close();
        });
        menu.exec(at);
    }

    /// A list's title: rename, and for a group, move or delete it; the user's own list stays first
    void groupMenu(int list, const QPoint &at)
    {
        if (list < 0 || list > m_d->groups.size()) return;
        const int group = list - 1;
        QMenu menu(this);
        QAction *renaming = menu.addAction(i18nc("@action:inmenu", "Rename"));
        QAction *up = group > 0 ? menu.addAction(i18nc("@action:inmenu", "Move up")) : nullptr;
        QAction *down = group >= 0 && group < m_d->groups.size() - 1 ? menu.addAction(i18nc("@action:inmenu", "Move down")) : nullptr;
        QAction *removing = group >= 0 ? menu.addAction(i18nc("@action:inmenu", "Delete")) : nullptr;
        QAction *chosen = menu.exec(at);
        if (!chosen) return;
        if (chosen == renaming) {
            rename(list);
            return;
        }
        QVector<KisStudioPicker::Group> groups = m_d->groups;
        if (chosen == up) std::swap(groups[group], groups[group - 1]);
        else if (chosen == down) std::swap(groups[group], groups[group + 1]);
        else if (chosen == removing) groups.removeAt(group);
        m_d->setLists(m_d->pinned, groups);
    }

    /// The list's name typed in place of its title; a group given another group's name joins it
    void rename(int list)
    {
        if (list < 0 || list > m_d->groups.size() || m_renaming) return;
        const auto header = std::find_if(m_d->headers.cbegin(), m_d->headers.cend(), [list](const Header &h) { return h.list == list; });
        if (header == m_d->headers.cend()) return;
        const QString was = list == 0 ? m_d->yoursTitle : m_d->groups[list - 1].title;
        auto *field = new QLineEdit(was, this);
        field->setGeometry(header->rect.adjusted(KisModernStyle::space1, KisModernStyle::hairline, -KisModernStyle::space1, 0));
        field->selectAll();
        field->installEventFilter(this);
        field->show();
        field->setFocus();
        m_renaming = field;
        m_finishRename = [this, field, was, list](bool keep) {
            if (m_renaming != field) return;
            m_renaming = nullptr;
            const QString title = field->text().trimmed();
            field->deleteLater();
            m_d->search->setFocus();
            if (list == 0) {
                if (keep && !title.isEmpty() && title != was) m_d->setYoursTitle(title);
                return;
            }
            QVector<KisStudioPicker::Group> groups = m_d->groups;
            const int at = groupNamed(groups, was);
            if (!keep || title.isEmpty() || title == was || at < 0) return;
            const int other = groupNamed(groups, title);
            if (other >= 0 && other != at) {
                groups[other].ids << groups[at].ids;
                groups[other].ids.removeDuplicates();
                groups.removeAt(at);
            } else {
                groups[at].title = title;
            }
            m_d->setLists(m_d->pinned, groups);
        };
        QObject::connect(field, &QLineEdit::editingFinished, this, [this]() { m_finishRename(true); });
    }

    int itemAt(const QPoint &pos) const
    {
        for (int i = 0; i < m_d->items.size(); i++) {
            if (m_d->items[i].rect.contains(pos)) return i;
        }
        return -1;
    }

    /// The title of one of the user's lists at \p pos, or -1
    int headerAt(const QPoint &pos) const
    {
        for (int i = 0; i < m_d->headers.size(); i++) {
            if (m_d->headers[i].list >= 0 && m_d->headers[i].rect.contains(pos)) return i;
        }
        return -1;
    }

    /// Finds where the dragged item would go: next to the item nearest the pointer in the list of the user's under
    /// it, or out of its list
    void updateDrop()
    {
        m_dropList = -1;
        m_dropAt = -1;
        m_dropLine = QRect();
        m_dropRemoves = false;
        // a little past a list's end still reaches it, unless that is the next list
        for (int pass = 0; pass < 2 && m_dropList < 0; pass++) {
            for (int list = 0; list < m_d->listAreas.size() && m_dropList < 0; list++) {
                const QRect area = m_d->listAreas[list].adjusted(0, 0, 0, pass ? m_d->m.cellHeight / 2 : 0);
                if (area.contains(m_pointer)) m_dropList = list;
            }
        }
        if (m_dropList < 0) {
            m_dropRemoves = m_d->items[m_dragging].list >= 0;
            return;
        }
        int nearest = -1;
        qreal distance = 0;
        for (int i = 0; i < m_d->items.size(); i++) {
            if (m_d->items[i].list != m_dropList) continue;
            const QPointF offset = QPointF(m_pointer) - QRectF(m_d->items[i].rect).center();
            const qreal d = offset.x() * offset.x() + offset.y() * offset.y();
            if (nearest < 0 || d < distance) {
                nearest = i;
                distance = d;
            }
        }
        if (nearest < 0) {
            m_dropAt = m_d->listIds(m_dropList).size();
            return;
        }
        // rows are ordered downwards, tiles across
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
        const Item dragged = m_d->items[m_dragging];
        QStringList pinned = m_d->pinned;
        QVector<KisStudioPicker::Group> groups = m_d->groups;
        auto idsOf = [&pinned, &groups](int list) -> QStringList & { return list == 0 ? pinned : groups[list - 1].ids; };
        if (m_dropList >= 0) {
            QStringList &to = idsOf(m_dropList);
            int at = m_dropAt;
            const int was = to.indexOf(dragged.id);
            if (was >= 0) {
                to.removeAt(was);
                if (at > was) at--;
            }
            to.insert(qBound(0, at, to.size()), dragged.id);
            // moved here from another of the user's lists
            if (dragged.list >= 0 && dragged.list != m_dropList) idsOf(dragged.list).removeAll(dragged.id);
        } else if (m_dropRemoves) {
            idsOf(dragged.list).removeAll(dragged.id);
        }
        m_dragging = -1;
        m_dropList = -1;
        m_dropLine = QRect();
        m_dropRemoves = false;
        if (pinned != m_d->pinned || groups != m_d->groups) {
            m_d->setLists(pinned, groups);
        } else {
            update();
        }
    }

    KisStudioPicker::Private *m_d;
    int m_press {-1};
    int m_pressHeader {-1};
    QPoint m_pressPos;
    QPoint m_pressGlobal;
    int m_pressScroll {0};
    bool m_touch {false};
    bool m_lifted {false};
    bool m_scrolling {false};
    QTimer m_hold;
    int m_dragging {-1};
    QPoint m_pointer;
    int m_dropList {-1};     ///< which of the user's lists it goes into
    int m_dropAt {-1};       ///< target position in that list
    bool m_dropRemoves {false};
    QRect m_dropLine;
    QPointer<QLineEdit> m_renaming;
    std::function<void(bool keep)> m_finishRename;
};

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

} // namespace

int KisStudioPicker::matchScore(const QString &query, const QString &text)
{
    const int at = text.indexOf(query, 0, Qt::CaseInsensitive);
    if (at < 0) return 0;
    const bool wordStart = at == 0 || !text.at(at - 1).isLetterOrNumber();
    return 1000 - (wordStart ? 0 : 200) - at;
}

QImage KisStudioPicker::samplePicture(const QSize &size)
{
    // Krita's splash art, loaded once at a size large enough for any sample
    static const QImage art = []() {
        QImage image(QStringLiteral(":/splash/0.png"));
        if (image.isNull()) image.load(QStringLiteral(":/splash/hd.jpg"));
        return image.isNull() ? image : image.scaledToWidth(640, Qt::SmoothTransformation).convertToFormat(QImage::Format_ARGB32);
    }();
    if (art.isNull()) {
        QImage plain(size, QImage::Format_ARGB32);
        plain.fill(QColor(128, 128, 128));
        return plain;
    }
    const QImage covering = art.scaled(size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    return covering.copy((covering.width() - size.width()) / 2, (covering.height() - size.height()) / 2, size.width(), size.height());
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
    return highestAlpha < 8 || (highest - lowest < 24 && highestAlpha - lowestAlpha < 10);
}

namespace {
QSet<QString> knownIds(const KisStudioPicker::Source &source)
{
    const QStringList all = source.allIds();
    return QSet<QString>(all.begin(), all.end());
}
} // namespace

QStringList KisStudioPicker::pinnedIds(const Source &source)
{
    QStringList ids;
    // an empty list is valid too: whether the key exists tells if the user made one
    if (KSharedConfig::openConfig()->group(QString()).hasKey(source.pinnedKey)) {
        ids = KisConfig(true).readEntry<QStringList>(source.pinnedKey, QStringList());
    } else if (source.initialPinned) {
        ids = source.initialPinned();
    }
    const QSet<QString> known = knownIds(source);
    ids.erase(std::remove_if(ids.begin(), ids.end(), [&known](const QString &id) { return !known.contains(id); }), ids.end());
    ids.removeDuplicates();
    return ids;
}

void KisStudioPicker::setPinnedIds(const Source &source, const QStringList &ids)
{
    // keep ids unknown right now (a plugin not loaded, another version's) for when they return
    QStringList kept = ids;
    const QSet<QString> known = knownIds(source);
    Q_FOREACH (const QString &id, KisConfig(true).readEntry<QStringList>(source.pinnedKey, QStringList())) {
        if (!known.contains(id) && !kept.contains(id)) kept << id;
    }
    KisConfig(false).writeEntry(source.pinnedKey, kept);
}

namespace {
QVector<KisStudioPicker::Group> storedGroups(const KisStudioPicker::Source &source)
{
    if (!KSharedConfig::openConfig()->group(QString()).hasKey(source.groupsKey)) {
        return source.initialGroups ? source.initialGroups() : QVector<KisStudioPicker::Group>();
    }
    QVector<KisStudioPicker::Group> groups;
    const QJsonArray array = QJsonDocument::fromJson(KisConfig(true).readEntry<QString>(source.groupsKey, QString()).toUtf8()).array();
    for (const QJsonValue &value : array) {
        const QJsonObject object = value.toObject();
        KisStudioPicker::Group group{object.value(QStringLiteral("title")).toString(), {}};
        for (const QJsonValue &id : object.value(QStringLiteral("ids")).toArray()) group.ids << id.toString();
        if (!group.title.isEmpty()) groups << group;
    }
    return groups;
}
} // namespace

QVector<KisStudioPicker::Group> KisStudioPicker::userGroups(const Source &source)
{
    if (source.groupsKey.isEmpty()) return {};
    const QSet<QString> known = knownIds(source);
    QVector<Group> shown;
    Q_FOREACH (Group group, storedGroups(source)) {
        group.ids.erase(std::remove_if(group.ids.begin(), group.ids.end(), [&known](const QString &id) { return !known.contains(id); }),
                        group.ids.end());
        group.ids.removeDuplicates();
        if (!group.ids.isEmpty()) shown << group;
    }
    return shown;
}

void KisStudioPicker::setUserGroups(const Source &source, const QVector<Group> &groups)
{
    if (source.groupsKey.isEmpty()) return;
    // keep ids unknown right now (a plugin not loaded, a mode only layers have in a brush's picker) for when they
    // return, and the groups only they are in, which this picker never showed
    const QSet<QString> known = knownIds(source);
    QVector<Group> kept = groups;
    Q_FOREACH (const Group &old, storedGroups(source)) {
        const int same = groupNamed(kept, old.title);
        if (same >= 0) {
            Q_FOREACH (const QString &id, old.ids) {
                if (!known.contains(id) && !kept[same].ids.contains(id)) kept[same].ids << id;
            }
        } else if (std::none_of(old.ids.begin(), old.ids.end(), [&known](const QString &id) { return known.contains(id); })) {
            kept << old;
        }
    }
    QJsonArray array;
    Q_FOREACH (const Group &group, kept) {
        QJsonObject object;
        object.insert(QStringLiteral("title"), group.title);
        object.insert(QStringLiteral("ids"), QJsonArray::fromStringList(group.ids));
        array << object;
    }
    KisConfig(false).writeEntry(source.groupsKey, QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact)));
}

QStringList KisStudioPicker::scrollOrder(const Source &source)
{
    QStringList order = pinnedIds(source);
    Q_FOREACH (const Group &group, userGroups(source)) order << group.ids;
    order.removeDuplicates();
    Q_FOREACH (const Group &group, source.otherGroups(order)) {
        order << group.ids;
    }
    return order;
}

KisStudioPicker::KisStudioPicker(const Source &source, const QString &current, const QSet<QString> &available, QWidget *parent)
    // not Qt::Popup on Windows, where translucent popups get a black background (see KisStudioGlass::popupWindowType)
    : QFrame(parent, KisStudioGlass::popupWindowType() | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint)
    , d(new Private)
{
    d->q = this;
    d->source = source;
    d->m = metricsFor(source.tiles, source.tileSize);
    d->original = current;
    d->available = available;
    d->pinned = pinnedIds(source);
    d->groups = userGroups(source);
    d->yoursTitle = source.yoursTitle;
    if (!source.groupsKey.isEmpty()) {
        const QString named = KisConfig(true).readEntry<QString>(source.groupsKey + QStringLiteral("Yours"), QString());
        if (!named.isEmpty()) d->yoursTitle = named;
    }
    setAttribute(Qt::WA_TranslucentBackground, KisStudioGlass::seeThroughWindows());
    setAttribute(Qt::WA_DeleteOnClose);

    d->previewTimer.setSingleShot(true);
    connect(&d->previewTimer, &QTimer::timeout, this, [this]() {
        if (!d->pendingPreview.isEmpty()) Q_EMIT sigPreview(d->pendingPreview);
    });

    auto *layout = new QVBoxLayout(this);
    const int shadowMargin = KisStudioGlass::shadowMargin(this, glassShadowMargin);
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
    d->scroll->viewport()->installEventFilter(this);
    d->grid->setAutoFillBackground(false);
    layout->addWidget(d->scroll);

    // lays out the recent items, the user's numbered list, then the other groups; or the search matches
    d->relayout = [this]() {
        static_cast<Grid *>(d->grid)->forgetPress();
        d->items.clear();
        d->headers.clear();
        d->listAreas.clear();
        const Metrics &m = d->m;
        const bool tiles = d->source.tiles;
        const int width = m.columns * m.cellWidth;
        const QString query = d->search->text().trimmed();
        auto nameOf = [this](const QString &id) { return d->source.name(id); };
        auto addItem = [&](const QString &id, int column, int top, int list, int order) {
            const QString name = list == 0 && d->source.pinnedName ? d->source.pinnedName(id) : nameOf(id);
            d->items << Item{id, name, QRect(column * m.cellWidth, top, m.cellWidth, m.cellHeight), order, list};
        };
        const auto rowsFor = [&m](const QStringList &ids) { return std::max(1, int((ids.size() + m.columns - 1) / m.columns)); };

        // left to right, then down; \p list is which of the user's lists they are, or -1
        auto placeAcross = [&](const QStringList &ids, int top, int list) {
            for (int i = 0; i < ids.size(); i++) {
                addItem(ids[i], i % m.columns, top + (i / m.columns) * m.cellHeight, list, list >= 0 ? i : -1);
            }
            return top + rowsFor(ids) * m.cellHeight;
        };
        // down, then the next column
        auto placeDown = [&](const QStringList &ids, int top, int list) {
            const int perColumn = rowsFor(ids);
            for (int i = 0; i < ids.size(); i++) {
                addItem(ids[i], i / perColumn, top + (i % perColumn) * m.cellHeight, list, list >= 0 ? i : -1);
            }
            return top + perColumn * m.cellHeight;
        };
        auto header = [&](const QString &title, int x, int y, int w, int list = -1) {
            d->headers << Header{title, QRect(x, y, w, headerHeight), list};
            return y + headerHeight;
        };

        int height = 0;
        if (!query.isEmpty()) {
            // a list's name finds what is in it
            QHash<QString, int> byGroup;
            const QVector<Group> lists = QVector<Group>{Group{d->yoursTitle, d->pinned}} + d->groups;
            Q_FOREACH (const Group &group, lists) {
                const int score = matchScore(query, group.title);
                if (score <= 0) continue;
                Q_FOREACH (const QString &id, group.ids) byGroup[id] = std::max(byGroup.value(id), score - 25);
            }
            QVector<QPair<int, QString>> scored;
            Q_FOREACH (const QString &id, d->source.allIds()) {
                int score = std::max(matchScore(query, nameOf(id)), byGroup.value(id));
                if (d->source.aliases) {
                    Q_FOREACH (const QString &alias, d->source.aliases(id)) {
                        score = std::max(score, matchScore(query, alias) - 50);
                    }
                }
                if (score > 0) scored.append(qMakePair(score, id));
            }
            // add fuzzy matches (typos, a few letters of each word) only when plain ones are few
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
            if (!d->pinned.isEmpty()) {
                y = header(d->yoursTitle, 0, y, width, d->source.groupsKey.isEmpty() ? -1 : 0);
                y = tiles ? placeAcross(d->pinned, y, 0) : placeDown(d->pinned, y, 0);
            }
            d->listAreas << QRect(0, yoursTop, width, y - yoursTop);
            for (int g = 0; g < d->groups.size(); g++) {
                const int top = y > 0 ? y + 8 : y;
                y = header(d->groups[g].title, 0, top, width, g + 1);
                y = tiles ? placeAcross(d->groups[g].ids, y, g + 1) : placeDown(d->groups[g].ids, y, g + 1);
                d->listAreas << QRect(0, top, width, y - top);
            }

            const QVector<Group> groups = d->source.otherGroups(d->pinned + d->grouped());
            if (tiles) {
                if (y > 0) y += 8;
                Q_FOREACH (const Group &group, groups) {
                    y = placeAcross(group.ids, header(group.title, 0, y, width), -1) + 4;
                }
                height = y;
            } else {
                // put each group into the shortest column
                QVector<int> heights(m.columns, y + 8);
                Q_FOREACH (const Group &group, groups) {
                    const int c = int(std::min_element(heights.begin(), heights.end()) - heights.begin());
                    int top = header(group.title, c * m.cellWidth, heights[c], m.cellWidth);
                    Q_FOREACH (const QString &id, group.ids) {
                        addItem(id, c, top, -1, -1);
                        top += m.cellHeight;
                    }
                    heights[c] = top;
                }
                height = *std::max_element(heights.begin(), heights.end());
            }
        }

        d->grid->setFixedSize(width, std::max(height, m.cellHeight));
        const int maxHeight = tiles ? 540 : 460;
        d->scroll->setFixedSize(width + (height > maxHeight ? d->scroll->verticalScrollBar()->sizeHint().width() : 0),
                                std::min(height, maxHeight) + 2);
        d->scroll->setVerticalScrollBarPolicy(height > maxHeight ? Qt::ScrollBarAsNeeded : Qt::ScrollBarAlwaysOff);

        d->highlighted = -1;
        for (int i = 0; i < d->items.size(); i++) {
            if (d->items[i].id == d->original) d->highlighted = i;
        }
        if (!query.isEmpty()) d->highlighted = d->items.isEmpty() ? -1 : 0;
        d->grid->update();
        adjustSize();
        keepOnScreen();
    };

    connect(d->search, &QLineEdit::textChanged, this, [this]() { d->relayout(); });
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
    // placed after the list, which is hidden while the view shows
    if (auto *box = qobject_cast<QVBoxLayout *>(layout())) {
        box->insertWidget(box->indexOf(d->scroll) + 1, view);
    }
    auto *link = new LinkButton(this);
    link->setCursor(Qt::PointingHandCursor);
    d->alternateButton = link;
    d->footer = new QHBoxLayout();
    d->footer->setContentsMargins(8, 2, 4, 0);
    d->footer->addStretch(1);
    d->footer->addWidget(link);
    if (auto *box = qobject_cast<QVBoxLayout *>(layout())) box->addLayout(d->footer);
    auto apply = [this, label](bool on) {
        d->alternate->setVisible(on);
        d->scroll->setVisible(!on);
        d->search->setVisible(!on);
        d->alternateButton->setText(on ? i18nc("@action:button back to the list", "List") : label);
        adjustSize();
        keepOnScreen();
    };
    connect(link, &QAbstractButton::clicked, this, [this, apply, rememberedKey]() {
        const bool on = !d->alternate->isVisible();
        KisConfig(false).writeEntry(rememberedKey, on);
        apply(on);
        // the search field, now hidden, had the focus and so the keys, Escape included
        if (on) setFocus();
        else d->search->setFocus();
    });
    apply(KisConfig(true).readEntry<bool>(rememberedKey, false));
}

void KisStudioPicker::pickId(const QString &id)
{
    d->pickId(id);
}

const KisStudioPicker::Source &KisStudioPicker::pickerSource() const
{
    return d->source;
}

void KisStudioPicker::popupBelow(QWidget *anchor)
{
    adjustSize();
    const int shadowMargin = KisStudioGlass::shadowMargin(this, glassShadowMargin);
    QPoint pos = anchor->mapToGlobal(QPoint(0, anchor->height())) - QPoint(shadowMargin, shadowMargin - 4);
    // if it would extend past the window, align it with the anchor's right edge instead
    const QWidget *window = anchor->window();
    const int windowRight = window->mapToGlobal(QPoint(window->width(), 0)).x();
    if (pos.x() + width() - shadowMargin > windowRight) {
        pos.setX(anchor->mapToGlobal(QPoint(anchor->width(), 0)).x() - width() + shadowMargin);
    }
    popupAt(pos, anchor);
}

void KisStudioPicker::popupNear(const QPoint &globalPos)
{
    adjustSize();
    // the search field just above the pointer, the items below it
    popupAt(globalPos - QPoint(width() / 2, KisStudioGlass::shadowMargin(this, glassShadowMargin) + 26), nullptr);
}

void KisStudioPicker::popupAt(QPoint pos, QWidget *anchor)
{
    QScreen *screen = anchor ? anchor->screen() : QGuiApplication::screenAt(pos + QPoint(width() / 2, KisStudioGlass::shadowMargin(this, glassShadowMargin) + 26));
    move(onScreen(pos, size(), screen));
    KisStudioGlass::setMovableByBackground(this);
    new KisPopupCloser(this, anchor);
    KisStudioMotion::showFaded(this);
    raise();
    activateWindow();
    if (d->search->isVisibleTo(this)) {
        d->search->setFocus();
    } else {
        setFocus();
    }
    if (d->highlighted >= 0) {
        const QRect rect = d->items[d->highlighted].rect;
        d->scroll->ensureVisible(rect.center().x(), rect.center().y(), 0, d->m.cellHeight * 2);
    }
}

void KisStudioPicker::keepOnScreen()
{
    // it may grow while shown (a search, the alternate view): keep it on screen
    if (isVisible()) move(onScreen(pos(), size(), screen()));
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
        // tiles are laid out across, so up and down move a whole row
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
            // with nothing typed, a digit picks that number from the user's list (0 is the tenth)
            if (d->search->text().isEmpty() && !(key->modifiers() & ~Qt::KeypadModifier) && key->key() >= Qt::Key_0
                && key->key() <= Qt::Key_9) {
                const int order = key->key() == Qt::Key_0 ? 9 : key->key() - Qt::Key_1;
                for (int i = 0; i < count; i++) {
                    if (d->items[i].list == 0 && d->items[i].order == order) {
                        d->pick(i);
                        return true;
                    }
                }
            }
            break;
        }
    }
    return QFrame::eventFilter(watched, event);
}

void KisStudioPicker::keyPressEvent(QKeyEvent *event)
{
    // the search field handles its own keys (see eventFilter); this is for the alternate view
    if (event->key() == Qt::Key_Escape) {
        close();
        return;
    }
    QFrame::keyPressEvent(event);
}

void KisStudioPicker::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const int shadowMargin = KisStudioGlass::shadowMargin(this, glassShadowMargin);
    const QRectF panel = QRectF(rect()).adjusted(shadowMargin, shadowMargin, -shadowMargin, -shadowMargin);
    const qreal radius = 12;
    if (shadowMargin) KisStudioGlass::paintShadow(painter, panel, radius, shadowMargin, 34, 4);
    const QPainterPath shape = KisStudioGlass::paint(painter, this, panel.adjusted(0.5, 0.5, -0.5, -0.5), radius);
    QColor border = palette().color(QPalette::WindowText);
    border.setAlphaF(0.08);
    KisStudioGlass::paintBorder(painter, this, shape, border);
}

void KisStudioPicker::hideEvent(QHideEvent *event)
{
    // closed without picking: end the preview
    if (!d->done) d->finish();
    QFrame::hideEvent(event);
}
