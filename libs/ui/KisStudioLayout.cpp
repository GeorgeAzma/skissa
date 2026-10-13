/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioLayout.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QDockWidget>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QHash>
#include <QAbstractSpinBox>
#include <QCursor>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMdiArea>
#include <QMdiSubWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QSplitter>
#include <QSplitterHandle>
#include <QStatusBar>
#include <QStyleOptionToolButton>
#include <QStylePainter>
#include <QTabletEvent>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVariantAnimation>
#include <QWheelEvent>
#include <QWidgetAction>
#include <QWindow>

#include <cmath>
#include <functional>

#include <KoCanvasController.h>
#include <KoToolFactoryBase.h>
#include <KoToolManager.h>
#include <KoToolRegistry.h>
#include <filter/kis_filter.h>
#include <filter/kis_filter_configuration.h>
#include <kactioncollection.h>
#include <klocalizedstring.h>

#include "KisDocument.h"
#include "KisMainWindow.h"
#include "KisModernStyle.h"
#include "KisResourceServerProvider.h"
#include "KisSettingsRegistry.h"
#include "KisStudioBrushMemory.h"
#include "KisStudioBrushPicker.h"
#include "KisStudioCanvasHud.h"
#include "KisStudioCanvasRing.h"
#include "KisStudioColorPanel.h"
#include "KisStudioEffectOverlay.h"
#include "KisStudioEffectsPicker.h"
#include "KisStudioFind.h"
#include "KisStudioGlass.h"
#include "KisStudioItemCard.h"
#include "KisStudioEmptyStates.h"
#include "KisStudioMotion.h"
#include "KisStudioPanelPicker.h"
#include "KisStudioScrubber.h"
#include "KisStudioSettings.h"
#include "KisStudioToolRail.h"
#include "KisStudioToolStrip.h"
#include "KisStudioWindowFrame.h"
#include "KisTouchMode.h"
#include "KisView.h"
#include "KisViewManager.h"
#include "KisWelcomePageWidget.h"
#include "canvas/kis_canvas2.h"
#include "canvas/kis_canvas_controller.h"
#include "kis_canvas_resource_provider.h"
#include "kis_config.h"
#include "kis_filter_manager.h"
#include "kis_icon_utils.h"
#include "kis_paintop_box.h"
#include "kis_selection_actions_panel_button.h"
#include "kis_slider_spin_box.h"
#include "kis_workspace_resource.h"

namespace {

const QString toolBoxId = QStringLiteral("ToolBox");

const QString activeKey = QStringLiteral("StudioLayoutActive");
const QString panelsKey = QStringLiteral("StudioLayoutPanels");
const QString pinnedKey = QStringLiteral("StudioLayoutPinnedPanels");
/// Older configs: the pinned panels on the left of the canvas
const QString leftKey = QStringLiteral("StudioLayoutLeftPanels");
/// How close to the canvas edge a dropped panel is pinned there
const int edgeDropZone = 140;
/// "id=height" of pinned panels
const QString heightsKey = QStringLiteral("StudioLayoutPanelHeights");
const QString foldedKey = QStringLiteral("StudioLayoutFoldedPanels");
const QString columnWidthKey = QStringLiteral("StudioLayoutPanelWidth");
/// "left" or "right"
const QString panelSideKey = QStringLiteral("StudioLayoutPanelSide");
const QString toolRailSideKey = QStringLiteral("StudioLayoutToolRailSide");
const QString panelsHiddenKey = QStringLiteral("StudioLayoutPanelsTucked");
const QString toolRailHiddenKey = QStringLiteral("StudioLayoutToolRailTucked");
const QString topBarHiddenKey = QStringLiteral("StudioLayoutTopBarTucked");
const QString classicStateKey = QStringLiteral("StudioLayoutClassicState");

/// The panels on the rail by default; the tool options open from the tool chip instead
QStringList defaultRailPanels()
{
    return {
        QStringLiteral("KisLayerBox"),
        QStringLiteral("PresetDocker"),
        QStringLiteral("StudioColor"),
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

/// A panel's title bar: dragged it moves the panel, clicked it folds it, right clicked it shows its card
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

    /// Folded, the panel is only this grip, with its name
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
        // a finger needs more to hold on to
        const bool touch = KisTouchMode::isActive();
        setFixedHeight(m_folded ? (touch ? 32 : 22) : (touch ? 22 : 10));
        auto *dock = qobject_cast<QDockWidget *>(parentWidget());
        if (m_folded && dock) dock->setMaximumHeight(height());
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        QColor ink = palette().color(QPalette::WindowText);
        if (m_folded) {
            ink.setAlphaF(underMouse() ? 0.85 : 0.6);
            QFont font = this->font();
            font.setPointSizeF(font.pointSizeF() * 0.9);
            painter.setFont(font);
            painter.setPen(ink);
            painter.drawText(rect().adjusted(10, 0, -10, 0), Qt::AlignLeft | Qt::AlignVCenter,
                             QFontMetrics(font).elidedText(m_title, Qt::ElideRight, width() - 20));
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

/// Where a dragged panel would go: into the column, or (column false) onto the canvas
struct PanelDrop {
    bool column {false};
    bool left {false};
    QString before;      ///< the panel it goes above; empty for the bottom
    QRect indicator;     ///< in main window coordinates
};

/// The active tool's name and icon at the start of the top bar; a click opens its options
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
        // painted without the mnemonic the accelerator manager adds
        const QString label = QString(text()).remove(QLatin1Char('&'));
        return QSize(padding + icon + gap + fontMetrics().horizontalAdvance(label) + padding,
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
        painter.drawText(area, Qt::AlignVCenter | Qt::AlignLeft | Qt::TextHideMnemonic, text());
    }

private:
    static constexpr int padding = 8;
    static constexpr int gap = 6;
};

/// The foreground color over the background one: a click opens the color panel, on the background it swaps them
class ColorChip : public QWidget
{
public:
    std::function<void()> openColors;
    std::function<void()> swapColors;

    explicit ColorChip(QWidget *parent) : QWidget(parent)
    {
        setFixedSize(40, 40);
        setCursor(Qt::PointingHandCursor);
        setToolTip(i18nc("@info:tooltip", "Colors (X swaps)"));
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

/// Properties of the canvas area: how far the pinned panels and the top pills cover its edges
const char *const leftInsetProperty = "studioCanvasLeftInset";
const char *const rightInsetProperty = "studioCanvasRightInset";
const char *const topInsetProperty = "studioCanvasTopInset";
/// On a widget whose empty space moves the window, as a title bar
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

/// \p widget as glass with a rim of \p rim opacity
void paintGlass(QPainter &painter, QWidget *widget, qreal radius, qreal rim)
{
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QPainterPath shape = KisStudioGlass::paint(painter, widget, QRectF(widget->rect()).adjusted(0.5, 0.5, -0.5, -0.5), radius);
    QColor border = widget->palette().color(QPalette::WindowText);
    border.setAlphaF(rim);
    KisStudioGlass::paintBorder(painter, widget, shape, border);
}

/// Six dots a pill or rail is dragged by; the mouse events go to its parent
class DragGrip : public QWidget
{
public:
    explicit DragGrip(QWidget *parent)
        : QWidget(parent)
    {
        setCursor(Qt::OpenHandCursor);
        setFixedHeight(14);
        setToolTip(i18nc("@info:tooltip", "Drag to switch sides"));
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
        // three along the longer side, two across; each on a pixel's middle so that none blurs
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
 * The top bar's grip: a drag moves the window, a tap tucks the bar away, a
 * double click maximizes. Not a system caption, whose clicks Qt never sees.
 */
class TopGrip : public DragGrip
{
public:
    std::function<void()> tapped;

    using DragGrip::DragGrip;

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
        QWidget *top = window();
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
        // at once, without waiting for a second click
        if (rect().contains(event->pos()) && tapped) tapped();
    }

    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton) return;
        // undoes the first click's tap
        if (tapped) tapped();
        m_pressed = true;
        m_doubleClicked = true;
        QWidget *top = window();
        if (top->isFullScreen()) return;
        if (top->isMaximized()) top->showNormal();
        else top->showMaximized();
    }

private:
    QPoint m_pressPos;
    bool m_pressed {false};
    bool m_doubleClicked {false};
};

/// Between two pinned panels: invisible, the glass shows through
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

/// The tool rail over the canvas's side, as tall as its tools
class RailOverlay : public QWidget
{
public:
    /// Dragged by its grip and let go nearer the other side of the canvas
    std::function<void(bool left)> moved;
    std::function<void()> tapped;

    explicit RailOverlay(QWidget *canvasArea)
        : QWidget(canvasArea)
    {
        setObjectName(QStringLiteral("studioToolRail"));
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(KisModernStyle::space1, KisModernStyle::space1, KisModernStyle::space1, KisModernStyle::space2);
        layout->setSpacing(KisModernStyle::space2);
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

    /// Tucked, only its grip shows, as wide as the rail was
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
        m_grip->setToolTip(tucked ? i18nc("@info:tooltip", "Show tools")
                                  : i18nc("@info:tooltip", "Drag to switch sides, click to hide"));
        place();
    }

    void place()
    {
        if (m_moving) return;
        // beside the pinned panels' pill and under the top pills
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
        // by the grip or between the tools: the rail follows the pointer
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
        paintGlass(painter, this, 14, m_moving ? 0.3 : 0.12);
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
 * A glass pill over the top of the canvas, as long as its contents: the tool
 * and brush bars on the left, or the window's buttons on the right. Its empty
 * space moves the window, as a title bar would.
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
        layout->setSizeConstraint(QLayout::SetFixedSize);
        if (!right) {
            m_grip = new TopGrip(this);
            m_grip->setFixedSize(22, 30);
            layout->addWidget(m_grip);
        }
        // hidden when the pill is tucked away to its grip
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
        keepHeight();
    }

    QWidget *contents() const { return m_contents; }
    TopGrip *grip() const { return m_grip; }

    /// Tucked away to its grip, the bar is as tall as it is open: the window handle and what is below stay put
    void keepHeight()
    {
        if (m_grip) m_grip->setFixedHeight(qMax(30, m_contents->sizeHint().height()));
    }

    void setTucked(bool tucked)
    {
        m_contents->setVisible(!tucked);
        keepHeight();
        if (m_grip) {
            m_grip->setToolTip(tucked ? i18nc("@info:tooltip", "Show bar")
                                      : i18nc("@info:tooltip", "Drag to move, double-click to maximize"));
        }
    }

    void place()
    {
        QWidget *area = parentWidget();
        move(m_right ? area->width() - margin - width() : margin + m_leftInset, margin);
        raise();
    }

    /// Room on its left for the window handle
    void setLeftInset(int inset)
    {
        if (inset == m_leftInset) return;
        m_leftInset = inset;
        place();
    }

    /// Resized, shown or hidden: what is placed under it moves
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
        paintGlass(painter, this, 12, 0.12);
    }

private:
    bool m_right {false};
    int m_leftInset {0};
    TopGrip *m_grip {nullptr};
    QWidget *m_contents {nullptr};
};

/// A system caption in the top left corner, with the application's icon: the tool rail's cap
class WindowHandle : public QWidget
{
public:
    explicit WindowHandle(QWidget *canvasArea)
        : QWidget(canvasArea)
    {
        setObjectName(QStringLiteral("studioWindowHandle"));
        setProperty(captionProperty, true);
        hide();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        paintGlass(painter, this, 12, 0.12);
        const int side = qMin(20, qMin(width(), height()) - 12);
        painter.setOpacity(0.85);
        QApplication::windowIcon().paint(&painter, QRect(QPoint(), QSize(side, side)).translated(rect().center() - QPoint(side / 2, side / 2)));
    }
};

/// A button of the window pill: the menu, Settings, Find
class PillButton : public QAbstractButton
{
public:
    /// A right click or a long press
    std::function<void()> contextClicked;

    PillButton(const QString &iconName, const QString &toolTip, QWidget *parent)
        : QAbstractButton(parent)
        , m_iconName(iconName)
    {
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::NoFocus);
        setAttribute(Qt::WA_Hover);
        setFixedSize(34, 30);
        setToolTip(toolTip);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        if (underMouse()) {
            QColor background = palette().color(QPalette::ButtonText);
            background.setAlphaF(0.09);
            painter.setPen(Qt::NoPen);
            painter.setBrush(background);
            painter.drawPath(KisModernStyle::squircle(QRectF(rect()).adjusted(1, 1, -1, -1), KisModernStyle::tileRadius));
        }
        const int iconSize = 18;
        KisIconUtils::loadIcon(m_iconName).paint(&painter, QRect((width() - iconSize) / 2, (height() - iconSize) / 2, iconSize, iconSize));
    }

    void contextMenuEvent(QContextMenuEvent *event) override
    {
        if (!contextClicked) {
            QAbstractButton::contextMenuEvent(event);
            return;
        }
        event->accept();
        contextClicked();
    }

private:
    QString m_iconName;
};

} // namespace

/**
 * The pinned panels in a glass pill over an edge of the canvas, under the
 * strip of panel buttons. Dragged by the strip it switches sides; its inner
 * edge sets its width.
 */
class KisStudioPanelColumn : public QWidget
{
public:
    /// From the canvas area's edges
    static constexpr int margin = 6;
    /// The dragged inner edge
    static constexpr int edge = 5;
    static constexpr int minimumColumnWidth = 220;

