/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioLayout.h"

#include <QAbstractButton>
#include <KoCanvasController.h>
#include <KoToolFactoryBase.h>
#include <KoToolManager.h>
#include <KoToolRegistry.h>
#include <QAction>
#include <QFileInfo>
#include <QApplication>
#include <QDockWidget>
#include <QFrame>
#include <QHBoxLayout>
#include <QVBoxLayout>

#include <functional>
#include <QHash>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMdiArea>
#include <QMdiSubWindow>
#include <QStatusBar>
#include <QMenuBar>
#include <QTabBar>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QVariantAnimation>
#include <QPainter>
#include <QPointer>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QStyleOptionToolButton>
#include <QStylePainter>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QWidgetAction>

#include <kactioncollection.h>
#include <kconfiggroup.h>
#include <KSharedConfig>
#include <klocalizedstring.h>

#include "KisDocument.h"
#include "KisMainWindow.h"
#include "KisStudioMotion.h"
#include "KisStudioBrushMemory.h"
#include "KisStudioCanvasRing.h"
#include "KisStudioBrushPicker.h"
#include "KisStudioEffectsPicker.h"
#include "KisView.h"
#include "KisResourceServerProvider.h"
#include "KisStudioCanvasHud.h"
#include "KisStudioColorPanel.h"
#include "KisStudioEffectOverlay.h"
#include "KisStudioFind.h"
#include "kis_filter_manager.h"
#include <filter/kis_filter.h>
#include <filter/kis_filter_configuration.h>
#include "KisStudioItemCard.h"
#include "KisStudioScrubber.h"
#include "KisStudioToolStrip.h"
#include "KisStudioWindowFrame.h"
#include "kis_canvas_resource_provider.h"
#include "canvas/kis_canvas_controller.h"
#include "KisStudioToolRail.h"
#include "KisViewManager.h"
#include "kis_paintop_box.h"
#include "kis_slider_spin_box.h"
#include "kis_workspace_resource.h"
#include "KisUiFont.h"
#include "KisModernStyle.h"
#include "KisTouchMode.h"
#include "kis_config.h"
#include "kis_icon_utils.h"

namespace {

const QString toolBoxId = QStringLiteral("ToolBox");

const QString activeKey = QStringLiteral("StudioLayoutActive");
const QString panelsKey = QStringLiteral("StudioLayoutPanels");
const QString pinnedKey = QStringLiteral("StudioLayoutPinnedPanels");
/// Pinned panels on the left of the canvas instead of the right
const QString leftKey = QStringLiteral("StudioLayoutLeftPanels");
/// How close to the left window edge a dropped panel button pins its panel there
const int leftDropZone = 140;
/// The heights of pinned panels, as "id=height", so that a short panel stays short
const QString heightsKey = QStringLiteral("StudioLayoutPanelHeights");
const QString foldedKey = QStringLiteral("StudioLayoutFoldedPanels");
const QString classicStateKey = QStringLiteral("StudioLayoutClassicState");

/// The panels that get a button on the rail; "+" on the rail adds any other
QStringList defaultRailPanels()
{
    return {
        QStringLiteral("KisLayerBox"),
        QStringLiteral("PresetDocker"),
        QStringLiteral("StudioColor"),
        // not the tool options: the tool chip in the top bar opens them where the tool is
        QStringLiteral("PaletteDocker"),
        QStringLiteral("History"),
    };
}

QPoint globalMousePos(const QMouseEvent *event)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return event->globalPosition().toPoint();
#else
    return event->globalPos();
#endif
}

QString railIconName(const QString &id)
{
    static const QHash<QString, QString> icons = {
        {QStringLiteral("KisLayerBox"), QStringLiteral("studio_layers")},
        {QStringLiteral("PresetDocker"), QStringLiteral("studio_brushes")},
        {QStringLiteral("StudioColor"), QStringLiteral("studio_color")},
        {QStringLiteral("ColorSelectorNg"), QStringLiteral("studio_color")},
        {QStringLiteral("WideGamutColorSelector"), QStringLiteral("studio_color")},
        {QStringLiteral("sharedtooldocker"), QStringLiteral("studio_tool_options")},
        {QStringLiteral("PaletteDocker"), QStringLiteral("studio_palette")},
        {QStringLiteral("History"), QStringLiteral("studio_history")},
    };
    return icons.value(id);
}

/**
 * The top edge of a panel, where its title was: drag it to move the panel,
 * between others, to the other side of the canvas, or onto the canvas to
 * unpin it; right click it for the panel's card. A faint line says it can
 * be grabbed, stronger under the pointer.
 */
class PanelGrip : public QWidget
{
public:
    std::function<void(const QPoint &globalPos)> dragged;
    std::function<void(const QPoint &globalPos)> dropped;
    std::function<void()> cardRequested;
    std::function<void()> clicked;

    explicit PanelGrip(QWidget *parent) : QWidget(parent)
    {
        setAttribute(Qt::WA_Hover);
        setCursor(Qt::OpenHandCursor);
        setToolTip(i18nc("@info:tooltip", "Drag to move, click to fold"));
        QObject::connect(KisTouchMode::instance(), &KisTouchMode::changed, this, [this]() { fit(); });
        fit();
    }

    /// Folded: the panel shows only this, with its name
    void setFolded(bool folded, const QString &title)
    {
        m_folded = folded;
        m_title = title;
        setToolTip(folded ? i18nc("@info:tooltip", "Drag to move, click to unfold") : i18nc("@info:tooltip", "Drag to move, click to fold"));
        fit();
        update();
    }
    bool isFolded() const { return m_folded; }

    QSize sizeHint() const override { return QSize(40, height()); }

protected:
    void fit()
    {
        // a finger needs more to hold on to; folded, the name needs a line
        const bool touch = KisTouchMode::isActive();
        setFixedHeight(m_folded ? (touch ? 32 : 22) : (touch ? 22 : 10));
        // folded, the panel is only this
        auto *dock = qobject_cast<QDockWidget *>(parentWidget());
        if (m_folded && dock) dock->setMaximumHeight(height());
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        QColor ink = palette().color(QPalette::WindowText);
        if (m_folded) {
            // the name on the left, a chevron saying it opens on the right
            ink.setAlphaF(underMouse() ? 0.85 : 0.6);
            QFont font = this->font();
            font.setPointSizeF(font.pointSizeF() * 0.9);
            painter.setFont(font);
            painter.setPen(ink);
            painter.drawText(rect().adjusted(10, 0, -28, 0), Qt::AlignLeft | Qt::AlignVCenter,
                             QFontMetrics(font).elidedText(m_title, Qt::ElideRight, width() - 38));
            QPen pen(ink, 1.4);
            pen.setCapStyle(Qt::RoundCap);
            pen.setJoinStyle(Qt::RoundJoin);
            painter.setPen(pen);
            const QPointF c(width() - 14, height() / 2.0);
            painter.drawPolyline(QPolygonF(QVector<QPointF>{c + QPointF(-3.5, -1.8), c + QPointF(0, 1.8), c + QPointF(3.5, -1.8)}));
            return;
        }
        ink.setAlphaF(m_dragging ? 0.6 : underMouse() ? 0.4 : 0.14);
        painter.setPen(Qt::NoPen);
        painter.setBrush(ink);
        const QRectF pill(width() / 2.0 - 14, height() / 2.0 - 1.5, 28, 3);
        painter.drawRoundedRect(pill, 1.5, 1.5);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) {
            m_pressPos = globalMousePos(event);
            m_pressed = true;
        }
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (!(event->buttons() & Qt::LeftButton)) return;
        const QPoint pos = globalMousePos(event);
        if (!m_dragging && (pos - m_pressPos).manhattanLength() >= QApplication::startDragDistance()) {
            m_dragging = true;
            setCursor(Qt::ClosedHandCursor);
            update();
        }
        if (m_dragging && dragged) dragged(pos);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        const bool wasPressed = m_pressed;
        m_pressed = false;
        if (!m_dragging) {
            // a click: not after a long press, which ends with a release far away
            if (wasPressed && event->button() == Qt::LeftButton && rect().contains(event->pos()) && clicked) {
                QTimer::singleShot(0, this, [this]() { if (clicked) clicked(); });
            }
            return;
        }
        m_dragging = false;
        setCursor(Qt::OpenHandCursor);
        update();
        // the panel may be rearranged, and this grip with it: after the release is delivered
        if (dropped) dropped(globalMousePos(event));
    }

    void contextMenuEvent(QContextMenuEvent *event) override
    {
        event->accept();
        if (cardRequested) cardRequested();
    }

private:
    QPoint m_pressPos;
    bool m_pressed {false};
    bool m_dragging {false};
    bool m_folded {false};
    QString m_title;
};

/// Where a dragged panel would go: a place in the column on one side, or none (the canvas)
struct PanelDrop {
    bool column {false};
    bool left {false};
    QString before;      ///< the panel it goes above; empty for the bottom of the column
    QRect indicator;     ///< in main window coordinates
};

/**
 * The first thing in the top bar: which tool the bar is about, with a chevron
 * that says it opens that tool's options. The rail shows the slot, this
 * shows the variant ("Contiguous Selection").
 */
class ToolChip : public QToolButton
{
public:
    explicit ToolChip(QWidget *parent)
        : QToolButton(parent)
    {
        setObjectName(QStringLiteral("studioToolChip"));
        setAutoRaise(true);
        setFocusPolicy(Qt::NoFocus);
        setAttribute(Qt::WA_Hover);
        setToolTip(i18nc("@info:tooltip", "Tool options"));
    }

    QSize sizeHint() const override
    {
        const int icon = iconSize().width();
        // painted by hand, so without the mnemonic the accelerator manager adds
        const QString label = QString(text()).remove(QLatin1Char('&'));
        return QSize(padding + icon + gap + fontMetrics().horizontalAdvance(label) + gap + chevron + padding,
                     qMax(28, icon + 8));
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QStylePainter painter(this);
        QStyleOptionToolButton option;
        initStyleOption(&option);
        if (option.state & (QStyle::State_MouseOver | QStyle::State_Sunken | QStyle::State_On)) {
            painter.drawPrimitive(QStyle::PE_PanelButtonTool, option);
        }

        const int icon = iconSize().width();
        QRect area = rect().adjusted(padding, 0, -padding, 0);
        this->icon().paint(&painter, QRect(area.left(), (height() - icon) / 2, icon, icon));
        area.setLeft(area.left() + icon + gap);
        painter.setPen(palette().color(QPalette::ButtonText));
        painter.drawText(area.adjusted(0, 0, -(gap + chevron), 0), Qt::AlignVCenter | Qt::AlignLeft | Qt::TextHideMnemonic, text());

        QColor dim = palette().color(QPalette::ButtonText);
        dim.setAlphaF(0.6);
        QPen pen(dim, 1.4);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        painter.setPen(pen);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QPointF tip(area.right() - chevron / 2.0, height() / 2.0 + 2);
        painter.drawPolyline(QPolygonF({tip + QPointF(-3.5, -3.5), tip, tip + QPointF(3.5, -3.5)}));
    }

private:
    static constexpr int padding = 8;
    static constexpr int gap = 6;
    static constexpr int chevron = 8;
};

