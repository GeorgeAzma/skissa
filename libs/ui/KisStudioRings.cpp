/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioRings.h"
#include <KisProfile.h>

#include <QAction>
#include <QApplication>
#include <QElapsedTimer>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QTimer>
#include <QtMath>

#include <KConfigGroup>
#include <KSharedConfig>
#include <kactioncollection.h>
#include <klocalizedstring.h>

#include <KisModernStyle.h>
#include <kis_icon_utils.h>

#include "KisMainWindow.h"
#include "KisStudioFind.h"
#include "KisStudioGlass.h"
#include "KisStudioHoldKeys.h"
#include "KisStudioItemCard.h"
#include "KisStudioMotion.h"
#include "kis_config.h"

namespace
{

const QString ringsKey = QStringLiteral("StudioRings");
QString itemsKey(const QString &id) { return QStringLiteral("StudioRing_") + id; }
QString nameKey(const QString &id) { return QStringLiteral("StudioRingName_") + id; }
QString actionName(const QString &id) { return QStringLiteral("studio_ring_") + id; }

constexpr qreal plateRadius = 150;
constexpr qreal slotDistance = 96;
constexpr qreal slotRadius = 28;
constexpr qreal middleRadius = 36;
/// Distance a pressed slot must move to start dragging it
constexpr qreal dragStart = 6;

/// A ring open around the pointer; like the canvas brush ring, each slot is hit anywhere in its sector
class CommandRing : public QWidget
{
public:
    CommandRing(KisStudioRings *rings, KisMainWindow *mainWindow)
        : QWidget(mainWindow)
        , m_rings(rings)
        , m_mainWindow(mainWindow)
    {
        KisStudioGlass::setOverOverlays(this);
        setAttribute(Qt::WA_NoSystemBackground);
        setMouseTracking(true);
        setFixedSize(int(2 * plateRadius + 16), int(2 * plateRadius + 16));
        hide();
        qApp->installEventFilter(this);
    }

    ~CommandRing() override
    {
        qApp->removeEventFilter(this);
    }

    QString ringId() const { return m_id; }

    /// Opens ring \p id at \p global, held for as long as \p key is down (0 if not opened by a key)
    void openAt(const QString &id, const QPoint &global, int key)
    {
        m_id = id;
        m_global = global;
        loadSlots();
        QWidget *area = parentWidget();
        const QSize half = size() / 2;
        QPoint at = area->mapFromGlobal(global);
        at.setX(qBound(half.width(), at.x(), qMax(half.width(), area->width() - half.width())));
        at.setY(qBound(half.height(), at.y(), qMax(half.height(), area->height() - half.height())));
        move(at - QPoint(half.width(), half.height()));
        m_center = QPointF(half.width(), half.height());
        m_held = key != 0;
        m_heldKey = key;
        m_since.restart();
        m_hover = -1;
        m_pressed = -1;
        m_dragging = false;
        show();
        raise();
        KisStudioMotion::showFaded(this);
        update();
    }

    void closeRing()
    {
        m_held = false;
        if (isVisible()) KisStudioMotion::hideFaded(this);
    }

protected:
    struct Slot {
        QString key;
        QString title;
        QIcon icon;
    };

    void loadSlots()
    {
        m_slots.clear();
        KisStudioFind *find = m_mainWindow->studioFind();
        Q_FOREACH (const QString &key, m_rings->items(m_id)) {
            Slot slot;
            slot.key = key;
            KisStudioFind::Item item;
            if (find && find->item(key, &item)) {
                slot.title = item.title;
                slot.icon = item.icon;
            } else {
                slot.title = key.section(QLatin1Char(':'), 1);
            }
            m_slots << slot;
        }
    }

    QPointF slotCenter(int index) const
    {
        const qreal angle = qDegreesToRadians(-90.0 + index * 360.0 / KisStudioRings::slotCount);
        return m_center + slotDistance * QPointF(qCos(angle), qSin(angle));
    }

