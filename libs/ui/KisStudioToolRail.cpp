/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioToolRail.h"

#include <kis_action_registry.h>

#include <QAction>
#include <QApplication>
#include <QContextMenuEvent>
#include <QDrag>
#include <QDragEnterEvent>
#include <QFrame>
#include <QMimeData>
#include <QPainter>
#include <QPointer>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <KSharedConfig>
#include <KoCanvasController.h>
#include <KoToolFactoryBase.h>
#include <KoToolManager.h>
#include <KoToolRegistry.h>
#include <kactioncollection.h>
#include <kconfiggroup.h>
#include <klocalizedstring.h>
#include <KisResourceModel.h>

#include "KisMainWindow.h"
#include "KisResourceServerProvider.h"
#include "KisStudioFind.h"
#include "KisStudioItemCard.h"
#include "KisView.h"
#include "canvas/kis_canvas_controller.h"
#include "KisViewManager.h"
#include "kis_canvas_resource_provider.h"
#include "kis_config.h"
#include "kis_icon_utils.h"
#include "kis_paintop_preset.h"

namespace {

const QString brushToolId = QStringLiteral("KritaShape/KisToolBrush");
const QString itemsKey = QStringLiteral("StudioRailItems");
const QString hiddenKey = QStringLiteral("StudioHiddenVariants");
const QString slotPrefix = QStringLiteral("slot:");
const QString toolPrefix = QStringLiteral("tool:");

enum class BrushMode {
    None,
    Paint,
    Blend,
    Erase,
};

struct ToolSlot {
    QString id;
    QString title;
    QString icon;
    BrushMode brush;
    QStringList tools; ///< the variants, the first is the default
};

QVector<ToolSlot> toolSlots()
{
    return {
        {QStringLiteral("paint"), i18nc("@info:tooltip tool slot", "Paint"), QStringLiteral("studio_paint"), BrushMode::Paint,
         {brushToolId, QStringLiteral("KritaShape/KisToolDyna"), QStringLiteral("KritaShape/KisToolMultiBrush"),
          QStringLiteral("KritaShape/KisToolSmartPatch")}},
        {QStringLiteral("blend"), i18nc("@info:tooltip tool slot", "Blend"), QStringLiteral("studio_blend"), BrushMode::Blend,
         {brushToolId}},
        {QStringLiteral("erase"), i18nc("@info:tooltip tool slot", "Erase"), QStringLiteral("studio_erase"), BrushMode::Erase,
         {brushToolId}},
        {QStringLiteral("select"), i18nc("@info:tooltip tool slot", "Select"), QStringLiteral("studio_select"), BrushMode::None,
         {QStringLiteral("KisToolSelectRectangular"), QStringLiteral("KisToolSelectElliptical"), QStringLiteral("KisToolSelectOutline"),
          QStringLiteral("KisToolSelectPolygonal"), QStringLiteral("KisToolSelectContiguous"), QStringLiteral("KisToolSelectSimilar"),
          QStringLiteral("KisToolSelectMagnetic"), QStringLiteral("KisToolSelectPath")}},
        {QStringLiteral("move"), i18nc("@info:tooltip tool slot", "Move and Transform"), QStringLiteral("studio_move"), BrushMode::None,
         {QStringLiteral("KritaTransform/KisToolMove"), QStringLiteral("KisToolTransform"), QStringLiteral("KisToolCrop"),
          QStringLiteral("InteractionTool")}},
        {QStringLiteral("fill"), i18nc("@info:tooltip tool slot", "Fill"), QStringLiteral("studio_fill"), BrushMode::None,
         {QStringLiteral("KritaFill/KisToolFill"), QStringLiteral("KritaFill/KisToolGradient"), QStringLiteral("KisToolEncloseAndFill"),
          QStringLiteral("KritaShape/KisToolLazyBrush")}},
        {QStringLiteral("shape"), i18nc("@info:tooltip tool slot", "Shape"), QStringLiteral("studio_shape"), BrushMode::None,
         {QStringLiteral("KritaShape/KisToolRectangle"), QStringLiteral("KritaShape/KisToolEllipse"), QStringLiteral("KritaShape/KisToolLine"),
          QStringLiteral("KisToolPolygon"), QStringLiteral("KisToolPolyline"), QStringLiteral("KisToolPath"), QStringLiteral("KisToolPencil"),
          QStringLiteral("PathTool"), QStringLiteral("KarbonCalligraphyTool"), QStringLiteral("KritaShape/KisToolKnife")}},
        {QStringLiteral("text"), i18nc("@info:tooltip tool slot", "Text"), QStringLiteral("studio_text"), BrushMode::None,
         {QStringLiteral("SvgTextTool")}},
        {QStringLiteral("guides"), i18nc("@info:tooltip tool slot", "Guides and References"), QStringLiteral("studio_guides"), BrushMode::None,
         {QStringLiteral("KisAssistantTool"), QStringLiteral("ToolReferenceImages"), QStringLiteral("KritaShape/KisToolMeasure")}},
    };
}

QStringList defaultItems()
{
    QStringList items;
    Q_FOREACH (const ToolSlot &slot, toolSlots()) {
        items << slotPrefix + slot.id;
    }
    return items;
}

/**
 * A rail button: a click uses it, a long press or right click shows its
 * card, dragging moves it along the rail or, off the rail, removes it.
 */
class RailButton : public QToolButton
{
public:
    explicit RailButton(QWidget *parent)
        : QToolButton(parent)
    {
        setAutoRaise(true);
        setCheckable(true);
        setFixedSize(40, 40);
        setIconSize(QSize(22, 22));
        setFocusPolicy(Qt::NoFocus);
        setContextMenuPolicy(Qt::PreventContextMenu);
        m_holdTimer.setSingleShot(true);
        m_holdTimer.setInterval(450);
        QObject::connect(&m_holdTimer, &QTimer::timeout, this, [this]() {
            m_held = true;
            setDown(false);
            if (onCard) onCard();
        });
    }

