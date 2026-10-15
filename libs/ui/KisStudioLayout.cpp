/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioLayout.h"

#include <KisResourceItemChooser.h>
#include "KisColorDrop.h"

#include <QAbstractButton>
#include <QTreeView>
#include <QListView>
#include <QAbstractItemView>
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
#include <QContextMenuEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
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
#include "KisStudioBarButtons.h"
#include "KisStudioBrushEditor.h"
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
#include "KisStudioSmoothing.h"
#include "KisStudioToolRail.h"
#include "KisStudioToolStrip.h"
#include "KisStudioWindowFrame.h"
#include "KisTouchMode.h"
#include "KisView.h"
#include "KisViewManager.h"
#include "KisWelcomePageWidget.h"
#include "KisStudioStartPage.h"
#include "widgets/KisStudioPopup.h"
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
/// Legacy key: the pinned panels on the left of the canvas, read when no panel side is saved
const QString leftKey = QStringLiteral("StudioLayoutLeftPanels");
/// A panel dropped this close to the canvas edge is pinned there
const int edgeDropZone = 140;
/// Heights set by the handles between pinned panels, as "id=height" entries; the others fit their contents
const QString heightsKey = QStringLiteral("StudioLayoutPanelSizes");
const QString foldedKey = QStringLiteral("StudioLayoutFoldedPanels");
const QString columnWidthKey = QStringLiteral("StudioLayoutPanelWidth");
/// The pinned panels docked on the far side of the canvas, away from the panel buttons
const QString farKey = QStringLiteral("StudioLayoutFarPanels");
const QString farWidthKey = QStringLiteral("StudioLayoutFarPanelWidth");
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
        {QStringLiteral("RecorderDocker"), QStringLiteral("studio_timelapse")},
        {QStringLiteral("StudioControls"), QStringLiteral("studio_controls")},
    };
    return icons.value(id);
}

/// A panel's title bar: drag to move the panel, click to fold it, right-click for its card
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
        setToolTip(i18nc("@info:tooltip a panel's title bar", "Drag: move\nClick: fold"));
        QObject::connect(KisTouchMode::instance(), &KisTouchMode::changed, this, [this]() { fit(); });
        fit();
    }

    /// A folded panel shows only this grip, with the panel's name
    void setFolded(bool folded, const QString &title)
    {
        m_folded = folded;
        m_title = title;
        setToolTip(folded ? i18nc("@info:tooltip a panel's title bar", "Drag: move\nClick: unfold") : i18nc("@info:tooltip a panel's title bar", "Drag: move\nClick: fold"));
        fit();
        update();
    }
    bool isFolded() const { return m_folded; }

    QSize sizeHint() const override { return QSize(40, height()); }

protected:
    void fit()
    {
        // taller in touch mode, for fingers
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
            // a click only if released over the grip; a long press can end far away
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

/// Where a dragged panel would land: in the column, or onto the canvas if \c column is false
struct PanelDrop {
    bool column {false};
    bool left {false};
    /// Into the column on the far side, away from the panel buttons
    bool far {false};
    QString before;      ///< the panel it goes above; empty for the bottom
    QRect indicator;     ///< in main window coordinates
};

/// The active tool's name and icon at the start of the top bar; a click opens its options
class ToolChip : public QToolButton
{
public:
    /// What fills the chip from its left (the brush's smoothing), 0 to 1; -1: nothing, and the chip isn't dragged
    std::function<qreal()> amount;
    std::function<void(qreal)> setAmount;
    /// The amount as shown while dragged
    std::function<QString()> amountText;
    std::function<void()> resetAmount;

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
        // measured without the '&' mnemonic the accelerator manager adds
        const QString label = QString(text()).remove(QLatin1Char('&'));
        int textWidth = fontMetrics().horizontalAdvance(label);
        // room for the smoothing while it is dragged, at its widest
        if (amount && amount() >= 0) textWidth = qMax(textWidth, fontMetrics().horizontalAdvance(scrubText(QStringLiteral("1000"))));
        return QSize(padding + icon + gap + textWidth + padding, qMax(28, icon + 8));
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
        const qreal filled = amount ? amount() : -1;
        if (filled > 0) {
            // the smoothing, as layer opacity fills its row
            painter.save();
            painter.setRenderHint(QPainter::Antialiasing, true);
            const QRectF box = QRectF(rect()).adjusted(1, 3, -1, -3);
            painter.setClipPath(KisModernStyle::squircle(box, KisModernStyle::cornerRadius));
            QColor fill = palette().color(QPalette::ButtonText);
            fill.setAlphaF(m_scrubbing ? 0.2 : 0.11);
            painter.fillRect(QRectF(box.left(), box.top(), box.width() * filled, box.height()), fill);
            painter.restore();
        }

        const int icon = iconSize().width();
        QRect area = rect().adjusted(padding, 0, -padding, 0);
        this->icon().paint(&painter, QRect(area.left(), (height() - icon) / 2, icon, icon));
        area.setLeft(area.left() + icon + gap);
        painter.setPen(palette().color(QPalette::ButtonText));
        const QString shown = m_scrubbing && amountText ? scrubText(amountText()) : text();
        painter.drawText(area, Qt::AlignVCenter | Qt::AlignLeft | Qt::TextHideMnemonic, fontMetrics().elidedText(shown, Qt::ElideRight, area.width()));
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        m_scrubbing = false;
        m_scrubbedThisPress = false;
        m_pressX = event->pos().x();
        QToolButton::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        const bool scrubbable = amount && setAmount && amount() >= 0;
        if (scrubbable && (event->buttons() & Qt::LeftButton)) {
            const bool fine = event->modifiers() & Qt::ShiftModifier;
            if (!m_scrubbing && qAbs(event->pos().x() - m_pressX) > QApplication::startDragDistance()) {
                // dragged sideways: the smoothing, not a click
                m_scrubbing = true;
                m_scrubbedThisPress = true;
                setDown(false);
                m_fine = !fine;
            }
            if (m_scrubbing) {
                if (fine != m_fine) {
                    // Shift held or let go: on from here, finer or not
                    m_fine = fine;
                    m_pressX = event->pos().x();
                    m_pressAmount = amount();
                }
                const qreal span = fine ? 800.0 : 200.0;
                setAmount(qBound(0.0, m_pressAmount + (event->pos().x() - m_pressX) / span, 1.0));
                update();
                event->accept();
                return;
            }
        }
        QToolButton::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (m_scrubbing) {
            m_scrubbing = false;
            setDown(false);
            update();
            event->accept();
            return;
        }
        QToolButton::mouseReleaseEvent(event);
    }

    void contextMenuEvent(QContextMenuEvent *event) override
    {
        // right click: the smoothing back to its default; not a finger held still after a drag
        if (amount && resetAmount && amount() >= 0 && !m_scrubbedThisPress) {
            resetAmount();
            update();
        }
        event->accept();
    }

private:
    static QString scrubText(const QString &value)
    {
        return i18nc("the brush chip while its smoothing is dragged, e.g. Smoothing 50", "Smoothing %1", value);
    }

    bool m_scrubbing {false};
    bool m_scrubbedThisPress {false};
    bool m_fine {false};
    int m_pressX {0};
    qreal m_pressAmount {0.0};

    static constexpr int padding = 8;
    static constexpr int gap = 6;
};

/// The foreground color over the background color: a click opens the color panel, a click on the background swaps them
class ColorChip : public QWidget
{
public:
    std::function<void()> openColors;
    std::function<void()> swapColors;

    explicit ColorChip(QWidget *parent) : QWidget(parent)
    {
        setFixedSize(40, 40);
        setCursor(Qt::PointingHandCursor);
        setToolTip(i18nc("@info:tooltip the color chip on the tool rail", "Colors\nDrag onto the image: fill\nClick the back one: swap"));
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
        // a ring in the window color separates the two even when they are similar
        painter.setPen(QPen(palette().color(QPalette::Window), 3));
        painter.setBrush(m_foreground);
        painter.drawEllipse(foregroundRect());
        painter.setPen(QPen(border, 1));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(foregroundRect().adjusted(-1.5, -1.5, 1.5, 1.5));
    }

    // a click acts on release, so the press can also start dragging a color onto the canvas
    void mousePressEvent(QMouseEvent *event) override
    {
        m_pressed = event->button() == Qt::LeftButton;
        m_pressAt = event->pos();
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (!m_pressed || (event->pos() - m_pressAt).manhattanLength() < QApplication::startDragDistance()) return;
        m_pressed = false;
        KisColorDrop::startDrag(this, onBackground(m_pressAt) ? m_background : m_foreground);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (!m_pressed || event->button() != Qt::LeftButton) return;
        m_pressed = false;
        if (onBackground(event->pos())) {
            if (swapColors) swapColors();
        } else if (openColors) {
            openColors();
        }
    }

private:
    QRectF foregroundRect() const { return QRectF(3, 3, 26, 26); }
    QRectF backgroundRect() const { return QRectF(17, 17, 20, 20); }
    bool onBackground(const QPoint &pos) const { return !foregroundRect().contains(pos) && backgroundRect().contains(pos); }

    bool m_pressed {false};
    QPoint m_pressAt;

