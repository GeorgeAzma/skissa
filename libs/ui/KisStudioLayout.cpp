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
#include <cmath>
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
#include <QSplitter>
#include <QSplitterHandle>
#include <QSet>
#include <QStyleOptionToolButton>
#include <QStylePainter>
#include <QTimer>
#include <QWindow>
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
#include "KisStudioPanelPicker.h"
#include "KisStudioGlass.h"
#include <QPainterPath>
#include "KisView.h"
#include "canvas/kis_canvas2.h"
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
/// How wide the pill of pinned panels is, and on which side of the canvas ("left" or "right")
const QString columnWidthKey = QStringLiteral("StudioLayoutPanelWidth");
const QString panelSideKey = QStringLiteral("StudioLayoutPanelSide");
/// The side of the canvas the tool rail is on ("left" or "right")
const QString toolRailSideKey = QStringLiteral("StudioLayoutToolRailSide");
/// Hidden by a tap on their grip: the pinned panels (the strip stays), the tools of the rail, the top bar
const QString panelsHiddenKey = QStringLiteral("StudioLayoutPanelsTucked");
const QString toolRailHiddenKey = QStringLiteral("StudioLayoutToolRailTucked");
const QString topBarHiddenKey = QStringLiteral("StudioLayoutTopBarTucked");
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
        painter.drawPath(KisModernStyle::squircle(pill, 1.5));
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
/// What lies over the canvas area's left and right edges (the pinned panels), for what is placed in it to keep clear of
const char *const leftInsetProperty = "studioCanvasLeftInset";
const char *const rightInsetProperty = "studioCanvasRightInset";

/// How far the pills over the top of the canvas reach down: what is on the sides starts under them
const char *const topInsetProperty = "studioCanvasTopInset";
/// A pill whose empty space (and grip) moves the window, as a title bar
const char *const captionProperty = "studioCaption";

QMargins canvasInsets(const QWidget *canvasArea)
{
    if (!canvasArea) return QMargins();
    return QMargins(canvasArea->property(leftInsetProperty).toInt(), canvasArea->property(topInsetProperty).toInt(),
                    canvasArea->property(rightInsetProperty).toInt(), 0);
}

/// How far the pinned panels' pill reaches in from the left and right edges, also when it is only its strip
const char *const leftPillProperty = "studioPanelPillLeft";
const char *const rightPillProperty = "studioPanelPillRight";

/// What a dragged pill or rail is held by: a grip line; the pointer goes through to what holds it
class DragGrip : public QWidget
{
public:
    explicit DragGrip(QWidget *parent)
        : QWidget(parent)
    {
        setCursor(Qt::OpenHandCursor);
        setFixedHeight(14);
        setToolTip(i18nc("@info:tooltip", "Drag to the other side of the canvas"));
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        QColor ink = palette().color(QPalette::WindowText);
        ink.setAlphaF(underMouse() ? 0.6 : 0.3);
        painter.setPen(Qt::NoPen);
        painter.setBrush(ink);
        /**
         * Six dots, held and dragged (a line would read as a minus): three
         * along the longer side, two across, as a handle is held. Each on a
         * pixel's middle, so that all six look alike and none blurs.
         */
        const bool upright = height() > width();
        const int along = 3, across = 2;
        const qreal pitch = 5.0;
        const qreal ratio = devicePixelRatioF();
        const QPointF center = QRectF(rect()).center();
        for (int i = 0; i < along; i++) {
            for (int j = 0; j < across; j++) {
                const qreal a = (i - (along - 1) / 2.0) * pitch;
                const qreal b = (j - (across - 1) / 2.0) * pitch;
                QPointF dot = center + (upright ? QPointF(b, a) : QPointF(a, b));
                dot = QPointF((std::floor(dot.x() * ratio) + 0.5) / ratio, (std::floor(dot.y() * ratio) + 0.5) / ratio);
                painter.drawEllipse(dot, 1.3, 1.3);
            }
        }
    }

    void enterEvent(QEvent *) override { update(); }
    void leaveEvent(QEvent *) override { update(); }
};

/**
 * The top bar's grip. Not a part of the title bar for the system (whose
 * clicks Qt never sees): a drag moves the window, a tap hides the bar (or
 * shows it again), a double click maximizes the window.
 */
class TopGrip : public DragGrip
{
public:
    std::function<void()> tapped;

    explicit TopGrip(QWidget *parent)
        : DragGrip(parent)
    {
        setProperty("studioTapGrip", true);
        m_tap.setSingleShot(true);
        // a tap waits for a second click, which makes it a double click
        m_tap.setInterval(QApplication::doubleClickInterval());
        QObject::connect(&m_tap, &QTimer::timeout, this, [this]() { if (tapped) tapped(); });
    }

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton) {
            DragGrip::mousePressEvent(event);
            return;
        }
        m_pressPos = globalMousePos(event);
        m_pressed = true;
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (!m_pressed || !(event->buttons() & Qt::LeftButton)) return;
        if ((globalMousePos(event) - m_pressPos).manhattanLength() < QApplication::startDragDistance()) return;
        m_pressed = false;
        m_tap.stop();
        QWidget *top = window();
        // the system moves the window, as by a title bar (a full screen one stays)
        if (!top->isFullScreen() && top->windowHandle()) top->windowHandle()->startSystemMove();
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (!m_pressed || event->button() != Qt::LeftButton) return;
        m_pressed = false;
        if (m_doubleClicked) {
            m_doubleClicked = false;
            return;
        }
        if (rect().contains(event->pos())) m_tap.start();
    }

    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton) return;
        m_tap.stop();
        m_pressed = true;
        m_doubleClicked = true;
        QWidget *top = window();
        if (top->isFullScreen()) return;
        if (top->isMaximized()) top->showNormal();
        else top->showMaximized();
    }

private:
    QTimer m_tap;
    QPoint m_pressPos;
    bool m_pressed {false};
    bool m_doubleClicked {false};
};

/// Between two pinned panels: nothing drawn, the glass shows; dragged, it shares their heights
class PanelSplitterHandle : public QSplitterHandle
{
public:
    using QSplitterHandle::QSplitterHandle;

protected:
    void paintEvent(QPaintEvent *) override {}
};

class PanelSplitter : public QSplitter
{
public:
    explicit PanelSplitter(QWidget *parent)
        : QSplitter(Qt::Vertical, parent)
    {
        setChildrenCollapsible(false);
        setHandleWidth(6);
    }

protected:
    QSplitterHandle *createHandle() override
    {
        return new PanelSplitterHandle(orientation(), this);
    }
};