/**
 * The colors in use, large enough to read at a glance: the foreground as a
 * big disc, the background behind it. Clicking the foreground opens the
 * color panel, clicking the background swaps the two.
 */
class ColorChip : public QWidget
{
public:
    std::function<void()> openColors;
    std::function<void()> swapColors;

    explicit ColorChip(QWidget *parent) : QWidget(parent)
    {
        setFixedSize(40, 40);
        setCursor(Qt::PointingHandCursor);
        setToolTip(i18nc("@info:tooltip", "Color (click), background color (click to swap, or X)"));
    }

    void setColors(const QColor &foreground, const QColor &background)
    {
        m_foreground = foreground;
        m_background = background;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        QColor border = palette().color(QPalette::WindowText);
        border.setAlphaF(0.35);
        painter.setPen(QPen(border, 1));
        painter.setBrush(m_background);
        painter.drawEllipse(backgroundRect());
        // a ring in the chrome color separates the two even when they are alike
        painter.setPen(QPen(palette().color(QPalette::Window), 3));
        painter.setBrush(m_foreground);
        painter.drawEllipse(foregroundRect());
        painter.setPen(QPen(border, 1));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(foregroundRect().adjusted(-1.5, -1.5, 1.5, 1.5));
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton) return;
        if (!foregroundRect().contains(event->pos()) && backgroundRect().contains(event->pos())) {
            if (swapColors) swapColors();
        } else if (openColors) {
            openColors();
        }
    }

private:
    QRectF foregroundRect() const { return QRectF(3, 3, 26, 26); }
    QRectF backgroundRect() const { return QRectF(17, 17, 20, 20); }

    QColor m_foreground {Qt::black};
    QColor m_background {Qt::white};
};

/// The document's name, and every open document a click away
class DocumentButton : public QToolButton
{
public:
    explicit DocumentButton(QWidget *parent) : QToolButton(parent)
    {
        setAutoRaise(true);
        setFocusPolicy(Qt::NoFocus);
        setToolButtonStyle(Qt::ToolButtonTextOnly);
        setPopupMode(QToolButton::InstantPopup);
        setToolTip(i18nc("@info:tooltip", "Open documents"));
    }
};

/**
 * The tools float over the canvas instead of taking a column of it: a panel
 * in the top left corner, as tall as what is in it. Empty, it is only a "+".
 */
class RailOverlay : public QWidget
{
public:
    explicit RailOverlay(QWidget *canvasArea)
        : QWidget(canvasArea)
    {
        setObjectName(QStringLiteral("studioToolRail"));
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(3, 3, 3, 6);
        layout->setSpacing(6);
        // follows its contents: adding a tool makes it taller, removing one shorter
        layout->setSizeConstraint(QLayout::SetFixedSize);
        canvasArea->installEventFilter(this);
    }

    void place()
    {
        move(10, 10);
        raise();
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == parentWidget() && (event->type() == QEvent::Resize || event->type() == QEvent::ChildAdded)) {
            place();
        }
        return QWidget::eventFilter(watched, event);
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        QColor background = palette().color(QPalette::Window);
        background.setAlphaF(0.94);
        QColor border = palette().color(QPalette::WindowText);
        border.setAlphaF(0.12);
        painter.setPen(QPen(border, 1));
        painter.setBrush(background);
        painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 14, 14);
    }
};

/// Looks like a search field, opens Find Anything
class FindField : public QAbstractButton
{
public:
    explicit FindField(QWidget *parent)
        : QAbstractButton(parent)
    {
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::NoFocus);
        setAttribute(Qt::WA_Hover);
        setFixedSize(34, 30);
        setToolTip(i18nc("@info:tooltip", "Find tools, commands, effects, brushes, layers and settings"));
        setAccessibleName(i18nc("@info:placeholder", "Find anything"));
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QPalette &pal = palette();
        if (underMouse()) {
            QColor background = pal.color(QPalette::ButtonText);
            background.setAlphaF(0.09);
            painter.setPen(Qt::NoPen);
            painter.setBrush(background);
            painter.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 7, 7);
        }
        const int iconSize = 18;
        KisIconUtils::loadIcon(QStringLiteral("studio_find"))
            .paint(&painter, QRect((width() - iconSize) / 2, (height() - iconSize) / 2, iconSize, iconSize));
    }
};

} // namespace

struct KisStudioLayout::Private
{
    KisMainWindow *mainWindow {nullptr};
    bool active {false};

    QPointer<QToolBar> rail;
    /// Brush size and opacity on the left edge
    QPointer<QWidget> brushRail;
    /// Replaces the menu bar with a single menu button while a document is open
    QPointer<QToolBar> topBar;
    QPointer<QWidget> findField;
    QPointer<QAction> findFieldAction;
    /// The toolbar the find field and document button were put on
    QPointer<QToolBar> placedOnBar;
    /// Toolbar entries that duplicate the brush rail, hidden while active
    QList<QPointer<QAction>> hiddenToolbarActions;
    /// Toolbar entries shown only for the tools they matter to
    QList<QPointer<QAction>> contextActions;
    /// Toolbars that only duplicate the main menu or shortcuts, hidden while active
    QList<QPointer<QToolBar>> hiddenToolBars;
    QSet<QString> leftPanels;
    /// The order and sides the pinned panels were last arranged in, so that it is only redone when it changed
    QString arrangement;
    /// The panels on the rail, in order
    QStringList railPanels;
    QHash<QString, QAction *> railActions;
    /// Rail buttons are inserted in front of it
    QAction *railSpacer {nullptr};

    /// Reordering a rail button by dragging it
    struct RailDrag {
        QPointer<QToolButton> button;
        QString id;
        QPoint pressPos;
        bool active {false};
        int targetIndex {-1};
    } railDrag;
    QPointer<QFrame> dropIndicator;
    /// Where a panel dragged by its grip or rail button would go, beside the canvas
    QPointer<QFrame> panelIndicator;

    /// Pinned panel heights by id, see heightsKey
    QHash<QString, int> heights;
    /// Pinned panels folded to their grip, by id
    QSet<QString> folded;
    QHash<QString, QPointer<PanelGrip>> grips;
    QTimer *heightSave {nullptr};
    /// Set while the panels are being arranged, whose resizing is not the user's
    bool settlingHeights {false};

    /// The pinned panels in the order they stack in: as on the rail, then the others
    QStringList pinnedOrder() const
    {
        QStringList order;
        Q_FOREACH (const QString &id, railPanels) {
            if (pinned.contains(id)) order << id;
        }
        Q_FOREACH (const QString &id, pinned) {
            if (!order.contains(id)) order << id;
        }
        return order;
    }

    /// The pinned panels shown on one side of the canvas, top to bottom
    QList<QDockWidget *> column(bool left, const QString &without) const
    {
        QList<QDockWidget *> docks;
        Q_FOREACH (const QString &id, pinnedOrder()) {
            QDockWidget *dock = mainWindow->dockWidget(id);
            if (id == without || !dock || dock == flyout || dock->isHidden() || dock->isFloating()
                || leftPanels.contains(id) != left) {
                continue;
            }
            docks << dock;
        }
        std::sort(docks.begin(), docks.end(), [](QDockWidget *a, QDockWidget *b) { return a->y() < b->y(); });
        return docks;
    }

    PanelDrop panelDropAt(const QPoint &globalPos, const QString &moving) const
    {
        PanelDrop drop;
        QWidget *canvasArea = mainWindow->centralWidget();
        if (!canvasArea) return drop;
        const QPoint pos = mainWindow->mapFromGlobal(globalPos);
        const QRect canvas = canvasArea->geometry();

        for (const bool left : {true, false}) {
            const QList<QDockWidget *> docks = column(left, moving);
            QRect area;
            Q_FOREACH (QDockWidget *dock, docks) {
                area |= dock->geometry();
            }
            // over the column, or near the canvas edge on a side without panels
            const bool overColumn = !area.isEmpty() && pos.x() >= area.left() - 8 && pos.x() <= area.right() + 8;
            const bool nearEdge = area.isEmpty() && canvas.contains(pos)
                                  && (left ? pos.x() < canvas.left() + leftDropZone : pos.x() > canvas.right() - leftDropZone);
            if (!overColumn && !nearEdge) continue;

            drop.column = true;
            drop.left = left;
            if (area.isEmpty()) {
                drop.indicator = QRect(left ? canvas.left() + 2 : canvas.right() - 4, canvas.top() + 6, 3, canvas.height() - 12);
                return drop;
            }
            int y = area.bottom() - 1;
            Q_FOREACH (QDockWidget *dock, docks) {
                if (pos.y() < dock->geometry().center().y()) {
                    drop.before = dock->objectName();
                    y = dock->geometry().top() - 1;
                    break;
                }
            }
            drop.indicator = QRect(area.left() + 4, qMax(area.top(), y - 1), area.width() - 8, 3);
            return drop;
        }
        return drop;
    }

    void showPanelDrop(const QPoint &globalPos, const QString &moving)
    {
        if (!panelIndicator) {
            panelIndicator = new QFrame(mainWindow);
            panelIndicator->setAutoFillBackground(true);
            panelIndicator->setBackgroundRole(QPalette::Highlight);
        }
        const PanelDrop drop = panelDropAt(globalPos, moving);
        if (!drop.column) {
            panelIndicator->hide();
            return;
        }
        panelIndicator->setGeometry(drop.indicator);
        panelIndicator->show();
        panelIndicator->raise();
    }

    void hidePanelDrop()
    {
        if (panelIndicator) panelIndicator->hide();
    }

    /// The unpinned panel that is currently open over the canvas
    QPointer<QDockWidget> flyout;
    /// What the flyout opened from, if not the rail: it goes right below it
    QPointer<QWidget> flyoutAnchor;
    QPointer<QToolButton> toolChip;
    QPointer<QAction> toolChipAction;
    QPointer<KisStudioToolStrip> toolStrip;
    QPointer<QAction> toolStripAction;
    QPointer<QAction> brushValuesAction;
    /// The toolbar's brush picture: a click opens the brush picker, the wheel steps through yours
    QPointer<QWidget> presetChip;
    /// Brushes taken from strokes (Alt+click) and from layers
    QPointer<KisStudioBrushMemory> brushMemory;
    /// The canvas's right click with a painting tool: your brushes in a ring
    QPointer<KisStudioCanvasRing> canvasRing;
    QString presetChipToolTip;
    int presetWheel {0};
    QPointer<QToolButton> documentButton;
    QPointer<QAction> documentButtonAction;
    QPointer<KisStudioCanvasHud> hud;
    QPointer<KisStudioEffectOverlay> effectOverlay;
    KisStudioWindowFrame *frame {nullptr};
    QPointer<QAction> windowButtonsAction;
    QPointer<QWidget> colorChip;
    QPointer<KisStudioToolRail> toolRail;
    QSet<QString> pinned;

