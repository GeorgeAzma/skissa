/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioToolRail.h"
#include "KisStudioGlass.h"
#include <QPainterPath>

#include <kis_action_registry.h>

#include <QAction>
#include <QApplication>
#include <QContextMenuEvent>
#include <QDrag>
#include <QDragEnterEvent>
#include <QFrame>
#include <QLineEdit>
#include <QKeyEvent>
#include <QValidator>
#include <QScreen>
#include <QMimeData>
#include <QPainter>
#include <QPointer>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>

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
#include "KisModernStyle.h"
#include "KisStudioBrushPicker.h"
#include "KisStudioMotion.h"
#include "KisTouchMode.h"
#include "KisResourceServerProvider.h"
#include "KisStudioFind.h"
#include "KisStudioItemCard.h"
#include "KisView.h"
#include "KisViewManager.h"
#include "canvas/kis_canvas_controller.h"
#include "kis_canvas_resource_provider.h"
#include "kis_config.h"
#include "kis_icon_utils.h"
#include "kis_paintop_preset.h"

namespace {

const QString brushToolId = QStringLiteral("KritaShape/KisToolBrush");
/// Not a variant of its own: the brush's multibrush toggle in the top bar switches to it
const QString multiBrushToolId = QStringLiteral("KritaShape/KisToolMultiBrush");
const QString itemsKey = QStringLiteral("StudioRailItems");
const QString hiddenKey = QStringLiteral("StudioHiddenVariants");
const QString slotPrefix = QStringLiteral("slot:");
const QString toolPrefix = QStringLiteral("tool:");
const QString spacePrefix = QStringLiteral("space:");
/// The effects picker, see KisStudioEffectsPicker
const QString effectsItem = QStringLiteral("action:studio_effects");
/// Set once the effects button was put on a rail that existed before it
const QString effectsOfferedKey = QStringLiteral("StudioRailEffectsOffered");

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
         {brushToolId, QStringLiteral("KritaShape/KisToolDyna"), multiBrushToolId, QStringLiteral("KritaShape/KisToolSmartPatch")}},
        {QStringLiteral("blend"), i18nc("@info:tooltip tool slot", "Blend"), QStringLiteral("studio_blend"), BrushMode::Blend,
         {brushToolId}},
        {QStringLiteral("erase"), i18nc("@info:tooltip tool slot", "Erase"), QStringLiteral("studio_erase"), BrushMode::Erase,
         {brushToolId}},
        {QStringLiteral("select"), i18nc("@info:tooltip tool slot", "Select"), QStringLiteral("studio_select"), BrushMode::None,
         {QStringLiteral("KisToolSelectRectangular"), QStringLiteral("KisToolSelectElliptical"), QStringLiteral("KisToolSelectOutline"),
          QStringLiteral("KisToolSelectPolygonal"), QStringLiteral("KisToolSelectContiguous"), QStringLiteral("KisToolSelectSimilar"),
          QStringLiteral("KisToolSelectMagnetic"), QStringLiteral("KisToolSelectPath")}},
        {QStringLiteral("move"), i18nc("@info:tooltip tool slot", "Transform"), QStringLiteral("studio_move"), BrushMode::None,
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
        {QStringLiteral("guides"), i18nc("@info:tooltip tool slot", "Guides"), QStringLiteral("studio_guides"), BrushMode::None,
         {QStringLiteral("KisAssistantTool"), QStringLiteral("ToolReferenceImages"), QStringLiteral("KritaShape/KisToolMeasure")}},
    };
}

QStringList defaultItems()
{
    return {slotPrefix + QStringLiteral("paint"), slotPrefix + QStringLiteral("blend"), slotPrefix + QStringLiteral("erase"),
            spacePrefix,
            slotPrefix + QStringLiteral("select"), slotPrefix + QStringLiteral("move"), slotPrefix + QStringLiteral("fill"),
            slotPrefix + QStringLiteral("shape"), slotPrefix + QStringLiteral("text"), slotPrefix + QStringLiteral("guides"),
            effectsItem};
}

/// What every widget on the rail can do: be dragged along it or off it
class RailItem
{
public:
    virtual ~RailItem() = default;
    std::function<void()> onDrag;

protected:
    /// Whether a move with the button down is a drag: a pen wobbles, only a clear move is
    static bool isDrag(QMouseEvent *event, const QPoint &pressPos)
    {
        return (event->buttons() & Qt::LeftButton)
            && (event->pos() - pressPos).manhattanLength() > 2 * QApplication::startDragDistance();
    }
};