class RailOverlay : public QWidget
{
public:
    /// Dragged by its grip and let go nearer the other side of the canvas
    std::function<void(bool left)> moved;
    /// Its grip tapped: its tools hidden, or shown again
    std::function<void()> tapped;

    explicit RailOverlay(QWidget *canvasArea)
        : QWidget(canvasArea)
    {
        setObjectName(QStringLiteral("studioToolRail"));
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(3, 0, 3, 6);
        layout->setSpacing(6);
        // follows its contents: adding a tool makes it taller, removing one shorter
        layout->setSizeConstraint(QLayout::SetFixedSize);
        m_grip = new DragGrip(this);
        layout->addWidget(m_grip);
        canvasArea->installEventFilter(this);
        setTucked(false);
    }

    void setLeft(bool left)
    {
        m_left = left;
        place();
    }

    /// Only its grip, as wide as the rail was; or all of it
    void setTucked(bool tucked)
    {
        if (tucked && !m_tucked) m_grip->setFixedWidth(qMax(m_grip->width(), 30));
        if (!tucked) {
            m_grip->setMinimumWidth(0);
            m_grip->setMaximumWidth(QWIDGETSIZE_MAX);
        }
        m_tucked = tucked;
        for (int i = 0; i < layout()->count(); i++) {
            QWidget *widget = layout()->itemAt(i)->widget();
            if (widget && widget != m_grip) widget->setVisible(!tucked);
        }
        m_grip->setToolTip(tucked ? i18nc("@info:tooltip", "Click to show the tools")
                                  : i18nc("@info:tooltip", "Drag to the other side of the canvas, click to hide the tools"));
        place();
    }

    void place()
    {
        if (m_moving) return;
        // beside the pinned panels' pill, if it is on the same side
        QWidget *area = parentWidget();
        const int top = area->property(topInsetProperty).toInt();
        const int y = top > 0 ? top + 6 : 10;
        if (m_left) {
            move(10 + area->property(leftPillProperty).toInt(), y);
        } else {
            move(area->width() - 10 - area->property(rightPillProperty).toInt() - width(), y);
        }
        raise();
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == parentWidget()
            && (event->type() == QEvent::Resize || event->type() == QEvent::ChildAdded || event->type() == QEvent::LayoutRequest)) {
            place();
        }
        return QWidget::eventFilter(watched, event);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        // by the grip (or between the tools): the rail follows the pointer, across the canvas
        if (event->button() == Qt::LeftButton) {
            m_moving = true;
            m_grab = event->pos().x();
            m_pressPos = globalMousePos(event);
            m_onGrip = m_grip->geometry().contains(event->pos());
            setCursor(Qt::ClosedHandCursor);
            event->accept();
            return;
        }
        QWidget::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (!m_moving) return;
        const QWidget *area = parentWidget();
        const int x = area->mapFromGlobal(globalMousePos(event)).x() - m_grab;
        move(qBound(4, x, qMax(4, area->width() - 4 - width())), y());
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (!m_moving || event->button() != Qt::LeftButton) {
            QWidget::mouseReleaseEvent(event);
            return;
        }
        m_moving = false;
        unsetCursor();
        if (m_onGrip && (globalMousePos(event) - m_pressPos).manhattanLength() < QApplication::startDragDistance()) {
            // a tap on the grip, not a drag
            place();
            if (tapped) tapped();
            return;
        }
        const bool left = x() + width() / 2 < parentWidget()->width() / 2;
        if (left != m_left && moved) moved(left);
        else place();
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        // glass: the canvas under it still shows
        const QPainterPath shape = KisStudioGlass::paint(painter, this, QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 14);
        QColor border = palette().color(QPalette::WindowText);
        border.setAlphaF(m_moving ? 0.3 : 0.12);
        KisStudioGlass::paintBorder(painter, this, shape, border);
    }

private:
    DragGrip *m_grip {nullptr};
    bool m_left {true};
    bool m_moving {false};
    bool m_tucked {false};
    bool m_onGrip {false};
    int m_grab {0};
    QPoint m_pressPos;
};

/**
 * A pill of glass over the top of the canvas, as long as what is in it:
 * the tool, its options and the brush's (on the left), or the document,
 * Find and the window's buttons (on the right). The canvas goes on under
 * it; its grip and its empty space move the window, as a title bar would.
 */
class TopPill : public QWidget
{
public:
    static constexpr int margin = 6;

    TopPill(QWidget *canvasArea, bool right)
        : QWidget(canvasArea)
        , m_right(right)
    {
        setObjectName(right ? QStringLiteral("studioWindowPill") : QStringLiteral("studioTopPill"));
        setProperty(captionProperty, true);
        auto *layout = new QHBoxLayout(this);
        layout->setContentsMargins(right ? 4 : 3, 1, 4, 1);
        layout->setSpacing(0);
        // as long as what is in it: a tool with more options makes it longer
        layout->setSizeConstraint(QLayout::SetFixedSize);
        if (!right) {
            m_grip = new TopGrip(this);
            // upright, in the pill's round end
            m_grip->setFixedSize(22, 30);
            layout->addWidget(m_grip);
        }
        // what it holds, hidden at once when the pill is tucked away to its grip
        m_contents = new QWidget(this);
        auto *contents = new QHBoxLayout(m_contents);
        contents->setContentsMargins(0, 0, 0, 0);
        contents->setSpacing(0);
        layout->addWidget(m_contents);
        setTucked(false);
        canvasArea->installEventFilter(this);
        hide();
    }

    /// \p widget in it, at \p index (counting the grip)
    void insert(int index, QWidget *widget)
    {
        QLayout *contents = m_contents->layout();
        const int at = m_grip ? index - 1 : index;
        static_cast<QHBoxLayout *>(contents)->insertWidget(qBound(0, at, contents->count()), widget);
    }

    QWidget *contents() const { return m_contents; }
    TopGrip *grip() const { return m_grip; }

    /// Only the grip; or all of it
    void setTucked(bool tucked)
    {
        m_contents->setVisible(!tucked);
        if (m_grip) {
            m_grip->setToolTip(tucked ? i18nc("@info:tooltip", "Click to show the bar")
                                      : i18nc("@info:tooltip", "Drag to move the window, click to hide the bar, double click to maximize"));
        }
    }