    /// What a docker looked like before entering the studio layout
    struct ClassicDock {
        QPointer<QWidget> titleBar;
        QDockWidget::DockWidgetFeatures features;
    };
    QHash<QDockWidget *, ClassicDock> classicDocks;
    /// The panels the studio layout manages: every docker but the toolbox
    QSet<QDockWidget *> panels;
    QVector<QMetaObject::Connection> connections;
};

KisStudioLayout::KisStudioLayout(KisMainWindow *mainWindow)
    : QObject(mainWindow)
    , d(new Private)
{
    d->mainWindow = mainWindow;

    // a resize ends when the pointer lets go of the separator; save once it has
    d->heightSave = new QTimer(this);
    d->heightSave->setSingleShot(true);
    d->heightSave->setInterval(400);
    connect(d->heightSave, &QTimer::timeout, this, &KisStudioLayout::saveHeights);

    /**
     * Canvas-only mode restores a saved window state when it is toggled,
     * partly through the event queue, so normalize after it is done.
     */
    if (QAction *canvasOnly = mainWindow->actionCollection()->action(QStringLiteral("view_show_canvas_only"))) {
        connect(canvasOnly, &QAction::toggled, this, [this]() {
            closeFlyout();
            QTimer::singleShot(0, this, &KisStudioLayout::normalize);
        });
    }
}

KisStudioLayout::~KisStudioLayout()
{
    if (d->active) {
        qApp->removeEventFilter(this);
    }
}

bool KisStudioLayout::isEnabledInConfig()
{
    return KisConfig(true).readEntry<bool>(activeKey, true);
}

bool KisStudioLayout::isActive() const
{
    return d->active;
}

void KisStudioLayout::setActive(bool active)
{
    if (d->active == active) {
        return;
    }

    KisConfig cfg(false);

    // remember the classic layout to come back to, when switching away from it
    if (active && !isEnabledInConfig()) {
        cfg.writeEntry(classicStateKey, d->mainWindow->saveState());
    }
    cfg.writeEntry(activeKey, active);

    if (active) {
        d->active = true;
        enter();
    } else {
        d->active = false;
        leave();
    }
}

void KisStudioLayout::enter()
{
    KisMainWindow *mainWindow = d->mainWindow;
    const QStringList pinned = KisConfig(true).readEntry<QStringList>(pinnedKey);
    d->pinned = QSet<QString>(pinned.begin(), pinned.end());
    const QStringList folded = KisConfig(true).readEntry<QStringList>(foldedKey);
    d->folded = QSet<QString>(folded.begin(), folded.end());
    const QStringList left = KisConfig(true).readEntry<QStringList>(leftKey);
    d->leftPanels = QSet<QString>(left.begin(), left.end());
    d->arrangement.clear();
    d->heights.clear();
    Q_FOREACH (const QString &entry, KisConfig(true).readEntry<QStringList>(heightsKey)) {
        const int separator = entry.lastIndexOf(QLatin1Char('='));
        if (separator > 0) {
            d->heights.insert(entry.left(separator), entry.mid(separator + 1).toInt());
        }
    }

    Q_FOREACH (QDockWidget *dock, mainWindow->dockWidgets()) {
        QWidget *titleBar = dock->titleBarWidget();
        if (titleBar && titleBar->inherits("KisUtilityTitleBar")) {
            // e.g. the animation timeline keeps its controls in the title bar
            continue;
        }

        d->classicDocks.insert(dock, {titleBar, dock->features()});

        if (dock->objectName() == toolBoxId) {
            dock->setTitleBarWidget(new QWidget(dock));
            dock->setFeatures(QDockWidget::NoDockWidgetFeatures);
            continue;
        }

        // no title: a panel's content says what it is; its top edge moves it, and has its card
        auto *grip = new PanelGrip(dock);
        const QString id = dock->objectName();
        grip->dragged = [this, id](const QPoint &pos) { d->showPanelDrop(pos, id); };
        grip->dropped = [this, id](const QPoint &pos) {
            QTimer::singleShot(0, this, [this, id, pos]() { dropPanel(id, pos, false); });
        };
        QPointer<PanelGrip> gripPointer = grip;
        grip->cardRequested = [this, id, gripPointer]() {
            if (gripPointer) showPanelCard(id, gripPointer);
        };
        grip->clicked = [this, id]() { gripClicked(id); };
        d->grips.insert(id, grip);
        dock->setTitleBarWidget(grip);
        dock->setFeatures(QDockWidget::DockWidgetClosable);
        d->panels.insert(dock);

        // a name, so that a shortcut given on its card is saved, and on the window, so that it works
        QAction *toggle = dock->toggleViewAction();
        if (toggle->objectName().isEmpty()) {
            toggle->setObjectName(QStringLiteral("studio_panel_") + dock->objectName());
            const QString saved = KConfigGroup(KSharedConfig::openConfig(QStringLiteral("kritashortcutsrc")), QStringLiteral("Shortcuts"))
                                      .readEntry(toggle->objectName(), QString());
            if (!saved.isEmpty() && saved != QLatin1String("none")) {
                toggle->setShortcuts(QKeySequence::listFromString(saved));
            }
            mainWindow->addAction(toggle);
        }

        d->connections << connect(dock->toggleViewAction(), &QAction::toggled, this, [this, dock](bool visible) {
            dockToggled(dock, visible);
        });
    }

    createRail();
    createBrushRail();
    createTopBar();

    /**
     * All the effects, to browse: from Find ("Effects"), its shortcut, or a
     * button on the tools panel; it opens where the pointer is.
     */
    if (!mainWindow->actionCollection()->action(QStringLiteral("studio_effects"))) {
        QAction *effects = new QAction(KisIconUtils::loadIcon(QStringLiteral("studio_effects")), i18nc("@action", "Effects"), mainWindow);
        effects->setObjectName(QStringLiteral("studio_effects"));
        effects->setToolTip(i18nc("@info:tooltip", "All the effects, with yours first"));
        const QString saved = KConfigGroup(KSharedConfig::openConfig(QStringLiteral("kritashortcutsrc")), QStringLiteral("Shortcuts"))
                                  .readEntry(effects->objectName(), QString());
        if (!saved.isEmpty() && saved != QLatin1String("none")) {
            effects->setShortcuts(QKeySequence::listFromString(saved));
        }
        mainWindow->actionCollection()->addAction(effects->objectName(), effects);
        mainWindow->addAction(effects);
        connect(effects, &QAction::triggered, mainWindow, [mainWindow]() {
            auto *picker = new KisStudioEffectsPicker(mainWindow);
            picker->popupNear(QCursor::pos());
        });
    }

    /**
     * All the brushes, yours first: from the brush picture in the top bar,
     * from Find ("Pick a Brush"), or its shortcut, where the pointer is.
     */
    if (!mainWindow->actionCollection()->action(QStringLiteral("studio_brush_picker"))) {
        QAction *brushes = new QAction(KisIconUtils::loadIcon(QStringLiteral("paintop_settings_01")), i18nc("@action", "Pick a Brush"), mainWindow);
        brushes->setObjectName(QStringLiteral("studio_brush_picker"));
        brushes->setToolTip(i18nc("@info:tooltip", "All the brushes, with yours first"));
        const QString saved = KConfigGroup(KSharedConfig::openConfig(QStringLiteral("kritashortcutsrc")), QStringLiteral("Shortcuts"))
                                  .readEntry(brushes->objectName(), QString());
        if (!saved.isEmpty() && saved != QLatin1String("none")) {
            brushes->setShortcuts(QKeySequence::listFromString(saved));
        }
        mainWindow->actionCollection()->addAction(brushes->objectName(), brushes);
        mainWindow->addAction(brushes);
        connect(brushes, &QAction::triggered, mainWindow, [mainWindow]() {
            auto *picker = new KisStudioBrushPicker(mainWindow);
            picker->popupNear(QCursor::pos());
        });
    }
    if (!d->brushMemory) {
        d->brushMemory = new KisStudioBrushMemory(mainWindow);
    }
    if (!d->canvasRing) {
        d->canvasRing = new KisStudioCanvasRing(mainWindow);
    }
    // the buttons' shortcuts work from the start, not only once Find was opened
    QTimer::singleShot(0, this, [this]() {
        if (KisStudioFind *find = d->mainWindow->studioFind()) find->registerButtons();
    });
    if (KisPaintopBox *paintopBox = mainWindow->viewManager() ? mainWindow->viewManager()->paintOpBox() : nullptr) {
        d->presetChip = paintopBox->presetChip();
        if (d->presetChip) {
            d->presetChipToolTip = d->presetChip->toolTip();
            d->presetChip->setToolTip(i18nc("@info:tooltip", "Brushes: click to pick one, scroll to go through yours, "
                                                             "right click for the brush presets to manage them"));
        }
    }

    // New/Open/Save are in the main menu, undo and redo are Ctrl+Z and Ctrl+Shift+Z
    for (const char *name : {"mainToolBar", "editToolBar"}) {
        QToolBar *toolBar = mainWindow->findChild<QToolBar *>(QLatin1String(name), Qt::FindDirectChildrenOnly);
        if (toolBar && !toolBar->isHidden()) {
            toolBar->hide();
            d->hiddenToolBars << toolBar;
        }
    }

    // the brush rail already has the size and opacity sliders
    KisConfig cfg(true);
    for (int i = 1; i <= 5; i++) {
        const QString slider = cfg.toolbarSlider(i);
        QAction *action = mainWindow->actionCollection()->action(QStringLiteral("brushslider%1").arg(i));
        if (action && action->isVisible() && (slider == QLatin1String("size") || slider == QLatin1String("opacity"))) {
            action->setVisible(false);
            d->hiddenToolbarActions << action;
        }
    }

    qApp->installEventFilter(this);
    d->connections << connect(KoToolManager::instance(), &KoToolManager::changedTool, this,
                              [this](KoCanvasController *controller) {
                                  if (KisStudioToolRail::isOwnController(d->mainWindow, controller)) updateContextBar();
                              });
    d->connections << connect(KoToolManager::instance(), &KoToolManager::toolOptionWidgetsChanged, this,
                              [this](KoCanvasController *controller, const QList<QPointer<QWidget>> &widgets) {
                                  // with two windows, the other one's tool is not this top bar's business
                                  if (!KisStudioToolRail::isOwnController(d->mainWindow, controller)) return;
                                  if (d->toolStrip) {
                                      d->toolStrip->setOptionWidgets(widgets);
                                      if (d->toolStripAction) {
                                          d->toolStripAction->setVisible(d->toolStrip->hasOptions() && !isRailHidden());
                                      }
                                  }
                              });
    if (QMdiArea *area = mdiArea()) {
        d->connections << connect(area, &QMdiArea::subWindowActivated, this, [this]() {
            adoptDocuments();
            updateDocumentButton();
        });
    }
    d->connections << connect(mainWindow, &QWidget::windowTitleChanged, this, &KisStudioLayout::updateDocumentButton);

    if (!d->hud) {
        d->hud = new KisStudioCanvasHud(mainWindow);
    }
    d->hud->setActive(true);
    mainWindow->setProperty("studioLayoutActive", true);

    // the top bar is the title bar
    if (!d->frame) {
        d->frame = new KisStudioWindowFrame(mainWindow, [this](const QPoint &pos) { return isCaptionAt(pos); });
    }
    d->frame->setEnabled(KisConfig(true).readEntry<bool>("StudioCustomTitleBar", true));

    // effects are adjusted on the canvas, not in a dialog
    if (!d->effectOverlay) {
        d->effectOverlay = new KisStudioEffectOverlay(mainWindow);
    }
    if (KisViewManager *viewManager = mainWindow->viewManager()) {
        QPointer<KisStudioEffectOverlay> overlay = d->effectOverlay;
        viewManager->filterManager()->setInlineEditor({
            [overlay](KisFilterSP filter, KisFilterConfigurationSP config) { return overlay && overlay->open(filter, config); },
            [overlay]() { if (overlay) overlay->accept(); },
            [overlay]() { if (overlay) overlay->reject(); },
        });
    }

    // the current tool's options, for the strip, without waiting for a tool change
    if (mainWindow->activeView()) {
        KoToolManager::instance()->initializeCurrentToolForCanvas();
    }

    normalize();
    updateContextBar();
}