    std::function<void()> onCard;
    std::function<void()> onDrag;
    bool hasVariants {false};
    QPointer<QAction> shortcutAction;

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::RightButton) {
            if (onCard) onCard();
            return;
        }
        if (event->button() == Qt::LeftButton) {
            m_held = false;
            m_dragged = false;
            m_pressPos = event->pos();
            m_holdTimer.start();
        }
        QToolButton::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        // a pen wobbles: only a clear move is a drag
        if ((event->buttons() & Qt::LeftButton) && !m_held && !m_dragged
            && (event->pos() - m_pressPos).manhattanLength() > 2 * QApplication::startDragDistance()) {
            m_holdTimer.stop();
            m_dragged = true;
            setDown(false);
            if (onDrag) onDrag();
            return;
        }
        QToolButton::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        m_holdTimer.stop();
        if (m_held || m_dragged) {
            // the hold showed the card or the drag moved it: not a click
            m_held = false;
            m_dragged = false;
            setDown(false);
            event->accept();
            return;
        }
        QToolButton::mouseReleaseEvent(event);
    }

    void paintEvent(QPaintEvent *event) override
    {
        QToolButton::paintEvent(event);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        if (hasVariants) {
            QColor mark = palette().color(QPalette::ButtonText);
            mark.setAlphaF(isChecked() ? 0.8 : 0.4);
            painter.setPen(Qt::NoPen);
            painter.setBrush(mark);
            const QPointF corner(width() - 5, 5);
            painter.drawPolygon(QPolygonF() << corner << corner - QPointF(5, 0) << corner + QPointF(0, 5));
        }
        if (shortcutAction && !shortcutAction->shortcut().isEmpty()) {
            KisStudioShortcuts::paintBadge(painter, rect(), KisStudioShortcuts::badgeText(shortcutAction->shortcut()), palette());
        }
    }

private:
    QTimer m_holdTimer;
    QPoint m_pressPos;
    bool m_held {false};
    bool m_dragged {false};
};

/// The "+" at the bottom: adds a tool, an effect, anything Find knows
class PlusButton : public QToolButton
{
public:
    explicit PlusButton(QWidget *parent) : QToolButton(parent)
    {
        setAutoRaise(true);
        setFixedSize(40, 32);
        setFocusPolicy(Qt::NoFocus);
        setToolTip(i18nc("@info:tooltip", "Add a tool, an effect or anything else (or drag one here from Find)"));
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        QToolButton::paintEvent(event);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        QColor ink = palette().color(QPalette::ButtonText);
        ink.setAlphaF(underMouse() ? 0.9 : 0.55);
        QPen pen(ink, 1.6);
        pen.setCapStyle(Qt::RoundCap);
        painter.setPen(pen);
        const QPointF c = QRectF(rect()).center();
        painter.drawLine(c - QPointF(6, 0), c + QPointF(6, 0));
        painter.drawLine(c - QPointF(0, 6), c + QPointF(0, 6));
    }
};

} // namespace

