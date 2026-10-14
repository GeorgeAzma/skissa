/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioBarButtons.h"

#include "KisStudioBrushEditor.h"
#include "KisStudioPenPopup.h"
#include "KisViewManager.h"
#include "kis_paintop_box.h"
#include "kis_paintop_option.h"
#include "widgets/KisStudioValueRow.h"

#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QContextMenuEvent>
#include <QDrag>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QSpinBox>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidgetAction>

#include <cmath>

#include <KisModernStyle.h>
#include <KoCanvasController.h>
#include <KoToolManager.h>
#include <kactioncollection.h>
#include <kis_config.h>
#include <klocalizedstring.h>

#include "KisMainWindow.h"
#include "KisStudioFind.h"
#include "KisStudioItemCard.h"
#include "KisStudioSymmetry.h"
#include "kis_icon_utils.h"

namespace
{

const QString itemsKey = QStringLiteral("StudioBarItems");
const QString symmetryItem = QStringLiteral("bar:symmetry");
const QString shapesItem = QStringLiteral("action:stroke_shapes");
const QString brushEditorItem = QStringLiteral("action:show_brush_editor");
const QString indexMimeType = QStringLiteral("application/x-krita-studio-bar-index");

QStringList defaultItems()
{
    return {symmetryItem, shapesItem, brushEditorItem, QStringLiteral("action:erase_action"),
            QStringLiteral("action:preserve_alpha"), QStringLiteral("action:reload_preset_action")};
}

/// The tools that paint with the brush, which are the ones symmetry and shapes apply to
bool paintsWithBrush(const QString &tool)
{
    static const QStringList tools = {QStringLiteral("KritaShape/KisToolBrush"), QStringLiteral("KritaShape/KisToolDyna"),
                                      QStringLiteral("KritaShape/KisToolMultiBrush"), QStringLiteral("KritaShape/KisToolLine"),
                                      QStringLiteral("KritaShape/KisToolRectangle"), QStringLiteral("KritaShape/KisToolEllipse"),
                                      QStringLiteral("KisToolPolygon"), QStringLiteral("KisToolPolyline"), QStringLiteral("KisToolPath"),
                                      QStringLiteral("KisToolPencil")};
    return tools.contains(tool);
}

/// The tools that paint, with a brush or by filling: eraser mode and alpha lock apply to them too
bool paints(const QString &tool)
{
    static const QStringList fills = {QStringLiteral("KritaFill/KisToolFill"), QStringLiteral("KritaFill/KisToolGradient"),
                                      QStringLiteral("KisToolEncloseAndFill")};
    return paintsWithBrush(tool) || fills.contains(tool);
}

/// Icon for a symmetry mode: mirror, radial or tile
QIcon symmetryIcon(int mode, const QColor &ink)
{
    QIcon icon;
    for (int size : {20, 40}) {
        QPixmap pixmap(size, size);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const qreal s = size / 20.0;
        QPen pen(ink, 1.4 * s);
        pen.setCapStyle(Qt::RoundCap);
        painter.setPen(pen);
        const QPointF c(10 * s, 10 * s);
        switch (mode) {
        case KisStudioSymmetry::Mirror:
            painter.drawLine(QPointF(10 * s, 3 * s), QPointF(10 * s, 17 * s));
            painter.drawArc(QRectF(3 * s, 5 * s, 6 * s, 10 * s), 90 * 16, 180 * 16);
            painter.drawArc(QRectF(11 * s, 5 * s, 6 * s, 10 * s), 270 * 16, 180 * 16);
            break;
        case KisStudioSymmetry::Radial:
            for (int i = 0; i < 6; i++) {
                const qreal a = i * M_PI / 3;
                painter.drawLine(c + QPointF(2.5 * s * std::cos(a), 2.5 * s * std::sin(a)), c + QPointF(7.5 * s * std::cos(a), 7.5 * s * std::sin(a)));
            }
            break;
        default:
            for (int i = 0; i < 4; i++) {
                painter.drawPath(KisModernStyle::squircle(QRectF((3.5 + 7 * (i % 2)) * s, (3.5 + 7 * (i / 2)) * s, 6 * s, 6 * s), 1.4 * s));
            }
            break;
        }
        painter.end();
        icon.addPixmap(pixmap);
    }
    return icon;
}

const QString brushPrefix = QStringLiteral("brush:");

/**
 * A brush setting pinned to the bar: its row, small, for the current brush (made again when the brush
 * changes). Dragging it sideways changes it, pulling it down moves it or takes it off, a click shows how
 * the pen drives it, its pin unpins it.
 */
class BrushValue : public KisStudioValueRow
{
public:
    BrushValue(KisMainWindow *mainWindow, KisPaintOpOption *option, QWidget *parent)
        : KisStudioValueRow(option->label(), parent)
    {
        setCompact(true);
        setFixedWidth(qMax(132, sizeHint().width() + 24));
        setPinnable(true, true);
        setPinOnHoverOnly(true);
        // a right click resets it; Find's card doesn't take it
        setProperty("ownContextMenu", true);
        KisStudioBrushEditor::bindRow(this, option, KisStudioBrushEditor::brushSettings(mainWindow));
        const QPointer<KisPaintOpOption> target(option);
        connect(this, &KisStudioValueRow::clicked, this, [this, mainWindow, target]() {
            if (!target || target->penInputs().isEmpty()) return;
            if (m_pen) {
                m_pen->close();
                return;
            }
            m_pen = new KisStudioPenPopup(target, mainWindow);
            setSelected(true);
            connect(m_pen, &QObject::destroyed, this, [this]() { setSelected(false); });
            m_pen->popupBeside(this, Qt::BottomEdge);
        });
    }

