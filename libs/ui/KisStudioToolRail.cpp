/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioToolRail.h"
#include "KisStudioTools.h"

#include <KisSentenceCase.h>

#include "KisStudioBrushEditor.h"

#include <QAction>
#include <QApplication>
#include <QContextMenuEvent>
#include <QDateTime>
#include <QDrag>
#include <QDragEnterEvent>
#include <QFrame>
#include <QLineEdit>
#include <QKeyEvent>
#include <QValidator>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <KoCanvasController.h>
#include <KoToolFactoryBase.h>
#include <KoToolManager.h>
#include <KoToolRegistry.h>
#include <KisResourceModel.h>
#include <kactioncollection.h>
#include <kis_action_registry.h>
#include <klocalizedstring.h>

#include "KisMainWindow.h"
#include "KisModernStyle.h"
#include "KisResourceServerProvider.h"
#include "KisStudioBrushPicker.h"
#include "KisStudioFind.h"
#include "KisStudioGlass.h"
#include "KisStudioItemCard.h"
#include "KisStudioMotion.h"
#include "KisTouchMode.h"
#include "KisView.h"
#include "KisViewManager.h"
#include "kis_paintop_box.h"
#include "canvas/kis_canvas_controller.h"
#include "kis_canvas_resource_provider.h"
#include "KisSettingsRegistry.h"
#include "kis_config.h"
#include "kis_icon_utils.h"
#include "kis_paintop_preset.h"

namespace {

const QString brushToolId = QStringLiteral("KritaShape/KisToolBrush");
/// Not a variant of its own; the multibrush toggle in the top bar switches to it
const QString multiBrushToolId = QStringLiteral("KritaShape/KisToolMultiBrush");

bool isBrushTool(const QString &toolId)
{
    return toolId == brushToolId || toolId == multiBrushToolId;
}

const QString itemsKey = QStringLiteral("StudioRailItems");
const QString hiddenKey = QStringLiteral("StudioHiddenVariants");
const QString slotPrefix = QStringLiteral("slot:");
const QString toolPrefix = QStringLiteral("tool:");
const QString spacePrefix = QStringLiteral("space:");
const QString effectsItem = QStringLiteral("action:studio_effects");
/// A brush pinned to the rail, by Find's key for it
const QString brushPrefix = QStringLiteral("brush:");
/// The pinned brushes that take the size and opacity in use instead of keeping their own
const QString sharedSizeKey = QStringLiteral("StudioRailItemsSharedSize");
/// Set once the effects button has been added to an existing rail, so removing it sticks
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
    QStringList tools; ///< the variants; the first is the default
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

/// A smudging brush, for Blend's first use
QString blendingBrush()
{
    KisPaintOpPresetResourceServer *server = KisResourceServerProvider::instance()->paintOpPresetServer();
    KisResourceModel *model = server ? server->resourceModel() : nullptr;
    for (int row = 0; model && row < model->rowCount(); row++) {
        const QString candidate = model->index(row, 0).data(Qt::UserRole + KisAbstractResourceModel::Name).toString();
        if (candidate.contains(QStringLiteral("blend"), Qt::CaseInsensitive)
            || candidate.contains(QStringLiteral("smudge"), Qt::CaseInsensitive)
            || candidate.contains(QStringLiteral("smear"), Qt::CaseInsensitive)) {
            return candidate;
        }
    }
    return QString();
}

QStringList defaultItems()
{
    // blending is a brush pinned there, the Blend slot only when there is no such brush
    const QString blender = blendingBrush();
    return {slotPrefix + QStringLiteral("paint"),
            blender.isEmpty() ? slotPrefix + QStringLiteral("blend") : brushPrefix + blender,
            slotPrefix + QStringLiteral("erase"),
            spacePrefix,
            slotPrefix + QStringLiteral("select"), slotPrefix + QStringLiteral("move"), slotPrefix + QStringLiteral("fill"),
            slotPrefix + QStringLiteral("shape"), slotPrefix + QStringLiteral("text"), slotPrefix + QStringLiteral("guides"),
            effectsItem};
}

/// A widget on the rail that can be dragged along it or off it
class RailItem
{
public:
    virtual ~RailItem() = default;
    std::function<void()> onDrag;

protected:
    /// Pens wobble, so only a clear move counts as a drag
    static bool isDrag(QMouseEvent *event, const QPoint &pressPos)
    {
        return (event->buttons() & Qt::LeftButton)
            && (event->pos() - pressPos).manhattanLength() > 2 * QApplication::startDragDistance();
    }
};

/// A click runs it; a long press or right click shows its card
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