bool KisStudioLayout::handleNativeEvent(void *message, void *result)
{
    return d->active && d->frame && d->frame->handleNativeEvent(message, result);
}

bool KisStudioLayout::isCaptionAt(const QPoint &pos) const
{
    KisMainWindow *mainWindow = d->mainWindow;
    QWidget *widget = mainWindow->childAt(pos);
    if (!widget) {
        // between and around the top toolbars
        int bottom = 0;
        Q_FOREACH (QToolBar *toolBar, mainWindow->findChildren<QToolBar *>(QString(), Qt::FindDirectChildrenOnly)) {
            if (toolBar->isVisible() && mainWindow->toolBarArea(toolBar) == Qt::TopToolBarArea) {
                bottom = qMax(bottom, toolBar->geometry().bottom());
            }
        }
        return pos.y() <= bottom;
    }
    QWidget *container = widget;
    while (container && !qobject_cast<QToolBar *>(container)) {
        container = container->parentWidget();
    }
    QToolBar *toolBar = qobject_cast<QToolBar *>(container);
    if (!toolBar || mainWindow->toolBarArea(toolBar) != Qt::TopToolBarArea) {
        return false;
    }
    // the bar itself and the empty space in it drag the window; controls are for clicking
    return widget == toolBar || QByteArray(widget->metaObject()->className()) == "QWidget";
}

QMdiArea *KisStudioLayout::mdiArea() const
{
    return d->mainWindow->findChild<QMdiArea *>();
}

void KisStudioLayout::adoptDocuments()
{
    QMdiArea *area = mdiArea();
    if (!d->active || !area) {
        return;
    }
    /**
     * One document fills the canvas area without tabs or a frame; the
     * document button in the top bar switches between several. Windows
     * view mode with frameless maximized windows does that, where tabbed
     * mode would keep a row of tabs for a single name.
     */
    if (area->viewMode() != QMdiArea::SubWindowView) {
        area->setViewMode(QMdiArea::SubWindowView);
    }
    Q_FOREACH (QMdiSubWindow *window, area->subWindowList()) {
        if (!(window->windowFlags() & Qt::FramelessWindowHint)) {
            window->setWindowFlags(window->windowFlags() | Qt::FramelessWindowHint);
        }
        if (!window->isMaximized()) {
            window->showMaximized();
        }
    }
}

void KisStudioLayout::releaseDocuments()
{
    QMdiArea *area = mdiArea();
    if (!area) {
        return;
    }
    Q_FOREACH (QMdiSubWindow *window, area->subWindowList()) {
        if (window->windowFlags() & Qt::FramelessWindowHint) {
            window->setWindowFlags(window->windowFlags() & ~Qt::FramelessWindowHint);
            window->showMaximized();
        }
    }
    // what Preferences say now, which may have changed while in the studio layout
    area->setViewMode(QMdiArea::ViewMode(KisConfig(true).readEntry<int>("mdi_viewmode", int(QMdiArea::TabbedView))));
}

void KisStudioLayout::updateDocumentButton()
{
    if (!d->documentButton) {
        return;
    }
    QMdiArea *area = mdiArea();
    const QList<QMdiSubWindow *> windows = area ? area->subWindowList() : QList<QMdiSubWindow *>();

    auto nameOf = [](QMdiSubWindow *window) {
        KisView *view = qobject_cast<KisView *>(window->widget());
        KisDocument *document = view ? view->document() : nullptr;
        if (!document) {
            return window->windowTitle();
        }
        const QString path = document->path();
        QString name = path.isEmpty() ? i18nc("a document that was never saved", "Untitled") : QFileInfo(path).fileName();
        if (document->isModified()) {
            // a dot, like the modified marker of most editors
            name += QStringLiteral(" •");
        }
        return name;
    };

    QMdiSubWindow *current = area ? area->currentSubWindow() : nullptr;
    d->documentButton->setText(current ? nameOf(current) : QString());

    QMenu *menu = d->documentButton->menu();
    menu->clear();
    Q_FOREACH (QMdiSubWindow *window, windows) {
        QAction *action = menu->addAction(nameOf(window));
        action->setCheckable(true);
        action->setChecked(window == current);
        QPointer<QMdiSubWindow> target = window;
        connect(action, &QAction::triggered, this, [area, target]() {
            if (target && area) area->setActiveSubWindow(target);
        });
    }
    menu->addSeparator();
    KisKActionCollection *actions = d->mainWindow->actionCollection();
    for (const char *name : {"file_new", "file_open", "file_close"}) {
        if (QAction *action = actions->action(QLatin1String(name))) {
            menu->addAction(action);
        }
    }
    // one document needs no switcher; its name is in the window title
    if (d->documentButtonAction) {
        d->documentButtonAction->setVisible(windows.size() > 1 && !isRailHidden());
    }
}

void KisStudioLayout::leave()
{
    closeFlyout();
    qApp->removeEventFilter(this);
    if (d->presetChip) {
        d->presetChip->setToolTip(d->presetChipToolTip);
    }
    delete d->brushMemory;
    delete d->canvasRing;
    if (d->hud) {
        d->hud->setActive(false);
    }
    d->mainWindow->setProperty("studioLayoutActive", false);
    if (d->frame) {
        d->frame->setEnabled(false);
    }
    if (d->windowButtonsAction) {
        d->windowButtonsAction->setVisible(false);
    }
    if (d->effectOverlay && d->effectOverlay->isVisible()) {
        d->effectOverlay->reject();
    }
    if (KisViewManager *viewManager = d->mainWindow->viewManager()) {
        viewManager->filterManager()->setInlineEditor({});
    }
    if (d->mainWindow->viewManager()) {
        d->mainWindow->viewManager()->showHideScrollbars();
    }
    releaseDocuments();
    d->mainWindow->statusBar()->setVisible(KisConfig(true).showStatusBar());
    if (d->toolStripAction) {
        d->toolStripAction->setVisible(false);
    }
    if (d->documentButtonAction) {
        d->documentButtonAction->setVisible(false);
    }

    Q_FOREACH (const QMetaObject::Connection &connection, d->connections) {
        disconnect(connection);
    }
    d->connections.clear();

    const bool showTitleBars = KisConfig(true).showDockerTitleBars();
    for (auto it = d->classicDocks.begin(); it != d->classicDocks.end(); ++it) {
        QDockWidget *dock = it.key();
        applyFold(dock, false);
        QWidget *studioTitleBar = dock->titleBarWidget();

        dock->setTitleBarWidget(it->titleBar);
        dock->setFeatures(it->features);
        if (studioTitleBar && studioTitleBar != it->titleBar) {
            studioTitleBar->deleteLater();
        }
        if (it->titleBar) {
            it->titleBar->setVisible(showTitleBars || dock->isFloating());
        }
    }
    d->classicDocks.clear();
    d->panels.clear();
    d->grips.clear();

    if (d->rail) {
        d->rail->hide();
    }
    if (d->brushRail) {
        d->brushRail->hide();
    }
    if (d->topBar) {
        d->topBar->hide();
    }
    if (d->findFieldAction) {
        d->findFieldAction->setVisible(false);
    }
    if (!isCanvasOnly()) {
        d->mainWindow->menuBar()->show();
    }
    Q_FOREACH (QAction *action, d->hiddenToolbarActions) {
        if (action) {
            action->setVisible(true);
        }
    }
    d->hiddenToolbarActions.clear();
    Q_FOREACH (QAction *action, d->contextActions) {
        if (action) {
            action->setVisible(true);
        }
    }
    d->contextActions.clear();
    Q_FOREACH (QToolBar *toolBar, d->hiddenToolBars) {
        if (toolBar) {
            toolBar->show();
        }
    }
    d->hiddenToolBars.clear();

    KisConfig cfg(true);
    const QByteArray classicState = cfg.readEntry<QByteArray>(classicStateKey);
    if (!classicState.isEmpty()) {
        d->mainWindow->restoreWorkspaceState(classicState);
    } else {
        // never used the classic layout yet, show what a fresh Krita would
        const QString workspaceName = cfg.readEntry<QString>(QStringLiteral("CurrentWorkspace"), QStringLiteral("Default"));
        KoResourceServer<KisWorkspaceResource> *server = KisResourceServerProvider::instance()->workspaceServer();
        KisWorkspaceResourceSP workspace = server->resource(QString(), QString(), workspaceName);
        if (!workspace) {
            workspace = server->resource(QString(), QString(), QStringLiteral("Default"));
        }
        if (workspace) {
            d->mainWindow->restoreWorkspace(workspace);
        }
    }
}