/**
 * A rail button: a click uses it, a long press or right click shows its
 * card, dragging moves it along the rail or, off the rail, removes it.
 */
class RailButton : public QToolButton, public RailItem
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
    }

    std::function<void()> onCard;
    std::function<void(int steps)> onWheel;
    bool hasVariants {false};
    QPointer<QAction> shortcutAction;

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) {
            m_dragged = false;
            m_pressPos = event->pos();
        }
        QToolButton::mousePressEvent(event);
    }

    // a right click, or a finger or pen held on it (KisTouchMode)
    void contextMenuEvent(QContextMenuEvent *event) override
    {
        event->accept();
        if (onCard) onCard();
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (!m_dragged && isDrag(event, m_pressPos)) {
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
            // the drag moved it: not a click
            m_dragged = false;
            setDown(false);
            event->accept();
            return;
        }
        QToolButton::mouseReleaseEvent(event);
    }

    void wheelEvent(QWheelEvent *event) override
    {
        const int steps = event->angleDelta().y() / 120;
        if (steps && onWheel) {
            onWheel(-steps);
            event->accept();
            return;
        }
        QToolButton::wheelEvent(event);
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
    QPoint m_pressPos;
    bool m_dragged {false};
};

/**
 * Space between groups of tools; a click names it ("Effects"), so what
 * follows reads as a group. Dragged off the rail like everything else.
 */
class SpaceItem : public QWidget, public RailItem
{
public:
    SpaceItem(const QString &name, QWidget *parent)
        : QWidget(parent), m_name(name)
    {
        setFixedSize(40, name.isEmpty() ? 14 : 20);
        setAttribute(Qt::WA_Hover);
        setCursor(Qt::PointingHandCursor);
        setToolTip(name.isEmpty() ? i18nc("@info:tooltip", "Gap: click to name, drag off to remove")
                                  : name);
    }

    std::function<void(const QString &name)> onRename;

    /// The names' look: small capitals, a little heavier than the text around
    QFont nameFont() const
    {
        QFont font = this->font();
        font.setPointSizeF(font.pointSizeF() * 0.66);
        font.setWeight(QFont::DemiBold);
        font.setCapitalization(QFont::AllUppercase);
        font.setLetterSpacing(QFont::PercentageSpacing, 104);
        return font;
    }

    /// Whether \p name is short enough to show whole: names are never cut short with "…"
    bool fits(const QString &name) const
    {
        return QFontMetricsF(nameFont()).horizontalAdvance(name.toUpper()) <= width() - 4;
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        QColor ink = palette().color(QPalette::WindowText);
        if (m_editor) {
            // where the name is typed, in place
            QColor field = ink;
            field.setAlphaF(0.1);
            painter.setPen(Qt::NoPen);
            painter.setBrush(field);
            painter.drawPath(KisModernStyle::squircle(QRectF(rect()).adjusted(1, 1, -1, -1), 5));
            return;
        }
        if (m_name.isEmpty()) {
            // a hairline, a little stronger when pointed at
            ink.setAlphaF(underMouse() ? 0.4 : 0.14);
            painter.setPen(QPen(ink, 1));
            painter.drawLine(QPointF(10, height() / 2.0), QPointF(width() - 10, height() / 2.0));
            return;
        }
        ink.setAlphaF(underMouse() ? 0.85 : 0.5);
        // a name from before names were kept short: a little tighter, then as many letters as fit (never "…")
        QFont font = nameFont();
        QString shown = m_name;
        if (!fits(shown)) {
            font.setLetterSpacing(QFont::PercentageSpacing, 96);
            font.setPointSizeF(font.pointSizeF() * 0.9);
            while (shown.size() > 1 && QFontMetricsF(font).horizontalAdvance(shown.toUpper()) > width() - 4) {
                shown.chop(1);
            }
        }
        painter.setFont(font);
        painter.setPen(ink);
        painter.drawText(rect(), Qt::AlignCenter, shown.trimmed());
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        m_pressPos = event->pos();
        m_dragged = false;
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (!m_dragged && isDrag(event, m_pressPos)) {
            m_dragged = true;
            if (onDrag) onDrag();
        }
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        // let go elsewhere (or a long press, see KisTouchMode): not a click
        if (m_dragged || event->button() != Qt::LeftButton || !rect().contains(event->pos()) || m_editor) {
            m_dragged = false;
            return;
        }
        startEditing();
    }

private:
    /// Named in place, as many letters as the rail shows: Return or a click away keeps it, Escape does not
    void startEditing()
    {
        setFixedHeight(20);
        m_editor = new QLineEdit(this);
        m_editor->setFrame(false);
        m_editor->setFont(nameFont());
        m_editor->setAlignment(Qt::AlignCenter);
        m_editor->setAttribute(Qt::WA_TranslucentBackground);
        m_editor->setAutoFillBackground(false);
        QPalette palette = m_editor->palette();
        palette.setColor(QPalette::Base, Qt::transparent);
        m_editor->setPalette(palette);
        m_editor->setTextMargins(0, 0, 0, 0);
        m_editor->setContentsMargins(0, 0, 0, 0);
        m_editor->setGeometry(rect());
        m_editor->setMaxLength(16);
        m_editor->setValidator(new FitValidator(this, m_editor));
        m_editor->setText(fits(m_name) ? m_name : QString());
        m_editor->installEventFilter(this);
        QObject::connect(m_editor, &QLineEdit::editingFinished, this, [this]() { finishEditing(true); });
        m_editor->show();
        m_editor->selectAll();
        m_editor->setFocus(Qt::MouseFocusReason);
        update();
    }