    ~BrushValue() override
    {
        if (m_pen) m_pen->close();
    }

private:
    QPointer<KisStudioPenPopup> m_pen;
};

/// A button on the bar: a click runs it, a drag moves it, a right click or long press shows its card
class BarButton : public QToolButton
{
public:
    explicit BarButton(QWidget *parent)
        : QToolButton(parent)
    {
        setAutoRaise(true);
        setFocusPolicy(Qt::NoFocus);
        setIconSize(QSize(20, 20));
        setToolButtonStyle(Qt::ToolButtonIconOnly);
        // its card is the bar's, with Remove, not Find's
        setProperty("ownContextMenu", true);
    }

    std::function<void()> onCard;
    std::function<void()> onDrag;

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) {
            m_dragged = false;
            m_pressPos = event->pos();
        }
        QToolButton::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        // pens wobble, so only a clear move counts as a drag
        if (!m_dragged && (event->buttons() & Qt::LeftButton)
            && (event->pos() - m_pressPos).manhattanLength() > 2 * QApplication::startDragDistance()) {
            m_dragged = true;
            setDown(false);
            if (onDrag) onDrag();
            return;
        }
        QToolButton::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (m_dragged) {
            m_dragged = false;
            setDown(false);
            event->accept();
            return;
        }
        QToolButton::mouseReleaseEvent(event);
    }

    void contextMenuEvent(QContextMenuEvent *event) override
    {
        event->accept();
        if (onCard) onCard();
    }

private:
    QPoint m_pressPos;
    bool m_dragged {false};
};

} // namespace

struct KisStudioBarButtons::Private {
    KisMainWindow *mainWindow {nullptr};
    QHBoxLayout *layout {nullptr};
    QStringList items;
    QList<QWidget *> widgets;
    QList<int> widgetItem;
    QFrame *indicator {nullptr};
    QPointer<QMenu> symmetryMenu;

    /// Items that couldn't be shown yet, since the actions behind them are made after the bar
    bool missing {false};

    KisKActionCollection *actions() const
    {
        return mainWindow->actionCollection();
    }