    /// Its geometry changed; \p done once a width drag is let go
    std::function<void(bool done)> resized;
    /// Dragged by its strip and let go nearer the other side of the canvas
    std::function<void(bool left)> moved;
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
        // a widget of its own: a cursor set on the column would be the panels' too
        m_grip = new QWidget(this);
        m_grip->setObjectName(QStringLiteral("studioPanelsEdge"));
        m_grip->setCursor(Qt::SizeHorCursor);
        m_grip->setAttribute(Qt::WA_NoSystemBackground);
        m_grip->installEventFilter(this);
        canvasArea->installEventFilter(this);
        hide();
    }

    QSplitter *splitter() const { return m_splitter; }
    int columnWidth() const { return m_width; }
    bool isLeft() const { return m_left; }

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
        placeGrip();
    }

    /// The panels under the strip, or only the strip
    void setPanelsShown(bool shown)
    {
        m_splitter->setVisible(shown);
        place();
        placeGrip();
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

    /// Takes \p docks in this order, each keeping its visibility
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
        if (watched == m_grip) {
            switch (event->type()) {
            case QEvent::MouseButtonPress:
                if (static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton) {
                    m_resizing = true;
                    update();
                    return true;
                }
                break;
            case QEvent::MouseMove:
                if (m_resizing) {
                    const QWidget *area = parentWidget();
                    const int x = area->mapFromGlobal(globalMousePos(static_cast<QMouseEvent *>(event))).x();
                    m_width = qBound(minimumColumnWidth, m_left ? x - margin : area->width() - margin - x,
                                     qMax(minimumColumnWidth, area->width() / 2));
                    place();
                    return true;
                }
                break;
            case QEvent::MouseButtonRelease:
                if (m_resizing && static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton) {
                    m_resizing = false;
                    if (resized) resized(true);
                    update();
                    return true;
                }
                break;
            default:
                break;
            }
        }
        return QWidget::eventFilter(watched, event);
    }

    void showEvent(QShowEvent *event) override
    {
        QWidget::showEvent(event);
        place();
        placeGrip();
    }

    void resizeEvent(QResizeEvent *event) override
    {
        QWidget::resizeEvent(event);
        placeGrip();
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        paintGlass(painter, this, 12, m_resizing || m_moving ? 0.3 : 0.12);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        // by the strip, the pill follows the pointer
        if (event->button() != Qt::LeftButton || !m_strip || event->pos().y() > m_strip->geometry().bottom()) {
            QWidget::mousePressEvent(event);
            return;
        }
        m_moving = true;
        m_grab = event->pos().x();
        m_pressPos = globalMousePos(event);
        setCursor(Qt::ClosedHandCursor);
        update();
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (!m_moving) {
            QWidget::mouseMoveEvent(event);
            return;
        }
        const QWidget *area = parentWidget();
        const int x = area->mapFromGlobal(globalMousePos(event)).x();
        move(qBound(margin, x - m_grab, qMax(margin, area->width() - margin - width())), y());
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton || !m_moving) {
            QWidget::mouseReleaseEvent(event);
            return;
        }
        m_moving = false;
        unsetCursor();
        update();
        if ((globalMousePos(event) - m_pressPos).manhattanLength() < QApplication::startDragDistance()) {
            place();
            if (tapped) tapped();
            return;
        }
        const bool left = x() + width() / 2 < parentWidget()->width() / 2;
        if (left != m_left && moved) moved(left);
        else place();
    }

private:
    /// Over the inner edge, only when the panels are shown
    void placeGrip()
    {
        m_grip->setGeometry(m_left ? width() - edge : 0, 0, edge, height());
        m_grip->setVisible(panelsShown());
        m_grip->raise();
    }

    void updateMargins()
    {
        m_layout->setContentsMargins(m_left ? 1 : edge, 1, m_left ? edge : 1, 1);
    }

    QVBoxLayout *m_layout {nullptr};
    QSplitter *m_splitter {nullptr};
    QPointer<QWidget> m_strip;
    QWidget *m_grip {nullptr};
    bool m_left {false};
    int m_width {0};
    bool m_resizing {false};
    bool m_moving {false};
    int m_grab {0};
    QPoint m_pressPos;
};

/**
 * An unpinned panel opened over the canvas, in glass. A child of the main
 * window, over the pills: a window of its own could not show QML panels, and
 * a floating dock would be saved with the window state (restoring one crashes).
 */
class FlyoutWindow : public QWidget
{
public:
    explicit FlyoutWindow(QWidget *mainWindow)
        : QWidget(mainWindow)
    {
        setObjectName(QStringLiteral("studioFlyout"));
        setAttribute(Qt::WA_TranslucentBackground);
        KisStudioGlass::setOverOverlays(this);
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(1, 1, 1, 1);
        layout->setSpacing(0);
    }

    void hold(QDockWidget *dock)
    {
        m_panel = dock;
        KisStudioGlass::setSeeThrough(dock, true);
        layout()->addWidget(dock);
        dock->show();
    }