    void place()
    {
        QWidget *area = parentWidget();
        move(m_right ? area->width() - margin - width() : margin, margin);
        raise();
    }

    /**
     * Its size, or whether it is shown, changed: what is under it (the tool
     * rail, the pinned panels) moves. Its layout sizes it only once it is
     * laid out, after it is first placed.
     */
    std::function<void()> changed;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == parentWidget() && event->type() == QEvent::Resize) {
            place();
        }
        return QWidget::eventFilter(watched, event);
    }

    void resizeEvent(QResizeEvent *event) override
    {
        QWidget::resizeEvent(event);
        place();
        if (changed) changed();
    }

    void showEvent(QShowEvent *event) override
    {
        QWidget::showEvent(event);
        if (changed) changed();
    }

    void hideEvent(QHideEvent *event) override
    {
        QWidget::hideEvent(event);
        if (changed) changed();
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QPainterPath shape = KisStudioGlass::paint(painter, this, QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 12);
        QColor border = palette().color(QPalette::WindowText);
        border.setAlphaF(0.12);
        KisStudioGlass::paintBorder(painter, this, shape, border);
    }

private:
    bool m_right {false};
    TopGrip *m_grip {nullptr};
    QWidget *m_contents {nullptr};
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
            painter.drawPath(KisModernStyle::squircle(QRectF(rect()).adjusted(1, 1, -1, -1), 7));
        }
        const int iconSize = 18;
        KisIconUtils::loadIcon(QStringLiteral("studio_find"))
            .paint(&painter, QRect((width() - iconSize) / 2, (height() - iconSize) / 2, iconSize, iconSize));
    }
};

} // namespace

/**
 * The pinned panels, over an edge of the canvas: a pill of glass, as the
 * rail, the canvas going on under it and showing through. On top is the
 * strip of panel buttons; with no panel shown, the pill is only that.
 * Dragged by the strip, it goes to the other side of the canvas; dragging
 * between two panels shares their heights; dragging the inner edge makes
 * it wider or narrower.
 */
class KisStudioPanelColumn : public QWidget
{
public:
    /// From the canvas area's edges
    static constexpr int margin = 6;
    /// The inner edge, that is dragged
    static constexpr int edge = 5;
    static constexpr int minimumColumnWidth = 220;

    /// Placed again (moved, made wider or narrower), or its width dragged and let go (\p done)
    std::function<void(bool done)> resized;
    /// Dragged by its strip and let go nearer the other side of the canvas
    std::function<void(bool left)> moved;
    /// Its strip tapped (not on a button): the panels hidden, or shown again
    std::function<void()> tapped;

    KisStudioPanelColumn(QWidget *canvasArea, bool left, int width)
        : QWidget(canvasArea)
        , m_left(left)
        , m_width(width)
    {
        setObjectName(QStringLiteral("studioPanels"));
        m_layout = new QVBoxLayout(this);
        m_layout->setSpacing(0);
        m_splitter = new PanelSplitter(this);
        m_layout->addWidget(m_splitter);
        updateMargins();
        setMouseTracking(true);
        canvasArea->installEventFilter(this);
        hide();
    }

    QSplitter *splitter() const { return m_splitter; }
    int columnWidth() const { return m_width; }
    bool isLeft() const { return m_left; }

    /// The strip of panel buttons, on top
    void setStrip(QWidget *strip)
    {
        if (m_strip == strip) return;
        m_strip = strip;
        m_layout->insertWidget(0, strip);
    }

    void setLeft(bool left)
    {
        m_left = left;
        updateMargins();
        place();
    }

    /// The panels under the strip shown, or only the strip
    void setPanelsShown(bool shown)
    {
        m_splitter->setVisible(shown);
        place();
    }
    bool panelsShown() const { return !m_splitter->isHidden(); }

    QList<QDockWidget *> panels() const
    {
        QList<QDockWidget *> docks;
        for (int i = 0; i < m_splitter->count(); i++) {
            if (auto *dock = qobject_cast<QDockWidget *>(m_splitter->widget(i))) docks << dock;
        }
        return docks;
    }

    /// Takes in \p docks, in this order, each shown or hidden as it was (the others are taken away by whoever wants them)
    void setPanels(const QList<QDockWidget *> &docks)
    {
        for (int i = 0; i < docks.size(); i++) {
            QDockWidget *dock = docks[i];
            const bool shown = !dock->isHidden();
            KisStudioGlass::setSeeThrough(dock, true);
            m_splitter->insertWidget(i, dock);
            dock->setVisible(shown);
        }
    }

    /// \p dock as tall as \p height, the others sharing what that takes or gives
    void resizePanel(QDockWidget *dock, int height)
    {
        const int index = m_splitter->indexOf(dock);
        if (index < 0) return;
        QList<int> sizes = m_splitter->sizes();
        int delta = height - sizes[index];
        sizes[index] = height;
        // from the panels below it first, then above, as far as each can give
        QList<int> others;
        for (int i = index + 1; i < sizes.size(); i++) others << i;
        for (int i = index - 1; i >= 0; i--) others << i;
        for (int i : qAsConst(others)) {
            QWidget *widget = m_splitter->widget(i);
            if (widget->isHidden() || delta == 0) continue;
            const int minimum = qMax(widget->minimumSizeHint().height(), widget->minimumHeight());
            const int maximum = widget->maximumHeight();
            const int size = qBound(minimum, sizes[i] - delta, maximum);
            delta -= sizes[i] - size;
            sizes[i] = size;
        }
        m_splitter->setSizes(sizes);
    }