    void finishEditing(bool keep)
    {
        if (!m_editor) return;
        QLineEdit *editor = m_editor;
        m_editor = nullptr;
        const QString name = editor->text().trimmed();
        editor->removeEventFilter(this);
        editor->hide();
        editor->deleteLater();
        if (!keep || name == m_name) {
            setFixedHeight(m_name.isEmpty() ? 14 : 20);
            update();
            return;
        }
        // the rail is made again with it (later: this item goes with the old rail)
        if (onRename) onRename(name);
    }

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == m_editor && event->type() == QEvent::KeyPress
            && static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape) {
            finishEditing(false);
            return true;
        }
        return QWidget::eventFilter(watched, event);
    }

    /// Takes what still shows whole on the rail, nothing longer
    class FitValidator : public QValidator
    {
    public:
        FitValidator(const SpaceItem *item, QObject *parent) : QValidator(parent), m_item(item) {}
        State validate(QString &input, int &) const override
        {
            return m_item->fits(input) ? Acceptable : Invalid;
        }

    private:
        const SpaceItem *m_item;
    };

    QString m_name;
    QPointer<QLineEdit> m_editor;
    QPoint m_pressPos;
    bool m_dragged {false};
};

/**
 * The "+" under the panel: adds a tool, an effect, anything Find knows. It
 * floats below the panel rather than sitting in it, so that showing it never
 * moves what is in the panel (the color chip right under the pointer).
 */
class PlusButton : public QToolButton
{
public:
    std::function<void()> left;