    /// What a button needs for \p key: a window action directly, anything else through Find
    bool resolve(const QString &key, KisStudioFind::Item *item) const
    {
        if (key.startsWith(QLatin1String("action:"))) {
            const QString name = key.mid(7);
            Q_FOREACH (QAction *action, KisStudioShortcuts::allActions(mainWindow)) {
                if (action->objectName() != name) continue;
                item->key = key;
                item->title = KisStudioShortcuts::plainText(action->text());
                // the brush editor's action has no icon of its own; its button had one
                item->icon = action->icon().isNull() && name == QLatin1String("show_brush_editor")
                    ? KisIconUtils::loadIcon(QStringLiteral("paintop_settings_02")) : action->icon();
                item->action = action;
                return true;
            }
        }
        if (key.startsWith(brushPrefix)) {
            item->key = key;
            item->title = key.mid(brushPrefix.size());
            return true;
        }
        KisStudioFind *find = mainWindow->studioFind();
        return find && find->item(key, item);
    }
};

KisStudioBarButtons::KisStudioBarButtons(KisMainWindow *mainWindow, QWidget *parent)
    : QWidget(parent)
    , d(new Private)
{
    d->mainWindow = mainWindow;
    setObjectName(QStringLiteral("studioBarButtons"));
    setAcceptDrops(true);
    d->layout = new QHBoxLayout(this);
    d->layout->setContentsMargins(KisModernStyle::space2, 0, KisModernStyle::space1, 0);
    d->layout->setSpacing(KisModernStyle::hairline);

    d->indicator = new QFrame(this);
    d->indicator->setAutoFillBackground(true);
    d->indicator->setBackgroundRole(QPalette::Highlight);
    d->indicator->hide();

    d->items = KisConfig(true).readEntry<QStringList>(itemsKey, defaultItems());
    connect(KoToolManager::instance(), &KoToolManager::changedTool, this, &KisStudioBarButtons::updateForTool);
    if (KisPaintopBox *box = mainWindow->viewManager() ? mainWindow->viewManager()->paintOpBox() : nullptr) {
        connect(box, &KisPaintopBox::sigOptionWidgetChanged, this, [this]() {
            Q_FOREACH (const QString &key, d->items) {
                if (key.startsWith(brushPrefix)) {
                    rebuild();
                    return;
                }
            }
        });
    }
    rebuild();
}

KisStudioBarButtons::~KisStudioBarButtons() = default;

QStringList KisStudioBarButtons::items() const
{
    return d->items;
}

void KisStudioBarButtons::setItems(const QStringList &items)
{
    QStringList unique;
    Q_FOREACH (const QString &item, items) {
        if (!unique.contains(item)) unique << item;
    }
    d->items = unique;
    KisConfig(false).writeEntry(itemsKey, d->items);
    rebuild();
    Q_EMIT sigItemsChanged();
}