    std::function<void(bool touch)> onCard;
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

    // a right click, or a long press via KisTouchMode
    void contextMenuEvent(QContextMenuEvent *event) override
    {
        event->accept();
        const bool touch = QDateTime::currentMSecsSinceEpoch() - qApp->property("longPressAt").toLongLong() < 1000;
        if (onCard) onCard(touch);
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
            m_dragged = false;
            setDown(false);
            event->accept();
            return;
        }
        QToolButton::mouseReleaseEvent(event);
    }

    void wheelEvent(QWheelEvent *event) override
    {
        if (onWheel) {
            if (const int steps = KisModernStyle::wheelSteps(event, this)) onWheel(-steps);
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
            KisStudioShortcuts::paintBadge(painter, rect(), KisStudioShortcuts::cornerBadgeText(shortcutAction->shortcut()), palette());
        }
    }

private:
    QPoint m_pressPos;
    bool m_dragged {false};
};

/// A gap between groups of tools; clicking it names the group below
class SpaceItem : public QWidget, public RailItem
{
public:
    SpaceItem(const QString &name, QWidget *parent)
        : QWidget(parent), m_name(name)
    {
        setFixedSize(40, name.isEmpty() ? 14 : 20);
        setAttribute(Qt::WA_Hover);
        setCursor(Qt::PointingHandCursor);
        setToolTip(name.isEmpty() ? i18nc("@info:tooltip a gap between groups of tools", "Gap\nClick: name\nDrag off: remove")
                                  : name);
    }

    std::function<void(const QString &name)> onRename;

    QFont nameFont() const
    {
        QFont font = this->font();
        font.setPointSizeF(font.pointSizeF() * 0.66);
        font.setWeight(QFont::DemiBold);
        font.setCapitalization(QFont::AllUppercase);
        font.setLetterSpacing(QFont::PercentageSpacing, 104);
        return font;
    }

    /// Whether \p name fits in full; names are never elided
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
            QColor field = ink;
            field.setAlphaF(0.1);
            painter.setPen(Qt::NoPen);
            painter.setBrush(field);
            painter.drawPath(KisModernStyle::squircle(QRectF(rect()).adjusted(1, 1, -1, -1), 5));
            return;
        }
        if (m_name.isEmpty()) {
            ink.setAlphaF(underMouse() ? 0.4 : 0.14);
            painter.setPen(QPen(ink, 1));
            painter.drawLine(QPointF(10, height() / 2.0), QPointF(width() - 10, height() / 2.0));
            return;
        }
        ink.setAlphaF(underMouse() ? 0.85 : 0.5);
        // a stored name too long to fit: tighten the font, then show as many letters as fit
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
        // released elsewhere, or after a long press (see KisTouchMode): not a click
        if (m_dragged || event->button() != Qt::LeftButton || !rect().contains(event->pos()) || m_editor) {
            m_dragged = false;
            return;
        }
        startEditing();
    }

private:
    /// Edits the name in place: Return or clicking away keeps it, Escape cancels
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
        // this rebuilds the rail, deleting this item too
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

    /// Accepts only names that fit on the rail
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
 * The "+" below the panel, which adds anything Find knows. It floats outside
 * the panel so that showing it never shifts the panel's contents.
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