void KisStudioLayout::normalize()
{

    if (!d->active) {
        return;
    }

    KisMainWindow *mainWindow = d->mainWindow;
    const bool panelsHidden = arePanelsHidden();

    /**
     * The pinned panels from the config decide what is shown, not the
     * restored state: it may come from a classic workspace or from the
     * window state saved before the studio layout was enabled.
     */
    Q_FOREACH (QDockWidget *dock, d->panels) {
        if (dock == d->flyout) {
            continue;
        }

        // a flyout never outlives the moment it was opened for
        if (dock->isFloating()) {
            dock->hide();
            dock->setFloating(false);
        }

        const bool isPinned = d->pinned.contains(dock->objectName());
        if (isPinned) {
            if (!panelsHidden) {
                dock->show();
            }
        } else {
            dock->hide();
        }
    }
    arrangePinned();

    // the tool rail replaces the toolbox
    if (QDockWidget *toolBox = mainWindow->dockWidget(toolBoxId)) {
        toolBox->hide();
    }

    if (d->rail) {
        d->rail->setVisible(!isRailHidden());
    }
    if (d->brushRail) {
        d->brushRail->setVisible(!isRailHidden());
    }
    if (d->topBar) {
        d->topBar->setVisible(!isRailHidden());
        moveTopBarToFront();
    }
    // the rail's commands are known once the GUI is built, which is after the rail
    if (d->toolRail) {
        d->toolRail->refresh();
    }
    placeFindField();
    if (d->findFieldAction) {
        d->findFieldAction->setVisible(!isRailHidden());
    }
    if (d->windowButtonsAction) {
        d->windowButtonsAction->setVisible(d->frame && d->frame->isEnabled() && !isRailHidden());
    }

    // restored window states bring these back
    for (const char *name : {"mainToolBar", "editToolBar"}) {
        QToolBar *toolBar = mainWindow->findChild<QToolBar *>(QLatin1String(name), Qt::FindDirectChildrenOnly);
        if (toolBar && !toolBar->isHidden()) {
            toolBar->hide();
            if (!d->hiddenToolBars.contains(toolBar)) {
                d->hiddenToolBars << toolBar;
            }
        }
    }
    // canvas-only mode manages the menu bar itself, the welcome page has no top bar
    if (!isCanvasOnly()) {
        mainWindow->menuBar()->setVisible(mainWindow->isWelcomeScreenShown());
    }

    // what the status bar said about the view is on the canvas now, the rest is in the document properties
    mainWindow->statusBar()->hide();
    adoptDocuments();
    updateDocumentButton();
    if (d->toolStripAction && d->toolStrip) {
        d->toolStripAction->setVisible(d->toolStrip->hasOptions() && !isRailHidden());
    }

    syncRail();
}

void KisStudioLayout::dockToggled(QDockWidget *dock, bool visible)
{

    syncRail();

    if (visible && dock != d->flyout && !d->pinned.contains(dock->objectName())) {
        /**
         * Something else showed an unpinned panel, e.g. the Dockers menu,
         * a shortcut or a restored workspace. Open it as a flyout, unless
         * it has been hidden again in the meantime, which is what
         * normalize() does after restoring a workspace.
         */
        QTimer::singleShot(0, this, [this, dock]() {
            if (d->active && !dock->isHidden() && dock != d->flyout
                && !d->pinned.contains(dock->objectName()) && !arePanelsHidden()) {
                showFlyout(dock);
            }
        });
    }
}

bool KisStudioLayout::arePanelsHidden() const
{
    QAction *showDockers = d->mainWindow->actionCollection()->action(QStringLiteral("view_toggledockers"));
    return isRailHidden() || (showDockers && !showDockers->isChecked());
}

bool KisStudioLayout::isRailHidden() const
{
    return d->mainWindow->isWelcomeScreenShown() || isCanvasOnly();
}

bool KisStudioLayout::isCanvasOnly() const
{
    QAction *canvasOnly = d->mainWindow->actionCollection()->action(QStringLiteral("view_show_canvas_only"));
    return canvasOnly && canvasOnly->isChecked();
}

void KisStudioLayout::closeFlyout()
{

    QDockWidget *dock = d->flyout;
    d->flyout = nullptr;
    d->flyoutAnchor = nullptr;
    if (!dock) {
        return;
    }

    dock->hide();
    dock->setFloating(false);
    syncRail();
}

void KisStudioLayout::showPanel(QDockWidget *dock)
{
    if (d->active && d->panels.contains(dock)) {
        togglePanel(dock, true);
    } else {
        dock->show();
        dock->raise();
    }
}

void KisStudioLayout::togglePanel(QDockWidget *dock, bool show)
{

    if (show) {
        if (d->pinned.contains(dock->objectName())) {
            dock->show();
            dock->raise();
        } else {
            showFlyout(dock);
        }
    } else if (dock == d->flyout) {
        closeFlyout();
    } else {
        dock->hide();
    }
}

void KisStudioLayout::showFlyout(QDockWidget *dock, QWidget *anchor)
{

    if (d->flyout && d->flyout != dock) {
        closeFlyout();
    }

    d->flyout = dock;
    d->flyoutAnchor = anchor;
    dock->setFloating(true);
    positionFlyout();
    KisStudioMotion::showFaded(dock);
    dock->raise();
    syncRail();
}

void KisStudioLayout::positionFlyout()
{
    QWidget *canvasArea = d->mainWindow->centralWidget();
    if (!d->flyout || !canvasArea) {
        return;
    }

    // a column along the right edge of the canvas, next to the pinned panels
    const QRect area(canvasArea->mapToGlobal(QPoint(0, 0)), canvasArea->size());
    const int margin = 6;
    int contentsWidth = d->flyout->widget() ? d->flyout->widget()->sizeHint().width() : 0;
    int contentsHeight = d->flyout->sizeHint().height();
    // a scroll area asks for little, the flyout should fit what it scrolls
    QScrollArea *scroll = d->flyout->findChild<QScrollArea *>();
    if (scroll && scroll->widget()) {
        const int frame = 2 * scroll->frameWidth();
        const int header = d->flyout->titleBarWidget() ? d->flyout->titleBarWidget()->sizeHint().height() : 0;
        contentsWidth = qMax(contentsWidth, scroll->widget()->sizeHint().width() + frame + scroll->verticalScrollBar()->sizeHint().width());
        contentsHeight = qMax(contentsHeight, header + scroll->widget()->sizeHint().height() + frame + 12);
    }
    const int width = qBound(260, contentsWidth + 2, 420);

    if (d->flyoutAnchor) {
        // right below what opened it, only as tall as its contents
        const int left = qMax(area.left() + margin, d->flyoutAnchor->mapToGlobal(QPoint(0, 0)).x());
        const int height = qBound(160, contentsHeight, area.height() - 2 * margin);
        d->flyout->setGeometry(left, area.top() + margin, width, height);
        return;
    }

    d->flyout->setGeometry(area.right() + 1 - margin - width, area.top() + margin,
                           width, area.height() - 2 * margin);
}

void KisStudioLayout::toggleToolOptions()
{
    QDockWidget *dock = d->mainWindow->dockWidget(QStringLiteral("sharedtooldocker"));
    if (!dock) {
        return;
    }
    if (d->flyout == dock) {
        closeFlyout();
    } else if (d->pinned.contains(dock->objectName())) {
        dock->show();
        dock->raise();
    } else {
        showFlyout(dock, d->toolChip);
    }
}

void KisStudioLayout::setPinned(QDockWidget *dock, bool pinned)
{

    const QString id = dock->objectName();

    if (pinned) {
        d->pinned.insert(id);
        if (dock == d->flyout) {
            d->flyout = nullptr;
        }
        dock->setFloating(false);
        d->arrangement.clear();
        arrangePinned();
        dock->show();
        applyFold(dock, d->folded.contains(id));
    } else {
        d->pinned.remove(id);
        applyFold(dock, false);
        const bool wasVisible = !dock->isHidden();
        dock->hide();
        if (wasVisible) {
            showFlyout(dock);
        }
    }

    savePinned();
    syncRail();
}

void KisStudioLayout::arrangePinned()
{
    /**
     * Pinned panels stack in the order of their rail buttons, on their side
     * of the canvas: reordering the rail reorders them. Only redone when the
     * order or a side changed, since it resets the panels' heights; the
     * heights they were given are put back after.
     */
    const QStringList order = d->pinnedOrder();
    QString arrangement;
    Q_FOREACH (const QString &id, order) {
        arrangement += (d->leftPanels.contains(id) ? QStringLiteral("L:") : QStringLiteral("R:")) + id + QLatin1Char(',');
    }
    if (arrangement == d->arrangement) {
        return;
    }
    d->arrangement = arrangement;

    QDockWidget *previous[2] = {nullptr, nullptr};
    Q_FOREACH (const QString &id, order) {
        QDockWidget *dock = d->mainWindow->dockWidget(id);
        if (!dock || dock == d->flyout) continue;
        const bool left = d->leftPanels.contains(id);
        QDockWidget *&above = previous[left ? 0 : 1];
        if (above) {
            d->mainWindow->splitDockWidget(above, dock, Qt::Vertical);
        } else {
            d->mainWindow->addDockWidget(left ? Qt::LeftDockWidgetArea : Qt::RightDockWidgetArea, dock);
        }
        above = dock;
    }
    restoreHeights();
}

void KisStudioLayout::restoreHeights()
{
    // the main window lays the docks out later: resize them once it has
    d->settlingHeights = true;
    QTimer::singleShot(0, this, [this]() {
        QList<QDockWidget *> docks;
        QList<int> sizes;
        Q_FOREACH (const QString &id, d->pinnedOrder()) {
            QDockWidget *dock = d->mainWindow->dockWidget(id);
            applyFold(dock, d->folded.contains(id) && d->pinned.contains(id));
            if (dock && !dock->isHidden() && !dock->isFloating() && d->heights.contains(id) && !d->folded.contains(id)) {
                docks << dock;
                sizes << d->heights.value(id);
            }
        }
        if (!docks.isEmpty()) {
            d->mainWindow->resizeDocks(docks, sizes, Qt::Vertical);
        }
        QTimer::singleShot(0, this, [this]() { d->settlingHeights = false; });
    });
}

void KisStudioLayout::saveHeights()
{
    Q_FOREACH (const QString &id, d->pinnedOrder()) {
        QDockWidget *dock = d->mainWindow->dockWidget(id);
        if (dock && !dock->isHidden() && !dock->isFloating() && !d->folded.contains(id)) {
            d->heights.insert(id, dock->height());
        }
    }
    QStringList entries;
    for (auto it = d->heights.constBegin(); it != d->heights.constEnd(); ++it) {
        entries << it.key() + QLatin1Char('=') + QString::number(it.value());
    }
    entries.sort();
    KisConfig(false).writeEntry(heightsKey, entries);
}