struct KisStudioToolRail::Private
{
    KisMainWindow *mainWindow {nullptr};
    QVBoxLayout *layout {nullptr};
    QVBoxLayout *itemsLayout {nullptr};
    QFrame *dropIndicator {nullptr};
    QToolButton *plus {nullptr};
    bool hovered {false};
    bool dragOver {false};

    QHash<QString, ToolSlot> slots;        ///< the built-in slots this build has, by id
    QHash<QString, QString> currentTool;   ///< slot id -> the variant it uses now
    QSet<QString> hidden;                  ///< variants hidden from their slot's card

    QStringList items;                     ///< what is on the rail, in order
    QVector<RailButton *> buttons;
    QVector<int> buttonItem;               ///< for every button, its index in items
    QHash<QString, QPointer<QAction>> ownActions; ///< shortcuts for items that have no action of their own

    QString activeSlot;
    QString brushSlot;                     ///< Paint or Blend, whichever the brush belongs to
    bool loadingPreset {false};

    QAction *eraseAction() const
    {
        return mainWindow->actionCollection()->action(QStringLiteral("erase_action"));
    }

    KisCanvasResourceProvider *resources() const
    {
        return mainWindow->viewManager() ? mainWindow->viewManager()->canvasResourceProvider() : nullptr;
    }

    QString slotWithMode(BrushMode mode) const
    {
        for (auto it = slots.constBegin(); it != slots.constEnd(); ++it) {
            if (it->brush == mode) return it.key();
        }
        return QString();
    }

    QStringList visibleVariants(const QString &slotId) const
    {
        QStringList visible;
        Q_FOREACH (const QString &tool, slots.value(slotId).tools) {
            if (!hidden.contains(tool)) visible << tool;
        }
        if (visible.isEmpty() && !slots.value(slotId).tools.isEmpty()) {
            visible << slots.value(slotId).tools.first();
        }
        return visible;
    }

    QAction *toolAction(const QString &toolId) const
    {
        return mainWindow->actionCollection()->action(toolId);
    }

    std::function<void(const QString &slotId)> activate;

    /**
     * What a slot's shortcut is: Erase is Krita's eraser toggle (E), Paint the
     * brush tool (B), the others their current variant's tool; Blend, which
     * no tool stands for, gets an action of its own.
     */
    QAction *slotAction(const QString &slotId)
    {
        const BrushMode mode = slots.value(slotId).brush;
        if (mode == BrushMode::Erase) {
            return eraseAction();
        }
        if (mode != BrushMode::Blend) {
            return toolAction(currentTool.value(slotId));
        }
        QPointer<QAction> &own = ownActions[slotPrefix + slotId];
        if (!own) {
            own = new QAction(slots.value(slotId).title, mainWindow);
            own->setObjectName(QStringLiteral("studio_slot_") + slotId);
            own->setShortcutContext(Qt::WindowShortcut);
            const QString saved = KConfigGroup(KSharedConfig::openConfig(QStringLiteral("kritashortcutsrc")), QStringLiteral("Shortcuts"))
                                      .readEntry(own->objectName(), QString());
            if (!saved.isEmpty() && saved != QLatin1String("none")) {
                own->setShortcuts(QKeySequence::listFromString(saved));
            }
            mainWindow->addAction(own);
            QObject::connect(own, &QAction::triggered, mainWindow, [this, slotId]() {
                if (activate) activate(slotId);
            });
        }
        return own;
    }

    /// The action whose shortcut a button shows and the card edits
    QAction *shortcutActionFor(const QString &key)
    {
        if (key.startsWith(slotPrefix)) {
            return slotAction(key.mid(slotPrefix.size()));
        }
        KisStudioFind::Item item;
        if (mainWindow->studioFind() && mainWindow->studioFind()->item(key, &item) && item.action) {
            return item.action;
        }
        QPointer<QAction> &own = ownActions[key];
        if (!own) {
            // brushes, blend modes and the like get an action of their own for their shortcut
            own = new QAction(item.title.isEmpty() ? key : item.title, mainWindow);
            own->setObjectName(QStringLiteral("studio_item_") + key);
            own->setShortcutContext(Qt::WindowShortcut);
            const QString saved = KConfigGroup(KSharedConfig::openConfig(QStringLiteral("kritashortcutsrc")), QStringLiteral("Shortcuts"))
                                      .readEntry(own->objectName(), QString());
            if (!saved.isEmpty() && saved != QLatin1String("none")) {
                own->setShortcuts(QKeySequence::listFromString(saved));
            }
            mainWindow->addAction(own);
            QObject::connect(own, &QAction::triggered, mainWindow, [this, key]() {
                if (mainWindow->studioFind()) mainWindow->studioFind()->run(key);
            });
        }
        return own;
    }