    /// The slot at \p pos (100 + index for an empty one), the middle (-2) or nothing (-1)
    int targetAt(const QPointF &pos) const
    {
        const QPointF delta = pos - m_center;
        const qreal distance = qSqrt(delta.x() * delta.x() + delta.y() * delta.y());
        if (distance < middleRadius) return -2;
        if (distance > plateRadius && !m_held) return -1;
        qreal angle = qRadiansToDegrees(qAtan2(delta.y(), delta.x())) + 90.0;
        while (angle < 0) angle += 360.0;
        const int index = int(qRound(angle / (360.0 / KisStudioRings::slotCount))) % KisStudioRings::slotCount;
        return index < m_slots.size() ? index : 100 + index;
    }

    void runSlot(int index)
    {
        const QString key = m_slots.value(index).key;
        closeRing();
        QPointer<KisMainWindow> window = m_mainWindow;
        QTimer::singleShot(0, m_mainWindow, [window, key]() {
            if (window && window->studioFind()) window->studioFind()->run(key);
        });
    }

    /// Lets the user pick new content for a slot in Find, then reopens the ring
    void pickFor(int index)
    {
        KisStudioFind *find = m_mainWindow->studioFind();
        if (!find) return;
        closeRing();
        QPointer<CommandRing> self(this);
        const QString id = m_id;
        const QPoint at = m_global;
        find->pick(i18nc("@info:placeholder", "Add to ring"), [self, id, index, at](const QString &key) {
            if (!self) return;
            QStringList items = self->m_rings->items(id);
            const int already = items.indexOf(key);
            if (index < items.size()) {
                if (already >= 0) items.swapItemsAt(index, already);
                else items[index] = key;
            } else if (already < 0) {
                items.append(key);
            }
            self->m_rings->setItems(id, items);
            self->openAt(id, at, 0);
        });
    }

    void slotMenu(int index, const QPoint &global)
    {
        QMenu menu(this);
        QAction *replace = menu.addAction(i18nc("@action:inmenu a slot of a ring", "Replace…"));
        QAction *remove = menu.addAction(i18nc("@action:inmenu a slot of a ring", "Remove"));
        QAction *chosen = menu.exec(global);
        if (chosen == replace) {
            pickFor(index);
        } else if (chosen == remove) {
            QStringList items = m_rings->items(m_id);
            if (index < items.size()) items.removeAt(index);
            m_rings->setItems(m_id, items);
            loadSlots();
            update();
        }
    }