    QColor m_foreground {Qt::black};
    QColor m_background {Qt::white};
};

/// Canvas area properties: how far the pinned panels and top pills cover its edges
const char *const leftInsetProperty = "studioCanvasLeftInset";
const char *const rightInsetProperty = "studioCanvasRightInset";
const char *const topInsetProperty = "studioCanvasTopInset";
/// Set on widgets whose empty space drags the window, like a title bar
const char *const captionProperty = "studioCaption";

QMargins canvasInsets(const QWidget *canvasArea)
{
    if (!canvasArea) return QMargins();
    return QMargins(canvasArea->property(leftInsetProperty).toInt(), canvasArea->property(topInsetProperty).toInt(),
                    canvasArea->property(rightInsetProperty).toInt(), 0);
}

/// How far the pinned panels' pill reaches in from the left and right edges, even when only its strip shows
const char *const leftPillProperty = "studioPanelPillLeft";
const char *const rightPillProperty = "studioPanelPillRight";

/// Paints \p widget as glass with a rim of opacity \p rim
void paintGlass(QPainter &painter, QWidget *widget, qreal radius, qreal rim)
{
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QPainterPath shape = KisStudioGlass::paint(painter, widget, QRectF(widget->rect()).adjusted(0.5, 0.5, -0.5, -0.5), radius);
    QColor border = widget->palette().color(QPalette::WindowText);
    border.setAlphaF(rim);
    KisStudioGlass::paintBorder(painter, widget, shape, border);
}

/// Six dots to drag a pill or rail by; mouse events go to the parent
class DragGrip : public QWidget
{
public:
    explicit DragGrip(QWidget *parent)
        : QWidget(parent)
    {
        setCursor(Qt::OpenHandCursor);
        setFixedHeight(14);
        setToolTip(i18nc("@info:tooltip", "Drag: switch sides"));
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
        // three along the longer side, two across, each centred on a pixel so none blurs
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
 * The top bar's grip: dragging moves the window, a tap tucks the bar away and
 * a double click maximizes. It is not a system caption, as Qt never sees clicks on those.
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
        // immediately, without waiting to see if a double click follows
        if (rect().contains(event->pos()) && tapped) tapped();
    }

    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton) return;
        // undo the tap from the first click
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

/// The handle between two stacked panels: drag to resize the one above, right-click to fit it to its contents
class PanelHandle : public QWidget
{
public:
    std::function<void()> pressed;
    /// Moved \p dy pixels since the press; \p done on release
    std::function<void(int dy, bool done)> dragged;
    std::function<void()> fit;

    explicit PanelHandle(QWidget *parent)
        : QWidget(parent)
    {
        setAttribute(Qt::WA_Hover);
        setCursor(Qt::SizeVerCursor);
        setToolTip(i18nc("@info:tooltip the handle between two panels", "Drag: resize\nRight-click: fit"));
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        // invisible until the pointer finds it, so the glass shows through; a line across, unlike a panel's grip
        if (!underMouse() && !m_pressed) return;
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        QColor ink = palette().color(QPalette::WindowText);
        ink.setAlphaF(m_pressed ? 0.5 : 0.25);
        painter.setPen(Qt::NoPen);
        painter.setBrush(ink);
        const QRectF line(12, height() / 2.0 - 1, width() - 24, 2);
        painter.drawPath(KisModernStyle::squircle(line, 1));
    }

    void enterEvent(QEvent *) override { update(); }
    void leaveEvent(QEvent *) override { update(); }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton) return;
        m_pressed = true;
        m_pressY = globalMousePos(event).y();
        update();
        if (pressed) pressed();
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (m_pressed && dragged) dragged(globalMousePos(event).y() - m_pressY, false);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (!m_pressed || event->button() != Qt::LeftButton) return;
        m_pressed = false;
        update();
        if (dragged) dragged(globalMousePos(event).y() - m_pressY, true);
    }

    void contextMenuEvent(QContextMenuEvent *event) override
    {
        event->accept();
        if (fit) fit();
    }

private:
    bool m_pressed {false};
    int m_pressY {0};
};

/**
 * Krita's resource lists turn into a strip of tiles when short, which a panel
 * fitted to its contents would then stay. In a column they stay lists.
 */
void keepUpright(QDockWidget *dock, bool upright)
{
    Q_FOREACH (KisResourceItemChooser *chooser, dock->findChildren<KisResourceItemChooser *>()) {
        if (upright) {
            // a list again first, as a tall chooser lays itself out
            chooser->setResponsiveness(true);
            if (chooser->height() <= 100) chooser->resize(chooser->width(), 200);
        }
        chooser->setResponsiveness(!upright);
    }
}

/// The scroll area that holds most of \p dock's contents: what the panel grows to show more of
QAbstractScrollArea *mainScrollArea(const QDockWidget *dock)
{
    QAbstractScrollArea *best = nullptr;
    int bestArea = 0;
    Q_FOREACH (QAbstractScrollArea *area, dock->findChildren<QAbstractScrollArea *>()) {
        if (!area->isVisibleTo(dock)) continue;
        const int size = area->width() * area->height();
        if (size > bestArea) {
            best = area;
            bestArea = size;
        }
    }
    return best;
}

/// How tall \p area's contents are: what it shows without scrolling; -1 if that can't be told
int contentsHeight(QAbstractScrollArea *area)
{
    if (auto *scroll = qobject_cast<QScrollArea *>(area)) {
        QWidget *widget = scroll->widget();
        if (!widget) return 0;
        return widget->hasHeightForWidth() ? widget->heightForWidth(scroll->viewport()->width()) : widget->sizeHint().height();
    }
    auto *view = qobject_cast<QAbstractItemView *>(area);
    QAbstractItemModel *model = view ? view->model() : nullptr;
    if (!model) return -1;
    const QModelIndex root = view->rootIndex();
    if (model->rowCount(root) == 0) return 0;
    const QModelIndex first = model->index(0, 0, root);
    QModelIndex last = model->index(model->rowCount(root) - 1, 0, root);
    if (auto *tree = qobject_cast<QTreeView *>(view)) {
        for (QModelIndex i = first; i.isValid(); i = tree->indexBelow(i)) last = i;
    }
    const QRect top = view->visualRect(first);
    const QRect bottom = view->visualRect(last);
    if (!top.isValid() || !bottom.isValid()) return -1;
    auto *list = qobject_cast<QListView *>(view);
    return bottom.bottom() - top.top() + 1 + (list ? 2 * list->spacing() : 0);
}

/**
 * The pinned panels of a column, stacked top to bottom. Each is as tall as its
 * contents, or as its handle set it; when they don't all fit, the ones that
 * scroll give up room, those sized by hand last.
 */
class PanelStack : public QWidget
{
public:
    /// Height of the handle between two panels; taller for fingers
    static int gap() { return KisTouchMode::isActive() ? 12 : 6; }
    /// A scrolling panel keeps at least this much of its contents
    static constexpr int minimumContents = 56;

    /// A panel's height set by its handle; 0: fitted to its contents
    std::function<int(const QDockWidget *)> userHeight;
    /// Its handle moved: \p height for \p dock, 0 to fit it again; \p done once let go
    std::function<void(QDockWidget *dock, int height, bool done)> resizedByUser;
    /// How tall the stack would like to be has changed
    std::function<void()> wantsHeight;

    explicit PanelStack(QWidget *parent)
        : QWidget(parent)
    {
        m_timer.setSingleShot(true);
        m_timer.setInterval(0);
        QObject::connect(&m_timer, &QTimer::timeout, this, [this]() { relayout(true); });
        QObject::connect(KisTouchMode::instance(), &KisTouchMode::changed, this, [this]() { later(); });
    }

    QList<QDockWidget *> panels() const
    {
        QList<QDockWidget *> docks;
        for (const QPointer<QDockWidget> &dock : m_panels) {
            if (dock && dock->parentWidget() == this) docks << dock;
        }
        return docks;
    }

    /// Takes \p docks in this order, each keeping its visibility
    void setPanels(const QList<QDockWidget *> &docks)
    {
        m_panels.clear();
        for (QDockWidget *dock : docks) {
            const bool shown = !dock->isHidden();
            KisStudioGlass::setSeeThrough(dock, true);
            keepUpright(dock, true);
            if (dock->parentWidget() != this) dock->setParent(this);
            dock->setVisible(shown);
            dock->removeEventFilter(m_watcher);
            watch(dock);
            m_panels << dock;
        }
        later();
    }

    /// The height all its shown panels want, with their handles
    int wantedHeight()
    {
        int total = 0;
        int shown = 0;
        for (QDockWidget *dock : panels()) {
            if (dock->isHidden()) continue;
            total += measure(dock).want;
            shown++;
        }
        return total + qMax(0, shown - 1) * gap();
    }

    QSize sizeHint() const override { return QSize(width(), m_wanted); }

    /// Lays the panels out again soon, once their contents have settled
    void later()
    {
        m_passes = 0;
        m_timer.start();
    }