    /// Whether a canvas controller belongs to this rail's window
    bool isOwnController(KoCanvasController *controller) const
    {
        KisView *active = mainWindow->activeView();
        if (!controller || !active) return true;
        return static_cast<KoCanvasController *>(active->canvasController()) == controller;
    }
};

QString KisStudioToolRail::toolName(KoToolFactoryBase *factory)
{
    // the action's text is the short name ("Transform Tool"); the tooltip may be a sentence
    QString name;
    if (KisActionRegistry::instance()->hasAction(factory->id())) {
        name = KisActionRegistry::instance()->getActionProperty(factory->id(), QStringLiteral("text"));
    }
    if (name.isEmpty()) {
        name = factory->toolTip();
    }
    name.remove(QLatin1Char('&'));
    if (name.endsWith(QStringLiteral(" Tool"))) {
        name.chop(5);
    }
    return name;
}

KisStudioToolRail::KisStudioToolRail(KisMainWindow *mainWindow, QWidget *parent)
    : QWidget(parent)
    , d(new Private)
{
    d->mainWindow = mainWindow;
    setAcceptDrops(true);

    d->layout = new QVBoxLayout(this);
    d->layout->setContentsMargins(4, 4, 4, 4);
    d->layout->setSpacing(2);
    d->itemsLayout = new QVBoxLayout();
    d->itemsLayout->setContentsMargins(0, 0, 0, 0);
    d->itemsLayout->setSpacing(2);
    d->layout->addLayout(d->itemsLayout);

    d->plus = new PlusButton(this);
    connect(d->plus, &QToolButton::clicked, this, [this]() {
        if (KisStudioFind *find = d->mainWindow->studioFind()) {
            find->pick(i18nc("@info:placeholder", "Add to the tools"), [this](const QString &key) {
                if (!d->items.contains(key)) {
                    setItems(d->items + QStringList{key});
                }
            });
        }
    });
    d->layout->addWidget(d->plus, 0, Qt::AlignHCenter);

    d->dropIndicator = new QFrame(this);
    d->dropIndicator->setAutoFillBackground(true);
    d->dropIndicator->setBackgroundRole(QPalette::Highlight);
    d->dropIndicator->hide();

    KisConfig cfg(true);
    Q_FOREACH (const ToolSlot &slot, toolSlots()) {
        // only the variants this build has
        ToolSlot available = slot;
        available.tools.clear();
        Q_FOREACH (const QString &tool, slot.tools) {
            if (KoToolRegistry::instance()->value(tool)) {
                available.tools << tool;
            }
        }
        if (available.tools.isEmpty()) {
            continue;
        }
        d->slots.insert(slot.id, available);
        const QString remembered = cfg.readEntry<QString>(QStringLiteral("StudioToolSlot_") + slot.id, available.tools.first());
        d->currentTool.insert(slot.id, available.tools.contains(remembered) ? remembered : available.tools.first());
    }

    const QStringList hidden = cfg.readEntry<QStringList>(hiddenKey, QStringList());
    d->hidden = QSet<QString>(hidden.begin(), hidden.end());
    d->items = cfg.readEntry<QStringList>(itemsKey, defaultItems());
    d->activate = [this](const QString &slotId) { activateSlot(slotId); };

    connect(KoToolManager::instance(), &KoToolManager::changedTool, this, [this](KoCanvasController *controller) {
        // other windows have their own rails
        if (d->isOwnController(controller)) updateActiveSlot();
    });
    if (QAction *erase = d->eraseAction()) {
        connect(erase, &QAction::toggled, this, &KisStudioToolRail::updateActiveSlot);
    }
    if (KisCanvasResourceProvider *resources = d->resources()) {
        connect(resources, &KisCanvasResourceProvider::sigPaintOpPresetChanged, this, &KisStudioToolRail::rememberPreset);
    }

    // the current brush decides between Paint and Blend, see rememberPreset()
    d->brushSlot = d->slotWithMode(BrushMode::Paint);
    rebuild();
    rememberPreset();
    updateActiveSlot();
}

KisStudioToolRail::~KisStudioToolRail() = default;

void KisStudioToolRail::addBottomWidget(QWidget *widget)
{
    widget->setParent(this);
    d->layout->addWidget(widget, 0, Qt::AlignHCenter);
}