    /// Over the canvas area's edge: as tall as it with panels, as small as the strip without
    void place()
    {
        QWidget *area = parentWidget();
        // under the pills over the top of the canvas
        const int top = area->property(topInsetProperty).toInt() + margin;
        QSize size;
        if (panelsShown()) {
            size = QSize(qBound(minimumColumnWidth, m_width, qMax(minimumColumnWidth, area->width() / 2)), qMax(0, area->height() - top - margin));
        } else {
            size = m_layout->sizeHint();
        }
        QRect geometry(m_left ? margin : area->width() - margin - size.width(), top, size.width(), size.height());
        if (m_moving) geometry.moveLeft(x());
        if (geometry != this->geometry()) {
            setGeometry(geometry);
            if (resized) resized(false);
        }
        raise();
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == parentWidget() && event->type() == QEvent::Resize && isVisible()) {
            place();
        }
        return QWidget::eventFilter(watched, event);
    }

    void showEvent(QShowEvent *event) override
    {
        QWidget::showEvent(event);
        place();
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QPainterPath shape = KisStudioGlass::paint(painter, this, QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 12);
        QColor border = palette().color(QPalette::WindowText);
        border.setAlphaF(m_resizing || m_moving ? 0.3 : 0.12);
        KisStudioGlass::paintBorder(painter, this, shape, border);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton) {
            QWidget::mousePressEvent(event);
            return;
        }
        if (onEdge(event->pos())) {
            m_resizing = true;
        } else if (m_strip && event->pos().y() <= m_strip->geometry().bottom()) {
            // by the strip: the pill follows the pointer, across the canvas
            m_moving = true;
            m_grab = event->pos().x();
            m_pressPos = globalMousePos(event);
            setCursor(Qt::ClosedHandCursor);
        } else {
            QWidget::mousePressEvent(event);
            return;
        }
        update();
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        const QWidget *area = parentWidget();
        const int x = area->mapFromGlobal(globalMousePos(event)).x();
        if (m_moving) {
            move(qBound(margin, x - m_grab, qMax(margin, area->width() - margin - width())), y());
            return;
        }
        if (m_resizing) {
            // the inner edge follows the pointer
            m_width = qBound(minimumColumnWidth, m_left ? x - margin : area->width() - margin - x, qMax(minimumColumnWidth, area->width() / 2));
            place();
            return;
        }
        if (onEdge(event->pos())) setCursor(Qt::SizeHorCursor);
        else unsetCursor();
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton || (!m_moving && !m_resizing)) {
            QWidget::mouseReleaseEvent(event);
            return;
        }
        if (m_moving && (globalMousePos(event) - m_pressPos).manhattanLength() < QApplication::startDragDistance()) {
            // a tap on the strip, not a drag
            m_moving = false;
            unsetCursor();
            place();
            update();
            if (tapped) tapped();
            return;
        }
        if (m_moving) {
            m_moving = false;
            unsetCursor();
            // the side it was let go nearer to
            const bool left = x() + width() / 2 < parentWidget()->width() / 2;
            if (left != m_left && moved) moved(left);
            else place();
        } else {
            m_resizing = false;
            if (resized) resized(true);
        }
        update();
    }

    void leaveEvent(QEvent *event) override
    {
        if (!m_resizing && !m_moving) unsetCursor();
        QWidget::leaveEvent(event);
    }

private:
    bool onEdge(const QPoint &pos) const
    {
        if (!panelsShown()) return false;
        return m_left ? pos.x() >= width() - edge : pos.x() < edge;
    }

    void updateMargins()
    {
        m_layout->setContentsMargins(m_left ? 1 : edge, 1, m_left ? edge : 1, 1);
    }

    QVBoxLayout *m_layout {nullptr};
    QSplitter *m_splitter {nullptr};
    QPointer<QWidget> m_strip;
    bool m_left {false};
    int m_width {0};
    bool m_resizing {false};
    bool m_moving {false};
    int m_grab {0};
    QPoint m_pressPos;
};

/**
 * A panel opened over the canvas, in a window of its own: glass, the canvas
 * under it showing through. Not a floating dock: the window keeps the state
 * of its docks, a floating one too, and restoring a floating panel crashes.
 * The panel is taken out of its column while it is here, and put back after.
 */
class FlyoutWindow : public QWidget
{
public:
    explicit FlyoutWindow(QWidget *mainWindow)
        : QWidget(mainWindow, Qt::Tool | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint)
    {
        setObjectName(QStringLiteral("studioFlyout"));
        setAttribute(Qt::WA_TranslucentBackground);
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(1, 1, 1, 1);
        layout->setSpacing(0);
    }

    QDockWidget *panel() const { return m_panel; }

    void hold(QDockWidget *dock)
    {
        m_panel = dock;
        KisStudioGlass::setSeeThrough(dock, true);
        layout()->addWidget(dock);
        dock->show();
    }