    /// Lays the panels out in the stack's height; \p ask: first see whether the stack wants another height
    void relayout(bool ask)
    {
        if (ask) {
            const int wanted = wantedHeight();
            if (wanted != m_wanted) {
                m_wanted = wanted;
                updateGeometry();
                // the column resizes the stack, which lays the panels out
                if (wantsHeight) wantsHeight();
            }
        }
        arrange();
    }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QWidget::resizeEvent(event);
        arrange();
        // a new width rewraps grids of tiles
        if (event->oldSize().width() != width()) later();
    }

private:
    struct Measure {
        int want {0};
        int minimum {0};
        bool byHand {false};
    };

    Measure measure(QDockWidget *dock)
    {
        Measure m;
        QWidget *title = dock->titleBarWidget();
        const int titleHeight = title ? title->height() : 0;
        if (dock->widget() && dock->widget()->property("studioFolded").toBool()) {
            m.want = m.minimum = titleHeight;
            return m;
        }
        int natural;
        QAbstractScrollArea *area = mainScrollArea(dock);
        const int contents = area ? contentsHeight(area) : -1;
        if (area && contents >= 0 && dock->height() <= titleHeight) {
            // not laid out yet: measured again once it is
            m_remeasure = true;
        }
        if (area && contents >= 0 && dock->height() > titleHeight) {
            // what isn't the scroll area, from the size hints (the current sizes are off when squeezed), and its frame
            const int chrome = dock->sizeHint().height() - area->sizeHint().height() + area->height() - area->viewport()->height();
            natural = chrome + qMax(contents, minimumContents);
            m.minimum = chrome + qMin(qMax(contents, 1), minimumContents);
        } else {
            natural = dock->hasHeightForWidth() ? dock->heightForWidth(width()) : dock->sizeHint().height();
            m.minimum = dock->minimumSizeHint().height();
        }
        const int byHand = userHeight ? userHeight(dock) : 0;
        m.byHand = byHand > 0;
        // a panel that scrolls is never taller than its contents; one that doesn't takes the height it is given
        m.want = m.byHand ? (area && contents >= 0 ? qMin(byHand, natural) : byHand) : natural;
        // never below what its own layout needs (Qt wouldn't make it smaller); a fold animating caps it, below that too
        m.minimum = qMax(m.minimum, qMax(titleHeight, qMax(dock->minimumSizeHint().height(), dock->minimumHeight())));
        m.minimum = qMin(m.minimum, dock->maximumHeight());
        m.want = qBound(m.minimum, m.want, dock->maximumHeight());
        return m;
    }

    void arrange()
    {
        m_remeasure = false;
        QList<QDockWidget *> shown;
        for (QDockWidget *dock : panels()) {
            if (!dock->isHidden()) shown << dock;
        }
        QVector<Measure> sizes;
        int total = qMax(0, shown.size() - 1) * gap();
        for (QDockWidget *dock : shown) {
            sizes << measure(dock);
            total += sizes.last().want;
        }
        // measured again after the column was sized: it follows
        if (total != m_wanted && m_passes < 3) {
            m_passes++;
            m_wanted = total;
            updateGeometry();
            QTimer::singleShot(0, this, [this]() {
                if (wantsHeight) wantsHeight();
            });
        }
        // too tall: the panels give up room down to their minimum, those fitted to their contents first
        int deficit = total - height();
        for (int round = 0; round < 2 && deficit > 0; round++) {
            const bool byHand = round == 1;
            int room = 0;
            for (int i = 0; i < sizes.size(); i++) {
                if (sizes[i].byHand == byHand && shown[i] != m_dragged) room += sizes[i].want - sizes[i].minimum;
            }
            if (room <= 0) continue;
            const int take = qMin(deficit, room);
            int taken = 0;
            for (int i = 0; i < sizes.size(); i++) {
                if (sizes[i].byHand != byHand || shown[i] == m_dragged) continue;
                const int share = int(qint64(take) * (sizes[i].want - sizes[i].minimum) / room);
                sizes[i].want -= share;
                taken += share;
            }
            // what rounding left over, from the first that can give it
            for (int i = 0; i < sizes.size() && taken < take; i++) {
                if (sizes[i].byHand != byHand || shown[i] == m_dragged) continue;
                const int share = qMin(take - taken, sizes[i].want - sizes[i].minimum);
                sizes[i].want -= share;
                taken += share;
            }
            deficit -= taken;
        }

        while (m_handles.size() < shown.size()) {
            auto *handle = new PanelHandle(this);
            m_handles << handle;
        }
        int y = 0;
        for (int i = 0; i < shown.size(); i++) {
            QDockWidget *dock = shown[i];
            dock->setGeometry(0, y, width(), sizes[i].want);
            y += sizes[i].want;
            PanelHandle *handle = m_handles[i];
            const bool between = i + 1 < shown.size();
            handle->setVisible(between);
            if (!between) continue;
            handle->setGeometry(0, y, width(), gap());
            handle->raise();
            QPointer<QDockWidget> target = dock;
            handle->pressed = [this, target]() { m_dragStart = target ? target->height() : 0; };
            handle->dragged = [this, target](int dy, bool done) {
                if (!target) return;
                m_dragged = done ? nullptr : target.data();
                if (resizedByUser) resizedByUser(target, qMax(1, m_dragStart + dy), done);
                relayout(true);
            };
            handle->fit = [this, target]() {
                if (target && resizedByUser) resizedByUser(target, 0, true);
                relayout(true);
            };
            y += gap();
        }
        for (int i = shown.size(); i < m_handles.size(); i++) m_handles[i]->hide();
        if (m_remeasure && m_passes < 3) {
            m_passes++;
            m_timer.start();
        }
    }

    /// Lays out again when a panel's contents change size
    void watch(QDockWidget *dock)
    {
        if (!m_watcher) {
            m_watcher = new Watcher(this);
        }
        dock->installEventFilter(m_watcher);
        QAbstractScrollArea *area = mainScrollArea(dock);
        auto *view = qobject_cast<QAbstractItemView *>(area);
        if (view && view->model() && !m_models.contains(view->model())) {
            QAbstractItemModel *model = view->model();
            m_models << model;
            for (auto signal : {&QAbstractItemModel::rowsInserted, &QAbstractItemModel::rowsRemoved}) {
                QObject::connect(model, signal, this, [this]() { later(); });
            }
            QObject::connect(model, &QAbstractItemModel::modelReset, this, [this]() { later(); });
            QObject::connect(model, &QAbstractItemModel::layoutChanged, this, [this]() { later(); });
        }
        if (auto *tree = qobject_cast<QTreeView *>(view)) {
            QObject::connect(tree, &QTreeView::expanded, this, [this]() { later(); }, Qt::UniqueConnection);
            QObject::connect(tree, &QTreeView::collapsed, this, [this]() { later(); }, Qt::UniqueConnection);
        }
    }

    class Watcher : public QObject
    {
    public:
        explicit Watcher(PanelStack *stack) : QObject(stack), m_stack(stack) {}

    protected:
        bool eventFilter(QObject *, QEvent *event) override
        {
            switch (event->type()) {
            case QEvent::Show:
            case QEvent::Hide:
            case QEvent::LayoutRequest:
                m_stack->later();
                break;
            default:
                break;
            }
            return false;
        }

    private:
        PanelStack *m_stack;
    };

    QList<QPointer<QDockWidget>> m_panels;
    QList<PanelHandle *> m_handles;
    QList<QPointer<QAbstractItemModel>> m_models;
    Watcher *m_watcher {nullptr};
    QPointer<QDockWidget> m_dragged;
    int m_dragStart {0};
    QTimer m_timer;
    int m_wanted {0};
    bool m_remeasure {false};
    /// Layouts redone in a row to measure panels that weren't laid out yet
    int m_passes {0};
};

/// Holds the tool rail over the side of the canvas, as tall as its tools
class RailOverlay : public QWidget
{
public:
    /// Called when dragged by its grip and dropped nearer the other side of the canvas
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

    /// When tucked, only the grip shows, as wide as the rail was
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
        m_grip->setToolTip(tucked ? i18nc("@info:tooltip", "Click: show tools\nDrag: switch sides")
                                  : i18nc("@info:tooltip", "Drag: switch sides\nClick: hide"));
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
        // pressed on the grip or between the tools: the rail follows the pointer
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
 * A glass pill over the top of the canvas, sized to its contents: the tool and
 * brush bars on the left, or the window's buttons on the right. Its empty space
 * drags the window like a title bar.
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

    /// Inserts \p widget at \p index, counting the grip
    void insert(int index, QWidget *widget)
    {
        QLayout *contents = m_contents->layout();
        const int at = m_grip ? index - 1 : index;
        static_cast<QHBoxLayout *>(contents)->insertWidget(qBound(0, at, contents->count()), widget);
        keepHeight();
    }

    QWidget *contents() const { return m_contents; }
    TopGrip *grip() const { return m_grip; }

    /// Keeps the grip as tall as the open bar, so tucking it doesn't move the window handle or what is below
    void keepHeight()
    {
        if (m_grip) m_grip->setFixedHeight(qMax(30, m_contents->sizeHint().height()));
    }

    void setTucked(bool tucked)
    {
        m_contents->setVisible(!tucked);
        keepHeight();
        if (m_grip) {
            m_grip->setToolTip(tucked ? i18nc("@info:tooltip", "Click: show the bar\nDrag: move\nDouble-click: maximize")
                                      : i18nc("@info:tooltip", "Drag: move\nClick: hide\nDouble-click: maximize"));
        }
    }