void KisStudioToolRail::setItems(const QStringList &items)
{
    d->items = items;
    d->items.removeDuplicates();
    KisConfig(false).writeEntry(itemsKey, d->items);
    rebuild();
    updateActiveSlot();
}

void KisStudioToolRail::updatePlus()
{
    // there when it can be used: pointed at, dragged onto, or nothing on the rail yet
    d->plus->setVisible(d->items.isEmpty() || d->buttons.isEmpty() || d->hovered || d->dragOver);
}

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
void KisStudioToolRail::enterEvent(QEnterEvent *event)
#else
void KisStudioToolRail::enterEvent(QEvent *event)
#endif
{
    d->hovered = true;
    updatePlus();
    QWidget::enterEvent(event);
}

void KisStudioToolRail::leaveEvent(QEvent *event)
{
    d->hovered = false;
    updatePlus();
    QWidget::leaveEvent(event);
}

void KisStudioToolRail::rebuild()
{
    while (QLayoutItem *item = d->itemsLayout->takeAt(0)) {
        if (QWidget *widget = item->widget()) {
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }
    d->buttons.clear();
    d->buttonItem.clear();

    KisStudioFind *find = d->mainWindow->studioFind();
    BrushMode previousMode = BrushMode::None;
    bool previousWasSlot = true;
    bool first = true;

    for (int i = 0; i < d->items.size(); i++) {
        const QString key = d->items[i];
        const bool isSlot = key.startsWith(slotPrefix);
        QIcon icon;
        QString title;
        BrushMode mode = BrushMode::None;
        bool variants = false;

        if (isSlot) {
            const QString slotId = key.mid(slotPrefix.size());
            if (!d->slots.contains(slotId)) continue;
            const ToolSlot &slot = d->slots[slotId];
            icon = KisIconUtils::loadIcon(slot.icon);
            title = slot.title;
            mode = slot.brush;
            variants = d->visibleVariants(slotId).size() > 1;
        } else {
            KisStudioFind::Item item;
            if (!find || !find->item(key, &item)) continue;
            icon = item.icon.isNull() ? KisIconUtils::loadIcon(QStringLiteral("studio_more")) : item.icon;
            title = item.title;
        }

        // gaps between the brush slots, the other slots and your own items
        if (!first && ((previousMode != BrushMode::None && mode == BrushMode::None && isSlot) || (previousWasSlot && !isSlot))) {
            d->itemsLayout->addSpacing(10);
        }
        first = false;
        previousMode = mode;
        previousWasSlot = isSlot;

        auto *button = new RailButton(this);
        button->setIcon(icon);
        button->setAccessibleName(title);
        button->hasVariants = variants;
        button->shortcutAction = d->shortcutActionFor(key);
        const int index = i;
        button->onCard = [this, index]() { showCard(index); };
        button->onDrag = [this, index, button, icon]() {
            const QString dragged = d->items.value(index);
            auto *drag = new QDrag(button);
            auto *mime = new QMimeData();
            mime->setData(KisStudioShortcuts::itemMimeType(), dragged.toUtf8());
            drag->setMimeData(mime);
            drag->setPixmap(icon.pixmap(32, 32));
            // dropped anywhere but on the rail: it is taken off the rail
            if (drag->exec(Qt::MoveAction) == Qt::IgnoreAction) {
                QStringList items = d->items;
                items.removeAll(dragged);
                QTimer::singleShot(0, this, [this, items]() { setItems(items); });
            }
        };
        connect(button, &QToolButton::clicked, this, [this, index]() { runItem(index); });
        d->itemsLayout->addWidget(button, 0, Qt::AlignHCenter);
        d->buttons << button;
        d->buttonItem << i;
    }
    updatePlus();
}

void KisStudioToolRail::runItem(int index)
{
    const QString key = d->items.value(index);
    if (key.startsWith(slotPrefix)) {
        const QString slotId = key.mid(slotPrefix.size());
        // clicking the active slot again shows its variants
        if (slotId == d->activeSlot && d->visibleVariants(slotId).size() > 1) {
            showCard(index);
            updateActiveSlot();
        } else {
            activateSlot(slotId);
        }
        return;
    }
    if (key.startsWith(toolPrefix)) {
        KoToolManager::instance()->switchToolRequested(key.mid(toolPrefix.size()));
    } else if (KisStudioFind *find = d->mainWindow->studioFind()) {
        find->run(key);
    }
    updateActiveSlot();
}

void KisStudioToolRail::showCard(int index)
{
    const int buttonIndex = d->buttonItem.indexOf(index);
    if (buttonIndex < 0) {
        return;
    }
    RailButton *button = d->buttons[buttonIndex];
    const QString key = d->items.value(index);
    KisStudioItemCard *card = nullptr;

    if (key.startsWith(slotPrefix)) {
        const QString slotId = key.mid(slotPrefix.size());
        const ToolSlot slot = d->slots.value(slotId);
        card = new KisStudioItemCard(d->mainWindow, KisIconUtils::loadIcon(slot.icon), slot.title, d->slotAction(slotId));
        const QStringList variants = d->visibleVariants(slotId);
        if (variants.size() > 1) {
            QVector<KisStudioItemCard::Choice> choices;
            Q_FOREACH (const QString &toolId, variants) {
                KoToolFactoryBase *factory = KoToolRegistry::instance()->value(toolId);
                if (!factory) continue;
                KisStudioItemCard::Choice choice;
                choice.key = toolPrefix + toolId;
                choice.icon = KisIconUtils::loadIcon(factory->iconName());
                choice.title = toolName(factory);
                choice.action = d->toolAction(toolId);
                choice.current = toolId == d->currentTool.value(slotId);
                choice.pick = [this, slotId, toolId]() { activateTool(slotId, toolId); };
                choice.hide = [this, toolId, card]() {
                    d->hidden.insert(toolId);
                    KisConfig(false).writeEntry(hiddenKey, QStringList(d->hidden.values()));
                    if (card) card->close();
                    rebuild();
                    updateActiveSlot();
                };
                choices << choice;
            }
            card->setChoices(choices);
        }
        QStringList hiddenHere;
        Q_FOREACH (const QString &toolId, slot.tools) {
            if (d->hidden.contains(toolId)) hiddenHere << toolId;
        }
        if (!hiddenHere.isEmpty()) {
            card->addCommand({KisIconUtils::loadIcon(QStringLiteral("visible")),
                              i18nc("@info:tooltip", "Show the %1 hidden variants", hiddenHere.size()),
                              [this, hiddenHere]() {
                                  Q_FOREACH (const QString &toolId, hiddenHere) d->hidden.remove(toolId);
                                  KisConfig(false).writeEntry(hiddenKey, QStringList(d->hidden.values()));
                                  rebuild();
                                  updateActiveSlot();
                              }});
        }
    } else {
        KisStudioFind::Item item;
        if (d->mainWindow->studioFind()) d->mainWindow->studioFind()->item(key, &item);
        card = new KisStudioItemCard(d->mainWindow, item.icon, item.title, d->shortcutActionFor(key));
    }

    card->addCommand({KisIconUtils::loadIcon(QStringLiteral("edit-delete")),
                      i18nc("@info:tooltip", "Take off the rail (or drag it off)"),
                      [this, key]() {
                          QStringList items = d->items;
                          items.removeAll(key);
                          QTimer::singleShot(0, this, [this, items]() { setItems(items); });
                      }});
    // the badges show the shortcut, which the card may have changed
    connect(card, &QObject::destroyed, this, [this]() {
        Q_FOREACH (RailButton *b, d->buttons) b->update();
    });
    card->popupBeside(button);
}

void KisStudioToolRail::showRestoreCard(const QPoint &globalPos)
{
    auto *card = new KisStudioItemCard(d->mainWindow, KisIconUtils::loadIcon(QStringLiteral("studio_more")),
                                       i18nc("@title tools rail", "Rail"), nullptr);
    QVector<KisStudioItemCard::Choice> choices;
    Q_FOREACH (const ToolSlot &slot, toolSlots()) {
        if (!d->slots.contains(slot.id) || d->items.contains(slotPrefix + slot.id)) continue;
        KisStudioItemCard::Choice choice;
        choice.key = slotPrefix + slot.id;
        choice.icon = KisIconUtils::loadIcon(slot.icon);
        choice.title = slot.title;
        choice.pick = [this, id = slot.id]() { setItems(d->items + QStringList{slotPrefix + id}); };
        choices << choice;
    }
    if (!choices.isEmpty()) {
        card->setChoices(choices);
    }
    card->addCommand({KisIconUtils::loadIcon(QStringLiteral("view-refresh")),
                      i18nc("@info:tooltip", "Put back the default tools and variants"),
                      [this]() {
                          d->hidden.clear();
                          KisConfig(false).writeEntry(hiddenKey, QStringList());
                          setItems(defaultItems());
                      }});
    // an invisible anchor at the click
    auto *anchor = new QWidget(this);
    anchor->setGeometry(QRect(mapFromGlobal(globalPos), QSize(1, 1)));
    card->popupBeside(anchor);
    anchor->deleteLater();
}

void KisStudioToolRail::contextMenuEvent(QContextMenuEvent *event)
{
    showRestoreCard(event->globalPos());
}

int KisStudioToolRail::dropIndex(int y) const
{
    for (int b = 0; b < d->buttons.size(); b++) {
        if (y < d->buttons[b]->geometry().center().y()) {
            return d->buttonItem[b];
        }
    }
    return d->items.size();
}

void KisStudioToolRail::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasFormat(KisStudioShortcuts::itemMimeType())) {
        event->acceptProposedAction();
        d->dragOver = true;
        updatePlus();
    }
}