    explicit PlusButton(QWidget *parent) : QToolButton(parent)
    {
        setFixedSize(30, 30);
        setAttribute(Qt::WA_Hover);
        setFocusPolicy(Qt::NoFocus);
        setCursor(Qt::PointingHandCursor);
        setToolTip(i18nc("@info:tooltip", "Add"));
    }

protected:
    void leaveEvent(QEvent *event) override
    {
        QToolButton::leaveEvent(event);
        if (left) left();
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        // a small round panel of its own, glass like the one above it
        const QRectF round = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        const QPainterPath shape = KisStudioGlass::paint(painter, this, round, round.width() / 2);
        QColor border = palette().color(QPalette::WindowText);
        border.setAlphaF(0.12);
        KisStudioGlass::paintBorder(painter, this, shape, border);
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
    QFrame *indicator {nullptr};
    QPointer<PlusButton> plus;             ///< a child of the canvas area, under the panel
    QTimer *plusHide {nullptr};            ///< lets the pointer cross from the panel to the "+"
    bool hovered {false};
    int hoverGap {-1};                     ///< the item index a click in the gap would insert a space at

    QHash<QString, ToolSlot> slots;        ///< the built-in slots this build has, by id
    QHash<QString, QString> currentTool;   ///< slot id -> the variant it uses now
    QSet<QString> hidden;                  ///< variants hidden from their slot's card

    QStringList items;                     ///< what is on the rail, in order
    QVector<QWidget *> widgets;            ///< what shows the items
    QVector<int> widgetItem;               ///< for every widget, its index in items
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
            if (!hidden.contains(tool) && tool != multiBrushToolId) visible << tool;
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

    /// An action for a shortcut that no existing action has; saved like the others
    QAction *ownAction(const QString &key, const QString &name, const QString &title, std::function<void()> run)
    {
        QPointer<QAction> &own = ownActions[key];
        if (!own) {
            own = new QAction(title, mainWindow);
            own->setObjectName(name);
            own->setShortcutContext(Qt::WindowShortcut);
            const QString saved = KConfigGroup(KSharedConfig::openConfig(QStringLiteral("kritashortcutsrc")), QStringLiteral("Shortcuts"))
                                      .readEntry(name, QString());
            if (!saved.isEmpty() && saved != QLatin1String("none")) {
                own->setShortcuts(QKeySequence::listFromString(saved));
            }
            mainWindow->addAction(own);
            QObject::connect(own, &QAction::triggered, mainWindow, std::move(run));
        }
        return own;
    }

    KisStudioToolRail *q {nullptr};

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
        return ownAction(slotPrefix + slotId, QStringLiteral("studio_slot_") + slotId, slots.value(slotId).title,
                         [this, slotId]() { q->activateSlot(slotId); });
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
        // brushes, blend modes and the like get an action of their own for their shortcut
        return ownAction(key, QStringLiteral("studio_item_") + key, item.title.isEmpty() ? key : item.title, [this, key]() {
            if (mainWindow->studioFind()) mainWindow->studioFind()->run(key);
        });
    }

    RailButton *buttonAt(int widget) const
    {
        return dynamic_cast<RailButton *>(widgets.value(widget));
    }
};