void KisStudioBarButtons::rebuild()
{
    while (QLayoutItem *item = d->layout->takeAt(0)) {
        if (QWidget *widget = item->widget()) {
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }
    d->widgets.clear();
    d->widgetItem.clear();

    d->missing = false;
    for (int i = 0; i < d->items.size(); i++) {
        const QString key = d->items[i];
        const int index = i;
        if (key.startsWith(brushPrefix)) {
            // a brush setting, if this brush has it
            KisPaintOpOption *option = KisStudioBrushEditor::brushOption(d->mainWindow, key.mid(brushPrefix.size()));
            if (!option) continue;
            auto *value = new BrushValue(d->mainWindow, option, this);
            value->setObjectName(QStringLiteral("studio_bar_") + QString(key).replace(QLatin1Char(':'), QLatin1Char('_')));
            connect(value, &KisStudioValueRow::pinClicked, this, [this, key]() {
                QTimer::singleShot(0, this, [this, key]() { toggle(d->mainWindow, key); });
            });
            connect(value, &KisStudioValueRow::pulledOut, this, [this, index, value]() { startDrag(index, value, value->grab()); });
            d->layout->addWidget(value);
            d->widgets << value;
            d->widgetItem << i;
            continue;
        }
        auto *button = new BarButton(this);
        // named by its item, so Find knows the button and its shortcut
        button->setObjectName(QStringLiteral("studio_bar_") + QString(key).replace(QLatin1Char(':'), QLatin1Char('_')));

        if (key == symmetryItem) {
            // a click toggles it, a long press or right click opens its modes
            button->setCheckable(true);
            button->setIcon(KisIconUtils::loadIcon(QStringLiteral("krita_tool_multihand")));
            button->setToolTip(i18nc("@info:tooltip", "Symmetry (hold or right-click for its modes)"));
            button->setChecked(KisStudioSymmetry::isOn(d->actions()));
            button->setMenu(symmetryMenu());
            button->setPopupMode(QToolButton::DelayedPopup);
            button->onCard = [this, button]() { symmetryMenu()->popup(button->mapToGlobal(QPoint(0, button->height()))); };
            connect(button, &QToolButton::clicked, this, [this, button]() {
                KisStudioSymmetry::setOn(d->actions(), !KisStudioSymmetry::isOn(d->actions()));
                button->setChecked(KisStudioSymmetry::isOn(d->actions()));
            });
            for (const char *name : {"hmirror_action", "vmirror_action"}) {
                if (QAction *mirror = d->actions()->action(QLatin1String(name))) {
                    connect(mirror, &QAction::toggled, button, [this, button]() { button->setChecked(KisStudioSymmetry::isOn(d->actions())); });
                }
            }
        } else {
            KisStudioFind::Item item;
            if (!d->resolve(key, &item)) {
                d->missing = true;
                delete button;
                continue;
            }
            QAction *action = item.action;
            // the brush editor's action is checkable, but it only opens the editor
            if (action && action->isCheckable() && key != brushEditorItem) {
                // the action keeps the button's state (and icon) in step, whatever changes it
                button->setDefaultAction(action);
                button->setToolButtonStyle(Qt::ToolButtonIconOnly);
            } else {
                button->setIcon(item.icon.isNull() ? KisIconUtils::loadIcon(QStringLiteral("studio_more")) : item.icon);
                button->setToolTip(item.title);
                connect(button, &QToolButton::clicked, this, [this, key, action, button]() {
                    if (key == brushEditorItem) {
                        KisStudioBrushEditor::toggle(d->mainWindow, button, Qt::BottomEdge);
                    } else if (action) {
                        action->trigger();
                    } else if (KisStudioFind *find = d->mainWindow->studioFind()) {
                        find->run(key);
                    }
                });
            }
            button->onCard = [this, index]() { showCard(index); };
        }
        button->onDrag = [this, index, button]() { startDrag(index, button, button->icon().pixmap(32, 32)); };
        d->layout->addWidget(button);
        d->widgets << button;
        d->widgetItem << i;
    }

    // with nothing on it, a "+" to bring buttons back
    if (d->widgets.isEmpty()) {
        auto *plus = new BarButton(this);
        plus->setIcon(KisIconUtils::loadIcon(QStringLiteral("list-add")));
        connect(plus, &QToolButton::clicked, this, [this, plus]() { showRestoreCard(plus->mapToGlobal(plus->rect().bottomLeft())); });
        d->layout->addWidget(plus);
    }
    updateForTool();
    updateGeometry();
}

void KisStudioBarButtons::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    if (d->missing) {
        QTimer::singleShot(0, this, &KisStudioBarButtons::rebuild);
    }
}

void KisStudioBarButtons::updateForTool()
{
    // the brush's own buttons only with the tools they apply to; anything added stays
    const QString tool = KoToolManager::instance()->activeToolId();
    const QStringList brushOnly = {symmetryItem, shapesItem, brushEditorItem, QStringLiteral("action:reload_preset_action")};
    const QStringList painting = {QStringLiteral("action:erase_action"), QStringLiteral("action:preserve_alpha")};
    for (int w = 0; w < d->widgets.size(); w++) {
        const QString key = d->items.value(d->widgetItem[w]);
        const bool brush = brushOnly.contains(key) || key.startsWith(brushPrefix);
        d->widgets[w]->setVisible(brush ? paintsWithBrush(tool) : painting.contains(key) ? paints(tool) : true);
    }
}