void KisStudioToolRail::dragMoveEvent(QDragMoveEvent *event)
{
    if (!event->mimeData()->hasFormat(KisStudioShortcuts::itemMimeType())) {
        return;
    }
    event->acceptProposedAction();
    const int index = dropIndex(event->pos().y());
    int y = d->buttons.isEmpty() ? 8 : d->buttons.last()->geometry().bottom() + 1;
    for (int b = 0; b < d->buttons.size(); b++) {
        if (d->buttonItem[b] >= index) {
            y = d->buttons[b]->geometry().top() - 1;
            break;
        }
    }
    d->dropIndicator->setGeometry(6, y - 1, width() - 12, 2);
    d->dropIndicator->show();
    d->dropIndicator->raise();
}

void KisStudioToolRail::dragLeaveEvent(QDragLeaveEvent *)
{
    d->dropIndicator->hide();
    d->dragOver = false;
    updatePlus();
}

void KisStudioToolRail::dropEvent(QDropEvent *event)
{
    d->dropIndicator->hide();
    d->dragOver = false;
    const QString key = QString::fromUtf8(event->mimeData()->data(KisStudioShortcuts::itemMimeType()));
    if (key.isEmpty()) {
        return;
    }
    int to = dropIndex(event->pos().y());
    QStringList items = d->items;
    const int from = items.indexOf(key);
    if (from >= 0) {
        items.removeAt(from);
        if (to > from) to--;
    }
    items.insert(qBound(0, to, items.size()), key);
    event->setDropAction(from >= 0 ? Qt::MoveAction : Qt::CopyAction);
    event->accept();
    // the dragged button is still on the stack of its drag
    QTimer::singleShot(0, this, [this, items]() { setItems(items); });
}