bool KisStudioToolRail::isOwnController(KisMainWindow *mainWindow, KoCanvasController *controller)
{
    KisView *active = mainWindow->activeView();
    if (!controller || !active) return true;
    return static_cast<KoCanvasController *>(active->canvasController()) == controller;
}

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
    setMouseTracking(true);

    d->layout = new QVBoxLayout(this);
    d->layout->setContentsMargins(4, 4, 4, 4);
    d->layout->setSpacing(4);
    d->itemsLayout = new QVBoxLayout();
    d->itemsLayout->setContentsMargins(0, 0, 0, 0);
    // the gaps are where a click adds a space
    d->itemsLayout->setSpacing(5);
    d->layout->addLayout(d->itemsLayout);

    d->plus = new PlusButton(this);
    d->plus->hide();
    d->plusHide = new QTimer(this);
    d->plusHide->setSingleShot(true);
    d->plusHide->setInterval(300);
    connect(d->plusHide, &QTimer::timeout, this, &KisStudioToolRail::updatePlus);
    connect(KisTouchMode::instance(), &KisTouchMode::changed, this, &KisStudioToolRail::updatePlus);
    d->plus->left = [this]() { d->plusHide->start(); };
    connect(d->plus, &QToolButton::clicked, this, [this]() {
        if (KisStudioFind *find = d->mainWindow->studioFind()) {
            find->pick(i18nc("@info:placeholder", "Add to the tools"), [this](const QString &key) {
                if (!d->items.contains(key)) {
                    setItems(d->items + QStringList{key});
                }
            });
        }
    });

    d->indicator = new QFrame(this);
    d->indicator->setAutoFillBackground(true);
    d->indicator->setBackgroundRole(QPalette::Highlight);
    d->indicator->hide();

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
        d->currentTool.insert(slot.id, available.tools.contains(remembered) && remembered != multiBrushToolId ? remembered : available.tools.first());
    }

    const QStringList hidden = cfg.readEntry<QStringList>(hiddenKey, QStringList());
    d->hidden = QSet<QString>(hidden.begin(), hidden.end());
    d->items = cfg.readEntry<QStringList>(itemsKey, defaultItems());
    // a rail made before the effects button existed gets it once; taken off, it stays off
    if (!cfg.readEntry<bool>(effectsOfferedKey, false)) {
        KisConfig writable(false);
        if (!d->items.contains(effectsItem)) {
            d->items << effectsItem;
            writable.writeEntry(itemsKey, d->items);
        }
        writable.writeEntry(effectsOfferedKey, true);
    }
    d->q = this;

    connect(KoToolManager::instance(), &KoToolManager::changedTool, this, [this](KoCanvasController *controller) {
        // other windows have their own rails
        if (isOwnController(d->mainWindow, controller)) updateActiveSlot();
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

KisStudioToolRail::~KisStudioToolRail()
{
    // not a child of the rail, see updatePlus()
    delete d->plus;
}

void KisStudioToolRail::addBottomWidget(QWidget *widget)
{
    widget->setParent(this);
    d->layout->addWidget(widget, 0, Qt::AlignHCenter);
}

void KisStudioToolRail::refresh()
{
    rebuild();
    updateActiveSlot();
}

void KisStudioToolRail::addItem(const QString &key)
{
    if (!d->items.contains(key)) {
        setItems(d->items + QStringList{key});
    }
}

bool KisStudioToolRail::hasItem(const QString &key) const
{
    return d->items.contains(key);
}

void KisStudioToolRail::removeItem(const QString &key)
{
    QStringList items = d->items;
    items.removeAll(key);
    setItems(items);
}

void KisStudioToolRail::setItems(const QStringList &items)
{
    // every space is its own item, the rest only once
    QStringList unique;
    Q_FOREACH (const QString &item, items) {
        if (item.startsWith(spacePrefix) || !unique.contains(item)) unique << item;
    }
    d->items = unique;
    KisConfig(false).writeEntry(itemsKey, d->items);
    rebuild();
    updateActiveSlot();
}

void KisStudioToolRail::updatePlus()
{
    if (!d->plus) {
        return;
    }
    // under the panel the rail is in, in the area the panel floats over
    QWidget *panel = parentWidget();
    QWidget *area = panel ? panel->parentWidget() : nullptr;
    if (area && d->plus->parentWidget() != area) {
        d->plus->setParent(area);
    }
    // there when it can be used: the panel or the "+" pointed at, or nothing on the rail yet
    // a finger cannot point at it: while one is used, it is always there
    const bool show = area && isVisible() && (d->widgets.isEmpty() || d->hovered || d->plus->underMouse() || KisTouchMode::isActive());
    if (show) {
        d->plus->move(panel->geometry().center().x() - d->plus->width() / 2, panel->geometry().bottom() + 6);
        d->plus->raise();
        KisStudioMotion::showFaded(d->plus);
    } else if (!isVisible()) {
        // the panel is gone: no lingering "+" without it
        d->plus->hide();
    } else {
        KisStudioMotion::hideFaded(d->plus);
    }
}

void KisStudioToolRail::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    updatePlus();
}

void KisStudioToolRail::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    updatePlus();
}

void KisStudioToolRail::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    updatePlus();
}

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
void KisStudioToolRail::enterEvent(QEnterEvent *event)
#else
void KisStudioToolRail::enterEvent(QEvent *event)
#endif
{
    d->hovered = true;
    d->plusHide->stop();
    updatePlus();
    QWidget::enterEvent(event);
}

void KisStudioToolRail::leaveEvent(QEvent *event)
{
    d->hovered = false;
    d->hoverGap = -1;
    d->indicator->hide();
    // later, so that the pointer can get to the "+"
    d->plusHide->start();
    QWidget::leaveEvent(event);
}

int KisStudioToolRail::gapAt(int y) const
{
    // between two widgets: the index of the item below the gap
    for (int w = 1; w < d->widgets.size(); w++) {
        const int top = d->widgets[w - 1]->geometry().bottom();
        const int bottom = d->widgets[w]->geometry().top();
        if (y > top - 1 && y < bottom + 1) {
            return d->widgetItem[w];
        }
    }
    return -1;
}

void KisStudioToolRail::showIndicator(int itemIndex, int thickness)
{
    int y = d->widgets.isEmpty() ? 6 : d->widgets.last()->geometry().bottom() + 2;
    for (int w = 0; w < d->widgets.size(); w++) {
        if (d->widgetItem[w] >= itemIndex) {
            y = d->widgets[w]->geometry().top() - 3;
            break;
        }
    }
    d->indicator->setGeometry(8, y - thickness / 2, width() - 16, thickness);
    d->indicator->show();
    d->indicator->raise();
}