/// A brush's picture as a rounded tile
QIcon pinnedIcon(const QIcon &picture, int side)
{
    const qreal ratio = qApp->devicePixelRatio();
    QPixmap tile(QSize(side, side) * ratio);
    tile.setDevicePixelRatio(ratio);
    tile.fill(Qt::transparent);
    QPainter painter(&tile);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.setClipPath(KisModernStyle::squircle(QRectF(0, 0, side, side), KisModernStyle::cornerRadius + 1));
    picture.paint(&painter, QRect(0, 0, side, side));
    return QIcon(tile);
}

} // namespace

struct KisStudioToolRail::Private
{
    KisMainWindow *mainWindow {nullptr};
    QVBoxLayout *layout {nullptr};
    QVBoxLayout *itemsLayout {nullptr};
    bool missing {false};
    QFrame *indicator {nullptr};
    QPointer<PlusButton> plus;             ///< a child of the canvas area, below the panel
    QTimer *plusHide {nullptr};            ///< delays hiding so the pointer can cross from the panel to the "+"
    bool hovered {false};
    int hoverGap {-1};                     ///< where a click in the hovered gap inserts a gap, or -1

    QHash<QString, ToolSlot> slots;        ///< the built-in slots available in this build, by id
    QHash<QString, QString> currentTool;   ///< slot id -> its current variant
    QSet<QString> hidden;                  ///< variants hidden from their slot's card
    QSet<QString> sharedSize;              ///< pinned brushes that take the size in use

    QStringList items;                     ///< the rail's items, in order
    QVector<QWidget *> widgets;            ///< the widgets showing the items
    QVector<int> widgetItem;               ///< for every widget, its index in items
    QHash<QString, QPointer<QAction>> ownActions; ///< hold the shortcuts of items that have no action

    QString activeSlot;
    QString brushSlot;                     ///< Paint, Blend or a pinned brush, whichever the current brush belongs to
    bool loadingPreset {false};
    KisStudioToolRail *q {nullptr};

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

    /// An action that holds an item's shortcut, saved like any other
    QAction *ownAction(const QString &key, const QString &name, const QString &title, std::function<void()> run,
                       const QKeySequence &defaultKey = QKeySequence())
    {
        QPointer<QAction> &own = ownActions[key];
        if (!own) {
            own = new QAction(title, mainWindow);
            own->setObjectName(name);
            own->setShortcutContext(Qt::WindowShortcut);
            own->setShortcut(defaultKey);
            own->setProperty("defaultShortcuts", QVariant::fromValue(defaultKey.isEmpty() ? QList<QKeySequence>() : QList<QKeySequence>{defaultKey}));
            KisStudioShortcuts::restore(own);
            // a default key someone already gave a command of their own stays theirs: two on one key and neither works
            if (!defaultKey.isEmpty() && own->shortcut() == defaultKey
                && !KisStudioShortcuts::savedShortcutsHave(own->objectName())) {
                Q_FOREACH (QAction *other, KisStudioShortcuts::allActions(mainWindow)) {
                    if (other != own && other->shortcuts().contains(defaultKey) && KisStudioShortcuts::toolsOf(other).isEmpty()) {
                        own->setShortcut(QKeySequence());
                        break;
                    }
                }
            }
            mainWindow->addAction(own);
            QObject::connect(own, &QAction::triggered, mainWindow, std::move(run));
        }
        return own;
    }

    /**
     * The action behind a slot's shortcut: the eraser toggle (E) for Erase, the brush tool (B) for Paint,
     * and for the rest an action of the slot's own, which goes back to the tool it was left on.
     */
    QAction *slotAction(const QString &slotId)
    {
        const BrushMode mode = slots.value(slotId).brush;
        if (mode == BrushMode::Erase) {
            return eraseAction();
        }
        if (mode == BrushMode::Paint) {
            return toolAction(currentTool.value(slotId));
        }
        const QKeySequence key = slotId == QLatin1String("move") ? QKeySequence(Qt::Key_T)
                               : slotId == QLatin1String("fill") ? QKeySequence(Qt::Key_F) : QKeySequence();
        return ownAction(slotPrefix + slotId, QStringLiteral("studio_slot_") + slotId, slots.value(slotId).title,
                         [this, slotId]() { q->activateSlot(slotId); }, key);
    }