void KisStudioToolRail::activateSlot(const QString &slotId)
{
    activateTool(slotId, d->currentTool.value(slotId));
}

void KisStudioToolRail::activateTool(const QString &slotId, const QString &toolId)
{
    const ToolSlot s = d->slots.value(slotId);
    if (toolId.isEmpty()) {
        return;
    }
    d->currentTool[slotId] = toolId;
    KisConfig(false).writeEntry(QStringLiteral("StudioToolSlot_") + slotId, toolId);

    QAction *erase = d->eraseAction();
    if (s.brush == BrushMode::Erase) {
        KoToolManager::instance()->switchToolRequested(toolId);
        if (erase && !erase->isChecked()) {
            erase->setChecked(true);
        }
    } else if (s.brush == BrushMode::Paint || s.brush == BrushMode::Blend) {
        const bool switchingBrush = d->brushSlot != slotId;
        KisCanvasResourceProvider *resources = d->resources();
        if (switchingBrush && resources && resources->currentPreset() && !d->brushSlot.isEmpty()) {
            // the slot we leave keeps the brush it was using
            KisConfig(false).writeEntry(QStringLiteral("StudioBrushPreset_") + d->brushSlot, resources->currentPreset()->name());
        }
        d->brushSlot = slotId;
        if (erase && erase->isChecked()) {
            erase->setChecked(false);
        }
        KoToolManager::instance()->switchToolRequested(toolId);
        if (switchingBrush) {
            loadPresetForSlot(slotId);
        }
    } else {
        KoToolManager::instance()->switchToolRequested(toolId);
    }
    updateActiveSlot();
}