void KisStudioLayout::gripClicked(const QString &id)
{
    QDockWidget *dock = d->mainWindow->dockWidget(id);
    if (!dock) {
        return;
    }
    // over the canvas: tucked away, as its rail button would
    if (!d->pinned.contains(id)) {
        if (dock == d->flyout) closeFlyout();
        return;
    }
    // next to the canvas: folded to a line with its name, or back, in a moment
    const bool fold = !d->folded.contains(id);
    if (fold) {
        saveHeights();
        d->folded.insert(id);
    } else {
        d->folded.remove(id);
    }
    KisConfig(false).writeEntry(foldedKey, QStringList(d->folded.values()));
    PanelGrip *grip = d->grips.value(id);
    const int from = dock->height();
    QWidget *content = dock->widget();
    if (!grip || !content || dock->isHidden()) {
        applyFold(dock, fold);
        return;
    }
    d->settlingHeights = true;
    int to;
    if (fold) {
        // free to shrink, the name shown at once; squeezed for good at the end
        content->setProperty("studioMinHeight", content->minimumHeight());
        content->setProperty("studioMaxHeight", content->maximumHeight());
        content->setMinimumHeight(0);
        grip->setFolded(true, dock->windowTitle());
        to = grip->height();
    } else {
        applyFold(dock, false);
        to = d->heights.value(id, qMax(160, content->sizeHint().height() + grip->height()));
        dock->setMaximumHeight(from);
    }
    auto *animation = new QVariantAnimation(this);
    animation->setStartValue(from);
    animation->setEndValue(to);
    animation->setDuration(140);
    animation->setEasingCurve(fold ? QEasingCurve::InOutCubic : QEasingCurve::OutCubic);
    QPointer<QDockWidget> target = dock;
    connect(animation, &QVariantAnimation::valueChanged, this, [this, target, fold](const QVariant &value) {
        if (!target) return;
        target->setMaximumHeight(value.toInt());
        // growing: the column hands it the space as it may take it
        if (!fold) d->mainWindow->resizeDocks({target.data()}, {value.toInt()}, Qt::Vertical);
    });
    connect(animation, &QVariantAnimation::finished, this, [this, target, fold, to]() {
        if (target) {
            if (fold) {
                // the rest of folding: the content held at no height
                QWidget *content = target->widget();
                content->setProperty("studioFolded", true);
                content->setMaximumHeight(0);
                target->setMaximumHeight(to);
            } else {
                target->setMaximumHeight(QWIDGETSIZE_MAX);
                d->mainWindow->resizeDocks({target.data()}, {to}, Qt::Vertical);
            }
        }
        QTimer::singleShot(0, this, [this]() { d->settlingHeights = false; });
    });
    animation->start(QAbstractAnimation::DeleteWhenStopped);
}

void KisStudioLayout::applyFold(QDockWidget *dock, bool fold)
{
    if (!dock || !dock->widget()) {
        return;
    }
    PanelGrip *grip = d->grips.value(dock->objectName());
    QWidget *content = dock->widget();
    const bool isFolded = content->property("studioFolded").toBool();
    if (isFolded == fold && (!grip || grip->isFolded() == fold)) {
        return;
    }
    /**
     * Not hidden: a dock with hidden content leaves the column, and the column
     * narrows. Squeezed to no height instead, it keeps its width, and the dock
     * may not grow past its grip.
     */
    if (fold) {
        content->setProperty("studioFolded", true);
        content->setProperty("studioMinHeight", content->minimumHeight());
        content->setProperty("studioMaxHeight", content->maximumHeight());
        content->setMinimumHeight(0);
        content->setMaximumHeight(0);
    } else if (isFolded) {
        content->setProperty("studioFolded", false);
        content->setMaximumHeight(content->property("studioMaxHeight").toInt());
        content->setMinimumHeight(content->property("studioMinHeight").toInt());
    }
    if (grip) {
        grip->setFolded(fold, dock->windowTitle());
    }
    dock->setMaximumHeight(fold && grip ? grip->height() : QWIDGETSIZE_MAX);
}

void KisStudioLayout::dropPanel(const QString &id, const QPoint &globalPos, bool fromRail)
{
    d->hidePanelDrop();
    QDockWidget *dock = d->mainWindow->dockWidget(id);
    if (!dock) {
        return;
    }
    const PanelDrop drop = d->panelDropAt(globalPos, id);

    if (!drop.column) {
        if (fromRail) {
            // a rail button dropped on the canvas: off the rail, and closed
            if (d->pinned.contains(id)) setPinned(dock, false);
            if (dock == d->flyout) closeFlyout();
            dock->hide();
            QStringList panels = d->railPanels;
            panels.removeAll(id);
            setRailPanels(panels);
        } else if (d->pinned.contains(id)) {
            // a panel dropped on the canvas: unpinned, it opens over the canvas from the rail
            setPinned(dock, false);
        }
        return;
    }

    if (drop.left) d->leftPanels.insert(id);
    else d->leftPanels.remove(id);
    KisConfig(false).writeEntry(leftKey, QStringList(d->leftPanels.values()));
    d->pinned.insert(id);
    savePinned();
    if (dock == d->flyout) {
        d->flyout = nullptr;
        d->flyoutAnchor = nullptr;
    }
    dock->setFloating(false);

    // stacking follows the rail: put it on the rail where the panels of that column are
    QStringList panels = d->railPanels;
    panels.removeAll(id);
    int at = drop.before.isEmpty() ? -1 : panels.indexOf(drop.before);
    if (at < 0) {
        // the bottom of the column: right after its last panel
        Q_FOREACH (QDockWidget *other, d->column(drop.left, id)) {
            at = qMax(at, panels.indexOf(other->objectName()) + 1);
        }
    }
    panels.insert(at < 0 ? panels.size() : qMin(at, panels.size()), id);
    d->arrangement.clear();
    dock->show();
    setRailPanels(panels);
}

void KisStudioLayout::setPanelSide(const QString &id, bool left)
{
    if (left) d->leftPanels.insert(id);
    else d->leftPanels.remove(id);
    KisConfig(false).writeEntry(leftKey, QStringList(d->leftPanels.values()));
    arrangePinned();
}

void KisStudioLayout::savePinned()
{
    QStringList pinned = d->pinned.values();
    pinned.sort();
    KisConfig(false).writeEntry(pinnedKey, pinned);
}

void KisStudioLayout::createRail()
{
    if (d->rail) {
        return;
    }

    KisMainWindow *mainWindow = d->mainWindow;

    QToolBar *rail = new QToolBar(i18nc("@title:window", "Panels"), mainWindow);
    rail->setObjectName(QStringLiteral("studioPanelRail"));
    rail->setMovable(false);
    rail->setFloatable(false);
    rail->setOrientation(Qt::Vertical);
    rail->setToolButtonStyle(Qt::ToolButtonIconOnly);
    rail->setIconSize(QSize(22, 22));
    rail->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(rail, &QWidget::customContextMenuRequested, this, &KisStudioLayout::showRailContextMenu);
    mainWindow->addToolBar(Qt::RightToolBarArea, rail);
    d->rail = rail;

    auto *spacer = new QWidget(rail);
    spacer->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    d->railSpacer = rail->addWidget(spacer);

    auto *addPanel = new QToolButton(rail);
    addPanel->setIcon(KisIconUtils::loadIcon(QStringLiteral("list-add")));
    addPanel->setToolTip(i18nc("@info:tooltip", "Add a panel"));
    addPanel->setAccessibleName(addPanel->toolTip());
    connect(addPanel, &QToolButton::clicked, this, [this]() {
        KisStudioFind *find = d->mainWindow->studioFind();
        if (!find) return;
        find->pick(i18nc("@info:placeholder", "Add a panel"), [this](const QString &key) {
            const QString id = key.mid(QStringLiteral("panel:").size());
            QDockWidget *dock = d->mainWindow->dockWidget(id);
            if (!dock) return;
            if (!d->railPanels.contains(id)) {
                setRailPanels(d->railPanels + QStringList{id});
            }
            showPanel(dock);
        }, QStringLiteral("panel:"));
    });
    rail->addWidget(addPanel);

    // shows where a dragged rail button will go
    auto *indicator = new QFrame(rail);
    indicator->setFrameShape(QFrame::NoFrame);
    indicator->setAutoFillBackground(true);
    indicator->setBackgroundRole(QPalette::Highlight);
    indicator->hide();
    d->dropIndicator = indicator;

    d->railPanels = KisConfig(true).readEntry<QStringList>(panelsKey, defaultRailPanels());
    rebuildRailButtons();
}

void KisStudioLayout::rebuildRailButtons()
{
    QToolBar *rail = d->rail;
    if (!rail) {
        return;
    }

    Q_FOREACH (QAction *action, d->railActions) {
        rail->removeAction(action);
        action->deleteLater();
    }
    d->railActions.clear();

    QStringList panels;
    Q_FOREACH (const QString &id, d->railPanels) {
        QDockWidget *dock = d->mainWindow->dockWidget(id);
        if (!dock || panels.contains(id)) {
            continue;
        }
        panels << id;

        QAction *action = new QAction(railIcon(dock), dock->windowTitle(), rail);
        action->setCheckable(true);
        connect(action, &QAction::triggered, this, [this, id](bool checked) {
            if (QDockWidget *dock = d->mainWindow->dockWidget(id)) {
                togglePanel(dock, checked);
            }
        });
        rail->insertAction(d->railSpacer, action);
        d->railActions.insert(id, action);
    }
    d->railPanels = panels;

    syncRail();
}

void KisStudioLayout::setRailPanels(const QStringList &panels)
{
    d->railPanels = panels;
    KisConfig(false).writeEntry(panelsKey, panels);
    rebuildRailButtons();
    arrangePinned();
}

QIcon KisStudioLayout::railIcon(QDockWidget *dock) const
{
    const QString iconName = railIconName(dock->objectName());
    if (!iconName.isEmpty()) {
        return KisIconUtils::loadIcon(iconName);
    }

    // the first letter of the title, for panels without an icon of their own
    const int size = 44;
    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QColor color = d->rail ? d->rail->palette().color(QPalette::ButtonText) : QColor(Qt::gray);
    painter.setPen(QPen(color, 3));
    painter.drawRoundedRect(QRectF(3, 3, size - 6, size - 6), 9, 9);
    QFont font = painter.font();
    font.setPixelSize(24);
    font.setBold(true);
    painter.setFont(font);
    painter.drawText(QRect(0, 0, size, size), Qt::AlignCenter, dock->windowTitle().left(1).toUpper());
    painter.end();
    return QIcon(pixmap);
}