    /// The action whose shortcut a button shows and the card edits
    QAction *shortcutActionFor(const QString &key)
    {
        if (key.startsWith(slotPrefix)) {
            return slotAction(key.mid(slotPrefix.size()));
        }
        KisStudioFind::Item item;
        if (key.startsWith(brushPrefix)) {
            if (mainWindow->studioFind()) mainWindow->studioFind()->item(key, &item);
            return ownAction(key, QStringLiteral("studio_item_") + key, item.title.isEmpty() ? key : item.title,
                             [this, key]() { q->activatePin(key); });
        }
        if (mainWindow->studioFind() && mainWindow->studioFind()->item(key, &item) && item.action) {
            return item.action;
        }
        // items with no action of their own: brushes, blend modes and the like
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
    // the action's text is a short name, the tooltip may be a sentence
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
    return KisSentenceCase::of(name);
}

KisStudioToolRail::KisStudioToolRail(KisMainWindow *mainWindow, QWidget *parent)
    : QWidget(parent)
    , d(new Private)
{
    d->mainWindow = mainWindow;
    setAcceptDrops(true);
    setMouseTracking(true);

    d->layout = new QVBoxLayout(this);
    d->layout->setContentsMargins(KisModernStyle::space1, KisModernStyle::space1, KisModernStyle::space1, KisModernStyle::space1);
    d->layout->setSpacing(KisModernStyle::space1);
    d->itemsLayout = new QVBoxLayout();
    d->itemsLayout->setContentsMargins(0, 0, 0, 0);
    // leave room to click between two items, which adds a gap
    d->itemsLayout->setSpacing(KisModernStyle::space1);
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
            find->pick(i18nc("@info:placeholder", "Add to the tools"), [this](const QString &key) { addItem(key); });
        }
    });

    d->indicator = new QFrame(this);
    d->indicator->setAutoFillBackground(true);
    d->indicator->setBackgroundRole(QPalette::Highlight);
    d->indicator->hide();

    KisConfig cfg(true);
    Q_FOREACH (const ToolSlot &slot, toolSlots()) {
        // keep only the variants available in this build
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
    const QStringList shared = cfg.readEntry<QStringList>(sharedSizeKey, QStringList());
    d->sharedSize = QSet<QString>(shared.begin(), shared.end());
    if (!KSharedConfig::openConfig()->group(QString()).hasKey(itemsKey)) {
        // never arranged: a rail that has used its Blend slot keeps it (and the brush it chose); a new one pins a
        // blending brush. Written down, so the brush found first doesn't change from one start to the next
        QStringList items = defaultItems();
        if (!cfg.readEntry<QString>(QStringLiteral("StudioBrushPreset_blend"), QString()).isEmpty()) {
            for (QString &item : items) {
                if (item.startsWith(brushPrefix)) item = slotPrefix + QStringLiteral("blend");
            }
        }
        KisSettingsRegistry::writeLater(itemsKey, items);
    }
    d->items = cfg.readEntry<QStringList>(itemsKey, defaultItems());
    // add the effects button only once, so removing it sticks
    if (!cfg.readEntry<bool>(effectsOfferedKey, false)) {
        KisConfig writable(false);
        if (!d->items.contains(effectsItem)) {
            d->items << effectsItem;
            writable.writeEntry(itemsKey, d->items);
        }
        writable.writeEntry(effectsOfferedKey, true);
    }
    d->q = this;
    // every slot's key works, on the rail or not (T for Transform, F for Fill: their tools lost their own)
    for (auto it = d->slots.constBegin(); it != d->slots.constEnd(); ++it) d->slotAction(it.key());

    // an undo or redo of the rail's items
    connect(KisSettingsRegistry::instance(), &KisSettingsRegistry::valueChanged, this, [this](const QString &id) {
        if (id != itemsKey) return;
        const QStringList items = KisConfig(true).readEntry<QStringList>(itemsKey, defaultItems());
        if (items == d->items) return;
        d->items = items;
        rebuild();
        updateActiveSlot();
    });

    if (KoToolManager *tools = KisStudioTools::manager(d->mainWindow)) {
        connect(tools, &KoToolManager::changedTool, this, [this](KoCanvasController *controller) {
            if (isOwnController(d->mainWindow, controller)) updateActiveSlot();
        });
    }
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

bool KisStudioToolRail::hasMissingItems() const
{
    return d->missing;
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
    // gaps may repeat; every other item appears only once
    QStringList unique;
    Q_FOREACH (const QString &item, items) {
        if (item.startsWith(spacePrefix) || !unique.contains(item)) unique << item;
    }
    d->items = unique;
    KisSettingsRegistry::writeItems(itemsKey, d->items);
    rebuild();
    updateActiveSlot();
}

void KisStudioToolRail::updatePlus()
{
    if (!d->plus) {
        return;
    }
    // place it below the panel, in the area the panel floats over
    QWidget *panel = parentWidget();
    QWidget *area = panel ? panel->parentWidget() : nullptr;
    if (area && d->plus->parentWidget() != area) {
        d->plus->setParent(area);
    }
    // show it while the panel or the "+" is hovered, and always when the rail is empty or in touch mode (no hover)
    const bool show = area && isVisible() && (d->widgets.isEmpty() || d->hovered || d->plus->underMouse() || KisTouchMode::isActive());
    if (show) {
        d->plus->move(panel->geometry().center().x() - d->plus->width() / 2, panel->geometry().bottom() + 6);
        d->plus->raise();
        KisStudioMotion::showFaded(d->plus);
    } else if (!isVisible()) {
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
    if (parentWidget()) parentWidget()->installEventFilter(this);
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
    clearGap();
    // delayed, so the pointer can reach the "+"
    d->plusHide->start();
    QWidget::leaveEvent(event);
}

void KisStudioToolRail::clearGap()
{
    d->hoverGap = -1;
    d->indicator->hide();
    unsetCursor();
    setToolTip(QString());
}

bool KisStudioToolRail::eventFilter(QObject *watched, QEvent *event)
{
    // moving from a gap onto an item sends the rail no move or leave event
    if (event->type() == QEvent::Enter && d->hoverGap >= 0) {
        clearGap();
    }
    // the panel holding the rail was moved or resized: the "+" below it follows
    if (watched == parentWidget() && (event->type() == QEvent::Move || event->type() == QEvent::Resize)) {
        updatePlus();
    }
    return QWidget::eventFilter(watched, event);
}

int KisStudioToolRail::gapAt(int y) const
{
    // returns the index of the item below the gap
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
    // a click between two items inserts a gap there
    const int gap = gapAt(event->pos().y());
    if (gap >= 0) {
        d->hoverGap = gap;
        showIndicator(gap, 1);
        setCursor(Qt::PointingHandCursor);
        setToolTip(i18nc("@info:tooltip", "Add gap"));
    } else {
        clearGap();
    }
    QWidget::mouseMoveEvent(event);
}

void KisStudioToolRail::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && d->hoverGap >= 0) {
        QStringList items = d->items;
        items.insert(d->hoverGap, spacePrefix);
        clearGap();
        setItems(items);
        return;
    }
    QWidget::mousePressEvent(event);
}