void KisStudioToolRail::updateActiveSlot()
{
    const QString toolId = KoToolManager::instance()->activeToolId();
    QAction *erase = d->eraseAction();
    const bool erasing = erase && erase->isChecked();

    QString active;
    if (toolId == brushToolId) {
        active = erasing ? d->slotWithMode(BrushMode::Erase) : d->brushSlot;
    } else {
        // slots on the rail first, then any slot that has the tool
        Q_FOREACH (const QString &key, d->items) {
            const QString id = key.mid(slotPrefix.size());
            if (key.startsWith(slotPrefix) && d->slots.contains(id) && d->slots[id].brush == BrushMode::None
                && d->slots[id].tools.contains(toolId)) {
                active = id;
                break;
            }
        }
        if (active.isEmpty()) {
            const QString paint = d->slotWithMode(BrushMode::Paint);
            if (!paint.isEmpty() && d->slots[paint].tools.contains(toolId)) {
                active = paint;
            }
        }
    }
    if (!active.isEmpty() && d->slots.value(active).tools.contains(toolId)) {
        d->currentTool[active] = toolId;
    }
    d->activeSlot = active;

    for (int b = 0; b < d->buttons.size(); b++) {
        const QString key = d->items.value(d->buttonItem[b]);
        RailButton *button = d->buttons[b];
        if (key.startsWith(slotPrefix)) {
            const QString id = key.mid(slotPrefix.size());
            button->setChecked(id == active);
            button->shortcutAction = d->slotAction(id);
            QString tip = d->slots.value(id).title;
            if (d->visibleVariants(id).size() > 1) {
                if (KoToolFactoryBase *factory = KoToolRegistry::instance()->value(d->currentTool.value(id))) {
                    tip = QStringLiteral("%1 · %2").arg(tip, toolName(factory));
                }
            }
            button->setToolTip(tip);
        } else {
            button->setChecked(key == toolPrefix + toolId && !d->items.contains(slotPrefix + active));
            button->setToolTip(button->accessibleName());
        }
        button->update();
    }
}

void KisStudioToolRail::rememberPreset()
{
    KisCanvasResourceProvider *resources = d->resources();
    if (d->loadingPreset || !resources || !resources->currentPreset()) {
        return;
    }
    const QString name = resources->currentPreset()->name();

    // a brush that one of the slots remembers belongs to that slot, so the
    // highlighted slot always matches the brush in use (e.g. after a restart)
    KisConfig cfg(true);
    for (auto it = d->slots.constBegin(); it != d->slots.constEnd(); ++it) {
        if ((it->brush == BrushMode::Paint || it->brush == BrushMode::Blend)
            && cfg.readEntry<QString>(QStringLiteral("StudioBrushPreset_") + it.key()) == name) {
            if (d->brushSlot != it.key()) {
                d->brushSlot = it.key();
                updateActiveSlot();
            }
            return;
        }
    }

    QAction *erase = d->eraseAction();
    if (KoToolManager::instance()->activeToolId() != brushToolId || (erase && erase->isChecked()) || d->brushSlot.isEmpty()) {
        return;
    }
    // a new brush picked while in Paint or Blend becomes that slot's brush
    KisConfig(false).writeEntry(QStringLiteral("StudioBrushPreset_") + d->brushSlot, name);
}

void KisStudioToolRail::loadPresetForSlot(const QString &slotId)
{
    KisCanvasResourceProvider *resources = d->resources();
    KisPaintOpPresetResourceServer *server = KisResourceServerProvider::instance()->paintOpPresetServer();
    if (!resources || !server) {
        return;
    }

    QString name = KisConfig(true).readEntry<QString>(QStringLiteral("StudioBrushPreset_") + slotId);
    if (name.isEmpty() && d->slots.value(slotId).brush == BrushMode::Blend) {
        // first use of Blend: a smudging brush from the bundled ones
        KisResourceModel *model = server->resourceModel();
        for (int row = 0; row < model->rowCount() && name.isEmpty(); row++) {
            const QString candidate = model->index(row, 0).data(Qt::UserRole + KisAbstractResourceModel::Name).toString();
            if (candidate.contains(QStringLiteral("blend"), Qt::CaseInsensitive)
                || candidate.contains(QStringLiteral("smudge"), Qt::CaseInsensitive)
                || candidate.contains(QStringLiteral("smear"), Qt::CaseInsensitive)) {
                name = candidate;
            }
        }
    }
    if (name.isEmpty() || (resources->currentPreset() && resources->currentPreset()->name() == name)) {
        return;
    }

    KisPaintOpPresetSP preset = server->resource(QString(), QString(), name);
    if (preset) {
        d->loadingPreset = true;
        resources->setPaintOpPreset(preset);
        d->loadingPreset = false;
        KisConfig(false).writeEntry(QStringLiteral("StudioBrushPreset_") + slotId, name);
    }
}