    /// The panel let go of, see-through no more; still a child of this window until put somewhere else
    QDockWidget *release()
    {
        QDockWidget *dock = m_panel;
        m_panel = nullptr;
        if (dock) {
            layout()->removeWidget(dock);
            KisStudioGlass::setSeeThrough(dock, false);
        }
        return dock;
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QPainterPath shape = KisStudioGlass::paint(painter, this, QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 12);
        QColor border = palette().color(QPalette::WindowText);
        border.setAlphaF(0.14);
        KisStudioGlass::paintBorder(painter, this, shape, border);
    }

private:
    QPointer<QDockWidget> m_panel;
};

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
    /// The pinned panels, over the canvas, with the panel buttons
    QPointer<KisStudioPanelColumn> panelColumn;
    /// Over the top of the canvas: the tool's and the brush's bars, and the window's (the document, Find, its buttons)
    QPointer<TopPill> topPill;
    QPointer<TopPill> windowPill;
    QPointer<QToolBar> windowBar;
    /// The side of the canvas the pinned panels are on, and the tool rail
    bool panelsLeft {false};
    bool toolRailLeft {true};
    /// Tucked away by a tap on their grip
    bool panelsTucked {false};
    bool toolRailTucked {false};
    bool topBarTucked {false};
    QPointer<QWidget> stripGrip;
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
            if (id == without || !dock || dock == flyout || dock->isHidden() || dock->isFloating() || panelsLeft != left) {
                continue;
            }
            docks << dock;
        }
        std::sort(docks.begin(), docks.end(), [this](QDockWidget *a, QDockWidget *b) { return placeOf(a).y() < placeOf(b).y(); });
        return docks;
    }

    /// Where \p dock is, in main window coordinates (in a column, it is not the window's child)
    QRect placeOf(const QWidget *dock) const
    {
        return QRect(dock->mapTo(mainWindow, QPoint()), dock->size());
    }

    PanelDrop panelDropAt(const QPoint &globalPos, const QString &moving) const
    {
        PanelDrop drop;
        QWidget *canvasArea = mainWindow->centralWidget();
        if (!canvasArea) return drop;
        const QPoint pos = mainWindow->mapFromGlobal(globalPos);
        const QRect canvas = canvasArea->geometry();

        // the side the pinned panels are on
        for (const bool left : {panelsLeft}) {
            const QList<QDockWidget *> docks = column(left, moving);
            QRect area;
            Q_FOREACH (QDockWidget *dock, docks) {
                area |= placeOf(dock);
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
                if (pos.y() < placeOf(dock).center().y()) {
                    drop.before = dock->objectName();
                    y = placeOf(dock).top() - 1;
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
    /// Where the flyout is shown
    QPointer<FlyoutWindow> flyoutWindow;
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
    // one pill of panels: on the left if it says so, or if all the pinned panels were on the left before there was one
    const QString panelSide = KisConfig(true).readEntry<QString>(panelSideKey, QString());
    d->panelsLeft = panelSide == QLatin1String("left")
                    || (panelSide.isEmpty() && !d->pinned.isEmpty() && d->leftPanels.contains(d->pinned));
    d->toolRailLeft = KisConfig(true).readEntry<QString>(toolRailSideKey, QStringLiteral("left")) != QLatin1String("right");
    d->panelsTucked = KisConfig(true).readEntry<bool>(panelsHiddenKey, false);
    d->toolRailTucked = KisConfig(true).readEntry<bool>(toolRailHiddenKey, false);
    d->topBarTucked = KisConfig(true).readEntry<bool>(topBarHiddenKey, false);
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
        // how it looked, for the panel picker: taken as it goes off screen
        d->connections << connect(dock, &QDockWidget::visibilityChanged, this, [dock](bool visible) {
            if (!visible) KisStudioPanelPicker::rememberLook(dock);
        });
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
            updateCanvasInsets();
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
    // in a pill over the top of the canvas: its grip, and what is empty in it, move the window; controls are for clicking
    for (QWidget *w = widget; w; w = w->parentWidget()) {
        if (w->property(captionProperty).toBool()) {
            if (widget->property("studioTapGrip").toBool()) return false;
            return widget == w || dynamic_cast<DragGrip *>(widget) || qobject_cast<QToolBar *>(widget)
                || QByteArray(widget->metaObject()->className()) == "QWidget";
        }
    }
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
    if (KisStudioPanelColumn *column = d->panelColumn) {
        Q_FOREACH (QDockWidget *dock, column->panels()) {
            const bool shown = !dock->isHidden();
            parkPanel(dock);
            dock->setVisible(shown);
        }
        column->hide();
    }
    d->arrangement.clear();
    updateCanvasInsets();

    if (d->rail) {
        d->rail->hide();
    }
    if (d->brushRail) {
        d->brushRail->hide();
    }
    if (d->topBar) {
        d->topBar->hide();
    }
    releaseTopBars();
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
            dockAway(dock);
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
    // the column follows the rail, whatever order a restored window state had
    d->arrangement.clear();
    arrangePinned();
    updateColumns();

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
    }
    adoptTopBars();
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
    updateColumns();

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
    releaseFlyout();
    parkPanel(dock);
    syncRail();
}

void KisStudioLayout::releaseFlyout()
{
    if (d->flyoutWindow) {
        d->flyoutWindow->release();
        d->flyoutWindow->hide();
    }
}

void KisStudioLayout::dockAway(QDockWidget *dock)
{
    dock->setFloating(false);
    // a panel that never was in a column cannot be docked back by Qt: it is put in one, hidden,
    // so that it is not left floating (and saved so: restoring a floating panel crashes Qt)
    if (dock->isFloating()) {
        const bool hidden = dock->isHidden();
        d->mainWindow->addDockWidget(Qt::RightDockWidgetArea, dock);
        if (hidden) dock->hide();
    }
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
            if (d->panelsTucked) setPanelsTucked(false);
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
    if (d->flyout == dock) {
        d->flyoutAnchor = anchor;
        positionFlyout();
        d->flyoutWindow->raise();
        return;
    }

    d->flyout = dock;
    d->flyoutAnchor = anchor;
    if (!d->flyoutWindow) {
        d->flyoutWindow = new FlyoutWindow(d->mainWindow);
    }
    // out of its column (where it is hidden), into a window of glass over the canvas
    if (dock->isFloating()) dockAway(dock);
    d->mainWindow->removeDockWidget(dock);
    d->flyoutWindow->hold(dock);
    positionFlyout();
    KisStudioMotion::showFaded(d->flyoutWindow);
    d->flyoutWindow->raise();
    syncRail();
}

void KisStudioLayout::positionFlyout()
{
    QWidget *canvasArea = mdiArea() ? mdiArea()->parentWidget() : nullptr;
    if (!d->flyout || !d->flyoutWindow || !canvasArea) {
        return;
    }

    // a column along the right edge of the canvas, next to the pinned panels
    const QRect area = QRect(canvasArea->mapToGlobal(QPoint(0, 0)), canvasArea->size()).marginsRemoved(canvasInsets(canvasArea));
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
        d->flyoutWindow->setGeometry(left, area.top() + margin, width, height);
        return;
    }

    d->flyoutWindow->setGeometry(area.right() + 1 - margin - width, area.top() + margin,
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
            d->flyoutAnchor = nullptr;
            releaseFlyout();
        }
        dockAway(dock);
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
        } else {
            parkPanel(dock);
        }
        d->arrangement.clear();
        arrangePinned();
    }

    savePinned();
    syncRail();
}

void KisStudioLayout::arrangePinned()
{
    /**
     * Pinned panels stack in the order of their rail buttons, in the pill
     * over the canvas: reordering the rail reorders them. Only redone when
     * the order or the side changed; the heights they were given are put
     * back after.
     */
    const QStringList order = d->pinnedOrder();
    QString arrangement = d->panelsLeft ? QStringLiteral("L:") : QStringLiteral("R:");
    Q_FOREACH (const QString &id, order) {
        arrangement += id + QLatin1Char(',');
    }
    if (arrangement == d->arrangement) {
        updateColumns();
        return;
    }
    d->arrangement = arrangement;

    KisStudioPanelColumn *column = panelColumn();
    if (!column) {
        return;
    }
    QList<QDockWidget *> docks;
    Q_FOREACH (const QString &id, order) {
        QDockWidget *dock = d->mainWindow->dockWidget(id);
        if (dock && dock != d->flyout) docks << dock;
    }
    // no longer pinned: back among the window's docks, hidden
    Q_FOREACH (QDockWidget *dock, column->panels()) {
        if (!docks.contains(dock)) parkPanel(dock);
    }
    // out of the window's dock areas, where nothing is to be (the canvas goes on under the pill)
    Q_FOREACH (QDockWidget *dock, docks) {
        if (dock->parentWidget() != column->splitter()) {
            const bool shown = !dock->isHidden();
            if (dock->isFloating()) dockAway(dock);
            d->mainWindow->removeDockWidget(dock);
            dock->setVisible(shown);
        }
    }
    column->setLeft(d->panelsLeft);
    column->setPanels(docks);
    restoreHeights();
    updateColumns();
}