void KisStudioToolRail::startDrag(int index, const QPixmap &pixmap)
{
    const QString dragged = d->items.value(index);
    // owned by the application: the rail can be rebuilt while it runs, which deletes its buttons
    auto *drag = new QDrag(qApp);
    auto *mime = new QMimeData();
    mime->setData(KisStudioShortcuts::itemMimeType(), dragged.toUtf8());
    // the index too, to tell several gaps apart
    mime->setData(QStringLiteral("application/x-krita-studio-rail-index"), QByteArray::number(index));
    drag->setMimeData(mime);
    if (!pixmap.isNull()) {
        drag->setPixmap(pixmap);
    }
    // dropping it off the rail removes it, unless the drag was cancelled with Esc
    const Qt::DropAction done = drag->exec(Qt::MoveAction);
    drag->deleteLater();
    if (done == Qt::IgnoreAction && !KisModernStyle::pointerHeld()) {
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

    // drop the shortcut actions of items no longer on the rail; a slot's stays, its key working off the rail too
    for (auto it = d->ownActions.begin(); it != d->ownActions.end();) {
        const bool used = d->items.contains(it.key()) || it.key().startsWith(slotPrefix);
        if (!used && it.value()) {
            it.value()->deleteLater();
        }
        it = used ? std::next(it) : d->ownActions.erase(it);
    }

    KisStudioFind *find = d->mainWindow->studioFind();
    d->missing = false;
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
            space->onDrag = [this, index]() { startDrag(index, QPixmap()); };
            space->installEventFilter(this);
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
            if (!d->slots.contains(slotId)) {
                d->missing = true;
                continue;
            }
            icon = KisIconUtils::loadIcon(d->slots[slotId].icon);
            title = d->slots[slotId].title;
            variants = d->visibleVariants(slotId).size() > 1;
        } else {
            KisStudioFind::Item item;
            if (!find || !find->item(key, &item)) {
                d->missing = true;
                continue;
            }
            icon = item.icon.isNull() ? KisIconUtils::loadIcon(QStringLiteral("studio_more")) : item.icon;
            title = item.title;
        }
        const bool pinned = key.startsWith(brushPrefix);
        if (pinned) {
            icon = pinnedIcon(icon, 28);
        }

        auto *button = new RailButton(this);
        if (pinned) {
            button->setIconSize(QSize(28, 28));
        }
        button->setIcon(icon);
        button->setAccessibleName(title);
        button->hasVariants = variants;
        button->shortcutAction = d->shortcutActionFor(key);
        button->onCard = [this, index](bool touch) { showCard(index, touch); };
        button->onDrag = [this, index, icon]() { startDrag(index, icon.pixmap(32, 32)); };
        connect(button, &QToolButton::clicked, this, [this, index]() { runItem(index); });
        if (variants) {
            // scrolling steps through the variants
            const QString slotId = key.mid(slotPrefix.size());
            button->onWheel = [this, slotId](int steps) {
                const QStringList visible = d->visibleVariants(slotId);
                const int at = qMax(0, visible.indexOf(d->currentTool.value(slotId)));
                const int next = ((at + steps) % visible.size() + visible.size()) % visible.size();
                activateTool(slotId, visible[next]);
            };
        }
        button->installEventFilter(this);
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
        const BrushMode brush = d->slots.value(slotId).brush;
        if (slotId == d->activeSlot && brush != BrushMode::None) {
            // clicking the active brush slot again opens the brush's settings
            const int widgetIndex = d->widgetItem.indexOf(index);
            KisStudioBrushEditor::toggle(d->mainWindow, widgetIndex >= 0 ? d->widgets[widgetIndex] : nullptr, Qt::RightEdge);
            updateActiveSlot();
        } else if (slotId == d->activeSlot && d->visibleVariants(slotId).size() > 1) {
            // clicking another active slot again shows its variants
            showCard(index);
            updateActiveSlot();
        } else {
            activateSlot(slotId);
        }
        return;
    }
    if (key.startsWith(brushPrefix)) {
        if (key == d->activeSlot) {
            const int widgetIndex = d->widgetItem.indexOf(index);
            KisStudioBrushEditor::toggle(d->mainWindow, widgetIndex >= 0 ? d->widgets[widgetIndex] : nullptr, Qt::RightEdge);
            updateActiveSlot();
        } else {
            activatePin(key);
        }
        return;
    }
    if (key.startsWith(toolPrefix)) {
        KisStudioTools::switchTo(key.mid(toolPrefix.size()));
    } else if (KisStudioFind *find = d->mainWindow->studioFind()) {
        find->run(key);
    }
    updateActiveSlot();
}