    void middleMenu(const QPoint &global)
    {
        QMenu menu(this);
        QAction *rename = menu.addAction(i18nc("@action:inmenu a ring", "Rename…"));
        QAction *remove = menu.addAction(i18nc("@action:inmenu a ring", "Delete Ring"));
        QAction *chosen = menu.exec(global);
        if (chosen == rename) {
            bool ok = false;
            const QString name = QInputDialog::getText(m_mainWindow, i18nc("@title:window", "Rename Ring"), i18nc("@label:textbox", "Name"),
                                                       QLineEdit::Normal, m_rings->name(m_id), &ok);
            if (ok && !name.trimmed().isEmpty()) m_rings->rename(m_id, name.trimmed());
            update();
        } else if (chosen == remove) {
            const QString id = m_id;
            closeRing();
            m_rings->remove(id);
        }
    }

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (!KisStudioMotion::isShown(this)) return false;
        const QEvent::Type type = event->type();
        if (type == QEvent::KeyPress && static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape) {
            closeRing();
            return true;
        }
        // On key release: if held, run what the pointer moved towards; if tapped, stay open for a click
        if (type == QEvent::KeyRelease && m_held && m_heldKey) {
            auto *key = static_cast<QKeyEvent *>(event);
            if (!key->isAutoRepeat() && key->key() == m_heldKey) {
                m_held = false;
                if (m_since.elapsed() >= KisStudioHoldKeys::holdTime) {
                    if (m_hover >= 0 && m_hover < m_slots.size()) runSlot(m_hover);
                    else closeRing();
                }
                update();
            }
            return false;
        }
        // A press anywhere else closes the ring
        if ((type == QEvent::MouseButtonPress || type == QEvent::TabletPress || type == QEvent::TouchBegin)
            && watched->isWidgetType() && watched != this) {
            auto *widget = static_cast<QWidget *>(watched);
            if (!isAncestorOf(widget) && !qobject_cast<QMenu *>(widget)) closeRing();
        }
        return false;
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (m_pressed >= 0) {
            if (!m_dragging && QLineF(event->localPos(), m_pressedAt).length() > dragStart) m_dragging = true;
            m_dragAt = event->localPos();
        }
        const int hover = targetAt(event->localPos());
        if (hover != m_hover || m_dragging) {
            m_hover = hover;
            update();
        }
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        const int target = targetAt(event->localPos());
        if (event->button() == Qt::RightButton) {
            if (target >= 0 && target < m_slots.size()) slotMenu(target, event->globalPos());
            else if (target == -2) middleMenu(event->globalPos());
            return;
        }
        if (event->button() != Qt::LeftButton) return;
        if (target >= 0 && target < m_slots.size()) {
            // Run on release, unless dragged to another slot
            m_pressed = target;
            m_pressedAt = event->localPos();
            m_dragAt = m_pressedAt;
        } else if (target >= 100) {
            pickFor(target - 100);
        } else if (target == -1) {
            closeRing();
        }
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton || m_pressed < 0) return;
        const int from = m_pressed;
        const int to = targetAt(event->localPos());
        m_pressed = -1;
        if (!m_dragging) {
            runSlot(from);
            return;
        }
        m_dragging = false;
        if (to >= 0 && to != from) {
            // Dropped on another slot: swap them; on an empty slot: move to the end
            QStringList items = m_rings->items(m_id);
            if (to >= 100 && from < items.size()) items.append(items.takeAt(from));
            else if (from < items.size() && to < items.size()) items.swapItemsAt(from, to);
            m_rings->setItems(m_id, items);
            loadSlots();
        }
        update();
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QPalette pal = palette();
        const QColor text = pal.color(QPalette::WindowText);
        const QColor accent = pal.color(QPalette::Highlight);
        QColor line = text;
        line.setAlphaF(0.1);

        const qreal plate = plateRadius - 6;
        const QRectF plateRect(m_center - QPointF(plate, plate), QSizeF(2 * plate, 2 * plate));
        KisStudioGlass::paintShadow(painter, plateRect, plate, 8, 26, 3);
        painter.save();
        const QPainterPath shape = KisStudioGlass::paint(painter, this, plateRect, plate);
        painter.restore();
        KisStudioGlass::paintBorder(painter, this, shape, line);

        // Highlight the hovered sector
        if (m_hover >= 0 && m_hover < m_slots.size() && !m_dragging) {
            QColor wedge = accent;
            wedge.setAlphaF(0.16);
            const qreal start = 90.0 - m_hover * 360.0 / KisStudioRings::slotCount - 180.0 / KisStudioRings::slotCount;
            QPainterPath path;
            path.moveTo(m_center);
            path.arcTo(plateRect, start, 360.0 / KisStudioRings::slotCount);
            path.closeSubpath();
            painter.setPen(Qt::NoPen);
            painter.setBrush(wedge);
            painter.drawPath(path);
        }

        for (int i = 0; i < KisStudioRings::slotCount; i++) {
            const QPointF c = slotCenter(i);
            if (i >= m_slots.size()) {
                // Empty slot: a "+" for adding an item
                QColor faint = text;
                faint.setAlphaF(m_hover == 100 + i ? 0.6 : 0.22);
                painter.setBrush(Qt::NoBrush);
                painter.setPen(QPen(faint, 1.2, Qt::DashLine));
                painter.drawEllipse(c, slotRadius - 6, slotRadius - 6);
                painter.setPen(QPen(faint, 1.6, Qt::SolidLine, Qt::RoundCap));
                painter.drawLine(c - QPointF(6, 0), c + QPointF(6, 0));
                painter.drawLine(c - QPointF(0, 6), c + QPointF(0, 6));
                continue;
            }
            const bool hovered = i == m_hover;
            const qreal r = hovered ? slotRadius + 2 : slotRadius;
            QColor face = text;
            face.setAlphaF(hovered ? 0.2 : 0.09);
            painter.setPen(hovered ? QPen(accent, 2) : Qt::NoPen);
            painter.setBrush(face);
            painter.save();
            if (m_dragging && i == m_pressed) painter.setOpacity(0.35);
            painter.drawEllipse(c, r, r);
            drawContent(painter, m_slots[i], c, r);
            painter.restore();
        }