KisStudioPanelColumn *KisStudioLayout::panelColumn()
{
    if (!d->panelColumn) {
        QMdiArea *area = mdiArea();
        if (!area) return nullptr;
        KisConfig cfg(true);
        const int width = cfg.readEntry<int>(columnWidthKey, cfg.readEntry<int>(QStringLiteral("StudioLayoutRightColumnWidth"), 270));
        auto *column = new KisStudioPanelColumn(area->parentWidget(), d->panelsLeft, width);
        column->resized = [this](bool done) {
            updateCanvasInsets();
            if (done && d->panelColumn) KisConfig(false).writeEntry(columnWidthKey, d->panelColumn->columnWidth());
        };
        column->moved = [this](bool left) { setPanelsSide(left); };
        column->tapped = [this]() { setPanelsTucked(!d->panelsTucked); };
        d->panelColumn = column;
    }
    if (d->rail) d->panelColumn->setStrip(d->rail);
    return d->panelColumn;
}

void KisStudioLayout::setPanelsTucked(bool tucked)
{
    d->panelsTucked = tucked;
    KisConfig(false).writeEntry(panelsHiddenKey, tucked);
    if (d->stripGrip) {
        d->stripGrip->setToolTip(tucked ? i18nc("@info:tooltip", "Click to show the panels")
                                        : i18nc("@info:tooltip", "Drag to the other side of the canvas, click to hide the panels"));
    }
    updateColumns();
}

void KisStudioLayout::setToolRailTucked(bool tucked)
{
    d->toolRailTucked = tucked;
    KisConfig(false).writeEntry(toolRailHiddenKey, tucked);
    if (auto *rail = static_cast<RailOverlay *>(d->brushRail.data())) rail->setTucked(tucked);
}

void KisStudioLayout::setTopBarTucked(bool tucked)
{
    d->topBarTucked = tucked;
    KisConfig(false).writeEntry(topBarHiddenKey, tucked);
    if (d->topPill) d->topPill->setTucked(tucked);
    updateCanvasInsets();
}

void KisStudioLayout::setPanelsSide(bool left)
{
    d->panelsLeft = left;
    KisConfig(false).writeEntry(panelSideKey, left ? QStringLiteral("left") : QStringLiteral("right"));
    d->arrangement.clear();
    arrangePinned();
}

void KisStudioLayout::setToolRailSide(bool left)
{
    d->toolRailLeft = left;
    KisConfig(false).writeEntry(toolRailSideKey, left ? QStringLiteral("left") : QStringLiteral("right"));
    if (auto *rail = static_cast<RailOverlay *>(d->brushRail.data())) rail->setLeft(left);
}

void KisStudioLayout::parkPanel(QDockWidget *dock)
{
    // where the window keeps the panels it does not show, and finds them again
    KisStudioGlass::setSeeThrough(dock, false);
    dock->hide();
    d->mainWindow->addDockWidget(Qt::RightDockWidgetArea, dock);
    dock->hide();
}

void KisStudioLayout::updateColumns()
{
    KisStudioPanelColumn *column = d->panelColumn;
    if (column) {
        bool any = false;
        Q_FOREACH (QDockWidget *dock, column->panels()) {
            any = any || !dock->isHidden();
        }
        // the strip is there as the rail was; the panels under it when they are shown
        column->setPanelsShown(any && !arePanelsHidden() && !d->panelsTucked);
        column->setVisible(d->active && !isRailHidden());
    }
    updateCanvasInsets();
}

void KisStudioLayout::updateCanvasInsets()
{
    QMdiArea *mdi = mdiArea();
    QWidget *area = mdi ? mdi->parentWidget() : nullptr;
    if (!area) {
        return;
    }
    KisStudioPanelColumn *column = d->panelColumn;
    const bool shown = column && !column->isHidden();
    const bool full = shown && column->panelsShown();
    const bool left = shown && column->isLeft();
    const int reach = shown ? (left ? column->geometry().right() + 1 : area->width() - column->x()) : 0;
    // the pill, whatever it shows: what is at the top of the canvas (the tool rail) goes beside it
    const int pillLeft = shown && left ? reach : 0;
    const int pillRight = shown && !left ? reach : 0;
    // the pills over the top of the canvas: what is on the sides starts under them
    int top = 0;
    for (TopPill *pill : {d->topPill.data(), d->windowPill.data()}) {
        if (pill && !pill->isHidden()) top = qMax(top, pill->geometry().bottom() + 1);
    }
    // the pill as tall as the canvas: what is at the bottom (the zoom pill) too, and the view
    const QMargins insets(full && left ? reach : 0, top, full && !left ? reach : 0, 0);
    if (canvasInsets(area) != insets || area->property(leftPillProperty).toInt() != pillLeft
        || area->property(rightPillProperty).toInt() != pillRight) {
        area->setProperty(leftInsetProperty, insets.left());
        area->setProperty(rightInsetProperty, insets.right());
        area->setProperty(topInsetProperty, insets.top());
        if (column) column->place();
        area->setProperty(leftPillProperty, pillLeft);
        area->setProperty(rightPillProperty, pillRight);
        QCoreApplication::postEvent(area, new QEvent(QEvent::LayoutRequest));
        if (d->flyout) positionFlyout();
    }

    // each view fits the image, and zooms and turns about the middle, of what the panels leave of it
    Q_FOREACH (QMdiSubWindow *subWindow, mdi->subWindowList()) {
        KisView *view = qobject_cast<KisView *>(subWindow->widget());
        KisCanvas2 *canvas = view ? view->canvasBase() : nullptr;
        QWidget *canvasWidget = canvas ? canvas->canvasWidget() : nullptr;
        if (!canvasWidget) continue;
        const QRect widget(canvasWidget->mapToGlobal(QPoint()), canvasWidget->size());
        QMarginsF viewInsets;
        viewInsets.setTop(qMax(0, area->mapToGlobal(QPoint(0, top)).y() - widget.top()));
        if (full && left) viewInsets.setLeft(qMax(0, column->mapToGlobal(QPoint(column->width(), 0)).x() - widget.left()));
        if (full && !left) viewInsets.setRight(qMax(0, widget.right() + 1 - column->mapToGlobal(QPoint()).x()));
        if (view->canvasController()) view->canvasController()->setViewportInsets(viewInsets);
    }
}