void KisStudioToolRail::mouseMoveEvent(QMouseEvent *event)
{
    // a click between two tools puts a space there
    d->hoverGap = gapAt(event->pos().y());
    if (d->hoverGap >= 0) {
        showIndicator(d->hoverGap, 1);
        setCursor(Qt::PointingHandCursor);
        setToolTip(i18nc("@info:tooltip", "Add gap"));
    } else {
        d->indicator->hide();
        unsetCursor();
        setToolTip(QString());
    }
    QWidget::mouseMoveEvent(event);
}

void KisStudioToolRail::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && d->hoverGap >= 0) {
        QStringList items = d->items;
        items.insert(d->hoverGap, spacePrefix);
        d->hoverGap = -1;
        d->indicator->hide();
        setItems(items);
        return;
    }
    QWidget::mousePressEvent(event);
}

void KisStudioToolRail::startDrag(int index, QWidget *source, const QPixmap &pixmap)
{
    const QString dragged = d->items.value(index);
    auto *drag = new QDrag(source);
    auto *mime = new QMimeData();
    mime->setData(KisStudioShortcuts::itemMimeType(), dragged.toUtf8());
    // the position too, so that one of several spaces moves and not another
    mime->setData(QStringLiteral("application/x-krita-studio-rail-index"), QByteArray::number(index));
    drag->setMimeData(mime);
    if (!pixmap.isNull()) {
        drag->setPixmap(pixmap);
    }
    // dropped anywhere but on the rail: it is taken off the rail
    if (drag->exec(Qt::MoveAction) == Qt::IgnoreAction) {
        QStringList items = d->items;
        if (index < items.size() && items[index] == dragged) {
            items.removeAt(index);
        }
        QTimer::singleShot(0, this, [this, items]() { setItems(items); });
    }
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
    d->widgets.clear();
    d->widgetItem.clear();

    // shortcuts of items that left the rail go with them
    for (auto it = d->ownActions.begin(); it != d->ownActions.end();) {
        const bool used = d->items.contains(it.key());
        if (!used && it.value()) {
            it.value()->deleteLater();
        }
        it = used ? std::next(it) : d->ownActions.erase(it);
    }

    KisStudioFind *find = d->mainWindow->studioFind();
    for (int i = 0; i < d->items.size(); i++) {
        const QString key = d->items[i];
        const int index = i;

        if (key.startsWith(spacePrefix)) {
            auto *space = new SpaceItem(key.mid(spacePrefix.size()), this);
            space->onRename = [this, index](const QString &name) {
                QStringList items = d->items;
                if (index < items.size()) items[index] = spacePrefix + name;
                QTimer::singleShot(0, this, [this, items]() { setItems(items); });
            };
            space->onDrag = [this, index, space]() { startDrag(index, space, QPixmap()); };
            d->itemsLayout->addWidget(space, 0, Qt::AlignHCenter);
            d->widgets << space;
            d->widgetItem << i;
            continue;
        }

        QIcon icon;
        QString title;
        bool variants = false;
        if (key.startsWith(slotPrefix)) {
            const QString slotId = key.mid(slotPrefix.size());
            if (!d->slots.contains(slotId)) continue;
            icon = KisIconUtils::loadIcon(d->slots[slotId].icon);
            title = d->slots[slotId].title;
            variants = d->visibleVariants(slotId).size() > 1;
        } else {
            KisStudioFind::Item item;
            if (!find || !find->item(key, &item)) continue;
            icon = item.icon.isNull() ? KisIconUtils::loadIcon(QStringLiteral("studio_more")) : item.icon;
            title = item.title;
        }

        auto *button = new RailButton(this);
        button->setIcon(icon);
        button->setAccessibleName(title);
        button->hasVariants = variants;
        button->shortcutAction = d->shortcutActionFor(key);
        button->onCard = [this, index]() { showCard(index); };
        button->onDrag = [this, index, button, icon]() { startDrag(index, button, icon.pixmap(32, 32)); };
        connect(button, &QToolButton::clicked, this, [this, index]() { runItem(index); });
        if (variants) {
            // scrolling over a slot steps through its variants, like the blend modes
            const QString slotId = key.mid(slotPrefix.size());
            button->onWheel = [this, slotId](int steps) {
                const QStringList visible = d->visibleVariants(slotId);
                const int at = qMax(0, visible.indexOf(d->currentTool.value(slotId)));
                const int next = ((at + steps) % visible.size() + visible.size()) % visible.size();
                activateTool(slotId, visible[next]);
            };
        }
        d->itemsLayout->addWidget(button, 0, Qt::AlignHCenter);
        d->widgets << button;
        d->widgetItem << i;
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
    const int widgetIndex = d->widgetItem.indexOf(index);
    if (widgetIndex < 0) {
        return;
    }
    QWidget *anchor = d->widgets[widgetIndex];
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
                choice.hide = [this, toolId]() {
                    d->hidden.insert(toolId);
                    KisConfig(false).writeEntry(hiddenKey, QStringList(d->hidden.values()));
                    QTimer::singleShot(0, this, [this]() {
                        rebuild();
                        updateActiveSlot();
                    });
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
                              i18nc("@info:tooltip", "%1 more", hiddenHere.size()),
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

    // what clicking between two buttons does, for fingers that cannot hit the gap
    card->addCommand({KisIconUtils::loadIcon(QStringLiteral("list-add")),
                      i18nc("@info:tooltip", "Add gap above"),
                      [this, index]() {
                          QStringList items = d->items;
                          items.insert(qBound(0, index, items.size()), spacePrefix);
                          QTimer::singleShot(0, this, [this, items]() { setItems(items); });
                      }});
    card->addCommand({KisIconUtils::loadIcon(QStringLiteral("edit-delete")),
                      i18nc("@info:tooltip", "Remove"),
                      [this, index, key]() {
                          QStringList items = d->items;
                          if (index < items.size() && items[index] == key) items.removeAt(index);
                          QTimer::singleShot(0, this, [this, items]() { setItems(items); });
                      }});
    // the badges show the shortcut, which the card may have changed
    connect(card, &QObject::destroyed, this, [this]() {
        Q_FOREACH (QWidget *widget, d->widgets) widget->update();
    });
    card->popupBeside(anchor);
}

void KisStudioToolRail::showRestoreCard(const QPoint &globalPos)
{
    auto *card = new KisStudioItemCard(d->mainWindow, KisIconUtils::loadIcon(QStringLiteral("studio_more")),
                                       i18nc("@title tools rail", "Tools"), nullptr);
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
                      i18nc("@info:tooltip", "Reset tools"),
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
    for (int w = 0; w < d->widgets.size(); w++) {
        if (y < d->widgets[w]->geometry().center().y()) {
            return d->widgetItem[w];
        }
    }
    return d->items.size();
}

void KisStudioToolRail::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasFormat(KisStudioShortcuts::itemMimeType())) {
        event->acceptProposedAction();
    }
}