        if (m_dragging && m_pressed >= 0 && m_pressed < m_slots.size()) {
            QColor face = text;
            face.setAlphaF(0.25);
            painter.setPen(QPen(accent, 2));
            painter.setBrush(face);
            painter.drawEllipse(m_dragAt, slotRadius - 3, slotRadius - 3);
            drawContent(painter, m_slots[m_pressed], m_dragAt, slotRadius - 3);
        }

        // The ring's name in the middle, and the hovered item's name below it
        QFont font = this->font();
        QFont small = font;
        small.setPointSizeF(font.pointSizeF() * 0.85);
        painter.setFont(small);
        QColor faint = text;
        faint.setAlphaF(m_hover == -2 ? 0.95 : 0.6);
        painter.setPen(faint);
        const QRectF middle(m_center.x() - middleRadius, m_center.y() - middleRadius, 2 * middleRadius, 2 * middleRadius);
        painter.drawText(middle, Qt::AlignCenter | Qt::TextWordWrap, QFontMetricsF(small).elidedText(m_rings->name(m_id), Qt::ElideRight, 2 * middleRadius * 1.6));

        QString name;
        if (m_hover >= 0 && m_hover < m_slots.size()) name = m_slots[m_hover].title;
        else if (m_hover >= 100) name = i18nc("@info an empty slot of a ring", "Add");
        if (!name.isEmpty()) {
            font.setBold(true);
            painter.setFont(font);
            const QFontMetrics metrics(font);
            const QRectF pill(m_center.x() - metrics.horizontalAdvance(name) / 2.0 - 10, m_center.y() + middleRadius + 4,
                              metrics.horizontalAdvance(name) + 20, metrics.height() + 8);
            painter.setPen(Qt::NoPen);
            painter.setBrush(accent);
            painter.drawPath(KisModernStyle::squircle(pill, pill.height() / 2));
            painter.setPen(pal.color(QPalette::HighlightedText));
            painter.drawText(pill, Qt::AlignCenter, name);
        }
    }

    void drawContent(QPainter &painter, const Slot &slot, const QPointF &c, qreal r)
    {
        if (!slot.icon.isNull()) {
            const int size = int(r * 0.95);
            slot.icon.paint(&painter, QRect(int(c.x() - size / 2.0), int(c.y() - size / 2.0), size, size));
            return;
        }
        // No icon: show the initials of the title
        QFont font = this->font();
        font.setBold(true);
        painter.setFont(font);
        painter.setPen(palette().color(QPalette::WindowText));
        QString letters;
        Q_FOREACH (const QString &word, slot.title.split(QLatin1Char(' '), Qt::SkipEmptyParts).mid(0, 2)) letters += word.left(1).toUpper();
        painter.drawText(QRectF(c.x() - r, c.y() - r, 2 * r, 2 * r), Qt::AlignCenter, letters);
    }

private:
    KisStudioRings *m_rings {nullptr};
    KisMainWindow *m_mainWindow {nullptr};
    QString m_id;
    QPoint m_global;
    QVector<Slot> m_slots;
    QPointF m_center;
    int m_hover {-1};
    bool m_held {false};
    int m_heldKey {0};
    QElapsedTimer m_since;
    int m_pressed {-1};
    QPointF m_pressedAt;
    QPointF m_dragAt;
    bool m_dragging {false};
};

} // namespace

struct KisStudioRings::Private
{
    KisMainWindow *mainWindow {nullptr};
    QPointer<CommandRing> ring;
    QHash<QString, QPointer<QAction>> actions;
};

KisStudioRings::KisStudioRings(KisMainWindow *mainWindow)
    : QObject(mainWindow)
    , d(new Private)
{
    d->mainWindow = mainWindow;
    Q_FOREACH (const QString &id, KisConfig(true).readEntry<QStringList>(ringsKey, QStringList())) {
        makeAction(id);
    }
}