void KisStudioBarButtons::showCard(int index)
{
    const int widgetIndex = d->widgetItem.indexOf(index);
    if (widgetIndex < 0) {
        return;
    }
    const QString key = d->items.value(index);
    KisStudioFind::Item item;
    d->resolve(key, &item);
    auto *card = new KisStudioItemCard(d->mainWindow, item.icon, item.title, item.action);
    card->addCommand({KisIconUtils::loadIcon(QStringLiteral("list-add")), i18nc("@info:tooltip", "Add a button"),
                      [this, index]() {
                          if (KisStudioFind *find = d->mainWindow->studioFind()) {
                              find->pick(i18nc("@info:placeholder", "Add to the bar"), [this, index](const QString &picked) {
                                  QStringList items = d->items;
                                  items.removeAll(picked);
                                  items.insert(qBound(0, index + 1, items.size()), picked);
                                  setItems(items);
                              });
                          }
                      }});
    card->addCommand({KisIconUtils::loadIcon(QStringLiteral("edit-delete")), i18nc("@info:tooltip", "Remove"),
                      [this, index, key]() {
                          QStringList items = d->items;
                          if (index < items.size() && items[index] == key) items.removeAt(index);
                          QTimer::singleShot(0, this, [this, items]() { setItems(items); });
                      }});
    card->popupBeside(d->widgets[widgetIndex]);
}

void KisStudioBarButtons::showRestoreCard(const QPoint &globalPos)
{
    auto *card = new KisStudioItemCard(d->mainWindow, KisIconUtils::loadIcon(QStringLiteral("studio_more")),
                                       i18nc("@title the top bar's buttons", "Buttons"), nullptr);
    QVector<KisStudioItemCard::Choice> choices;
    Q_FOREACH (const QString &key, defaultItems()) {
        if (d->items.contains(key)) continue;
        KisStudioItemCard::Choice choice;
        choice.key = key;
        if (key == symmetryItem) {
            choice.icon = KisIconUtils::loadIcon(QStringLiteral("krita_tool_multihand"));
            choice.title = i18nc("@info:tooltip", "Symmetry");
        } else {
            KisStudioFind::Item item;
            if (!d->resolve(key, &item)) continue;
            choice.icon = item.icon;
            choice.title = item.title;
            choice.action = item.action;
        }
        choice.pick = [this, key]() { setItems(d->items + QStringList{key}); };
        choices << choice;
    }
    if (!choices.isEmpty()) {
        card->setChoices(choices);
    }
    card->addCommand({KisIconUtils::loadIcon(QStringLiteral("list-add")), i18nc("@info:tooltip", "Add a button"),
                      [this]() {
                          if (KisStudioFind *find = d->mainWindow->studioFind()) {
                              find->pick(i18nc("@info:placeholder", "Add to the bar"),
                                         [this](const QString &picked) { setItems(d->items + QStringList{picked}); });
                          }
                      }});
    card->addCommand({KisIconUtils::loadIcon(QStringLiteral("view-refresh")), i18nc("@info:tooltip", "Reset buttons"),
                      [this]() { setItems(defaultItems()); }});
    // an invisible anchor at the click
    auto *anchor = new QWidget(this);
    anchor->setGeometry(QRect(mapFromGlobal(globalPos), QSize(1, 1)));
    card->popupBeside(anchor);
    anchor->deleteLater();
}

void KisStudioBarButtons::contextMenuEvent(QContextMenuEvent *event)
{
    showRestoreCard(event->globalPos());
}

KisStudioBarButtons *KisStudioBarButtons::of(KisMainWindow *mainWindow)
{
    return mainWindow ? mainWindow->findChild<KisStudioBarButtons *>(QStringLiteral("studioBarButtons")) : nullptr;
}

bool KisStudioBarButtons::has(KisMainWindow *mainWindow, const QString &key)
{
    KisStudioBarButtons *bar = of(mainWindow);
    return bar && bar->d->items.contains(key);
}