void KisStudioLayout::restoreHeights()
{
    // the column lays the panels out later: sized once it has
    d->settlingHeights = true;
    QTimer::singleShot(0, this, [this]() {
        Q_FOREACH (const QString &id, d->pinnedOrder()) {
            applyFold(d->mainWindow->dockWidget(id), d->folded.contains(id) && d->pinned.contains(id));
        }
        if (KisStudioPanelColumn *column = d->panelColumn) {
            QList<int> sizes;
            Q_FOREACH (QDockWidget *dock, column->panels()) {
                const QString id = dock->objectName();
                sizes << (dock->isHidden() ? 0 : d->folded.contains(id) ? dock->minimumSizeHint().height()
                                                                          : d->heights.value(id, qMax(160, dock->sizeHint().height())));
            }
            column->splitter()->setSizes(sizes);
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
    // fast at first, easing into place: no wind-up, no overshoot
    animation->setDuration(220);
    animation->setEasingCurve(QEasingCurve::OutExpo);
    QPointer<QDockWidget> target = dock;
    connect(animation, &QVariantAnimation::valueChanged, this, [this, target, fold](const QVariant &value) {
        if (!target) return;
        target->setMaximumHeight(value.toInt());
        // growing: the column hands it the space as it may take it
        if (!fold) resizePanel(target, value.toInt());
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
                resizePanel(target, to);
            }
        }
        QTimer::singleShot(0, this, [this]() { d->settlingHeights = false; });
    });
    animation->start(QAbstractAnimation::DeleteWhenStopped);
}

void KisStudioLayout::resizePanel(QDockWidget *dock, int height)
{
    if (d->panelColumn) d->panelColumn->resizePanel(dock, height);
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
            takeOffRail(id);
        } else if (d->pinned.contains(id)) {
            // a panel dropped on the canvas: unpinned, it opens over the canvas from the rail
            setPinned(dock, false);
        }
        return;
    }

    d->pinned.insert(id);
    savePinned();
    if (dock == d->flyout) {
        d->flyout = nullptr;
        d->flyoutAnchor = nullptr;
        releaseFlyout();
    }
    dockAway(dock);

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

    // a strip on top of the pinned panels' pill: a few panels are open at a time, not a column of them
    QToolBar *rail = new QToolBar(i18nc("@title:window", "Panels"), mainWindow);
    // not the name it had as a toolbar of the window: a restored window state would take it back there
    rail->setObjectName(QStringLiteral("studioPanelStrip"));
    rail->setMovable(false);
    rail->setFloatable(false);
    rail->setOrientation(Qt::Horizontal);
    rail->setToolButtonStyle(Qt::ToolButtonIconOnly);
    rail->setIconSize(QSize(20, 20));
    rail->setContextMenuPolicy(Qt::CustomContextMenu);
    KisStudioGlass::setSeeThrough(rail, true);
    connect(rail, &QWidget::customContextMenuRequested, this, &KisStudioLayout::showRailContextMenu);
    d->rail = rail;

    // between the buttons and "+": what the pill is dragged by
    auto *spacer = new DragGrip(rail);
    d->stripGrip = spacer;
    spacer->setMinimumHeight(0);
    spacer->setMaximumHeight(QWIDGETSIZE_MAX);
    spacer->setMinimumSize(24, 0);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    d->railSpacer = rail->addWidget(spacer);
    setPanelsTucked(d->panelsTucked);

    auto *addPanel = new QToolButton(rail);
    addPanel->setIcon(KisIconUtils::loadIcon(QStringLiteral("list-add")));
    addPanel->setToolTip(i18nc("@info:tooltip", "Add a panel"));
    addPanel->setAccessibleName(addPanel->toolTip());
    connect(addPanel, &QToolButton::clicked, this, [this, addPanel]() {
        // every panel as it looks; the rail's first, and changed there too
        KisStudioPanelPicker::Rail rail;
        rail.key = panelsKey;
        rail.defaults = defaultRailPanels;
        rail.icon = [this](QDockWidget *dock) { return railIcon(dock); };
        auto *picker = new KisStudioPanelPicker(d->mainWindow, rail);
        connect(picker, &KisStudioPicker::sigPicked, this, [this](const QString &id) {
            QDockWidget *dock = d->mainWindow->dockWidget(id);
            if (!dock) return;
            if (!d->railPanels.contains(id)) {
                setRailPanels(d->railPanels + QStringList{id});
            }
            showPanel(dock);
        });
        connect(picker, &QObject::destroyed, this, [this]() {
            // starred, unstarred or reordered there
            const QStringList panels = KisConfig(true).readEntry<QStringList>(panelsKey, defaultRailPanels());
            if (panels != d->railPanels) setRailPanels(panels);
        });
        picker->popupBelow(addPanel);
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
    // the toolbar the window had it in, the last time
    mainWindow->removeToolBar(rail);
    panelColumn();
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
                // tucked away, a panel's button brings the panels back, with it among them
                togglePanel(dock, checked || d->panelsTucked);
                syncRail();
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
    painter.drawPath(KisModernStyle::squircle(QRectF(3, 3, size - 6, size - 6), 9));
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

void KisStudioLayout::takeOffRail(const QString &id)
{
    // off the rail, and closed: a panel stays only where it can be opened again
    if (QDockWidget *dock = d->mainWindow->dockWidget(id)) {
        if (d->pinned.contains(id)) setPinned(dock, false);
        if (dock == d->flyout) closeFlyout();
        dock->hide();
    }
    QStringList panels = d->railPanels;
    panels.removeAll(id);
    setRailPanels(panels);
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
                      [this, id]() { takeOffRail(id); }});
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

        // off the strip: where the panel would go in the pill, or the canvas
        const QPoint global = globalMousePos(mouseEvent);
        if (!QRect(d->rail->mapToGlobal(QPoint(0, 0)), d->rail->size()).adjusted(0, -12, 0, 12).contains(global)) {
            if (d->dropIndicator) d->dropIndicator->hide();
            d->showPanelDrop(global, drag.id);
            return true;
        }
        d->hidePanelDrop();

        // the gap between the strip's buttons that is closest to the cursor
        const int x = d->rail->mapFromGlobal(global).x();
        drag.targetIndex = d->railPanels.size();
        int indicatorX = 0;
        for (int i = 0; i < d->railPanels.size(); i++) {
            QWidget *widget = d->rail->widgetForAction(d->railActions.value(d->railPanels[i]));
            if (!widget) {
                continue;
            }
            indicatorX = widget->geometry().right() + 1;
            if (x < widget->geometry().center().x()) {
                drag.targetIndex = i;
                indicatorX = widget->geometry().left();
                break;
            }
        }
        if (d->dropIndicator) {
            d->dropIndicator->setGeometry(indicatorX - 1, 3, 2, d->rail->height() - 6);
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
        if (!QRect(d->rail->mapToGlobal(QPoint(0, 0)), d->rail->size()).adjusted(0, -12, 0, 12).contains(release)) {
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
    rail->moved = [this](bool left) { setToolRailSide(left); };
    rail->tapped = [this]() { setToolRailTucked(!d->toolRailTucked); };
    rail->setLeft(d->toolRailLeft);
    rail->setTucked(d->toolRailTucked);
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

    KisStudioGlass::setSeeThrough(bar, true);
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

void KisStudioLayout::adoptTopBars()
{
    QMdiArea *mdi = mdiArea();
    QWidget *area = mdi ? mdi->parentWidget() : nullptr;
    if (!area) {
        return;
    }
    if (!d->topPill) {
        d->topPill = new TopPill(area, false);
        d->topPill->changed = [self = QPointer<KisStudioLayout>(this)]() { if (self) self->updateCanvasInsets(); };
        d->topPill->grip()->tapped = [this]() { setTopBarTucked(!d->topBarTucked); };
        d->topPill->setTucked(d->topBarTucked);
    }
    if (!d->windowPill) {
        d->windowPill = new TopPill(area, true);
        d->windowPill->changed = [self = QPointer<KisStudioLayout>(this)]() { if (self) self->updateCanvasInsets(); };
        auto *bar = new QToolBar(i18nc("@title:window", "Window"), d->windowPill);
        bar->setObjectName(QStringLiteral("studioWindowBar"));
        bar->setMovable(false);
        bar->setFloatable(false);
        bar->setContextMenuPolicy(Qt::PreventContextMenu);
        KisStudioGlass::setSeeThrough(bar, true);
        d->windowPill->insert(0, bar);
        // a toolbar is hidden until a main window shows it
        bar->show();
        d->windowBar = bar;
    }

    /**
     * The tool's bar and the brush's (the window's, made from its GUI
     * description) in the pill, out of the window's top row: the canvas
     * goes on under them. A restored window state, or Configure Toolbars,
     * may put the brush's back there; it is taken again here.
     */
    QToolBar *brushBar = d->mainWindow->findChild<QToolBar *>(QStringLiteral("BrushesAndStuff"));
    int index = 1;
    for (QToolBar *bar : {d->topBar.data(), brushBar}) {
        if (!bar) continue;
        if (bar->parentWidget() != d->topPill->contents()) {
            d->mainWindow->removeToolBar(bar);
            bar->setMovable(false);
            KisStudioGlass::setSeeThrough(bar, true);
            d->topPill->insert(index, bar);
        }
        index++;
    }
    // the brush's options are always there (the tool's bar shows itself, see normalize); a toolbar is
    // hidden until a main window shows it, and taken out of the window it is not
    if (brushBar) brushBar->show();
    // a restored window state hides the toolbars it finds that it has no place for
    if (d->windowBar) d->windowBar->show();

    const bool shown = d->active && !isRailHidden();
    d->topPill->setVisible(shown);
    d->windowPill->setVisible(shown);
    if (shown) {
        d->topPill->place();
        d->windowPill->place();
    }
    updateCanvasInsets();
}

void KisStudioLayout::releaseTopBars()
{
    // back in the window's top row, as Krita has them
    QToolBar *brushBar = d->mainWindow->findChild<QToolBar *>(QStringLiteral("BrushesAndStuff"));
    if (brushBar && brushBar->parentWidget() != d->mainWindow) {
        const bool shown = !brushBar->isHidden();
        KisStudioGlass::setSeeThrough(brushBar, false);
        brushBar->setMovable(true);
        d->mainWindow->addToolBar(Qt::TopToolBarArea, brushBar);
        brushBar->setVisible(shown);
    }
    if (d->topPill) d->topPill->hide();
    if (d->windowPill) d->windowPill->hide();
    updateCanvasInsets();
}

void KisStudioLayout::placeFindField()
{
    /**
     * The document, Find and the window's buttons, in the pill over the
     * top right of the canvas. The actions are ours, not a toolbar's: they
     * go on the window pill's bar once, and stay.
     */
    if (d->documentButton && !d->documentButtonAction) {
        auto *action = new QWidgetAction(this);
        action->setDefaultWidget(d->documentButton);
        d->documentButtonAction = action;
        if (d->windowBar) d->windowBar->addAction(action);
    }
    if (d->findField && !d->findFieldAction) {
        auto *action = new QWidgetAction(this);
        action->setDefaultWidget(d->findField);
        d->findFieldAction = action;
        if (d->windowBar) d->windowBar->addAction(action);
    }
    if (d->frame && d->frame->isEnabled() && !d->windowButtonsAction) {
        auto *action = new QWidgetAction(this);
        action->setDefaultWidget(d->frame->createButtons(d->mainWindow));
        d->windowButtonsAction = action;
        if (d->windowBar) d->windowBar->addAction(action);
    }

    QToolBar *brushBar = d->mainWindow->findChild<QToolBar *>(QStringLiteral("BrushesAndStuff"));
    if (!brushBar || brushBar == d->placedOnBar) {
        return;
    }
    d->placedOnBar = brushBar;
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
    // a flyout is for the moment it was opened for: never saved with the window as a floating panel
    // (restoring one floating crashes Qt), so closed before the window keeps how it is laid out
    if (watched == d->mainWindow && event->type() == QEvent::Close) {
        if (d->flyout) closeFlyout();
        Q_FOREACH (QDockWidget *dock, d->mainWindow->dockWidgets()) {
            if (dock->isFloating() && dock->isHidden()) dockAway(dock);
        }
    }

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
        break;
    }

    case QEvent::MouseButtonPress:
    case QEvent::TabletPress:
        // clicking anywhere else in the window closes the flyout, the
        // rail handles clicks on its own buttons
        if (d->flyout && d->flyout->isVisible()) {
            QWidget *widget = qobject_cast<QWidget *>(watched);
            // in the flyout: its own window, not the main one
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