KisStudioRings::~KisStudioRings() = default;

QAction *KisStudioRings::makeAction(const QString &id)
{
    auto *action = new QAction(KisIconUtils::loadIcon(QStringLiteral("studio_ring")), name(id), d->mainWindow);
    action->setObjectName(actionName(id));
    action->setShortcutContext(Qt::WindowShortcut);
    // Open once per key press, not on auto-repeat
    action->setAutoRepeat(false);
    KisStudioShortcuts::restore(action);
    d->mainWindow->actionCollection()->addAction(action->objectName(), action);
    connect(action, &QAction::triggered, this, [this, id]() { open(id); });
    action->installEventFilter(this);
    d->actions.insert(id, action);
    return action;
}

bool KisStudioRings::eventFilter(QObject *watched, QEvent *event)
{
    // Opened by its shortcut (not a menu or Find): remember the key so releasing it can select
    if (event->type() == QEvent::Shortcut && !static_cast<QShortcutEvent *>(event)->isAmbiguous()) {
        auto *action = qobject_cast<QAction *>(watched);
        const QString id = d->actions.key(action);
        if (action && action->isEnabled() && !id.isEmpty()) {
            const QKeySequence key = static_cast<QShortcutEvent *>(event)->key();
            openRing(id, key.isEmpty() ? 0 : key[0] & ~int(Qt::KeyboardModifierMask));
            return true;
        }
    }
    return QObject::eventFilter(watched, event);
}

QAction *KisStudioRings::createRing()
{
    KisConfig cfg(false);
    QStringList ids = cfg.readEntry<QStringList>(ringsKey, QStringList());
    int number = 1;
    while (ids.contains(QString::number(number))) number++;
    const QString id = QString::number(number);
    ids << id;
    cfg.writeEntry(ringsKey, ids);
    cfg.writeEntry(nameKey(id), i18nc("a ring of commands, numbered", "Ring %1", number));
    return makeAction(id);
}

QStringList KisStudioRings::items(const QString &id) const
{
    return KisConfig(true).readEntry<QStringList>(itemsKey(id), QStringList()).mid(0, slotCount);
}

void KisStudioRings::setItems(const QString &id, const QStringList &items)
{
    QStringList kept = items;
    kept.removeAll(QString());
    KisConfig(false).writeEntry(itemsKey(id), kept.mid(0, slotCount));
}

QString KisStudioRings::name(const QString &id) const
{
    return KisConfig(true).readEntry<QString>(nameKey(id), i18nc("a ring of commands, numbered", "Ring %1", id));
}

void KisStudioRings::rename(const QString &id, const QString &name)
{
    KisConfig(false).writeEntry(nameKey(id), name);
    if (QAction *action = d->actions.value(id)) action->setText(name);
}

void KisStudioRings::remove(const QString &id)
{
    KisConfig cfg(false);
    QStringList ids = cfg.readEntry<QStringList>(ringsKey, QStringList());
    ids.removeAll(id);
    cfg.writeEntry(ringsKey, ids);
    KSharedConfig::openConfig()->group(QString()).deleteEntry(itemsKey(id));
    KSharedConfig::openConfig()->group(QString()).deleteEntry(nameKey(id));
    KConfigGroup shortcuts(KSharedConfig::openConfig(KisProfile::configFile(QStringLiteral("kritashortcutsrc"))), QStringLiteral("Shortcuts"));
    shortcuts.deleteEntry(actionName(id));
    if (QAction *action = d->actions.take(id)) {
        d->mainWindow->actionCollection()->takeAction(action);
        action->deleteLater();
    }
}

void KisStudioRings::open(const QString &id)
{
    openRing(id, 0);
}

void KisStudioRings::openRing(const QString &id, int heldKey)
{
    if (!d->ring) d->ring = new CommandRing(this, d->mainWindow);
    if (KisStudioMotion::isShown(d->ring) && d->ring->ringId() == id) {
        d->ring->closeRing();
        return;
    }
    d->ring->openAt(id, QCursor::pos(), heldKey);
}