void KisStudioBarButtons::toggle(KisMainWindow *mainWindow, const QString &key)
{
    KisStudioBarButtons *bar = of(mainWindow);
    if (!bar) return;
    QStringList items = bar->d->items;
    if (items.contains(key)) {
        items.removeAll(key);
    } else {
        // after the other pinned settings, before the buttons
        int at = 0;
        while (at < items.size() && items[at].startsWith(brushPrefix)) at++;
        items.insert(at, key);
    }
    bar->setItems(items);
}

void KisStudioBarButtons::dragItem(const QString &key, QWidget *source)
{
    auto *drag = new QDrag(source);
    auto *mime = new QMimeData();
    mime->setData(KisStudioShortcuts::itemMimeType(), key.toUtf8());
    drag->setMimeData(mime);
    drag->setPixmap(source->grab());
    drag->setHotSpot(QPoint(source->width() / 2, source->height() / 2));
    drag->exec(Qt::CopyAction);
}

void KisStudioBarButtons::startDrag(int index, QWidget *source, const QPixmap &pixmap)
{
    const QString dragged = d->items.value(index);
    auto *drag = new QDrag(source);
    auto *mime = new QMimeData();
    mime->setData(KisStudioShortcuts::itemMimeType(), dragged.toUtf8());
    mime->setData(indexMimeType, QByteArray::number(index));
    drag->setMimeData(mime);
    if (!pixmap.isNull()) {
        drag->setPixmap(pixmap);
    }
    // dropping it off the bar removes it, unless the drag was cancelled with Esc
    if (drag->exec(Qt::MoveAction) == Qt::IgnoreAction && !KisModernStyle::pointerHeld()) {
        QStringList items = d->items;
        if (index < items.size() && items[index] == dragged) {
            items.removeAt(index);
        }
        QTimer::singleShot(0, this, [this, items]() { setItems(items); });
    }
}

int KisStudioBarButtons::dropIndex(int x) const
{
    for (int w = 0; w < d->widgets.size(); w++) {
        if (d->widgets[w]->isVisible() && x < d->widgets[w]->geometry().center().x()) {
            return d->widgetItem[w];
        }
    }
    return d->items.size();
}

void KisStudioBarButtons::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasFormat(KisStudioShortcuts::itemMimeType())) {
        event->acceptProposedAction();
    }
}

void KisStudioBarButtons::dragMoveEvent(QDragMoveEvent *event)
{
    const QString key = QString::fromUtf8(event->mimeData()->data(KisStudioShortcuts::itemMimeType()));
    KisStudioFind::Item item;
    // only what can be a button: the bar's own items and what Find can run
    if (key != symmetryItem && !d->resolve(key, &item)) {
        event->ignore();
        return;
    }
    event->acceptProposedAction();
    const int index = dropIndex(event->pos().x());
    int x = width() - KisModernStyle::space1;
    for (int w = 0; w < d->widgets.size(); w++) {
        if (d->widgets[w]->isVisible() && d->widgetItem[w] >= index) {
            x = d->widgets[w]->geometry().left() - 1;
            break;
        }
    }
    d->indicator->setGeometry(x - 1, 4, 2, height() - 8);
    d->indicator->show();
    d->indicator->raise();
}

void KisStudioBarButtons::dragLeaveEvent(QDragLeaveEvent *)
{
    d->indicator->hide();
}

void KisStudioBarButtons::dropEvent(QDropEvent *event)
{
    d->indicator->hide();
    const QString key = QString::fromUtf8(event->mimeData()->data(KisStudioShortcuts::itemMimeType()));
    if (key.isEmpty()) {
        return;
    }
    int to = dropIndex(event->pos().x());
    QStringList items = d->items;
    int from = items.indexOf(key);
    if (from >= 0) {
        items.removeAt(from);
        if (to > from) to--;
    }
    items.insert(qBound(0, to, items.size()), key);
    event->setDropAction(from >= 0 && event->mimeData()->hasFormat(indexMimeType) ? Qt::MoveAction : Qt::CopyAction);
    event->accept();
    // deferred: the dragged widget is still in use by its drag
    QTimer::singleShot(0, this, [this, items]() { setItems(items); });
}