    /// Lets go of the panel, which stays a child of this window until docked elsewhere
    void release()
    {
        if (m_panel) {
            layout()->removeWidget(m_panel);
            KisStudioGlass::setSeeThrough(m_panel, false);
        }
        m_panel = nullptr;
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        paintGlass(painter, this, 12, 0.14);
    }

private:
    QPointer<QDockWidget> m_panel;
};

struct KisStudioLayout::Private
{
    KisMainWindow *mainWindow {nullptr};
    bool active {false};

    /// The strip of panel buttons heading the pinned panels
    QPointer<QToolBar> rail;
    QPointer<QToolButton> addPanelButton;
    /// The tool rail, with the color chip under its tools
    QPointer<QWidget> brushRail;
    QPointer<KisStudioToolRail> toolRail;
    QPointer<QWidget> windowHandle;
    /// The tool chip, its options, size and opacity
    QPointer<QToolBar> topBar;
    QPointer<QWidget> findField;
    QPointer<QAction> findFieldAction;
    QPointer<QWidget> menuButton;
    QPointer<QAction> menuButtonAction;
    QPointer<QWidget> settingsButton;
    QPointer<QAction> settingsButtonAction;
    QPointer<KisStudioSettings> settings;
    QPointer<QMenu> mainMenu;
    /// The brush bar whose menu buttons were made to open on a long press, and those buttons
    QPointer<QToolBar> placedOnBar;
    QList<QPointer<QToolButton>> delayedPopups;
    /// Toolbar entries that duplicate the top bar's scrubbers
    QList<QPointer<QAction>> hiddenToolbarActions;
    /// Toolbar entries shown only for the tools they matter to
    QList<QPointer<QAction>> contextActions;
    /// Toolbars that only duplicate the main menu or shortcuts
    QList<QPointer<QToolBar>> hiddenToolBars;
    /// The side and order the pinned panels were last arranged in
    QString arrangement;
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
    /// Where a dragged panel would go beside the canvas
    QPointer<QFrame> panelIndicator;

    QSet<QString> pinned;
    QHash<QString, int> heights;
    QSet<QString> folded;
    QHash<QString, QPointer<PanelGrip>> grips;
    QPointer<KisStudioPanelColumn> panelColumn;
    QPointer<TopPill> topPill;
    QPointer<TopPill> windowPill;
    /// Canvas only: what the pointer brought back by nearing its edge (see peek())
    QList<QPointer<QWidget>> peeking;
    QPointer<QToolBar> windowBar;
    bool panelsLeft {false};
    bool toolRailLeft {true};
    bool panelsTucked {false};
    bool toolRailTucked {false};
    bool topBarTucked {false};
    QPointer<QWidget> stripGrip;
    QTimer *heightSave {nullptr};
    /// While the panels are arranged or folded: their resizing is not the user's
    bool settlingHeights {false};

    /// The unpinned panel open over the canvas, and what it opened from (it goes below it)
    QPointer<QDockWidget> flyout;
    QPointer<FlyoutWindow> flyoutWindow;
    QPointer<QWidget> flyoutAnchor;
    QPointer<QToolButton> toolChip;
    /// The flyout is to be fitted to its contents again
    bool flyoutRefit {false};
    QPointer<QAction> toolChipAction;
    QPointer<KisStudioToolStrip> toolStrip;
    QPointer<QAction> toolStripAction;
    QPointer<QAction> brushValuesAction;
    /// The brush bar's preset button: a click opens the brush picker, the wheel steps through your brushes
    QPointer<QWidget> presetChip;
    QString presetChipToolTip;
    QPointer<KisStudioBrushMemory> brushMemory;
    QPointer<KisStudioCanvasRing> canvasRing;
    QPointer<QToolButton> documentButton;
    QPointer<QAction> documentButtonAction;
    QPointer<KisStudioCanvasHud> hud;
    QPointer<KisStudioEffectOverlay> effectOverlay;
    KisStudioWindowFrame *frame {nullptr};
    QPointer<QAction> windowButtonsAction;

    /// A docker's title bar and features before the studio layout
    struct ClassicDock {
        QPointer<QWidget> titleBar;
        QDockWidget::DockWidgetFeatures features;
    };
    QHash<QDockWidget *, ClassicDock> classicDocks;
    /// Every docker but the toolbox
    QSet<QDockWidget *> panels;
    QVector<QMetaObject::Connection> connections;

    /// The pinned panels in stacking order: as on the rail, then the others
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

    /// The pinned panels shown in the column, top to bottom, but \p without
    QList<QDockWidget *> column(const QString &without) const
    {
        QList<QDockWidget *> docks;
        Q_FOREACH (const QString &id, pinnedOrder()) {
            QDockWidget *dock = mainWindow->dockWidget(id);
            if (id != without && dock && dock != flyout && !dock->isHidden() && !dock->isFloating()) {
                docks << dock;
            }
        }
        std::sort(docks.begin(), docks.end(), [this](QDockWidget *a, QDockWidget *b) { return placeOf(a).y() < placeOf(b).y(); });
        return docks;
    }

    /// In main window coordinates (a panel in the column is not the window's child)
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
        const bool left = panelsLeft;
        const QList<QDockWidget *> docks = column(moving);
        QRect area;
        Q_FOREACH (QDockWidget *dock, docks) {
            area |= placeOf(dock);
        }
        // over the column, or near the canvas edge when it has no panels
        const bool overColumn = !area.isEmpty() && pos.x() >= area.left() - 8 && pos.x() <= area.right() + 8;
        const bool nearEdge = area.isEmpty() && canvas.contains(pos)
                              && (left ? pos.x() < canvas.left() + edgeDropZone : pos.x() > canvas.right() - edgeDropZone);
        if (!overColumn && !nearEdge) return drop;

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

    /// Whether \p global is on the strip of panel buttons, or near it
    bool overStrip(const QPoint &global) const
    {
        return rail && QRect(rail->mapToGlobal(QPoint(0, 0)), rail->size()).adjusted(0, -12, 0, 12).contains(global);
    }
};

KisStudioLayout::KisStudioLayout(KisMainWindow *mainWindow)
    : QObject(mainWindow)
    , d(new Private)
{
    d->mainWindow = mainWindow;
    // the settings as they were at the start: what changes later and needs a restart is pending
    KisSettingsRegistry::instance();

    // saved once a resize has settled
    d->heightSave = new QTimer(this);
    d->heightSave->setSingleShot(true);
    d->heightSave->setInterval(400);
    connect(d->heightSave, &QTimer::timeout, this, &KisStudioLayout::saveHeights);

    // canvas-only mode restores a window state partly through the event queue
    if (QAction *canvasOnly = mainWindow->actionCollection()->action(QStringLiteral("view_show_canvas_only"))) {
        connect(canvasOnly, &QAction::toggled, this, [this]() {
            closeFlyout();
            d->peeking.clear();
            QTimer::singleShot(0, this, &KisStudioLayout::normalize);
        });
    }

    connect(mainWindow, &KisMainWindow::themeChanged, this, [this]() {
        if (!d->active) return;
        rebuildRailButtons();
        if (d->addPanelButton) d->addPanelButton->setIcon(KisIconUtils::loadIcon(QStringLiteral("list-add")));
        updateContextBar();
    });
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
    // the classic layout to come back to
    if (active && !isEnabledInConfig()) {
        cfg.writeEntry(classicStateKey, d->mainWindow->saveState());
    }
    cfg.writeEntry(activeKey, active);
    d->active = active;
    if (active) {
        enter();
    } else {
        leave();
    }
}