void KisStudioLayout::showRailContextMenu(const QPoint &pos)
{
    const QString clickedId = d->railActions.key(d->rail->actionAt(pos));
    if (!clickedId.isEmpty() && d->mainWindow->dockWidget(clickedId)) {
        QWidget *button = d->rail->widgetForAction(d->rail->actionAt(pos));
        showPanelCard(clickedId, button ? button : d->rail);
        return;
    }

    // the rail itself: panels are added with "+", so only putting it back is left
    auto *card = new KisStudioItemCard(d->mainWindow, KisIconUtils::loadIcon(QStringLiteral("studio_more")),
                                       i18nc("@title panels rail", "Panels"), nullptr);
    card->addCommand({KisIconUtils::loadIcon(QStringLiteral("view-refresh")), i18nc("@info:tooltip", "Put back the default panels"),
                      [this]() { setRailPanels(defaultRailPanels()); }});
    auto *anchor = new QWidget(d->rail);
    anchor->setGeometry(QRect(pos, QSize(1, 1)));
    card->popupBeside(anchor);
    anchor->deleteLater();
}

void KisStudioLayout::showPanelCard(const QString &id, QWidget *anchor)
{
    QPointer<QDockWidget> dock = d->mainWindow->dockWidget(id);
    if (!dock) {
        return;
    }
    // the panel's card: its shortcut, pinning it next to the canvas, taking it off the rail
    auto *card = new KisStudioItemCard(d->mainWindow, KisIconUtils::loadIcon(railIconName(id)),
                                       dock->windowTitle(), dock->toggleViewAction());
    card->addCommand({KisIconUtils::loadIcon(QStringLiteral("edit-delete")), i18nc("@info:tooltip", "Take off the rail (or drag its button onto the canvas)"),
                      [this, id]() {
                          QStringList panels = d->railPanels;
                          panels.removeAll(id);
                          setRailPanels(panels);
                      }});
    card->popupBeside(anchor);
}

bool KisStudioLayout::handleRailDrag(QObject *watched, QEvent *event)
{
    Private::RailDrag &drag = d->railDrag;

    switch (event->type()) {
    case QEvent::MouseButtonPress: {
        auto *button = qobject_cast<QToolButton *>(watched);
        auto *mouseEvent = static_cast<QMouseEvent *>(event);
        if (!button || !d->rail || button->parentWidget() != d->rail || mouseEvent->button() != Qt::LeftButton) {
            return false;
        }
        const QString id = d->railActions.key(button->defaultAction());
        if (id.isEmpty()) {
            return false;
        }
        drag = Private::RailDrag();
        drag.button = button;
        drag.id = id;
        drag.pressPos = globalMousePos(mouseEvent);
        return false; // let the button show its pressed state
    }

    case QEvent::MouseMove: {
        auto *mouseEvent = static_cast<QMouseEvent *>(event);
        if (!drag.button || watched != drag.button || !(mouseEvent->buttons() & Qt::LeftButton)) {
            return false;
        }
        if (!drag.active) {
            if ((globalMousePos(mouseEvent) - drag.pressPos).manhattanLength() < QApplication::startDragDistance()) {
                return false;
            }
            drag.active = true;
            drag.button->setDown(false);
        }

        // off the rail: where the panel would go beside the canvas
        const QPoint global = globalMousePos(mouseEvent);
        if (!QRect(d->rail->mapToGlobal(QPoint(0, 0)), d->rail->size()).adjusted(-12, 0, 12, 0).contains(global)) {
            if (d->dropIndicator) d->dropIndicator->hide();
            d->showPanelDrop(global, drag.id);
            return true;
        }
        d->hidePanelDrop();

        // the gap between rail buttons that is closest to the cursor
        const int y = d->rail->mapFromGlobal(global).y();
        drag.targetIndex = d->railPanels.size();
        int indicatorY = 0;
        for (int i = 0; i < d->railPanels.size(); i++) {
            QWidget *widget = d->rail->widgetForAction(d->railActions.value(d->railPanels[i]));
            if (!widget) {
                continue;
            }
            indicatorY = widget->geometry().bottom() + 1;
            if (y < widget->geometry().center().y()) {
                drag.targetIndex = i;
                indicatorY = widget->geometry().top();
                break;
            }
        }
        if (d->dropIndicator) {
            d->dropIndicator->setGeometry(3, indicatorY - 1, d->rail->width() - 6, 2);
            d->dropIndicator->show();
            d->dropIndicator->raise();
        }
        return true;
    }

    case QEvent::MouseButtonRelease: {
        if (!drag.button || watched != drag.button) {
            return false;
        }
        const bool wasActive = drag.active;
        const QString id = drag.id;
        int target = drag.targetIndex;
        drag.button->setDown(false);
        drag = Private::RailDrag();
        if (d->dropIndicator) {
            d->dropIndicator->hide();
        }
        if (!wasActive) {
            return false; // a plain click
        }

        const QPoint release = globalMousePos(static_cast<QMouseEvent *>(event));
        d->hidePanelDrop();
        if (!QRect(d->rail->mapToGlobal(QPoint(0, 0)), d->rail->size()).adjusted(-12, 0, 12, 0).contains(release)) {
            // beside the canvas it is pinned there, on the canvas it is taken off the rail
            QTimer::singleShot(0, this, [this, id, release]() { dropPanel(id, release, true); });
            return true;
        }

        QStringList panels = d->railPanels;
        const int from = panels.indexOf(id);
        if (from >= 0 && target >= 0) {
            panels.removeAt(from);
            if (target > from) {
                target--;
            }
            panels.insert(qBound(0, target, panels.size()), id);
            // the buttons are recreated, do it after the release has been delivered
            QTimer::singleShot(0, this, [this, panels]() { setRailPanels(panels); });
        }
        return true;
    }

    default:
        return false;
    }
}

void KisStudioLayout::createBrushRail()
{
    if (d->brushRail) {
        return;
    }

    KisMainWindow *mainWindow = d->mainWindow;
    KisPaintopBox *paintopBox = mainWindow->viewManager() ? mainWindow->viewManager()->paintOpBox() : nullptr;
    if (!paintopBox) {
        return;
    }

    auto *rail = new RailOverlay(mainWindow->centralWidget());
    auto *toolRail = new KisStudioToolRail(mainWindow, rail);
    d->toolRail = toolRail;
    if (KisStudioFind *find = mainWindow->studioFind()) {
        QPointer<KisStudioToolRail> target = toolRail;
        find->setTools([target](const QString &key) { return target && target->hasItem(key); },
                       [target](const QString &key, bool on) {
                           if (!target) return;
                           if (on) target->addItem(key);
                           else target->removeItem(key);
                       });
    }

    auto *contents = new QWidget(rail);
    auto *layout = new QVBoxLayout(contents);
    layout->setContentsMargins(0, 2, 0, 0);
    layout->setSpacing(0);

    if (KisViewManager *viewManager = mainWindow->viewManager()) {
        KisCanvasResourceProvider *resources = viewManager->canvasResourceProvider();
        auto *chip = new ColorChip(contents);
        chip->setColors(resources->fgColor().toQColor(), resources->bgColor().toQColor());
        auto refresh = [chip, resources]() {
            chip->setColors(resources->fgColor().toQColor(), resources->bgColor().toQColor());
        };
        connect(resources, &KisCanvasResourceProvider::sigFGColorChanged, chip, refresh);
        connect(resources, &KisCanvasResourceProvider::sigBGColorChanged, chip, refresh);
        // the colors are only known once a canvas is there
        connect(mainWindow, &KisMainWindow::activeViewChanged, chip, refresh);
        chip->swapColors = [resources]() {
            const KoColor foreground = resources->fgColor();
            resources->setFGColor(resources->bgColor());
            resources->setBGColor(foreground);
        };
        chip->openColors = [this]() {
            if (QDockWidget *colors = d->mainWindow->dockWidget(KisStudioColorPanel::id())) {
                showPanel(colors);
            }
        };
        layout->addWidget(chip, 0, Qt::AlignHCenter);
        d->colorChip = chip;
    }

    toolRail->addBottomWidget(contents);
    rail->layout()->addWidget(toolRail);
    rail->place();
    d->brushRail = rail;
}

void KisStudioLayout::createTopBar()
{
    if (d->topBar) {
        return;
    }

    KisMainWindow *mainWindow = d->mainWindow;

    QToolBar *bar = new QToolBar(i18nc("@title:window", "Tool"), mainWindow);
    bar->setObjectName(QStringLiteral("studioTopBar"));
    bar->setMovable(false);
    bar->setFloatable(false);
    bar->setContextMenuPolicy(Qt::PreventContextMenu);

    auto *toolChip = new ToolChip(bar);
    toolChip->setIconSize(bar->iconSize());
    connect(toolChip, &QToolButton::clicked, this, &KisStudioLayout::toggleToolOptions);
    d->toolChipAction = bar->addWidget(toolChip);
    d->toolChip = toolChip;

    auto *strip = new KisStudioToolStrip(bar);
    d->toolStripAction = bar->addWidget(strip);
    d->toolStripAction->setVisible(false);
    d->toolStrip = strip;

    // size and opacity, only for the tools that paint with them
    if (KisPaintopBox *paintopBox = mainWindow->viewManager() ? mainWindow->viewManager()->paintOpBox() : nullptr) {
        auto *values = new QWidget(bar);
        auto *valuesLayout = new QHBoxLayout(values);
        valuesLayout->setContentsMargins(6, 0, 0, 0);
        valuesLayout->setSpacing(2);
        const QVector<QPair<QString, KisStudioScrubber::Kind>> scrubbers = {
            {QStringLiteral("size"), KisStudioScrubber::Kind::Size},
            {QStringLiteral("opacity"), KisStudioScrubber::Kind::Opacity},
        };
        for (const auto &entry : scrubbers) {
            KisDoubleSliderSpinBox *source = paintopBox->brushSlider(entry.first);
            if (!source) {
                continue;
            }
            auto *scrubber = new KisStudioScrubber(entry.second,
                                                   entry.second == KisStudioScrubber::Kind::Size
                                                       ? i18nc("@info:tooltip", "Brush size: drag sideways, scroll, or double click to type")
                                                       : i18nc("@info:tooltip", "Opacity: drag sideways, scroll, or double click to type"),
                                                   values);
            scrubber->setRange(entry.second == KisStudioScrubber::Kind::Size ? qMax(source->minimum(), 1.0) : source->minimum(),
                               source->maximum());
            scrubber->setValue(source->value());
            valuesLayout->addWidget(scrubber);
            connect(scrubber, &KisStudioScrubber::valueCommitted, source, [source](qreal value) {
                source->setValue(value);
            });
            connect(paintopBox, &KisPaintopBox::sigBrushSliderValueChanged, scrubber,
                    [scrubber, id = entry.first](const QString &sliderID, qreal value) {
                        if (sliderID == id) {
                            scrubber->setValue(value);
                        }
                    });
        }
        d->brushValuesAction = bar->addWidget(values);
        d->brushValuesAction->setVisible(false);
    }

    auto *documents = new DocumentButton(mainWindow);
    documents->hide();
    documents->setMenu(new QMenu(documents));
    d->documentButton = documents;

    mainWindow->addToolBar(Qt::TopToolBarArea, bar);
    d->topBar = bar;

    // Find at the end of the top bar, placed there by placeFindField()
    auto *field = new FindField(mainWindow);
    field->hide();
    connect(field, &QAbstractButton::clicked, this, [this]() {
        if (QAction *find = d->mainWindow->actionCollection()->action(QStringLiteral("find_anything"))) {
            find->trigger();
        }
    });
    d->findField = field;
}