QMenu *KisStudioBarButtons::symmetryMenu()
{
    if (d->symmetryMenu) {
        return d->symmetryMenu;
    }
    // owned by the bar, not a button: rearranging the bar rebuilds the buttons
    d->symmetryMenu = new QMenu(this);
    QMenu *menu = d->symmetryMenu;
    auto *panel = new QWidget(menu);
    panel->setFixedWidth(230);
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(KisModernStyle::space3, KisModernStyle::space2, KisModernStyle::space3, KisModernStyle::space2);
    layout->setSpacing(KisModernStyle::space2);
    KisStudioSymmetry *symmetry = KisStudioSymmetry::instance();
    KisKActionCollection *actions = d->actions();
    // the menu's own text color, as it sits on its own glass rather than the bar's
    const QColor ink = menu->palette().color(QPalette::WindowText);

    auto toggle = [panel](const QIcon &icon, const QString &text) {
        auto *button = new QToolButton(panel);
        button->setCheckable(true);
        button->setAutoRaise(true);
        button->setIcon(icon);
        button->setIconSize(QSize(20, 20));
        button->setText(text);
        button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        button->setFocusPolicy(Qt::NoFocus);
        // buttons in a row share its width equally
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        return button;
    };
    auto row = [](QWidget *parent, const QString &label, QWidget *control) {
        auto *line = new QWidget(parent);
        auto *lineLayout = new QHBoxLayout(line);
        lineLayout->setContentsMargins(0, 0, 0, 0);
        lineLayout->addWidget(new QLabel(label, line));
        lineLayout->addStretch(1);
        control->setFixedWidth(96);
        lineLayout->addWidget(control);
        return line;
    };

    // picking a mode also turns symmetry on
    auto *modes = new QHBoxLayout();
    modes->setSpacing(KisModernStyle::hairline);
    auto *modeGroup = new QButtonGroup(panel);
    const QStringList modeNames = {i18nc("symmetry mode", "Mirror"), i18nc("symmetry mode", "Radial"), i18nc("symmetry mode", "Tile")};
    const QStringList modeTips = {
        i18nc("@info:tooltip", "Mirrored across an axis, with every brush tool. Drag the dot on an axis to move it."),
        i18nc("@info:tooltip", "Copies around a center. Drag the center to move it, the dot beside it to turn; double-click the center to reset."),
        i18nc("@info:tooltip", "Each stroke repeats across the image, or the selection, at this spacing."),
    };
    for (int mode = KisStudioSymmetry::Mirror; mode <= KisStudioSymmetry::Tile; mode++) {
        QToolButton *button = toggle(symmetryIcon(mode, ink), modeNames[mode]);
        button->setToolTip(modeTips[mode]);
        modeGroup->addButton(button, mode);
        modes->addWidget(button);
    }
    layout->addLayout(modes);

    // mirror: which axes to use (Krita's mirror axes, moved and rotated on the canvas)
    auto *mirrorPage = new QWidget(panel);
    auto *mirrorLayout = new QHBoxLayout(mirrorPage);
    mirrorLayout->setContentsMargins(0, 0, 0, 0);
    QAction *acrossAction = actions->action(QStringLiteral("hmirror_action"));
    QAction *downAction = actions->action(QStringLiteral("vmirror_action"));
    QToolButton *across = toggle(acrossAction ? acrossAction->icon() : QIcon(), i18nc("mirror axis", "Across"));
    QToolButton *down = toggle(downAction ? downAction->icon() : QIcon(), i18nc("mirror axis", "Down"));
    mirrorLayout->setSpacing(KisModernStyle::hairline);
    mirrorLayout->addWidget(across);
    mirrorLayout->addWidget(down);
    layout->addWidget(mirrorPage);

    // radial: number of copies, and whether neighbouring copies are mirrored
    auto *radialPage = new QWidget(panel);
    auto *radialLayout = new QVBoxLayout(radialPage);
    radialLayout->setContentsMargins(0, 0, 0, 0);
    auto *copies = new QSpinBox(radialPage);
    copies->setRange(2, KisStudioSymmetry::maximumCopies);
    radialLayout->addWidget(row(radialPage, i18nc("radial symmetry", "Copies"), copies));
    auto *mirrored = new QCheckBox(i18nc("radial symmetry", "Kaleidoscope"), radialPage);
    radialLayout->addWidget(mirrored);
    layout->addWidget(radialPage);

    // tile: spacing between copies
    auto *tilePage = new QWidget(panel);
    auto *tileLayout = new QVBoxLayout(tilePage);
    tileLayout->setContentsMargins(0, 0, 0, 0);
    auto spacingBox = [tilePage]() {
        auto *box = new QSpinBox(tilePage);
        box->setRange(0, 20000);
        box->setSingleStep(10);
        box->setSuffix(i18nc("pixels", " px"));
        box->setSpecialValueText(i18nc("tile spacing of 0", "None"));
        return box;
    };
    QSpinBox *spacingX = spacingBox();
    QSpinBox *spacingY = spacingBox();
    tileLayout->addWidget(row(tilePage, i18nc("tile spacing", "Across"), spacingX));
    tileLayout->addWidget(row(tilePage, i18nc("tile spacing", "Down"), spacingY));
    layout->addWidget(tilePage);

    // give the pages one height, since the open menu doesn't resize when they switch
    const int pageHeight = std::max({mirrorPage->sizeHint().height(), radialPage->sizeHint().height(), tilePage->sizeHint().height()});
    for (QWidget *page : {mirrorPage, radialPage, tilePage}) {
        page->setFixedHeight(pageHeight);
        page->layout()->setAlignment(Qt::AlignTop);
    }

    auto *action = new QWidgetAction(menu);
    action->setDefaultWidget(panel);
    menu->addAction(action);

    // sync the controls with the current settings when the menu opens and on every change
    auto show = [=]() {
        const int mode = symmetry->mode();
        if (QAbstractButton *button = modeGroup->button(mode)) button->setChecked(true);
        mirrorPage->setVisible(mode == KisStudioSymmetry::Mirror);
        radialPage->setVisible(mode == KisStudioSymmetry::Radial);
        tilePage->setVisible(mode == KisStudioSymmetry::Tile);
        across->setChecked(symmetry->mirrorAcross());
        down->setChecked(symmetry->mirrorDown());
        const QSignalBlocker blockCopies(copies), blockMirrored(mirrored), blockX(spacingX), blockY(spacingY);
        copies->setValue(symmetry->copies());
        mirrored->setChecked(symmetry->mirrored());
        spacingX->setValue(symmetry->spacing().width());
        spacingY->setValue(symmetry->spacing().height());
    };
    connect(menu, &QMenu::aboutToShow, panel, show);
    connect(modeGroup, QOverload<int>::of(&QButtonGroup::buttonClicked), panel, [actions, symmetry, show](int mode) {
        symmetry->setMode(KisStudioSymmetry::Mode(mode));
        KisStudioSymmetry::setOn(actions, true);
        show();
    });
    auto axes = [actions, symmetry, across, down]() {
        symmetry->setMirrorAxes(across->isChecked(), down->isChecked());
        if (symmetry->mode() == KisStudioSymmetry::Mirror) KisStudioSymmetry::setOn(actions, true);
        across->setChecked(symmetry->mirrorAcross());
        down->setChecked(symmetry->mirrorDown());
    };
    connect(across, &QToolButton::clicked, panel, axes);
    connect(down, &QToolButton::clicked, panel, axes);
    connect(copies, QOverload<int>::of(&QSpinBox::valueChanged), panel, [symmetry](int value) { symmetry->setCopies(value); });
    connect(mirrored, &QCheckBox::toggled, panel, [symmetry](bool on) { symmetry->setMirrored(on); });
    auto spacing = [symmetry, spacingX, spacingY]() { symmetry->setSpacing(QSize(spacingX->value(), spacingY->value())); };
    connect(spacingX, QOverload<int>::of(&QSpinBox::valueChanged), panel, spacing);
    connect(spacingY, QOverload<int>::of(&QSpinBox::valueChanged), panel, spacing);
    return menu;
}