void KisStudioToolRail::showCard(int index, bool touch)
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
                    KisSettingsRegistry::writeLater(hiddenKey, QStringList(d->hidden.values()));
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
                                  KisSettingsRegistry::writeLater(hiddenKey, QStringList(d->hidden.values()));
                                  rebuild();
                                  updateActiveSlot();
                              }});
        }
    } else {
        KisStudioFind::Item item;
        if (d->mainWindow->studioFind()) d->mainWindow->studioFind()->item(key, &item);
        card = new KisStudioItemCard(d->mainWindow, key.startsWith(brushPrefix) ? pinnedIcon(item.icon, 28) : item.icon, item.title,
                                     d->shortcutActionFor(key));
        if (key.startsWith(brushPrefix)) {
            KisStudioItemCard::Command own;
            own.toolTip = i18nc("@option:check a brush pinned to the tools keeps its own size and opacity", "Own size");
            own.checkable = true;
            own.checked = !d->sharedSize.contains(key);
            own.run = [this, key]() {
                if (!d->sharedSize.remove(key)) d->sharedSize.insert(key);
                KisSettingsRegistry::writeLater(sharedSizeKey, QStringList(d->sharedSize.values()));
            };
            card->addCommand(own);
        }
    }

    // same as clicking between two buttons, for touch where the gap is hard to hit
    if (touch) {
        card->addCommand({KisIconUtils::loadIcon(QStringLiteral("list-add")),
                          i18nc("@info:tooltip", "Add gap above"),
                          [this, index]() {
                              QStringList items = d->items;
                              items.insert(qBound(0, index, items.size()), spacePrefix);
                              QTimer::singleShot(0, this, [this, items]() { setItems(items); });
                          }});
    }
    card->addCommand({KisIconUtils::loadIcon(QStringLiteral("edit-delete")),
                      i18nc("@info:tooltip", "Remove"),
                      [this, index, key]() {
                          QStringList items = d->items;
                          if (index < items.size() && items[index] == key) items.removeAt(index);
                          QTimer::singleShot(0, this, [this, items]() { setItems(items); });
                      }});
    // repaint the shortcut badges, which the card may have changed
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
                          KisSettingsRegistry::writeLater(hiddenKey, QStringList());
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
    // a move along the rail, or the same item dropped from elsewhere
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
    // deferred: the dragged widget is still in use by its drag
    QTimer::singleShot(0, this, [this, items]() { setItems(items); });
}