void KisStudioLayout::moveTopBarToFront()
{
    KisMainWindow *mainWindow = d->mainWindow;
    if (!d->topBar) {
        return;
    }

    /**
     * One row: the top bar goes right in front of the brush toolbar, which
     * holds the context options and Find. Positions can't decide this, they
     * are meaningless until the window has been shown (e.g. started minimized).
     */
    QToolBar *brushBar = mainWindow->findChild<QToolBar *>(QStringLiteral("BrushesAndStuff"), Qt::FindDirectChildrenOnly);
    if (brushBar && mainWindow->toolBarArea(brushBar) == Qt::TopToolBarArea) {
        mainWindow->removeToolBarBreak(d->topBar);
        mainWindow->removeToolBarBreak(brushBar);
        mainWindow->insertToolBar(brushBar, d->topBar);
    }
}

void KisStudioLayout::placeFindField()
{
    QToolBar *brushBar = d->mainWindow->findChild<QToolBar *>(QStringLiteral("BrushesAndStuff"), Qt::FindDirectChildrenOnly);
    if (!brushBar || brushBar == d->placedOnBar) {
        return;
    }
    /**
     * The actions are ours, not the toolbar's: Configure Toolbars deletes and
     * recreates the toolbar, which only gives our widgets back this way, and
     * they are put on the new toolbar the next time the layout is normalized.
     */
    d->placedOnBar = brushBar;
    if (d->documentButton && !d->documentButtonAction) {
        auto *action = new QWidgetAction(this);
        action->setDefaultWidget(d->documentButton);
        d->documentButtonAction = action;
    }
    if (d->findField && !d->findFieldAction) {
        auto *action = new QWidgetAction(this);
        action->setDefaultWidget(d->findField);
        d->findFieldAction = action;
    }
    if (d->documentButtonAction) {
        brushBar->addAction(d->documentButtonAction);
    }
    if (d->findFieldAction) {
        brushBar->addAction(d->findFieldAction);
    }
    if (d->frame && d->frame->isEnabled()) {
        if (!d->windowButtonsAction) {
            auto *action = new QWidgetAction(this);
            action->setDefaultWidget(d->frame->createButtons(d->mainWindow));
            d->windowButtonsAction = action;
        }
        brushBar->addAction(d->windowButtonsAction);
    }

    // buttons with a menu open it on a long press instead of with a separate arrow
    Q_FOREACH (QToolButton *button, brushBar->findChildren<QToolButton *>()) {
        if (button->menu() && button->popupMode() == QToolButton::MenuButtonPopup) {
            button->setPopupMode(QToolButton::DelayedPopup);
        }
    }
}

void KisStudioLayout::updateContextBar()
{
    if (!d->active) {
        return;
    }

    const QString tool = KoToolManager::instance()->activeToolId();
    auto any = [&tool](std::initializer_list<const char *> ids) {
        for (const char *id : ids) {
            if (tool == QLatin1String(id)) return true;
        }
        return false;
    };

    const bool shapeTool = any({"KritaShape/KisToolRectangle", "KritaShape/KisToolEllipse", "KisToolPolygon",
                                "KisToolPolyline", "KisToolPath", "KisToolPencil"});
    // tools that put down paint with the current brush preset
    const bool paints = shapeTool || any({"KritaShape/KisToolBrush", "KritaShape/KisToolDyna",
                                          "KritaShape/KisToolMultiBrush", "KritaShape/KisToolLine"});
    const bool fills = any({"KritaFill/KisToolFill", "KisToolEncloseAndFill", "KritaShape/KisToolLazyBrush"});
    const bool gradient = any({"KritaFill/KisToolGradient"});

    const QVector<QPair<const char *, bool>> rules = {
        {"paintops", paints},
        {"paintop_options", paints},
        {"composite_actions", paints},
        {"mirror_actions", paints},
        {"gradients", gradient},
        {"patterns", fills || shapeTool},
        // on the rail, large, for every tool
        {"dual", false},
        // set up once, not per stroke: they are in Find Anything
        {"workspaces", false},
        {"select_layout", false},
    };

    if (d->brushValuesAction) {
        d->brushValuesAction->setVisible(paints && !isRailHidden());
    }
    if (d->toolChip) {
        KoToolFactoryBase *factory = KoToolRegistry::instance()->value(tool);
        if (d->toolChipAction) {
            d->toolChipAction->setVisible(factory && !isRailHidden());
        }
        if (factory) {
            d->toolChip->setIcon(KisIconUtils::loadIcon(factory->iconName()));
            d->toolChip->setText(KisStudioToolRail::toolName(factory));
        }
    }
    if (d->flyout && d->flyoutAnchor) {
        // the options of the new tool arrive after this, and have another size
        QTimer::singleShot(0, this, &KisStudioLayout::positionFlyout);
    }

    KisKActionCollection *actions = d->mainWindow->actionCollection();
    for (const auto &rule : rules) {
        QAction *action = actions->action(QLatin1String(rule.first));
        if (!action || d->hiddenToolbarActions.contains(action)) {
            continue;
        }
        if (!d->contextActions.contains(action)) {
            d->contextActions << action;
        }
        action->setVisible(rule.second);
    }
}

void KisStudioLayout::syncRail()
{
    for (auto it = d->railActions.begin(); it != d->railActions.end(); ++it) {
        QDockWidget *dock = d->mainWindow->dockWidget(it.key());
        QSignalBlocker blocker(it.value());
        it.value()->setChecked(dock && !dock->isHidden());
    }
}

bool KisStudioLayout::handlePresetChip(QEvent *event)
{
    switch (event->type()) {
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick:
        // the picker opens on the release, as the presets did
        return static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton;
    case QEvent::MouseButtonRelease: {
        auto *mouse = static_cast<QMouseEvent *>(event);
        if (mouse->button() != Qt::LeftButton) {
            return false;
        }
        // not after a long press, which ends with a release far away
        if (d->presetChip->rect().contains(mouse->pos())) {
            auto *picker = new KisStudioBrushPicker(d->mainWindow);
            picker->popupBelow(d->presetChip);
        }
        return true;
    }
    case QEvent::ContextMenu:
        // the brush presets as they were: tags, importing, editing
        QMetaObject::invokeMethod(d->presetChip, "showPopupWidget");
        return true;
    case QEvent::Wheel: {
        // a notch is one brush; a touchpad's small steps add up to one
        d->presetWheel += static_cast<QWheelEvent *>(event)->angleDelta().y();
        if (qAbs(d->presetWheel) < 120) {
            return true;
        }
        const int step = d->presetWheel < 0 ? 1 : -1;
        d->presetWheel = 0;
        KisViewManager *view = d->mainWindow->viewManager();
        KisCanvasResourceProvider *resources = view ? view->canvasResourceProvider() : nullptr;
        if (resources && resources->currentPreset()) {
            const QString next = KisStudioBrushPicker::neighbour(d->mainWindow, resources->currentPreset()->name(), step);
            if (!next.isEmpty()) {
                KisStudioBrushPicker::use(view, next);
            }
        }
        return true;
    }
    default:
        return false;
    }
}

bool KisStudioLayout::eventFilter(QObject *watched, QEvent *event)
{
    if (handleRailDrag(watched, event)) {
        return true;
    }

    if (d->presetChip && watched == d->presetChip && handlePresetChip(event)) {
        return true;
    }

    if (event->type() == QEvent::Resize && !d->settlingHeights) {
        // a pinned panel made taller or shorter keeps that height
        auto *dock = qobject_cast<QDockWidget *>(watched);
        if (dock && d->panels.contains(dock) && d->pinned.contains(dock->objectName()) && !dock->isFloating()) {
            d->heightSave->start();
        }
    }

    switch (event->type()) {
    case QEvent::Paint: {
        // a flyout has no window frame, give it a hairline border (Windows 11 rounds it and draws one)
        QDockWidget *dock = qobject_cast<QDockWidget *>(watched);
        if (dock && dock->isFloating() && d->panels.contains(dock) && !KisModernStyle::systemRoundsWindows()) {
            QColor border = dock->palette().color(QPalette::WindowText);
            border.setAlphaF(0.25);
            QPainter painter(dock);
            painter.setPen(border);
            painter.drawRect(dock->rect().adjusted(0, 0, -1, -1));
        }
        break;
    }

    case QEvent::MouseButtonPress:
    case QEvent::TabletPress:
        // clicking anywhere else in the window closes the flyout, the
        // rail handles clicks on its own buttons
        if (d->flyout && d->flyout->isVisible()) {
            QWidget *widget = qobject_cast<QWidget *>(watched);
            const bool onRail = d->rail && widget && (widget == d->rail || d->rail->isAncestorOf(widget));
            // the chip toggles the tool options itself, and they follow the tool picked on the tool rail
            const bool onChip = d->toolChip && widget == d->toolChip;
            const bool pickingTool = d->flyoutAnchor && d->flyoutAnchor == d->toolChip && d->brushRail && widget
                                     && d->brushRail->isAncestorOf(widget);
            if (widget && widget->window() == d->mainWindow && !onRail && !onChip && !pickingTool) {
                closeFlyout();
            }
        }
        break;

    case QEvent::KeyPress:
        if (d->flyout && static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape
            && d->flyout->isAncestorOf(QApplication::focusWidget())) {
            closeFlyout();
            return true;
        }
        break;

    case QEvent::Move:
    case QEvent::Resize:
    case QEvent::WindowStateChange:
    case QEvent::Show:
        // also after minimizing: a flyout placed while minimized would be far off
        if (d->flyout && (watched == d->mainWindow || watched == d->mainWindow->centralWidget())) {
            QTimer::singleShot(0, this, &KisStudioLayout::positionFlyout);
        }
        break;

    default:
        break;
    }

    return QObject::eventFilter(watched, event);
}