    void place()
    {
        QWidget *area = parentWidget();
        move(m_right ? area->width() - margin - width() : margin + m_leftInset, margin);
        raise();
    }

    /// Space on the left reserved for the window handle
    void setLeftInset(int inset)
    {
        if (inset == m_leftInset) return;
        m_leftInset = inset;
        place();
    }

    /// Called when resized, shown or hidden, so whatever is placed below can follow
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

/// A system caption in the top-left corner showing the application icon; it caps the tool rail
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
        KisIconUtils::loadIcon(QStringLiteral("krita-branding")).paint(&painter, QRect(QPoint(), QSize(side, side)).translated(rect().center() - QPoint(side / 2, side / 2)));
    }
};

/// A button on the window pill: the menu, Settings or Find
class PillButton : public QAbstractButton
{
public:
    /// Called on a right click or long press
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
 * The pinned panels in a glass pill over an edge of the canvas, below the
 * strip of panel buttons. Dragging the strip switches sides; dragging the
 * inner edge sets the width.
 */
class KisStudioPanelColumn : public QWidget
{
public:
    /// Distance from the canvas area's edges
    static constexpr int margin = 6;
    /// Width of the draggable inner edge
    static constexpr int edge = 5;
    static constexpr int minimumColumnWidth = 220;

    /// Called when the geometry changes; \p done is true once a width drag ends
    std::function<void(bool done)> resized;
    /// Called when dragged by the strip and dropped nearer the other side of the canvas
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
        m_stack = new PanelStack(this);
        m_stack->wantsHeight = [this]() { place(); };
        m_layout->addWidget(m_stack);
        updateMargins();
        setMouseTracking(true);
        // a separate widget, since a cursor set on the column would also apply to the panels
        m_grip = new QWidget(this);
        m_grip->setObjectName(QStringLiteral("studioPanelsEdge"));
        m_grip->setCursor(Qt::SizeHorCursor);
        m_grip->setAttribute(Qt::WA_NoSystemBackground);
        m_grip->installEventFilter(this);
        canvasArea->installEventFilter(this);
        hide();
    }

    PanelStack *stack() const { return m_stack; }
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

    /// Shows the panels below the strip, or only the strip
    void setPanelsShown(bool shown)
    {
        m_stack->setVisible(shown);
        place();
        placeGrip();
    }
    bool panelsShown() const { return !m_stack->isHidden(); }

    QList<QDockWidget *> panels() const { return m_stack->panels(); }

    /// Takes \p docks in this order, each keeping its visibility
    void setPanels(const QList<QDockWidget *> &docks) { m_stack->setPanels(docks); }