void KisStudioLayout::enter()
{
    KisMainWindow *mainWindow = d->mainWindow;
    // the pills and panels live in the main window's canvas area
    mainWindow->setCanvasDetached(false);

    KisConfig cfg(true);
    auto readSet = [&cfg](const QString &key) {
        const QStringList list = cfg.readEntry<QStringList>(key);
        return QSet<QString>(list.begin(), list.end());
    };
    d->pinned = readSet(pinnedKey);
    d->folded = readSet(foldedKey);
    const QString panelSide = cfg.readEntry<QString>(panelSideKey, QString());
    d->panelsLeft = panelSide == QLatin1String("left")
                    || (panelSide.isEmpty() && !d->pinned.isEmpty() && readSet(leftKey).contains(d->pinned));
    d->toolRailLeft = cfg.readEntry<QString>(toolRailSideKey, QStringLiteral("left")) != QLatin1String("right");
    d->panelsTucked = cfg.readEntry<bool>(panelsHiddenKey, false);
    d->toolRailTucked = cfg.readEntry<bool>(toolRailHiddenKey, false);
    d->topBarTucked = cfg.readEntry<bool>(topBarHiddenKey, false);
    d->arrangement.clear();
    d->heights.clear();
    Q_FOREACH (const QString &entry, cfg.readEntry<QStringList>(heightsKey)) {
        const int separator = entry.lastIndexOf(QLatin1Char('='));
        if (separator > 0) {
            d->heights.insert(entry.left(separator), entry.mid(separator + 1).toInt());
        }
    }

    Q_FOREACH (QDockWidget *dock, mainWindow->dockWidgets()) {
        QWidget *titleBar = dock->titleBarWidget();
        // e.g. the animation timeline keeps its controls in the title bar
        if (titleBar && titleBar->inherits("KisUtilityTitleBar")) {
            continue;
        }

        d->classicDocks.insert(dock, {titleBar, dock->features()});

        if (dock->objectName() == toolBoxId) {
            dock->setTitleBarWidget(new QWidget(dock));
            dock->setFeatures(QDockWidget::NoDockWidgetFeatures);
            continue;
        }

        auto *grip = new PanelGrip(dock);
        const QString id = dock->objectName();
        grip->dragged = [this, id](const QPoint &pos) { d->showPanelDrop(pos, id); };
        // the panel may be rearranged, and its grip with it: after the release is delivered
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
        // for the panel picker, taken as it goes off screen
        d->connections << connect(dock, &QDockWidget::visibilityChanged, this, [dock](bool visible) {
            if (!visible) KisStudioPanelPicker::rememberLook(dock);
        });
        dock->setFeatures(QDockWidget::DockWidgetClosable);
        d->panels.insert(dock);

        // a name, so that a shortcut given on its card is saved, and on the window, so that it works
        QAction *toggle = dock->toggleViewAction();
        if (toggle->objectName().isEmpty()) {
            toggle->setObjectName(QStringLiteral("studio_panel_") + dock->objectName());
            KisStudioShortcuts::restore(toggle);
            mainWindow->addAction(toggle);
        }

        d->connections << connect(toggle, &QAction::toggled, this, [this, dock](bool visible) {
            dockToggled(dock, visible);
        });
    }

    createRail();
    createBrushRail();
    createTopBar();

    // the effects and brush pickers, from Find, their shortcuts or the top bar; they open where the pointer is
    auto addPicker = [mainWindow](const QString &name, const QString &icon, const QString &text, std::function<void()> open) {
        if (mainWindow->actionCollection()->action(name)) return;
        QAction *action = new QAction(KisIconUtils::loadIcon(icon), text, mainWindow);
        action->setObjectName(name);
        KisStudioShortcuts::restore(action);
        mainWindow->actionCollection()->addAction(name, action);
        mainWindow->addAction(action);
        connect(action, &QAction::triggered, mainWindow, open);
    };
    addPicker(QStringLiteral("studio_effects"), QStringLiteral("studio_effects"), i18nc("@action", "Effects"), [mainWindow]() {
        (new KisStudioEffectsPicker(mainWindow))->popupNear(QCursor::pos());
    });
    addPicker(QStringLiteral("studio_brush_picker"), QStringLiteral("paintop_settings_01"), i18nc("@action", "Brushes"), [mainWindow]() {
        (new KisStudioBrushPicker(mainWindow))->popupNear(QCursor::pos());
    });
    if (!d->brushMemory) {
        d->brushMemory = new KisStudioBrushMemory(mainWindow);
    }
    if (!d->canvasRing) {
        d->canvasRing = new KisStudioCanvasRing(mainWindow);
    }
    // read ahead (a tenth of a second) of the first right press or brush chip scroll
    QTimer::singleShot(3000, this, []() { KisStudioBrushPicker::yourBrushes(); });
    // the buttons' shortcuts work before Find is first opened
    QTimer::singleShot(0, this, [this]() {
        if (KisStudioFind *find = d->mainWindow->studioFind()) find->registerButtons();
    });
    KisViewManager *viewManager = mainWindow->viewManager();
    if (KisPaintopBox *paintopBox = viewManager ? viewManager->paintOpBox() : nullptr) {
        d->presetChip = paintopBox->presetChip();
        if (d->presetChip) {
            d->presetChipToolTip = d->presetChip->toolTip();
            d->presetChip->setToolTip(i18nc("@info:tooltip", "Brushes (right-click to manage)"));
        }
    }

    // the top bar's scrubbers are the size and opacity sliders
    for (int i = 1; i <= 5; i++) {
        const QString slider = cfg.toolbarSlider(i);
        QAction *action = mainWindow->actionCollection()->action(QStringLiteral("brushslider%1").arg(i));
        if (action && action->isVisible() && (slider == QLatin1String("size") || slider == QLatin1String("opacity"))) {
            action->setVisible(false);
            d->hiddenToolbarActions << action;
        }
    }

    qApp->installEventFilter(this);
    KisStudioEmptyStates::install();
    if (KisCanvasResourceProvider *resources = viewManager ? viewManager->canvasResourceProvider() : nullptr) {
        d->connections << connect(resources, &KisCanvasResourceProvider::sigPaintOpPresetChanged, this, [this]() { updateToolChip(); });
    }
    d->connections << connect(KoToolManager::instance(), &KoToolManager::changedTool, this,
                              [this](KoCanvasController *controller) {
                                  if (KisStudioToolRail::isOwnController(d->mainWindow, controller)) updateContextBar();
                              });
    d->connections << connect(KoToolManager::instance(), &KoToolManager::toolOptionWidgetsChanged, this,
                              [this](KoCanvasController *controller, const QList<QPointer<QWidget>> &widgets) {
                                  // another window's tool is not this top bar's business
                                  if (!KisStudioToolRail::isOwnController(d->mainWindow, controller) || !d->toolStrip) return;
                                  d->toolStrip->setOptionWidgets(widgets);
                                  if (d->toolStripAction) {
                                      d->toolStripAction->setVisible(d->toolStrip->hasOptions() && (!isRailHidden() || d->peeking.contains(QPointer<QWidget>(d->topPill.data()))));
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

    if (!d->frame) {
        d->frame = new KisStudioWindowFrame(mainWindow, [this](const QPoint &pos) { return isCaptionAt(pos); });
    }
    d->frame->setEnabled(cfg.readEntry<bool>("StudioCustomTitleBar", true));

    // effects are adjusted on the canvas, not in a dialog
    if (!d->effectOverlay) {
        d->effectOverlay = new KisStudioEffectOverlay(mainWindow);
    }
    if (viewManager) {
        QPointer<KisStudioEffectOverlay> overlay = d->effectOverlay;
        viewManager->filterManager()->setInlineEditor({
            [overlay](KisFilterSP filter, KisFilterConfigurationSP config) { return overlay && overlay->open(filter, config); },
            [overlay]() { if (overlay) overlay->accept(); },
            [overlay]() { if (overlay) overlay->reject(); },
        });
    }

    // the current tool's options for the strip, without waiting for a tool change
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
    // in a pill: its grip and empty space move the window, its controls are for clicking
    for (QWidget *w = widget; w; w = w->parentWidget()) {
        if (w->property(captionProperty).toBool()) {
            if (dynamic_cast<TopGrip *>(widget)) return false;
            return widget == w || dynamic_cast<DragGrip *>(widget) || qobject_cast<QToolBar *>(widget)
                || QByteArray(widget->metaObject()->className()) == "QWidget";
        }
    }
    if (!widget) {
        // between and around the toolbars in the window's top row
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
    // each document fills the canvas area without tabs or a frame; the document button switches between them
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
    // one document needs no switcher
    if (d->documentButtonAction) {
        d->documentButtonAction->setVisible(windows.size() > 1 && !isRailHidden());
    }
}

void KisStudioLayout::leave()
{
    KisMainWindow *mainWindow = d->mainWindow;
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
    if (auto *welcome = mainWindow->findChild<KisWelcomePageWidget *>()) {
        welcome->setContentsMargins(QMargins());
    }
    mainWindow->setProperty("studioLayoutActive", false);
    if (d->frame) {
        d->frame->setEnabled(false);
    }
    if (d->effectOverlay && d->effectOverlay->isVisible()) {
        d->effectOverlay->reject();
    }
    if (KisViewManager *viewManager = mainWindow->viewManager()) {
        viewManager->filterManager()->setInlineEditor({});
        viewManager->showHideScrollbars();
    }
    releaseDocuments();
    mainWindow->statusBar()->setVisible(KisConfig(true).showStatusBar());
    for (QAction *action : {d->windowButtonsAction.data(), d->toolStripAction.data(), d->documentButtonAction.data(),
                            d->menuButtonAction.data(), d->settingsButtonAction.data(), d->findFieldAction.data()}) {
        if (action) action->setVisible(false);
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
    if (!isCanvasOnly()) {
        mainWindow->menuBar()->show();
    }
    const QList<QPointer<QAction>> hiddenActions = d->hiddenToolbarActions + d->contextActions;
    for (QAction *action : hiddenActions) {
        if (action) action->setVisible(true);
    }
    d->hiddenToolbarActions.clear();
    d->contextActions.clear();
    Q_FOREACH (QToolBar *toolBar, d->hiddenToolBars) {
        if (toolBar) toolBar->show();
    }
    d->hiddenToolBars.clear();

    KisConfig cfg(true);
    const QByteArray classicState = cfg.readEntry<QByteArray>(classicStateKey);
    if (!classicState.isEmpty()) {
        mainWindow->restoreWorkspaceState(classicState);
        return;
    }
    // never in the classic layout yet: what a fresh Krita shows
    const QString workspaceName = cfg.readEntry<QString>(QStringLiteral("CurrentWorkspace"), QStringLiteral("Default"));
    KoResourceServer<KisWorkspaceResource> *server = KisResourceServerProvider::instance()->workspaceServer();
    KisWorkspaceResourceSP workspace = server->resource(QString(), QString(), workspaceName);
    if (!workspace) {
        workspace = server->resource(QString(), QString(), QStringLiteral("Default"));
    }
    if (workspace) {
        mainWindow->restoreWorkspace(workspace);
    }
}

void KisStudioLayout::normalize()
{
    if (!d->active) {
        return;
    }

    KisMainWindow *mainWindow = d->mainWindow;
    const bool panelsHidden = arePanelsHidden();
    const bool railHidden = isRailHidden();
    const bool canvasOnly = isCanvasOnly();

    // the pinned panels decide what is shown, not a restored classic workspace or older window state
    Q_FOREACH (QDockWidget *dock, d->panels) {
        if (dock == d->flyout) {
            continue;
        }
        // never left floating: restoring a floating panel crashes
        if (dock->isFloating()) {
            dock->hide();
            dockAway(dock);
        }
        if (!d->pinned.contains(dock->objectName())) {
            dock->hide();
        } else if (!panelsHidden) {
            dock->show();
        }
    }
    // in the rail's order, whatever order a restored state had
    d->arrangement.clear();
    arrangePinned();
    updateColumns();

    // the tool rail replaces the toolbox
    if (QDockWidget *toolBox = mainWindow->dockWidget(toolBoxId)) {
        toolBox->hide();
    }

    if (d->rail) {
        d->rail->setVisible(!railHidden);
    }
    if (d->brushRail) {
        d->brushRail->setVisible(!railHidden);
    }
    adoptTopBars();
    // the rail's commands are known once the GUI is built, after the rail
    if (d->toolRail) {
        d->toolRail->refresh();
    }
    placeWindowButtons();
    // on the start page too: the window pill is its title bar
    for (QAction *action : {d->menuButtonAction.data(), d->settingsButtonAction.data(), d->findFieldAction.data()}) {
        if (action) action->setVisible(!canvasOnly);
    }
    if (d->windowButtonsAction) {
        d->windowButtonsAction->setVisible(d->frame && d->frame->isEnabled() && !canvasOnly);
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
    // the menu button has the menus; canvas-only mode manages the menu bar itself
    if (!canvasOnly) {
        mainWindow->menuBar()->hide();
    }
    // the canvas shows what the status bar said about the view
    mainWindow->statusBar()->hide();
    adoptDocuments();
    updateDocumentButton();
    if (d->toolStripAction && d->toolStrip) {
        d->toolStripAction->setVisible(d->toolStrip->hasOptions() && !railHidden);
    }

    syncRail();
}

void KisStudioLayout::dockToggled(QDockWidget *dock, bool visible)
{
    syncRail();
    updateColumns();

    if (!visible && dock == d->flyout) {
        // hidden by its shortcut, its card or a workspace: no empty flyout left behind
        QTimer::singleShot(0, this, [this, dock]() {
            if (dock == d->flyout && dock->isHidden()) closeFlyout();
        });
    } else if (visible && dock != d->flyout && !d->pinned.contains(dock->objectName())) {
        // shown by the Dockers menu, a shortcut or a workspace: a flyout, unless normalize() hid it again
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
    if (!dock) {
        return;
    }
    releaseFlyout();
    parkPanel(dock);
    syncRail();
}

void KisStudioLayout::releaseFlyout()
{
    d->flyout = nullptr;
    d->flyoutAnchor = nullptr;
    if (d->flyoutWindow) {
        d->flyoutWindow->release();
        d->flyoutWindow->hide();
    }
}

void KisStudioLayout::dockAway(QDockWidget *dock)
{
    dock->setFloating(false);
    // Qt cannot dock back a panel that never was in a dock area: it is put in one, so that it is not saved floating
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
        if (dock->isHidden()) dock->show();
        positionFlyout();
        d->flyoutWindow->raise();
        return;
    }

    d->flyout = dock;
    d->flyoutAnchor = anchor;
    if (!d->flyoutWindow) {
        d->flyoutWindow = new FlyoutWindow(d->mainWindow);
    }
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

    const QRect area = QRect(canvasArea->mapToGlobal(QPoint(0, 0)), canvasArea->size()).marginsRemoved(canvasInsets(canvasArea));
    const int margin = 6;
    int contentsWidth = d->flyout->widget() ? d->flyout->widget()->sizeHint().width() : 0;
    int contentsHeight = d->flyout->sizeHint().height();
    // a scroll area asks for little: the flyout fits what it scrolls
    QScrollArea *scroll = d->flyout->findChild<QScrollArea *>();
    if (scroll && scroll->widget()) {
        const int frame = 2 * scroll->frameWidth();
        const int header = d->flyout->titleBarWidget() ? d->flyout->titleBarWidget()->sizeHint().height() : 0;
        contentsWidth = qMax(contentsWidth, scroll->widget()->sizeHint().width() + frame + scroll->verticalScrollBar()->sizeHint().width());
        contentsHeight = qMax(contentsHeight, header + scroll->widget()->sizeHint().height() + frame + 12);
    }
    // within the canvas area: past it, the main window cuts it off
    const int width = qMax(0, qMin(qBound(260, contentsWidth + 2, 420), area.width() - 2 * margin));

    QRect global(area.right() + 1 - margin - width, area.top() + margin, width, area.height() - 2 * margin);
    if (d->flyoutAnchor) {
        // below what opened it, as tall as its contents, beside the tool rail rather than over it
        int left = qMax(area.left() + margin, d->flyoutAnchor->mapToGlobal(QPoint(0, 0)).x());
        if (QWidget *rail = d->brushRail; rail && rail->isVisible()) {
            const QRect at(rail->mapToGlobal(QPoint()), rail->size());
            if (at.left() < left + width && at.right() >= left) {
                left = at.center().x() < area.center().x() ? at.right() + 1 + margin : at.left() - margin - width;
            }
        }
        left = qBound(area.left() + margin, left, area.right() + 1 - margin - width);
        global = QRect(left, area.top() + margin, width, qBound(80, contentsHeight, area.height() - 2 * margin));
    }
    QWidget *parent = d->flyoutWindow->parentWidget();
    d->flyoutWindow->setGeometry(QRect(parent->mapFromGlobal(global.topLeft()), global.size()));
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
        togglePanel(dock, true);
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
    // in the rail's order; only redone when the order or the side changed
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
    Q_FOREACH (QDockWidget *dock, column->panels()) {
        if (!docks.contains(dock)) parkPanel(dock);
    }
    // out of the window's dock areas: the canvas goes on under the pill
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
        // the older key as a fallback
        const int width = cfg.readEntry<int>(columnWidthKey, cfg.readEntry<int>(QStringLiteral("StudioLayoutRightColumnWidth"), 270));
        auto *column = new KisStudioPanelColumn(area->parentWidget(), d->panelsLeft, width);
        column->resized = [this](bool done) {
            // the views are fitted again only once a width drag ends: per move, it took most of a frame
            updateCanvasInsets(done);
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
        d->stripGrip->setToolTip(tucked ? i18nc("@info:tooltip", "Show panels")
                                        : i18nc("@info:tooltip", "Drag to switch sides, click to hide"));
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
    KisStudioGlass::setSeeThrough(dock, false);
    dock->hide();
    d->mainWindow->addDockWidget(Qt::RightDockWidgetArea, dock);
    dock->hide();
}

void KisStudioLayout::placeWelcomePage()
{
    auto *welcome = d->mainWindow->findChild<KisWelcomePageWidget *>();
    QWidget *rail = d->brushRail;
    if (!welcome || !rail) return;
    QMargins room;
    if (rail->isVisible()) {
        const QRect at(rail->mapTo(d->mainWindow, QPoint()), rail->size());
        const QRect page(welcome->mapTo(d->mainWindow, QPoint()), welcome->size());
        if (at.center().x() < page.center().x()) room.setLeft(qMax(0, at.right() + 13 - page.left()));
        else room.setRight(qMax(0, page.right() + 13 - at.left()));
    }
    if (welcome->contentsMargins() != room) welcome->setContentsMargins(room);
}

void KisStudioLayout::updateColumns()
{
    KisStudioPanelColumn *column = d->panelColumn;
    if (column) {
        bool any = false;
        Q_FOREACH (QDockWidget *dock, column->panels()) {
            any = any || !dock->isHidden();
        }
        column->setPanelsShown(any && !arePanelsHidden() && !d->panelsTucked);
        column->setVisible(d->active && !isRailHidden());
    }
    updateCanvasInsets();
}

void KisStudioLayout::updateCanvasInsets(bool views)
{
    placeWindowHandle();
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
    // the tool rail goes beside the pill, even when it is only its strip
    const int pillLeft = shown && left ? reach : 0;
    const int pillRight = shown && !left ? reach : 0;
    int top = 0;
    for (TopPill *pill : {d->topPill.data(), d->windowPill.data()}) {
        // brought back over the canvas for a moment, it moves nothing
        if (pill && !pill->isHidden() && !d->peeking.contains(pill)) top = qMax(top, pill->geometry().bottom() + 1);
    }
    // with its panels, the pill covers the canvas's side: the zoom pill and the view keep clear of it
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

    // each view fits, zooms and rotates about the part of it the pills leave free
    if (!views) {
        return;
    }
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
    // sized once the column has laid the panels out
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
    // a flyout closes, as by its rail button
    if (!d->pinned.contains(id)) {
        if (dock == d->flyout) closeFlyout();
        return;
    }
    // a fold still running (a quick second click) is finished first
    const QString foldName = QStringLiteral("studioFold_") + id;
    if (auto *running = findChild<QVariantAnimation *>(foldName, Qt::FindDirectChildrenOnly)) {
        running->setObjectName(QString());
        if (running->state() == QAbstractAnimation::Running) running->setCurrentTime(running->duration());
    }
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
        // free to shrink, the name shown at once; the content is squeezed at the end
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
    animation->setObjectName(foldName);
    animation->setStartValue(from);
    animation->setEndValue(to);
    animation->setDuration(KisModernStyle::enterDuration + 40);
    animation->setEasingCurve(KisModernStyle::motionCurve());
    QPointer<QDockWidget> target = dock;
    connect(animation, &QVariantAnimation::valueChanged, this, [this, target, fold](const QVariant &value) {
        if (!target) return;
        target->setMaximumHeight(value.toInt());
        if (!fold && d->panelColumn) d->panelColumn->resizePanel(target, value.toInt());
    });
    connect(animation, &QVariantAnimation::finished, this, [this, target, fold, to]() {
        if (target) {
            if (fold) {
                QWidget *content = target->widget();
                content->setProperty("studioFolded", true);
                content->setMaximumHeight(0);
                target->setMaximumHeight(to);
            } else {
                target->setMaximumHeight(QWIDGETSIZE_MAX);
                if (d->panelColumn) d->panelColumn->resizePanel(target, to);
            }
        }
        QTimer::singleShot(0, this, [this]() {
            // still settling while another panel folds
            Q_FOREACH (QVariantAnimation *other, findChildren<QVariantAnimation *>(QString(), Qt::FindDirectChildrenOnly)) {
                if (other->objectName().startsWith(QLatin1String("studioFold_")) && other->state() == QAbstractAnimation::Running) return;
            }
            d->settlingHeights = false;
        });
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
    // squeezed to no height rather than hidden: a dock with hidden content would narrow the column
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
            takeOffRail(id);
        } else if (d->pinned.contains(id)) {
            setPinned(dock, false);
        }
        return;
    }

    d->pinned.insert(id);
    savePinned();
    if (dock == d->flyout) {
        releaseFlyout();
    }
    dockAway(dock);

    // the column stacks in the rail's order: it goes on the rail where it goes in the column
    QStringList panels = d->railPanels;
    panels.removeAll(id);
    int at = drop.before.isEmpty() ? -1 : panels.indexOf(drop.before);
    if (at < 0) {
        Q_FOREACH (QDockWidget *other, d->column(id)) {
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

    QToolBar *rail = new QToolBar(i18nc("@title:window", "Panels"), mainWindow);
    // not the name it had as a window toolbar: a restored window state would take it back
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

    // between the buttons and "+": the pill is dragged by it
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
    d->addPanelButton = addPanel;
    connect(addPanel, &QToolButton::clicked, this, [this, addPanel]() {
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
        // the rail's panels may have been starred, unstarred or reordered there
        connect(picker, &QObject::destroyed, this, [this]() {
            const QStringList panels = KisConfig(true).readEntry<QStringList>(panelsKey, defaultRailPanels());
            if (panels != d->railPanels) setRailPanels(panels);
        });
        picker->popupBelow(addPanel);
    });
    rail->addWidget(addPanel);

    // where a dragged rail button will go
    auto *indicator = new QFrame(rail);
    indicator->setFrameShape(QFrame::NoFrame);
    indicator->setAutoFillBackground(true);
    indicator->setBackgroundRole(QPalette::Highlight);
    indicator->hide();
    d->dropIndicator = indicator;

    d->railPanels = KisConfig(true).readEntry<QStringList>(panelsKey, defaultRailPanels());
    rebuildRailButtons();
    // hidden until normalize() shows it in the column
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
                // while tucked away, a button brings the panels back with its own among them
                togglePanel(dock, checked || d->panelsTucked);
                syncRail();
            }
        });
        rail->insertAction(d->railSpacer, action);
        if (QWidget *button = rail->widgetForAction(action)) button->setProperty(KisModernStyle::tabButtonProperty, true);
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

    // the title's first letter, for panels without an icon of their own
    const int size = 44;
    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QColor color = d->rail ? d->rail->palette().color(QPalette::ButtonText) : QColor(Qt::gray);
    painter.setPen(QPen(color, 3));
    painter.drawPath(KisModernStyle::squircle(QRectF(3, 3, size - 6, size - 6), KisModernStyle::cardRadius));
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

    // the strip itself: panels are added with "+", so resetting is all that is left
    auto *card = new KisStudioItemCard(d->mainWindow, KisIconUtils::loadIcon(QStringLiteral("studio_more")),
                                       i18nc("@title panels rail", "Panels"), nullptr);
    card->addCommand({KisIconUtils::loadIcon(QStringLiteral("view-refresh")), i18nc("@info:tooltip", "Reset panels"),
                      [this]() { setRailPanels(defaultRailPanels()); }});
    auto *anchor = new QWidget(d->rail);
    anchor->setGeometry(QRect(pos, QSize(1, 1)));
    card->popupBeside(anchor);
    anchor->deleteLater();
}

void KisStudioLayout::takeOffRail(const QString &id)
{
    // closed too: a panel stays open only where it can be found again
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
    QDockWidget *dock = d->mainWindow->dockWidget(id);
    if (!dock) {
        return;
    }
    // its shortcut, and taking it off the rail
    auto *card = new KisStudioItemCard(d->mainWindow, KisIconUtils::loadIcon(railIconName(id)),
                                       dock->windowTitle(), dock->toggleViewAction());
    card->addCommand({KisIconUtils::loadIcon(QStringLiteral("edit-delete")), i18nc("@info:tooltip", "Remove"),
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

        // off the strip: where the panel would go in the column, or the canvas
        const QPoint global = globalMousePos(mouseEvent);
        if (!d->overStrip(global)) {
            if (d->dropIndicator) d->dropIndicator->hide();
            d->showPanelDrop(global, drag.id);
            return true;
        }
        d->hidePanelDrop();

        // the gap between buttons closest to the pointer
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
        QPointer<QToolButton> button = drag.button;
        drag = Private::RailDrag();
        if (d->dropIndicator) {
            d->dropIndicator->hide();
        }
        if (!wasActive) {
            // a plain click: the button, still down, takes its release as one
            return false;
        }
        button->setDown(false);

        const QPoint release = globalMousePos(static_cast<QMouseEvent *>(event));
        d->hidePanelDrop();
        if (!d->overStrip(release)) {
            // pinned beside the canvas, or taken off the rail on it
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
            // the buttons are recreated: after the release is delivered
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
    KisViewManager *viewManager = mainWindow->viewManager();
    if (!viewManager || !viewManager->paintOpBox()) {
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
    layout->setContentsMargins(0, KisModernStyle::hairline, 0, 0);
    layout->setSpacing(0);

    KisCanvasResourceProvider *resources = viewManager->canvasResourceProvider();
    auto *chip = new ColorChip(contents);
    auto refresh = [chip, resources]() {
        chip->setColors(resources->fgColor().toQColor(), resources->bgColor().toQColor());
    };
    refresh();
    connect(resources, &KisCanvasResourceProvider::sigFGColorChanged, chip, refresh);
    connect(resources, &KisCanvasResourceProvider::sigBGColorChanged, chip, refresh);
    // the colors are only known once there is a canvas
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
    strip->setActions(mainWindow->actionCollection());
    d->toolStripAction = bar->addWidget(strip);
    d->toolStripAction->setVisible(false);
    d->toolStrip = strip;

    // size and opacity, only for the tools that paint with them
    if (KisPaintopBox *paintopBox = mainWindow->viewManager() ? mainWindow->viewManager()->paintOpBox() : nullptr) {
        auto *values = new QWidget(bar);
        auto *valuesLayout = new QHBoxLayout(values);
        valuesLayout->setContentsMargins(KisModernStyle::space2, 0, 0, 0);
        valuesLayout->setSpacing(KisModernStyle::hairline);
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
                                                       ? i18nc("@info:tooltip", "Brush size")
                                                       : i18nc("@info:tooltip", "Opacity"),
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
            // a right click: the value the brush came with, as it was picked
            scrubber->setDefaultValue(source->value());
            if (KisCanvasResourceProvider *resources = mainWindow->viewManager()->canvasResourceProvider()) {
                connect(resources, &KisCanvasResourceProvider::sigPaintOpPresetChanged, scrubber, [scrubber, source]() {
                    QTimer::singleShot(0, scrubber, [scrubber, source]() { scrubber->setDefaultValue(source->value()); });
                });
            }
        }
        d->brushValuesAction = bar->addWidget(values);
        d->brushValuesAction->setVisible(false);
    }

    // the document's name, and every open document a click away
    auto *documents = new QToolButton(mainWindow);
    documents->setObjectName(QStringLiteral("studio_documents"));
    documents->setAutoRaise(true);
    documents->setFocusPolicy(Qt::NoFocus);
    documents->setToolButtonStyle(Qt::ToolButtonTextOnly);
    documents->setPopupMode(QToolButton::InstantPopup);
    documents->setToolTip(i18nc("@info:tooltip", "Open documents"));
    documents->hide();
    documents->setMenu(new QMenu(documents));
    d->documentButton = documents;

    KisStudioGlass::setSeeThrough(bar, true);
    d->topBar = bar;

    // put on the window pill by placeWindowButtons()
    auto *menuButton = new PillButton(QStringLiteral("studio_menu"), i18nc("@info:tooltip", "Menu"), mainWindow);
    menuButton->hide();
    connect(menuButton, &QAbstractButton::clicked, this, [this, menuButton]() { showMainMenu(menuButton); });
    d->menuButton = menuButton;

    auto *settingsButton = new PillButton(QStringLiteral("studio_settings"), i18nc("@info:tooltip", "Settings · right click: Quick"), mainWindow);
    settingsButton->hide();
    settingsButton->setProperty("ownContextMenu", true);
    connect(settingsButton, &QAbstractButton::clicked, this, [this]() { openSettings(QString(), true); });
    settingsButton->contextClicked = [this, settingsButton]() {
        if (d->settings && d->active) d->settings->showQuick(settingsButton);
    };
    d->settingsButton = settingsButton;

    auto *field = new PillButton(QStringLiteral("studio_find"),
                                 i18nc("@info:tooltip", "Find anything"), mainWindow);
    field->setAccessibleName(i18nc("@info:placeholder", "Find anything"));
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
    if (!d->windowHandle) {
        d->windowHandle = new WindowHandle(area);
    }
    ensureSettings();
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
        d->windowBar = bar;
    }

    // the tool bar and the brush bar in the top pill; a restored window state or Configure Toolbars may take the brush bar back
    const bool tools = d->active && !isRailHidden();
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
    // shown after the insertion: removeToolBar() hides them, as a restored window state hides toolbars it has no place for
    if (d->topBar) d->topBar->setVisible(tools);
    if (brushBar) brushBar->show();
    if (d->windowBar) d->windowBar->show();

    // the window pill also on the start page: it is the title bar
    const bool window = d->active && !isCanvasOnly();
    d->topPill->setVisible(tools);
    d->windowHandle->setVisible(tools);
    d->windowPill->setVisible(window);
    if (tools) d->topPill->place();
    if (window) d->windowPill->place();
    updateCanvasInsets();
}

void KisStudioLayout::placeWindowHandle()
{
    QWidget *handle = d->windowHandle;
    TopPill *pill = d->topPill;
    if (!handle || !pill) return;
    const int height = pill->height();
    QRect place(TopPill::margin, TopPill::margin, height, height);
    // the cap of a tool rail on the left, as wide as it
    QWidget *rail = d->brushRail;
    if (rail && rail->isVisible() && rail->parentWidget() == handle->parentWidget() && rail->x() < rail->parentWidget()->width() / 2) {
        place.setLeft(rail->x());
        place.setWidth(rail->width());
    }
    if (handle->geometry() != place) handle->setGeometry(place);
    handle->raise();
    pill->setLeftInset(handle->isVisible() ? place.right() + 1 : 0);
}

void KisStudioLayout::releaseTopBars()
{
    QToolBar *brushBar = d->mainWindow->findChild<QToolBar *>(QStringLiteral("BrushesAndStuff"));
    if (brushBar && brushBar->parentWidget() != d->mainWindow) {
        const bool shown = !brushBar->isHidden();
        KisStudioGlass::setSeeThrough(brushBar, false);
        brushBar->setMovable(true);
        d->mainWindow->addToolBar(Qt::TopToolBarArea, brushBar);
        brushBar->setVisible(shown);
    }
    for (QToolButton *button : qAsConst(d->delayedPopups)) {
        if (button) button->setPopupMode(QToolButton::MenuButtonPopup);
    }
    d->delayedPopups.clear();
    d->placedOnBar = nullptr;
    if (d->topPill) d->topPill->hide();
    if (d->windowPill) d->windowPill->hide();
    if (d->windowHandle) d->windowHandle->hide();
    if (d->settings) d->settings->hide();
    updateCanvasInsets();
}

void KisStudioLayout::ensureSettings()
{
    QMdiArea *mdi = mdiArea();
    QWidget *area = mdi ? mdi->parentWidget() : nullptr;
    if (d->settings || !area) {
        return;
    }
    d->settings = new KisStudioSettings(d->mainWindow, area);
    // under the window pill, at the right, as tall as the canvas allows
    d->settings->setPlacement([this, area](const QSize &wanted) {
        const int margin = TopPill::margin;
        const QRect pill = d->windowPill && d->windowPill->isVisible() ? d->windowPill->geometry() : QRect(area->width() - margin, margin, 0, 0);
        const int top = pill.bottom() + 1 + margin;
        const int width = qMin(wanted.width(), area->width() - 2 * margin);
        const int right = pill.isEmpty() ? area->width() - margin : pill.right() + 1;
        return QRect(qMax(margin, right - width), top, width, qMax(120, area->height() - top - margin));
    });
}

void KisStudioLayout::openSettings(const QString &cardId, bool toggle)
{
    // with the studio layout off too: there are no other settings
    ensureSettings();
    if (!d->settings) {
        return;
    }
    if (toggle && d->settings->isVisible() && cardId.isEmpty()) d->settings->hide();
    else d->settings->open(cardId);
}

void KisStudioLayout::showMainMenu(QWidget *button)
{
    // the file commands most needed without a shortcut, then the menu bar's menus; rebuilt for the recent files
    if (!d->mainMenu) {
        d->mainMenu = new QMenu(d->mainWindow);
    }
    QMenu *menu = d->mainMenu;
    menu->clear();
    KisKActionCollection *actions = d->mainWindow->actionCollection();
    auto add = [&](std::initializer_list<const char *> names) {
        for (const char *name : names) {
            if (QAction *action = actions->action(QLatin1String(name))) menu->addAction(action);
        }
        menu->addSeparator();
    };
    add({"file_new", "file_open", "file_open_recent"});
    add({"file_save", "file_save_as", "file_export_file"});
    Q_FOREACH (QAction *top, d->mainWindow->menuBar()->actions()) {
        if (top->menu() && top->isVisible()) menu->addAction(top);
    }
    menu->addSeparator();
    add({"file_quit"});

    // right-aligned under the button
    const QSize size = menu->sizeHint();
    const QPoint corner = button->mapToGlobal(QPoint(button->width(), button->height() + 4));
    menu->popup(QPoint(corner.x() - size.width(), corner.y()));
}

void KisStudioLayout::placeWindowButtons()
{
    // put on the window pill's bar once
    auto place = [this](QWidget *widget, QPointer<QAction> &action) {
        if (!widget || action) return;
        auto *widgetAction = new QWidgetAction(this);
        widgetAction->setDefaultWidget(widget);
        action = widgetAction;
        if (d->windowBar) d->windowBar->addAction(widgetAction);
    };
    place(d->documentButton, d->documentButtonAction);
    place(d->menuButton, d->menuButtonAction);
    place(d->settingsButton, d->settingsButtonAction);
    place(d->findField, d->findFieldAction);
    if (d->frame && d->frame->isEnabled() && !d->windowButtonsAction) {
        place(d->frame->createButtons(d->mainWindow), d->windowButtonsAction);
    }

    QToolBar *brushBar = d->mainWindow->findChild<QToolBar *>(QStringLiteral("BrushesAndStuff"));
    if (!brushBar || brushBar == d->placedOnBar) {
        return;
    }
    d->placedOnBar = brushBar;
    // menus open on a long press instead of by a separate arrow
    Q_FOREACH (QToolButton *button, brushBar->findChildren<QToolButton *>()) {
        if (button->menu() && button->popupMode() == QToolButton::MenuButtonPopup) {
            button->setPopupMode(QToolButton::DelayedPopup);
            d->delayedPopups << button;
        }
    }
}

void KisStudioLayout::updateToolChip()
{
    if (!d->toolChip) {
        return;
    }
    const QString tool = KoToolManager::instance()->activeToolId();
    KoToolFactoryBase *factory = KoToolRegistry::instance()->value(tool);
    if (d->toolChipAction) {
        d->toolChipAction->setVisible(factory && (!isRailHidden() || d->peeking.contains(QPointer<QWidget>(d->topPill.data()))));
    }
    if (!factory) {
        return;
    }
    d->toolChip->setIcon(KisIconUtils::loadIcon(factory->iconName()));
    // a brush tool is the brush it paints with; the tool by its icon, and its name on hover
    const bool brushTool = tool == QLatin1String("KritaShape/KisToolBrush") || tool == QLatin1String("KritaShape/KisToolDyna")
        || tool == QLatin1String("KritaShape/KisToolMultiBrush");
    KisViewManager *view = d->mainWindow->viewManager();
    KisCanvasResourceProvider *resources = view ? view->canvasResourceProvider() : nullptr;
    const QString brush = brushTool && resources && resources->currentPreset() ? KisStudioBrushPicker::title(resources->currentPreset()->name()) : QString();
    const QString toolName = KisStudioToolRail::toolName(factory);
    d->toolChip->setText(brush.isEmpty() ? toolName : brush);
    d->toolChip->setToolTip(brush.isEmpty() ? i18nc("@info:tooltip", "Tool options") : i18nc("@info:tooltip tool chip: the tool, its options", "%1 options", toolName));
    d->toolChip->updateGeometry();
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
        // in Symmetry, on the tool strip
        {"mirror_actions", false},
        {"gradients", gradient},
        {"patterns", fills || shapeTool},
        // the color chip on the rail has the colors
        {"dual", false},
        // set up once, not per stroke: they are in Find
        {"workspaces", false},
        {"select_layout", false},
    };

    if (d->brushValuesAction) {
        d->brushValuesAction->setVisible(paints && (!isRailHidden() || d->peeking.contains(QPointer<QWidget>(d->topPill.data()))));
    }
    updateToolChip();
    if (d->flyout && d->flyoutAnchor) {
        // the new tool's options arrive later, in another size
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
        // the picker opens on the release
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
        // Krita's preset popup: tags, importing, editing
        QMetaObject::invokeMethod(d->presetChip, "showPopupWidget");
        return true;
    case QEvent::Wheel: {
        // a notch is one brush; a touchpad's small steps add up to one
        const int steps = KisModernStyle::wheelSteps(static_cast<QWheelEvent *>(event), d->presetChip);
        if (!steps) {
            return true;
        }
        const int step = steps < 0 ? 1 : -1;
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

void KisStudioLayout::peek(const QPoint &global)
{
    QMdiArea *mdi = mdiArea();
    QWidget *area = mdi ? mdi->parentWidget() : nullptr;
    if (!area || !isCanvasOnly() || !d->active) {
        return;
    }
    const QPoint at = area->mapFromGlobal(global);
    if (!area->rect().contains(at)) {
        return;
    }
    // what each edge brings back: the bars at the top, the tool rail and the panels at their sides
    const bool toolsLeft = d->brushRail && d->brushRail->x() < area->width() / 2;
    struct Edge {
        QList<QWidget *> widgets;
        bool near;
    };
    const int reach = 6;
    const QList<Edge> edges = {
        {{d->topPill.data()}, at.y() < reach},
        {{d->brushRail.data()}, toolsLeft ? at.x() < reach : at.x() >= area->width() - reach},
        {{d->panelColumn.data()}, toolsLeft ? at.x() >= area->width() - reach : at.x() < reach},
    };
    for (const Edge &edge : edges) {
        for (QWidget *widget : edge.widgets) {
            if (!widget) continue;
            const bool shown = d->peeking.contains(widget);
            // kept while the pointer is over it, or nearly
            const bool over = shown && widget->geometry().adjusted(-48, -48, 48, 48).contains(widget->parentWidget()->mapFrom(area, at));
            if (edge.near && !shown) {
                d->peeking << widget;
                if (widget == d->topPill) {
                    // the whole of it: in canvas only its tool and brush parts are hidden
                    if (d->topBar) d->topBar->show();
                    if (d->toolStripAction && d->toolStrip) d->toolStripAction->setVisible(d->toolStrip->hasOptions());
                    updateContextBar();
                }
                if (widget == d->topPill || widget == d->windowPill) static_cast<TopPill *>(widget)->place();
                widget->raise();
                KisStudioMotion::showFaded(widget);
            } else if (shown && !edge.near && !over) {
                d->peeking.removeAll(widget);
                KisStudioMotion::hideFaded(widget);
            }
        }
    }
}

bool KisStudioLayout::eventFilter(QObject *watched, QEvent *event)
{
    if ((event->type() == QEvent::MouseMove || event->type() == QEvent::TabletMove) && isCanvasOnly()) {
        // the pointer over this window (another window's moves are its own)
        auto *widget = qobject_cast<QWidget *>(watched);
        const QPoint global = event->type() == QEvent::MouseMove ? static_cast<QMouseEvent *>(event)->globalPos()
                                                                  : static_cast<QTabletEvent *>(event)->globalPos();
        if (widget && widget->window() == d->mainWindow) peek(global);
    }
    // a field keeps Undo and Redo for its own typing only: with none of it to take back, they are the window's
    if (event->type() == QEvent::ShortcutOverride) {
        auto *key = static_cast<QKeyEvent *>(event);
        const bool undo = key->matches(QKeySequence::Undo);
        const bool redo = key->matches(QKeySequence::Redo);
        if (undo || redo) {
            QLineEdit *edit = qobject_cast<QLineEdit *>(watched);
            if (!edit) {
                if (auto *spin = qobject_cast<QAbstractSpinBox *>(watched)) edit = spin->findChild<QLineEdit *>();
            }
            if (edit && edit->window() == d->mainWindow && !(undo ? edit->isUndoAvailable() : edit->isRedoAvailable())) {
                event->ignore();
                return true;
            }
        }
    }
    // closed before the window state is saved: restoring a floating panel crashes Qt
    if (watched == d->mainWindow && event->type() == QEvent::Close) {
        if (d->flyout) closeFlyout();
        Q_FOREACH (QDockWidget *dock, d->mainWindow->dockWidgets()) {
            if (dock->isFloating() && dock->isHidden()) dockAway(dock);
        }
    }

    if (watched == d->brushRail && d->brushRail) {
        const QEvent::Type type = event->type();
        if (type == QEvent::Move || type == QEvent::Resize || type == QEvent::Show || type == QEvent::Hide) {
            placeWelcomePage();
            placeWindowHandle();
        }
    }

    // an open flyout's contents grew or shrank (options shown by a checkbox): it fits them again, once
    if (event->type() == QEvent::LayoutRequest && d->flyout && !d->flyoutRefit) {
        auto *widget = qobject_cast<QWidget *>(watched);
        if (widget && d->flyout->isAncestorOf(widget)) {
            d->flyoutRefit = true;
            QTimer::singleShot(0, this, [this]() {
                d->flyoutRefit = false;
                positionFlyout();
            });
        }
    }

    if (handleRailDrag(watched, event)) {
        return true;
    }

    if (d->presetChip && watched == d->presetChip && handlePresetChip(event)) {
        return true;
    }

    // any button of the window, right clicked or held: its card, to give it a shortcut
    if (event->type() == QEvent::ContextMenu) {
        auto *button = qobject_cast<QAbstractButton *>(watched);
        if (button && button->window() == d->mainWindow && button->contextMenuPolicy() == Qt::DefaultContextMenu
            && !dynamic_cast<KisSelectionActionsPanelButton *>(button)) {
            bool ownCard = false;
            // these have cards or menus of their own, as has the canvas's selection bar
            for (QWidget *w = button; w && !ownCard; w = w->parentWidget()) {
                ownCard = w->inherits("KisStudioToolRail") || w == d->rail || w->property("ownContextMenu").toBool();
            }
            KisStudioFind *find = d->mainWindow->studioFind();
            if (!ownCard && find && find->showCard(button)) {
                return true;
            }
        }
    }

    // a pinned panel resized by the user keeps that height
    if (event->type() == QEvent::Resize && !d->settlingHeights) {
        auto *dock = qobject_cast<QDockWidget *>(watched);
        if (dock && d->panels.contains(dock) && d->pinned.contains(dock->objectName()) && !dock->isFloating()) {
            d->heightSave->start();
        }
    }

    switch (event->type()) {
    case QEvent::MouseButtonPress:
    case QEvent::TabletPress:
        // a press anywhere else in the window closes the flyout; the rail handles its own buttons
        if (d->flyout && d->flyout->isVisible()) {
            QWidget *widget = qobject_cast<QWidget *>(watched);
            // by position: a press not taken in the flyout goes on to what holds it
            const QPoint at = event->type() == QEvent::MouseButtonPress ? static_cast<QMouseEvent *>(event)->pos()
                                                                         : static_cast<QTabletEvent *>(event)->pos();
            const bool inFlyout = widget && d->flyoutWindow && d->flyoutWindow->isVisible()
                                  && d->flyoutWindow->rect().contains(d->flyoutWindow->mapFromGlobal(widget->mapToGlobal(at)));
            const bool onRail = d->rail && widget && (widget == d->rail || d->rail->isAncestorOf(widget));
            // the chip toggles the tool options itself, and they follow the tool picked on the tool rail
            const bool onChip = d->toolChip && widget == d->toolChip;
            const bool pickingTool = d->flyoutAnchor && d->flyoutAnchor == d->toolChip && d->brushRail && widget
                                     && d->brushRail->isAncestorOf(widget);
            if (widget && widget->window() == d->mainWindow && !inFlyout && !onRail && !onChip && !pickingTool) {
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
        // also after minimizing: placed while minimized, it would be far off
        if (d->flyout && (watched == d->mainWindow || watched == d->mainWindow->centralWidget())) {
            QTimer::singleShot(0, this, &KisStudioLayout::positionFlyout);
        }
        break;

    default:
        break;
    }

    return QObject::eventFilter(watched, event);
}