void KisStudioToolRail::dragMoveEvent(QDragMoveEvent *event)
{
    if (!event->mimeData()->hasFormat(KisStudioShortcuts::itemMimeType())) {
        return;
    }
    event->acceptProposedAction();
    showIndicator(dropIndex(event->pos().y()), 2);
}

void KisStudioToolRail::dragLeaveEvent(QDragLeaveEvent *)
{
    d->indicator->hide();
}

void KisStudioToolRail::dropEvent(QDropEvent *event)
{
    d->indicator->hide();
    const QString key = QString::fromUtf8(event->mimeData()->data(KisStudioShortcuts::itemMimeType()));
    if (key.isEmpty()) {
        return;
    }
    int to = dropIndex(event->pos().y());
    QStringList items = d->items;
    // moved along the rail: from where it was, or the same item elsewhere on the rail
    int from = -1;
    if (event->source() && event->mimeData()->hasFormat(QStringLiteral("application/x-krita-studio-rail-index"))) {
        from = event->mimeData()->data(QStringLiteral("application/x-krita-studio-rail-index")).toInt();
        if (items.value(from) != key) from = -1;
    }
    if (from < 0 && !key.startsWith(spacePrefix)) {
        from = items.indexOf(key);
    }
    if (from >= 0) {
        items.removeAt(from);
        if (to > from) to--;
    }
    items.insert(qBound(0, to, items.size()), key);
    event->setDropAction(from >= 0 ? Qt::MoveAction : Qt::CopyAction);
    event->accept();
    // the dragged widget is still on the stack of its drag
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

    for (int w = 0; w < d->widgets.size(); w++) {
        RailButton *button = d->buttonAt(w);
        if (!button) continue;
        const QString key = d->items.value(d->widgetItem[w]);
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
    KisStudioBrushPicker::recordUse(name);

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