void KisStudioToolRail::activateSlot(const QString &slotId)
{
    activateTool(slotId, d->currentTool.value(slotId));
}

void KisStudioToolRail::activateTool(const QString &slotId, const QString &toolId)
{
    if (toolId.isEmpty()) {
        return;
    }
    const ToolSlot s = d->slots.value(slotId);
    d->currentTool[slotId] = toolId;
    KisSettingsRegistry::writeLater(QStringLiteral("StudioToolSlot_") + slotId, toolId);

    QAction *erase = d->eraseAction();
    if (s.brush == BrushMode::Erase) {
        KisStudioTools::switchTo(toolId);
        if (erase && !erase->isChecked()) {
            erase->setChecked(true);
        }
    } else if (s.brush == BrushMode::Paint || s.brush == BrushMode::Blend) {
        const bool switchingBrush = d->brushSlot != slotId;
        KisCanvasResourceProvider *resources = d->resources();
        if (switchingBrush && resources && resources->currentPreset() && d->slots.contains(d->brushSlot)) {
            // the slot being left remembers its brush
            KisSettingsRegistry::writeLater(QStringLiteral("StudioBrushPreset_") + d->brushSlot, resources->currentPreset()->name());
        }
        d->brushSlot = slotId;
        if (erase && erase->isChecked()) {
            erase->setChecked(false);
        }
        KisStudioTools::switchTo(toolId);
        if (switchingBrush) {
            loadPresetForSlot(slotId);
        }
    } else {
        KisStudioTools::switchTo(toolId);
    }
    updateActiveSlot();
}