    /// Places the column over the canvas area's edge, as tall as its panels want within it; just the strip without them
    void place()
    {
        QWidget *area = parentWidget();
        // below the pills along the top of the canvas
        const int top = area->property(topInsetProperty).toInt() + margin;
        QSize size;
        if (panelsShown()) {
            const QMargins margins = m_layout->contentsMargins();
            const int strip = m_strip && !m_strip->isHidden() ? m_strip->sizeHint().height() : 0;
            const int wanted = margins.top() + margins.bottom() + strip + m_stack->wantedHeight();
            size = QSize(qBound(minimumColumnWidth, m_width, qMax(minimumColumnWidth, area->width() / 2)),
                         qBound(0, wanted, qMax(0, area->height() - top - margin)));
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
        // pressed on the strip: the pill follows the pointer
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
    /// The resize grip covers the inner edge, only while the panels are shown
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
    PanelStack *m_stack {nullptr};
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
 * An unpinned panel opened in glass over the canvas. It is a child of the main
 * window, above the pills: a separate window could not show QML panels, and a
 * floating dock would be saved with the window state, which crashes on restore.
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

    /// The strip of panel buttons above the pinned panels
    QPointer<QToolBar> rail;
    QPointer<QToolButton> addPanelButton;
    /// The tool rail, with the color chip under its tools
    QPointer<QWidget> brushRail;
    QPointer<KisStudioToolRail> toolRail;
    QPointer<QWidget> windowHandle;
    /// Holds the tool chip, the tool options, size and opacity
    QPointer<QToolBar> topBar;
    /// The bar's arrangeable switches and commands, after the brush's own controls
    QPointer<QToolBar> buttonsBar;
    QPointer<QWidget> findField;
    QPointer<QAction> findFieldAction;
    QPointer<QWidget> menuButton;
    QPointer<QAction> menuButtonAction;
    QPointer<QWidget> settingsButton;
    QPointer<QAction> settingsButtonAction;
    QPointer<KisStudioSettings> settings;
    QPointer<QMenu> mainMenu;
    /// The brush bar whose menu buttons were switched to open on a long press, and those buttons
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
    /// Rail buttons are inserted before this
    QAction *railSpacer {nullptr};

    /// State of dragging a rail button to reorder it
    struct RailDrag {
        QPointer<QToolButton> button;
        QString id;
        QPoint pressPos;
        bool active {false};
        int targetIndex {-1};
    } railDrag;
    QPointer<QFrame> dropIndicator;
    /// Shows where a dragged panel would land beside the canvas
    QPointer<QFrame> panelIndicator;

    QSet<QString> pinned;
    QHash<QString, int> heights;
    QSet<QString> folded;
    QHash<QString, QPointer<PanelGrip>> grips;
    QPointer<KisStudioPanelColumn> panelColumn;
    /// The column on the other side of the canvas, without the panel buttons
    QPointer<KisStudioPanelColumn> farColumn;
    /// Shown in place of Krita's welcome page, which is kept aside meanwhile
    QPointer<KisStudioStartPage> startPage;
    QPointer<QWidget> classicWelcome;
    QSet<QString> far;
    QPointer<TopPill> topPill;
    QPointer<TopPill> windowPill;
    /// In canvas-only mode, what the pointer brought back by nearing its edge (see peek())
    QList<QPointer<QWidget>> peeking;
    QPointer<QToolBar> windowBar;
    bool panelsLeft {false};
    bool toolRailLeft {true};
    bool panelsTucked {false};
    bool toolRailTucked {false};
    bool topBarTucked {false};
    QPointer<QWidget> stripGrip;

    /// The unpinned panel open over the canvas, and the widget it opened from (it is placed below it)
    QPointer<QDockWidget> flyout;
    QPointer<FlyoutWindow> flyoutWindow;
    QPointer<QWidget> flyoutAnchor;
    QPointer<QToolButton> toolChip;
    /// The flyout needs refitting to its contents
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

    /// A docker's title bar and features, restored when leaving the Studio layout
    struct ClassicDock {
        QPointer<QWidget> titleBar;
        QDockWidget::DockWidgetFeatures features;
    };
    QHash<QDockWidget *, ClassicDock> classicDocks;
    /// Every docker except the toolbox
    QSet<QDockWidget *> panels;
    QVector<QMetaObject::Connection> connections;

    /// The pinned panels in stacking order: rail order first, then the rest
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

    /// The pinned panels shown in a column, top to bottom, excluding \p without; \p farSide: the far column's
    QList<QDockWidget *> column(const QString &without, bool farSide = false) const
    {
        QList<QDockWidget *> docks;
        Q_FOREACH (const QString &id, pinnedOrder()) {
            QDockWidget *dock = mainWindow->dockWidget(id);
            if (id != without && far.contains(id) == farSide && dock && dock != flyout && !dock->isHidden() && !dock->isFloating()) {
                docks << dock;
            }
        }
        std::sort(docks.begin(), docks.end(), [this](QDockWidget *a, QDockWidget *b) { return placeOf(a).y() < placeOf(b).y(); });
        return docks;
    }

    /// \p dock's geometry in main window coordinates (panels in the column aren't its direct children)
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
        // the column with the panel buttons first, then the one across the canvas
        for (const bool farSide : {false, true}) {
            const bool left = farSide ? !panelsLeft : panelsLeft;
            const QList<QDockWidget *> docks = column(moving, farSide);
            QRect area;
            Q_FOREACH (QDockWidget *dock, docks) {
                area |= placeOf(dock);
            }
            // over a column, or near the canvas edge where it has no panels
            const bool overColumn = !area.isEmpty() && pos.x() >= area.left() - 8 && pos.x() <= area.right() + 8;
            const bool nearEdge = area.isEmpty() && canvas.contains(pos)
                                  && (left ? pos.x() < canvas.left() + edgeDropZone : pos.x() > canvas.right() - edgeDropZone);
            if (!overColumn && !nearEdge) continue;

            drop.column = true;
            drop.left = left;
            drop.far = farSide;
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
    // snapshot the settings at startup, so later changes that need a restart show as pending
    KisSettingsRegistry::instance();

    // save the heights once a resize has settled

    // canvas-only mode restores the window state partly through the event queue, so normalize afterwards
    if (QAction *canvasOnly = mainWindow->actionCollection()->action(QStringLiteral("view_show_canvas_only"))) {
        connect(canvasOnly, &QAction::toggled, this, [this](bool on) {
            closeFlyout();
            // a fade from peeking would hide what the mode just brought back
            for (const QPointer<QWidget> &widget : d->peeking) KisStudioMotion::stopFade(widget);
            d->peeking.clear();
            QTimer::singleShot(0, this, &KisStudioLayout::normalize);
            // full screen and back without Windows animating the window from its restored size
            if (d->frame && d->active) {
                if (on) {
                    d->frame->setTransitionsEnabled(false);
                } else {
                    QTimer::singleShot(600, d->frame, [frame = d->frame]() { if (frame) frame->setTransitionsEnabled(true); });
                }
            }
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
    // save the classic layout to return to
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
    d->far = readSet(farKey);
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
        // deferred until the release is delivered, since the drop may rearrange the panel and its grip
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
        // snapshot for the panel picker as the panel goes off screen
        d->connections << connect(dock, &QDockWidget::visibilityChanged, this, [dock](bool visible) {
            if (!visible) KisStudioPanelPicker::rememberLook(dock);
        });
        dock->setFeatures(QDockWidget::DockWidgetClosable);
        d->panels.insert(dock);

        // named so a shortcut set on its card is saved, and added to the window so the shortcut works
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

    showStartPage(true);
    createRail();
    createBrushRail();
    createTopBar();

    // the effects and brush pickers, opened at the pointer from Find, a shortcut or the top bar
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
    // preload the brushes (about a tenth of a second) ahead of the first right press or brush chip scroll
    QTimer::singleShot(3000, this, []() { KisStudioBrushPicker::yourBrushes(); });
    // so the buttons' shortcuts work before Find is first opened
    QTimer::singleShot(0, this, [this]() {
        if (KisStudioFind *find = d->mainWindow->studioFind()) find->registerButtons();
    });
    KisViewManager *viewManager = mainWindow->viewManager();
    if (KisPaintopBox *paintopBox = viewManager ? viewManager->paintOpBox() : nullptr) {
        paintopBox->setStudioButtonsHidden(true);
        // the brush editor's action (its shortcut, Find) opens the quick editor, under the bar's button if it has one
        disconnect(paintopBox, &KisPaintopBox::sigQuickEditorRequested, this, nullptr);
        connect(paintopBox, &KisPaintopBox::sigQuickEditorRequested, this, [this]() {
            // below the brush's chip, or a bar button of its own if it was put back
            QWidget *button = d->mainWindow->findChild<QWidget *>(QStringLiteral("studio_bar_action_show_brush_editor"));
            if (!button || !button->isVisible()) button = d->toolChip;
            KisStudioBrushEditor::toggle(d->mainWindow, button && button->isVisible() ? button : nullptr, Qt::BottomEdge);
        });
        d->presetChip = paintopBox->presetChip();
        if (d->presetChip) {
            d->presetChipToolTip = d->presetChip->toolTip();
            d->presetChip->setToolTip(i18nc("@info:tooltip", "Brushes\nScroll: next brush\nRight-click: manage"));
        }
    }

    // the top bar's scrubbers replace the toolbar's size and opacity sliders
    for (int i = 1; i <= 5; i++) {
        const QString slider = cfg.toolbarSlider(i);
        QAction *action = mainWindow->actionCollection()->action(QStringLiteral("brushslider%1").arg(i));
        if (action && action->isVisible() && (slider == QLatin1String("size") || slider == QLatin1String("opacity"))) {
            action->setVisible(false);
            d->hiddenToolbarActions << action;
        }
    }
    // menu items for what only the classic layout has: toolbars, styles and themes of their own, a status bar, tiled windows
    for (const char *name : {"options_configure_toolbars", "lock_toolbars", "toolbars_submenu_action", "style_menu", "theme_menu",
                             "showStatusBar", "windows_tile", "windows_cascade", "windows_next", "windows_previous", "help_about_kde"}) {
        QAction *action = mainWindow->actionCollection()->action(QLatin1String(name));
        if (action && action->isVisible()) {
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
                                  // ignore tool changes in other windows
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

    // fill the strip with the current tool's options without waiting for a tool change
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
    // in a pill, empty space and drag grips act as caption; controls don't, nor does TopGrip, which moves the window itself
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
    // no switcher for a single document
    if (d->documentButtonAction) {
        d->documentButtonAction->setVisible(windows.size() > 1 && !isRailHidden());
    }
}

void KisStudioLayout::leave()
{
    KisMainWindow *mainWindow = d->mainWindow;
    closeFlyout();
    qApp->removeEventFilter(this);
    // folds still running would size the classic docks when they end
    Q_FOREACH (QVariantAnimation *fold, findChildren<QVariantAnimation *>(QString(), Qt::FindDirectChildrenOnly)) {
        if (fold->objectName().startsWith(QLatin1String("studioFold_")) && fold->state() == QAbstractAnimation::Running) {
            fold->setCurrentTime(fold->duration());
        }
    }
    for (const QPointer<QWidget> &widget : d->peeking) KisStudioMotion::stopFade(widget);
    d->peeking.clear();
    if (d->presetChip) {
        d->presetChip->setToolTip(d->presetChipToolTip);
    }
    delete d->brushMemory;
    delete d->canvasRing;
    if (d->hud) {
        d->hud->setActive(false);
    }
    showStartPage(false);
    if (auto *welcome = mainWindow->findChild<KisWelcomePageWidget *>()) {
        welcome->setContentsMargins(QMargins());
    }
    mainWindow->setProperty("studioLayoutActive", false);
    if (d->frame) {
        d->frame->setTransitionsEnabled(true);
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
    for (KisStudioPanelColumn *column : {d->panelColumn.data(), d->farColumn.data()}) {
        if (!column) continue;
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
    if (d->buttonsBar) {
        d->buttonsBar->hide();
    }
    if (KisPaintopBox *paintopBox = mainWindow->viewManager() ? mainWindow->viewManager()->paintOpBox() : nullptr) {
        paintopBox->setStudioButtonsHidden(false);
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
    // no saved classic layout yet: use the workspace a fresh Krita shows
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

    // the pinned panels decide what is shown, not a restored workspace or window state
    Q_FOREACH (QDockWidget *dock, d->panels) {
        if (dock == d->flyout) {
            continue;
        }
        // never leave a panel floating: restoring a floating panel crashes
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
    // the rail's commands only exist once the GUI is built, which happens after the rail
    if (d->toolRail) {
        d->toolRail->refresh();
    }
    placeWindowButtons();
    // also on the start page, where the window pill acts as the title bar
    for (QAction *action : {d->menuButtonAction.data(), d->settingsButtonAction.data(), d->findFieldAction.data()}) {
        if (action) action->setVisible(!canvasOnly);
    }
    if (d->windowButtonsAction) {
        d->windowButtonsAction->setVisible(d->frame && d->frame->isEnabled() && !canvasOnly);
    }

    // hide these again, since restoring a window state brings them back
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
    // the canvas HUD takes over the status bar's view information
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
        // hidden by its shortcut, its card or a workspace: don't leave an empty flyout behind
        QTimer::singleShot(0, this, [this, dock]() {
            if (dock == d->flyout && dock->isHidden()) closeFlyout();
        });
    } else if (visible && dock != d->flyout && !d->pinned.contains(dock->objectName())) {
        // shown by the Dockers menu, a shortcut or a workspace: open it as a flyout, unless normalize() hid it again
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
    // Qt can't re-dock a panel that was never in a dock area, so add it to one to keep it from being saved floating
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
    // a scroll area has a small size hint, so fit the flyout to what it scrolls
    QScrollArea *scroll = d->flyout->findChild<QScrollArea *>();
    if (scroll && scroll->widget()) {
        const int frame = 2 * scroll->frameWidth();
        const int header = d->flyout->titleBarWidget() ? d->flyout->titleBarWidget()->sizeHint().height() : 0;
        contentsWidth = qMax(contentsWidth, scroll->widget()->sizeHint().width() + frame + scroll->verticalScrollBar()->sizeHint().width());
        contentsHeight = qMax(contentsHeight, header + scroll->widget()->sizeHint().height() + frame + 12);
    }
    // stay within the canvas area, as the main window clips anything beyond it
    const int width = qMax(0, qMin(qBound(260, contentsWidth + 2, 420), area.width() - 2 * margin));

    QRect global(area.right() + 1 - margin - width, area.top() + margin, width, area.height() - 2 * margin);
    // below the panel icons when nothing is pinned beside them, so another panel is still a click away
    if (KisStudioPanelColumn *column = d->panelColumn; column && column->isVisible()) {
        const QRect strip(column->mapToGlobal(QPoint()), column->size());
        if (strip.intersects(global)) global.setTop(strip.bottom() + 1 + margin);
    }
    // beside the panels docked on that side, rather than over them
    if (KisStudioPanelColumn *far = d->farColumn; far && far->isVisible() && !far->isLeft()) {
        const QRect pill(far->mapToGlobal(QPoint()), far->size());
        if (pill.intersects(global)) global.moveRight(pill.left() - 1 - margin);
    }
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
        d->far.remove(id);
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
    // in the rail's order; only redone when the order or a side has changed
    const QStringList order = d->pinnedOrder();
    QString arrangement = d->panelsLeft ? QStringLiteral("L:") : QStringLiteral("R:");
    Q_FOREACH (const QString &id, order) {
        arrangement += (d->far.contains(id) ? QStringLiteral("far ") : QString()) + id + QLatin1Char(',');
    }
    if (arrangement == d->arrangement) {
        updateColumns();
        return;
    }
    d->arrangement = arrangement;

    KisStudioPanelColumn *column = panelColumn();
    KisStudioPanelColumn *far = farColumn();
    if (!column || !far) {
        return;
    }
    QList<QDockWidget *> near;
    QList<QDockWidget *> across;
    Q_FOREACH (const QString &id, order) {
        QDockWidget *dock = d->mainWindow->dockWidget(id);
        if (dock && dock != d->flyout) (d->far.contains(id) ? across : near) << dock;
    }
    Q_FOREACH (QDockWidget *dock, column->panels() + far->panels()) {
        if (!near.contains(dock) && !across.contains(dock)) parkPanel(dock);
    }
    // take them out of the window's dock areas so the canvas extends under the pills
    Q_FOREACH (QDockWidget *dock, near + across) {
        if (dock->parentWidget() != column->stack() && dock->parentWidget() != far->stack()) {
            const bool shown = !dock->isHidden();
            if (dock->isFloating()) dockAway(dock);
            d->mainWindow->removeDockWidget(dock);
            dock->setVisible(shown);
        }
    }
    column->setLeft(d->panelsLeft);
    far->setLeft(!d->panelsLeft);
    column->setPanels(near);
    far->setPanels(across);
    restoreHeights();
    updateColumns();
}

KisStudioPanelColumn *KisStudioLayout::panelColumn()
{
    if (!d->panelColumn) {
        KisConfig cfg(true);
        // fall back to the legacy key
        d->panelColumn = createColumn(d->panelsLeft, cfg.readEntry<int>(columnWidthKey, cfg.readEntry<int>(QStringLiteral("StudioLayoutRightColumnWidth"), 270)),
                                      columnWidthKey);
        if (!d->panelColumn) return nullptr;
        d->panelColumn->moved = [this](bool left) { setPanelsSide(left); };
        d->panelColumn->tapped = [this]() { setPanelsTucked(!d->panelsTucked); };
    }
    if (d->rail) d->panelColumn->setStrip(d->rail);
    return d->panelColumn;
}

KisStudioPanelColumn *KisStudioLayout::farColumn()
{
    if (!d->farColumn) {
        // as wide as the other until its own width is set
        const int width = d->panelColumn ? d->panelColumn->columnWidth() : 270;
        d->farColumn = createColumn(!d->panelsLeft, KisConfig(true).readEntry<int>(farWidthKey, width), farWidthKey);
        if (d->farColumn) d->farColumn->setObjectName(QStringLiteral("studioFarPanels"));
    }
    return d->farColumn;
}

KisStudioPanelColumn *KisStudioLayout::createColumn(bool left, int width, const QString &widthKey)
{
    QMdiArea *area = mdiArea();
    if (!area) return nullptr;
    auto *column = new KisStudioPanelColumn(area->parentWidget(), left, width);
    QPointer<KisStudioPanelColumn> self = column;
    column->resized = [this, self, widthKey](bool done) {
        // refit the views only when a width drag ends; doing it on every move costs most of a frame
        updateCanvasInsets(done);
        if (done && self) KisConfig(false).writeEntry(widthKey, self->columnWidth());
    };
    column->stack()->userHeight = [this](const QDockWidget *dock) { return d->heights.value(dock->objectName(), 0); };
    column->stack()->resizedByUser = [this](QDockWidget *dock, int height, bool done) {
        if (height > 0) d->heights.insert(dock->objectName(), height);
        else d->heights.remove(dock->objectName());
        if (done) saveHeights();
    };
    return column;
}

void KisStudioLayout::setPanelsTucked(bool tucked)
{
    d->panelsTucked = tucked;
    KisConfig(false).writeEntry(panelsHiddenKey, tucked);
    if (d->stripGrip) {
        d->stripGrip->setToolTip(tucked ? i18nc("@info:tooltip", "Click: show panels\nDrag: switch sides")
                                        : i18nc("@info:tooltip", "Drag: switch sides\nClick: hide"));
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
    keepUpright(dock, false);
    dock->hide();
    d->mainWindow->addDockWidget(Qt::RightDockWidgetArea, dock);
    dock->hide();
}

void KisStudioLayout::showNewImagePopup()
{
    auto *popup = new KisStudioPopup(d->mainWindow);
    popup->setAttribute(Qt::WA_DeleteOnClose);
    auto *layout = new QVBoxLayout(popup);
    popup->fitLayout(layout, KisModernStyle::space3);
    QPointer<KisStudioPopup> self = popup;
    layout->addWidget(KisStudioStartPage::createNewTiles(d->mainWindow, popup, [self]() {
        if (self) self->close();
    }));
    popup->adjustSize();
    // centred under the top of the window, where the menu opens
    QWidget *area = d->mainWindow->centralWidget();
    const QRect at(area->mapToGlobal(QPoint()), area->size());
    popup->popupAt(QPoint(at.center().x() - popup->width() / 2, at.top() + 64));
}

void KisStudioLayout::showStartPage(bool show)
{
    if (show && !d->startPage) {
        auto *welcome = d->mainWindow->findChild<KisWelcomePageWidget *>();
        QWidget *viewport = welcome ? welcome->parentWidget() : nullptr;
        auto *scroller = viewport ? qobject_cast<QScrollArea *>(viewport->parentWidget()) : nullptr;
        if (!scroller || scroller->widget() != welcome) return;
        d->classicWelcome = scroller->takeWidget();
        // kept as the window's own, for what still looks it up
        d->classicWelcome->setParent(d->mainWindow);
        d->classicWelcome->hide();
        d->startPage = new KisStudioStartPage(d->mainWindow);
        scroller->setWidget(d->startPage);
        // the window shows it after laying out for it, and raises it over the pills: lay out again once it shows
        d->startPage->installEventFilter(this);
    } else if (!show && d->startPage) {
        QWidget *viewport = d->startPage->parentWidget();
        auto *scroller = viewport ? qobject_cast<QScrollArea *>(viewport->parentWidget()) : nullptr;
        if (scroller) {
            scroller->takeWidget();
            if (d->classicWelcome) {
                scroller->setWidget(d->classicWelcome);
                d->classicWelcome->show();
            }
        }
        delete d->startPage;
    }
}

void KisStudioLayout::placeWelcomePage()
{
    QWidget *welcome = d->startPage ? static_cast<QWidget *>(d->startPage) : d->mainWindow->findChild<KisWelcomePageWidget *>();
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
    for (KisStudioPanelColumn *column : {d->panelColumn.data(), d->farColumn.data()}) {
        if (!column) continue;
        bool any = false;
        Q_FOREACH (QDockWidget *dock, column->panels()) {
            any = any || !dock->isHidden();
        }
        column->setPanelsShown(any && !arePanelsHidden() && !d->panelsTucked);
        // the far column is nothing without its panels; the other keeps its strip
        const bool kept = column == d->panelColumn || column->panelsShown();
        column->setVisible(d->active && kept && (!isRailHidden() || d->peeking.contains(QPointer<QWidget>(column))));
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
    // how far each side's pill reaches in, and whether it has its panels out (rather than only its strip)
    int pillLeft = 0;
    int pillRight = 0;
    int fullLeft = 0;
    int fullRight = 0;
    for (KisStudioPanelColumn *column : {d->panelColumn.data(), d->farColumn.data()}) {
        if (!column || column->isHidden()) continue;
        const bool left = column->isLeft();
        const int reach = left ? column->geometry().right() + 1 : area->width() - column->x();
        // the tool rail sits beside the pill, even when only its strip shows
        (left ? pillLeft : pillRight) = qMax(left ? pillLeft : pillRight, reach);
        if (column->panelsShown()) (left ? fullLeft : fullRight) = qMax(left ? fullLeft : fullRight, reach);
    }
    int top = 0;
    for (TopPill *pill : {d->topPill.data(), d->windowPill.data()}) {
        // a pill only peeking over the canvas doesn't push anything
        if (pill && !pill->isHidden() && !d->peeking.contains(pill)) top = qMax(top, pill->geometry().bottom() + 1);
    }
    // with its panels shown, the pill covers the canvas's side, so the HUD and the view keep clear of it
    const QMargins insets(fullLeft, top, fullRight, 0);
    if (canvasInsets(area) != insets || area->property(leftPillProperty).toInt() != pillLeft
        || area->property(rightPillProperty).toInt() != pillRight) {
        area->setProperty(leftInsetProperty, insets.left());
        area->setProperty(rightInsetProperty, insets.right());
        area->setProperty(topInsetProperty, insets.top());
        for (KisStudioPanelColumn *column : {d->panelColumn.data(), d->farColumn.data()}) {
            if (column) column->place();
        }
        area->setProperty(leftPillProperty, pillLeft);
        area->setProperty(rightPillProperty, pillRight);
        QCoreApplication::postEvent(area, new QEvent(QEvent::LayoutRequest));
        if (d->flyout) positionFlyout();
    }

    // each view fits, zooms and rotates around the part the pills leave uncovered
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
        if (fullLeft) viewInsets.setLeft(qMax(0, area->mapToGlobal(QPoint(fullLeft, 0)).x() - widget.left()));
        if (fullRight) viewInsets.setRight(qMax(0, widget.right() + 1 - area->mapToGlobal(QPoint(area->width() - fullRight, 0)).x()));
        if (view->canvasController()) view->canvasController()->setViewportInsets(viewInsets);
    }
}

void KisStudioLayout::restoreHeights()
{
    // fold once the column holds the panels, then fit them
    QTimer::singleShot(0, this, [this]() {
        Q_FOREACH (const QString &id, d->pinnedOrder()) {
            applyFold(d->mainWindow->dockWidget(id), d->folded.contains(id) && d->pinned.contains(id));
        }
        for (KisStudioPanelColumn *column : {d->panelColumn.data(), d->farColumn.data()}) {
            if (column) column->stack()->later();
        }
    });
}

void KisStudioLayout::saveHeights()
{
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
    // on a flyout, the grip closes it like its rail button does
    if (!d->pinned.contains(id)) {
        if (dock == d->flyout) closeFlyout();
        return;
    }
    // finish a fold still running from a quick previous click
    const QString foldName = QStringLiteral("studioFold_") + id;
    if (auto *running = findChild<QVariantAnimation *>(foldName, Qt::FindDirectChildrenOnly)) {
        running->setObjectName(QString());
        if (running->state() == QAbstractAnimation::Running) running->setCurrentTime(running->duration());
    }
    const bool fold = !d->folded.contains(id);
    if (fold) {
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
    int to;
    if (fold) {
        // let it shrink and show the name at once; the content is squeezed when the animation ends
        content->setProperty("studioMinHeight", content->minimumHeight());
        content->setProperty("studioMaxHeight", content->maximumHeight());
        content->setMinimumHeight(0);
        grip->setFolded(true, dock->windowTitle());
        to = grip->height();
    } else {
        applyFold(dock, false);
        to = d->heights.value(id, qMax(160, dock->sizeHint().height()));
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
        if (auto *stack = target ? dynamic_cast<PanelStack *>(target->parentWidget()) : nullptr) stack->relayout(true);
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
            }
        }
        if (auto *stack = target ? dynamic_cast<PanelStack *>(target->parentWidget()) : nullptr) stack->relayout(true);
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
    // squeeze the content to zero height rather than hiding it, which would narrow the column
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
    if (auto *stack = dynamic_cast<PanelStack *>(dock->parentWidget())) stack->later();
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
    if (drop.far) d->far.insert(id);
    else d->far.remove(id);
    savePinned();
    if (dock == d->flyout) {
        releaseFlyout();
    }
    dockAway(dock);

    // the columns follow the rail's order, so insert it on the rail where it lands in its column
    QStringList panels = d->railPanels;
    panels.removeAll(id);
    int at = drop.before.isEmpty() ? -1 : panels.indexOf(drop.before);
    if (at < 0) {
        Q_FOREACH (QDockWidget *other, d->column(id, drop.far)) {
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
    QStringList far = d->far.values();
    far.sort();
    KisConfig(false).writeEntry(farKey, far);
}

void KisStudioLayout::createRail()
{
    if (d->rail) {
        return;
    }

    KisMainWindow *mainWindow = d->mainWindow;

    QToolBar *rail = new QToolBar(i18nc("@title:window", "Panels"), mainWindow);
    // a new object name, so restoring a window state doesn't claim it as the old window toolbar
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

    // sits between the buttons and the "+", and drags the pill
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
        // the picker may have starred, unstarred or reordered the rail's panels
        connect(picker, &QObject::destroyed, this, [this]() {
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
                // while the panels are tucked away, a button brings them back with its own panel shown
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

    // clicked on the strip itself: panels are added with "+", so only Reset is offered
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
    // close it too, so no panel stays open without a way back to it
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
    // the card edits its shortcut and can take it off the rail
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

        // off the strip: show where the panel would land in the column or on the canvas
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
            // a plain click: let the button, still down, receive the release
            return false;
        }
        button->setDown(false);

        const QPoint release = globalMousePos(static_cast<QMouseEvent *>(event));
        d->hidePanelDrop();
        if (!d->overStrip(release)) {
            // pin it beside the canvas, or take it off the rail if dropped on the canvas
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
            // deferred until the release is delivered, since the buttons are recreated
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
    // a brush tool's chip carries its smoothing, dragged sideways; a click opens the brush editor
    const QPointer<KisMainWindow> window = mainWindow;
    toolChip->amount = [window]() { return KisStudioSmoothing::amount(window); };
    toolChip->setAmount = [window](qreal amount) { KisStudioSmoothing::setAmount(window, amount); };
    toolChip->amountText = [window]() { return KisStudioSmoothing::text(window); };
    toolChip->resetAmount = [window]() { KisStudioSmoothing::reset(window); };
    connect(toolChip, &QToolButton::clicked, this, [this, toolChip]() {
        if (KisStudioStrokePage::applies(d->mainWindow)) {
            KisStudioBrushEditor::toggle(d->mainWindow, toolChip, Qt::BottomEdge);
        } else {
            toggleToolOptions();
        }
    });
    KisStudioSmoothing::watch(mainWindow);
    connect(KisStudioSmoothing::instance(), &KisStudioSmoothing::sigToolOptionsRequested, this, [this](KisMainWindow *window) {
        if (window != d->mainWindow) return;
        if (KisStudioBrushEditor::isOpen(d->mainWindow)) KisStudioBrushEditor::toggle(d->mainWindow, nullptr, Qt::BottomEdge);
        toggleToolOptions();
    });
    connect(KisStudioSmoothing::instance(), &KisStudioSmoothing::sigChanged, toolChip, qOverload<>(&QWidget::update));
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
            // a right click resets to the brush's value from when it was picked
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

    // shows the document's name, with every open document a click away
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

    auto *buttonsBar = new QToolBar(i18nc("@title:window", "Buttons"), mainWindow);
    buttonsBar->setObjectName(QStringLiteral("studioButtonsBar"));
    buttonsBar->setMovable(false);
    buttonsBar->setFloatable(false);
    buttonsBar->setContextMenuPolicy(Qt::PreventContextMenu);
    buttonsBar->addWidget(new KisStudioBarButtons(mainWindow, buttonsBar));
    KisStudioGlass::setSeeThrough(buttonsBar, true);
    d->buttonsBar = buttonsBar;

    // placed on the window pill by placeWindowButtons()
    auto *menuButton = new PillButton(QStringLiteral("studio_menu"), i18nc("@info:tooltip", "Menu"), mainWindow);
    menuButton->setAccessibleName(i18nc("@action:button", "Menu"));
    menuButton->hide();
    connect(menuButton, &QAbstractButton::clicked, this, [this, menuButton]() { showMainMenu(menuButton); });
    d->menuButton = menuButton;

    auto *settingsButton = new PillButton(QStringLiteral("studio_settings"), i18nc("@info:tooltip", "Settings\nRight-click: Quick"), mainWindow);
    settingsButton->hide();
    settingsButton->setProperty("ownContextMenu", true);
    connect(settingsButton, &QAbstractButton::clicked, this, [this]() { openSettings(QString(), true); });
    settingsButton->contextClicked = [this, settingsButton]() {
        if (d->settings && d->active) d->settings->showQuick(settingsButton);
    };
    d->settingsButton = settingsButton;

    auto *field = new PillButton(QStringLiteral("studio_find"), QString(), mainWindow);
    // its key is all its tooltip says
    field->setProperty("keyOnlyTip", true);
    field->setProperty("runsAction", QStringLiteral("find_anything"));
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

    // move the tool bar and brush bar into the top pill; restoring a window state or Configure Toolbars can take the brush bar back
    const bool tools = d->active && !isRailHidden();
    QToolBar *brushBar = d->mainWindow->findChild<QToolBar *>(QStringLiteral("BrushesAndStuff"));
    int index = 1;
    for (QToolBar *bar : {d->topBar.data(), brushBar, d->buttonsBar.data()}) {
        if (!bar) continue;
        if (bar->parentWidget() != d->topPill->contents()) {
            d->mainWindow->removeToolBar(bar);
            bar->setMovable(false);
            KisStudioGlass::setSeeThrough(bar, true);
            d->topPill->insert(index, bar);
        }
        index++;
    }
    // show them after inserting: removeToolBar() hides them, as does restoring a window state with no place for them
    if (d->topBar) d->topBar->setVisible(tools);
    if (d->buttonsBar) d->buttonsBar->setVisible(tools);
    if (brushBar) brushBar->show();
    if (d->windowBar) d->windowBar->show();

    // the window pill shows on the start page too, as its title bar
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
    // cap a tool rail on the left, matching its width
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
    // below the window pill on the right, as tall as the canvas allows
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
    // works with the Studio layout off too, as there is no other settings UI
    ensureSettings();
    if (!d->settings) {
        return;
    }
    if (toggle && d->settings->isVisible() && cardId.isEmpty()) d->settings->hide();
    else d->settings->open(cardId);
}

void KisStudioLayout::showMainMenu(QWidget *button)
{
    // the most used file commands, then the menu bar's menus; rebuilt each time to pick up recent files
    if (!d->mainMenu) {
        d->mainMenu = new QMenu(d->mainWindow);
    }
    QMenu *menu = d->mainMenu;
    menu->clear();
    qDeleteAll(menu->findChildren<QMenu *>(QString(), Qt::FindDirectChildrenOnly));
    KisKActionCollection *actions = d->mainWindow->actionCollection();
    QSet<QAction *> shown;
    auto add = [&](std::initializer_list<const char *> names) {
        for (const char *name : names) {
            if (QAction *action = actions->action(QLatin1String(name))) {
                menu->addAction(action);
                shown.insert(action);
            }
        }
        menu->addSeparator();
    };
    add({"file_new", "file_open", "file_open_recent"});
    add({"file_save", "file_save_as", "file_export_file"});
    Q_FOREACH (QAction *top, d->mainWindow->menuBar()->actions()) {
        if (!top->menu() || !top->isVisible()) continue;
        if (top->menu()->objectName() != QLatin1String("file")) {
            menu->addAction(top);
            continue;
        }
        // File without the commands listed above it
        QMenu *file = menu->addMenu(top->text());
        bool separate = false;
        Q_FOREACH (QAction *action, top->menu()->actions()) {
            if (action->isSeparator()) {
                separate = !file->isEmpty();
            } else if (!shown.contains(action) && action->isVisible() && action->objectName() != QLatin1String("file_quit")) {
                if (separate) file->addSeparator();
                separate = false;
                file->addAction(action);
            }
        }
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
    // add each widget to the window pill's bar once
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
    // brush tools show the brush's name; the tool shows as its icon, with its name on hover
    const bool brushTool = tool == QLatin1String("KritaShape/KisToolBrush") || tool == QLatin1String("KritaShape/KisToolDyna")
        || tool == QLatin1String("KritaShape/KisToolMultiBrush");
    KisViewManager *view = d->mainWindow->viewManager();
    KisCanvasResourceProvider *resources = view ? view->canvasResourceProvider() : nullptr;
    const QString brush = brushTool && resources && resources->currentPreset() ? KisStudioBrushPicker::title(resources->currentPreset()->name()) : QString();
    const QString toolName = KisStudioToolRail::toolName(factory);
    d->toolChip->setText(brush.isEmpty() ? toolName : brush);
    // a right click resets the smoothing, instead of showing the chip's card
    d->toolChip->setProperty("ownContextMenu", bool(KisStudioSmoothing::options(d->mainWindow)));
    if (KisStudioSmoothing::options(d->mainWindow)) {
        d->toolChip->setToolTip(i18nc("@info:tooltip the brush chip: drag sideways for smoothing, click for the brush's settings", "Drag: smoothing\nClick: brush settings\nRight-click: reset"));
    } else if (KisStudioStrokePage::applies(d->mainWindow)) {
        d->toolChip->setToolTip(i18nc("@info:tooltip", "Brush settings"));
    } else {
        d->toolChip->setToolTip(brush.isEmpty() ? i18nc("@info:tooltip", "Tool options") : i18nc("@info:tooltip tool chip: the tool, its options", "%1 options", toolName));
    }
    d->toolChip->update();
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
        // moved into Symmetry on the tool strip
        {"mirror_actions", false},
        {"gradients", gradient},
        {"patterns", fills || shapeTool},
        // the color chip on the rail replaces it
        {"dual", false},
        // set up once rather than per stroke; available in Find
        {"workspaces", false},
        {"select_layout", false},
    };

    if (d->brushValuesAction) {
        d->brushValuesAction->setVisible(paints && (!isRailHidden() || d->peeking.contains(QPointer<QWidget>(d->topPill.data()))));
    }
    updateToolChip();
    if (d->flyout && d->flyoutAnchor) {
        // the new tool's options arrive later, at a different size
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
        // only if released over the chip, so not after a long press that ends elsewhere
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
        QWidget *widget;
        bool near;
    };
    const int reach = 6;
    const QList<Edge> edges = {
        {d->topPill.data(), at.y() < reach},
        {d->brushRail.data(), toolsLeft ? at.x() < reach : at.x() >= area->width() - reach},
        {d->panelColumn.data(), d->panelColumn && d->panelColumn->isLeft() ? at.x() < reach : at.x() >= area->width() - reach},
        {d->farColumn.data(), d->farColumn && d->farColumn->panelsShown() && (d->farColumn->isLeft() ? at.x() < reach : at.x() >= area->width() - reach)},
    };
    for (const Edge &edge : edges) {
        QWidget *widget = edge.widget;
        if (!widget) continue;
        const bool shown = d->peeking.contains(widget);
        // keep it while the pointer is over or near it
        const bool over = shown && widget->geometry().adjusted(-48, -48, 48, 48).contains(widget->parentWidget()->mapFrom(area, at));
        if (edge.near && !shown) {
            d->peeking << widget;
            if (widget == d->topPill) {
                // show it whole: canvas-only mode hides its tool and brush parts
                if (d->topBar) d->topBar->show();
                if (d->buttonsBar) d->buttonsBar->show();
                if (d->toolStripAction && d->toolStrip) d->toolStripAction->setVisible(d->toolStrip->hasOptions());
                updateContextBar();
                d->topPill->place();
            }
            widget->raise();
            KisStudioMotion::showFaded(widget);
        } else if (shown && !edge.near && !over) {
            d->peeking.removeAll(widget);
            KisStudioMotion::hideFaded(widget);
        }
    }
}

bool KisStudioLayout::eventFilter(QObject *watched, QEvent *event)
{
    if ((event->type() == QEvent::MouseMove || event->type() == QEvent::TabletMove) && isCanvasOnly()) {
        // only moves over this window
        auto *widget = qobject_cast<QWidget *>(watched);
        const QPoint global = event->type() == QEvent::MouseMove ? static_cast<QMouseEvent *>(event)->globalPos()
                                                                  : static_cast<QTabletEvent *>(event)->globalPos();
        if (widget && widget->window() == d->mainWindow) peek(global);
    }
    // a text field takes Undo and Redo only while it has typing of its own to undo; otherwise they go to the window
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
    // dock panels before the window state is saved, since restoring a floating panel crashes Qt
    if (watched == d->mainWindow && event->type() == QEvent::Close) {
        if (d->flyout) closeFlyout();
        Q_FOREACH (QDockWidget *dock, d->mainWindow->dockWidgets()) {
            if (dock->isFloating() && dock->isHidden()) dockAway(dock);
        }
    }

    if (watched == d->startPage && d->startPage && (event->type() == QEvent::Show || event->type() == QEvent::Hide)) {
        QTimer::singleShot(0, this, &KisStudioLayout::normalize);
    }

    if (watched == d->brushRail && d->brushRail) {
        const QEvent::Type type = event->type();
        if (type == QEvent::Move || type == QEvent::Resize || type == QEvent::Show || type == QEvent::Hide) {
            placeWelcomePage();
            placeWindowHandle();
        }
    }

    // refit an open flyout once when its contents change size (e.g. options revealed by a checkbox)
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

    // right-clicking or holding any button in the window shows its card, to give it a shortcut
    if (event->type() == QEvent::ContextMenu) {
        auto *button = qobject_cast<QAbstractButton *>(watched);
        if (button && button->window() == d->mainWindow && button->contextMenuPolicy() == Qt::DefaultContextMenu
            && !dynamic_cast<KisSelectionActionsPanelButton *>(button)) {
            bool ownCard = false;
            // these have their own cards or menus, as does the canvas's selection bar
            for (QWidget *w = button; w && !ownCard; w = w->parentWidget()) {
                ownCard = w->inherits("KisStudioToolRail") || w == d->rail || w->property("ownContextMenu").toBool();
            }
            KisStudioFind *find = d->mainWindow->studioFind();
            if (!ownCard && find && find->showCard(button)) {
                return true;
            }
        }
    }

    switch (event->type()) {
    case QEvent::MouseButtonPress:
    case QEvent::TabletPress:
        // a press anywhere else in the window closes the flyout; the rail handles its own buttons
        if (d->flyout && d->flyout->isVisible()) {
            QWidget *widget = qobject_cast<QWidget *>(watched);
            // test by position, since a press the flyout ignores propagates to its parent
            const QPoint at = event->type() == QEvent::MouseButtonPress ? static_cast<QMouseEvent *>(event)->pos()
                                                                         : static_cast<QTabletEvent *>(event)->pos();
            const bool inFlyout = widget && d->flyoutWindow && d->flyoutWindow->isVisible()
                                  && d->flyoutWindow->rect().contains(d->flyoutWindow->mapFromGlobal(widget->mapToGlobal(at)));
            const bool onRail = d->rail && widget && (widget == d->rail || d->rail->isAncestorOf(widget));
            // the chip toggles the tool options itself, and they should follow a tool picked on the tool rail
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
        // also when restored from minimized, as it would have been placed far off
        if (d->flyout && (watched == d->mainWindow || watched == d->mainWindow->centralWidget())) {
            QTimer::singleShot(0, this, &KisStudioLayout::positionFlyout);
        }
        break;

    default:
        break;
    }

    return QObject::eventFilter(watched, event);
}