void KisStudioToolRail::updateActiveSlot()
{
    const QString toolId = KisStudioTools::activeToolId();
    QAction *erase = d->eraseAction();
    const bool erasing = erase && erase->isChecked();

    // a pinned brush taken off the rail leaves its brush to Paint
    if (d->brushSlot.startsWith(brushPrefix) && !d->items.contains(d->brushSlot)) {
        d->brushSlot = d->slotWithMode(BrushMode::Paint);
    }
    QString active;
    if (isBrushTool(toolId)) {
        active = erasing ? d->slotWithMode(BrushMode::Erase) : d->brushSlot;
    } else {
        // prefer a non-brush slot on the rail that has the tool, then fall back to Paint
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
    // the multibrush is a toggle on the brush, not a variant to return to
    if (!active.isEmpty() && toolId != multiBrushToolId && d->slots.value(active).tools.contains(toolId)) {
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
                // "Guides · Guides" says nothing more
                KoToolFactoryBase *factory = KoToolRegistry::instance()->value(d->currentTool.value(id));
                if (factory && toolName(factory).compare(tip, Qt::CaseInsensitive) != 0) {
                    tip = QStringLiteral("%1 · %2").arg(tip, toolName(factory));
                }
            }
            button->setToolTip(tip);
        } else if (key.startsWith(brushPrefix)) {
            button->setChecked(key == active);
            button->setToolTip(button->accessibleName());
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

    // a pinned brush lights its own button, and Paint keeps the brush it had
    if (d->items.contains(brushPrefix + name)) {
        if (d->brushSlot != brushPrefix + name) {
            d->brushSlot = brushPrefix + name;
            updateActiveSlot();
        }
        return;
    }
    if (d->brushSlot.startsWith(brushPrefix)) {
        d->brushSlot = d->slotWithMode(BrushMode::Paint);
        updateActiveSlot();
    }

    // a brush remembered by a slot selects that slot, so the highlight matches the brush (e.g. after a restart)
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
    if (!isBrushTool(KisStudioTools::activeToolId()) || (erase && erase->isChecked()) || d->brushSlot.isEmpty()) {
        return;
    }
    // a brush picked in Paint or Blend becomes that slot's brush
    KisSettingsRegistry::writeLater(QStringLiteral("StudioBrushPreset_") + d->brushSlot, name);
}

void KisStudioToolRail::activatePin(const QString &key)
{
    KisCanvasResourceProvider *resources = d->resources();
    KisPaintOpPresetResourceServer *server = KisResourceServerProvider::instance()->paintOpPresetServer();
    const QString name = key.mid(brushPrefix.size());
    KisPaintOpPresetSP preset = server ? server->resource(QString(), QString(), name) : KisPaintOpPresetSP();
    if (!resources || !preset) {
        return;
    }
    d->brushSlot = key;
    QAction *erase = d->eraseAction();
    if (erase && erase->isChecked()) {
        erase->setChecked(false);
    }
    KisStudioTools::switchTo(brushToolId);
    if (!resources->currentPreset() || resources->currentPreset()->name() != name) {
        const qreal size = resources->size();
        const qreal opacity = resources->opacity();
        d->loadingPreset = true;
        if (KisPaintopBox *box = d->mainWindow->viewManager() ? d->mainWindow->viewManager()->paintOpBox() : nullptr) {
            box->resourceSelected(preset);
        } else {
            resources->setPaintOpPreset(preset);
        }
        d->loadingPreset = false;
        if (d->sharedSize.contains(key)) {
            resources->setSize(size);
            resources->setOpacity(opacity);
        }
    }
    updateActiveSlot();
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
        name = blendingBrush();
    }
    if (name.isEmpty() || (resources->currentPreset() && resources->currentPreset()->name() == name)) {
        return;
    }

    KisPaintOpPresetSP preset = server->resource(QString(), QString(), name);
    if (preset) {
        d->loadingPreset = true;
        // through the paintop box, which switches the editors to the brush's engine
        if (KisPaintopBox *box = d->mainWindow->viewManager() ? d->mainWindow->viewManager()->paintOpBox() : nullptr) {
            box->resourceSelected(preset);
        } else {
            resources->setPaintOpPreset(preset);
        }
        d->loadingPreset = false;
        KisSettingsRegistry::writeLater(QStringLiteral("StudioBrushPreset_") + slotId, name);
    }
}
