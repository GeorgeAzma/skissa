/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioLayout.h"
#include "KisStudioTools.h"

#include <KisSentenceCase.h>
#include <KisResourceItemChooser.h>
#include "KisColorDrop.h"

#include <QAbstractButton>
#include <QTreeView>
#include <QListView>
#include <QAbstractItemView>
#include <QAction>
#include <QElapsedTimer>
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
#include <QActionEvent>
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
#include <limits>

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
#include "KisSkissaUpdater.h"
#include "KisStudioBarButtons.h"
#include "KisStudioBrushTweaks.h"
#include "KisStudioBrushEditor.h"
#include "KisStudioMakeBrush.h"
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

/// Menu items only the classic layout has: toolbars, styles and themes of their own, a status bar, tiled windows; and
/// the resource database's explorer, a developer's tool
const QStringList &classicOnlyActions()
{
    static const QStringList names = {QStringLiteral("options_configure_toolbars"), QStringLiteral("lock_toolbars"), QStringLiteral("dbexplorer"),
                                      QStringLiteral("toolbars_submenu_action"), QStringLiteral("style_menu"),
                                      QStringLiteral("theme_menu"), QStringLiteral("showStatusBar"), QStringLiteral("windows_tile"),
                                      QStringLiteral("windows_cascade"), QStringLiteral("windows_next"),
                                      QStringLiteral("windows_previous"), QStringLiteral("help_about_kde")};
    return names;
}

/// Whether \p menu shows anything: a menu whose submenus are all empty (Scripts without scripts) leads nowhere
bool hasContent(const QMenu *menu)
{
    Q_FOREACH (QAction *action, menu->actions()) {
        if (!action->isVisible() || action->isSeparator()) continue;
        if (!action->menu() || hasContent(action->menu())) return true;
    }
    return false;
}

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
/// The pinned panels in floating groups of their own, as "id=group" entries (groups from 2 on)
const QString groupsKey = QStringLiteral("StudioLayoutPanelGroups");
/// The pinned panels in a lane of their group's other than the first, as "id=lane" entries
const QString lanesKey = QStringLiteral("StudioLayoutPanelLanes");
/// A floating group's own setting: Spot, Width or Side
QString groupKey(int group, const char *what)
{
    return QStringLiteral("StudioLayoutPanelGroup%1%2").arg(group).arg(QLatin1String(what));
}
/// "left" or "right"
const QString panelSideKey = QStringLiteral("StudioLayoutPanelSide");
const QString toolRailSideKey = QStringLiteral("StudioLayoutToolRailSide");
const QString panelsHiddenKey = QStringLiteral("StudioLayoutPanelsTucked");
const QString toolRailHiddenKey = QStringLiteral("StudioLayoutToolRailTucked");
const QString topBarHiddenKey = QStringLiteral("StudioLayoutTopBarTucked");
/// Where each pill was dragged to: "across,down", as shares of the room it has (see shareOf()); across is from its own side
const QString toolRailSpotKey = QStringLiteral("StudioLayoutToolRailSpot");
const QString panelSpotKey = QStringLiteral("StudioLayoutPanelSpot");
const QString farSpotKey = QStringLiteral("StudioLayoutFarPanelSpot");
const QString topBarSpotKey = QStringLiteral("StudioLayoutTopBarSpot");
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

    /// The only panel of its group, whose grip moves and folds it: no bar of its own (but its name when folded).
    /// Hidden, a dock's title bar would still keep its room.
    bool isAlone() const { return m_alone; }
    void setAlone(bool alone)
    {
        if (alone == m_alone) return;
        m_alone = alone;
        fit();
        update();
    }

    // its height is fixed (see fit()), and right after it shows again its geometry may not be yet
    QSize sizeHint() const override { return QSize(40, minimumHeight()); }

protected:
    void fit()
    {
        // taller in touch mode, for fingers
        const bool touch = KisTouchMode::isActive();
        setFixedHeight(m_folded ? (touch ? 32 : 22) : m_alone ? 0 : (touch ? 22 : 10));
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
    bool m_alone {false};
    QString m_title;
};

/// Where a dragged panel would land: in the column, or onto the canvas if \c column is false
struct PanelDrop {
    bool column {false};
    bool left {false};
    /// The group it goes into: 0 the column with the panel buttons, 1 the one across the canvas, then floating ones
    int group {0};
    /// Into a new floating group, there (main window coordinates)
    bool fresh {false};
    /// The lane it goes into, left to right; a new one there if \c newLane
    int lane {0};
    bool newLane {false};
    QPoint at;
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
const char *const bottomInsetProperty = "studioCanvasBottomInset";
/// Set on widgets whose empty space drags the window, like a title bar
const char *const captionProperty = "studioCaption";

QMargins canvasInsets(const QWidget *canvasArea)
{
    if (!canvasArea) return QMargins();
    return QMargins(canvasArea->property(leftInsetProperty).toInt(), canvasArea->property(topInsetProperty).toInt(),
                    canvasArea->property(rightInsetProperty).toInt(), canvasArea->property(bottomInsetProperty).toInt());
}

/**
 * Where a pill that can be dragged anywhere sits along one axis: the share of the room it can move in,
 * 0 against the near edge and 1 against the far one. Dropped near an edge or the middle, it is caught
 * there (so corners are easy to hit), and it stays there as the window changes size.
 */
qreal shareOf(int offset, int room)
{
    const int snap = 20;
    if (room <= 0 || offset < snap) return 0;
    if (offset > room - snap) return 1;
    if (qAbs(offset - room / 2) < snap / 2) return 0.5;
    return qreal(offset) / room;
}

int offsetOf(qreal share, int room)
{
    return qRound(qBound(0.0, share, 1.0) * qMax(0, room));
}

/// A pill's place as "across,down" shares (see shareOf())
QPointF readSpot(const QString &key)
{
    const QStringList parts = KisConfig(true).readEntry<QString>(key, QString()).split(QLatin1Char(','));
    if (parts.size() < 2) return QPointF();
    return QPointF(qBound(0.0, parts[0].toDouble(), 1.0), qBound(0.0, parts[1].toDouble(), 1.0));
}

/// The height a group of panels had when its place was taken, written after its spot; 0 if not
int readSpotHeight(const QString &key)
{
    const QStringList parts = KisConfig(true).readEntry<QString>(key, QString()).split(QLatin1Char(','));
    return parts.size() >= 3 ? qMax(0, parts[2].toInt()) : 0;
}

/// \p height: the height of a group of panels when its place was taken, so a folded one keeps its top
void writeSpot(const QString &key, const QPointF &spot, int height = 0)
{
    QString value = QString::number(spot.x()) + QLatin1Char(',') + QString::number(spot.y());
    if (height > 0) value += QLatin1Char(',') + QString::number(height);
    KisSettingsRegistry::writeLater(key, value);
}

/// How far the pinned panels' pill reaches in from the left and right edges, even when only its strip shows
const char *const leftPillProperty = "studioPanelPillLeft";
const char *const rightPillProperty = "studioPanelPillRight";
/// Where the window's handle in the top left corner ends, or 0 while it is hidden
const char *const handleBottomProperty = "studioWindowHandleBottom";
/// Where the tool rail is over the canvas area, or empty while it is hidden: the panels keep off it
const char *const railRectProperty = "studioToolRailRect";

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
        setToolTip(i18nc("@info:tooltip", "Drag: move"));
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

/// The top bar's grip: dragging moves the bar, a tap tucks it away
class TopGrip : public DragGrip
{
public:
    std::function<void()> tapped;
    std::function<void(const QPoint &globalPos)> pressed;
    std::function<void(const QPoint &globalPos)> dragged;
    std::function<void()> dropped;

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
        m_dragging = false;
        if (pressed) pressed(m_pressPos);
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (!m_pressed || !(event->buttons() & Qt::LeftButton)) return;
        const QPoint pos = globalMousePos(event);
        if (!m_dragging && (pos - m_pressPos).manhattanLength() < QApplication::startDragDistance()) return;
        m_dragging = true;
        setCursor(Qt::ClosedHandCursor);
        if (dragged) dragged(pos);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (!m_pressed || event->button() != Qt::LeftButton) return;
        m_pressed = false;
        if (m_dragging) {
            m_dragging = false;
            setCursor(Qt::OpenHandCursor);
            if (dropped) dropped();
            return;
        }
        if (rect().contains(event->pos()) && tapped) tapped();
    }

private:
    QPoint m_pressPos;
    bool m_pressed {false};
    bool m_dragging {false};
};

/// The handle between two stacked panels: drag to resize the one above, right-click to fit it to its contents
class PanelHandle : public QWidget
{
public:
    std::function<void()> pressed;
    /// Moved \p dy pixels since the press; \p done on release
    std::function<void(int dy, bool done)> dragged;
    std::function<void()> fit;
    /// Let go without moving; none under the last panel
    std::function<void()> tapped;

    explicit PanelHandle(QWidget *parent)
        : QWidget(parent)
    {
        setAttribute(Qt::WA_Hover);
        setCursor(Qt::SizeVerCursor);
        setBetween(false);
    }

    /// Between two panels, where a tap takes them apart, or under the last
    void setBetween(bool between)
    {
        if (between == m_between && !toolTip().isEmpty()) return;
        m_between = between;
        setToolTip(between ? i18nc("@info:tooltip the handle between two panels", "Drag: resize\nClick: separate\nRight-click: fit")
                           : i18nc("@info:tooltip the handle under the last panel", "Drag: resize\nRight-click: fit"));
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
        m_moved = false;
        m_pressY = globalMousePos(event).y();
        update();
        if (pressed) pressed();
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (!m_pressed) return;
        const int dy = globalMousePos(event).y() - m_pressY;
        // a tap (or a pen's wobble) isn't a resize
        if (!m_moved && qAbs(dy) < QApplication::startDragDistance()) return;
        m_moved = true;
        if (dragged) dragged(dy, false);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (!m_pressed || event->button() != Qt::LeftButton) return;
        m_pressed = false;
        update();
        if (m_moved) {
            if (dragged) dragged(globalMousePos(event).y() - m_pressY, true);
        } else if (m_between && tapped) {
            tapped();
        }
    }

    void contextMenuEvent(QContextMenuEvent *event) override
    {
        event->accept();
        if (fit) fit();
    }

private:
    bool m_pressed {false};
    bool m_moved {false};
    bool m_between {false};
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
            if (!chooser->property("studioResponsive").isValid()) chooser->setProperty("studioResponsive", chooser->isResponsive());
            // a list again first: a responsive chooser lays itself out by its height when resized
            if (chooser->isResponsive() && chooser->height() <= 100) {
                const QSize before = chooser->size();
                chooser->resize(chooser->width(), 200);
                QResizeEvent resized(chooser->size(), before);
                QCoreApplication::sendEvent(chooser, &resized);
            }
            chooser->setResponsiveness(false);
        } else if (chooser->property("studioResponsive").isValid()) {
            // as it was before it was in a column
            chooser->setResponsiveness(chooser->property("studioResponsive").toBool());
            chooser->setProperty("studioResponsive", QVariant());
        }
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

/// How tall \p area's contents are: what it shows without scrolling; -1 if that can't be told. Past \p enough,
/// just that: a tree's rows are walked one by one
int contentsHeight(QAbstractScrollArea *area, int enough = std::numeric_limits<int>::max())
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
    const QRect top = view->visualRect(first);
    if (auto *tree = qobject_cast<QTreeView *>(view)) {
        for (QModelIndex i = first; i.isValid(); i = tree->indexBelow(i)) {
            last = i;
            if (enough < std::numeric_limits<int>::max() && top.isValid() && view->visualRect(i).bottom() - top.top() + 1 >= enough) {
                return enough;
            }
        }
    }
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
    /// Height of the handle under each panel; taller for fingers
    static int gap() { return KisTouchMode::isActive() ? 12 : 6; }
    /// A scrolling panel keeps at least this much of its contents
    static constexpr int minimumContents = 56;
    /// The height a scrolling panel is first given is its contents', within these: a short list has room to
    /// grow into, and a long one (measured perhaps before it was filtered) doesn't take the whole column
    static constexpr int settledMinimum = 160;
    static constexpr int settledMaximum = 360;

    /// A panel's height, set by its handle or when it was first laid out; 0: not set yet
    std::function<int(const QDockWidget *)> userHeight;
    /// A panel that scrolls was laid out for the first time and fitted to its contents: its height from now on
    std::function<void(QDockWidget *dock, int height)> settled;
    /// Its handle moved: \p height for \p dock, 0 to fit it again; \p done once let go
    std::function<void(QDockWidget *dock, int height, bool done)> resizedByUser;
    /// How tall the stack would like to be has changed
    std::function<void()> wantsHeight;
    /// The handle under \p above was tapped: the panels below it go apart
    std::function<void(QDockWidget *above)> separated;
    /// One of its panels was shown or hidden
    std::function<void()> shownChanged;

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
        // a handle under the last one too, so it can be made taller
        return total + shown * gap();
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
        // none on the only panel of a group, whose grip moves and folds it instead (see PanelGrip::setAlone())
        const int titleHeight = title && !title->isHidden() ? qMax(title->height(), title->minimumHeight()) : 0;
        if (dock->widget() && dock->widget()->property("studioFolded").toBool()) {
            m.want = m.minimum = titleHeight;
            return m;
        }
        int natural;
        QAbstractScrollArea *area = mainScrollArea(dock);
        int byHand = userHeight ? userHeight(dock) : 0;
        // with its height set, only whether it has a little to show counts: a long list (layers, the history
        // after every stroke) isn't walked row by row
        const int contents = area ? contentsHeight(area, byHand > 0 ? minimumContents : std::numeric_limits<int>::max()) : -1;
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
        // a panel that scrolls keeps the height it first fitted, so what grows in it (layers, brushes) scrolls
        // instead of pushing the panels below it down; its handle sets another, a right click fits it again
        if (byHand <= 0 && area && contents >= 0 && dock->height() > titleHeight && settled) {
            byHand = qBound(settledMinimum, natural, settledMaximum);
            settled(dock, byHand);
        }
        m.byHand = byHand > 0;
        m.want = m.byHand ? byHand : natural;
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
        int total = shown.size() * gap();
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
            // only when it moves: a geometry set again can make a panel ask for another layout
            const QRect place(0, y, width(), sizes[i].want);
            if (dock->geometry() != place) dock->setGeometry(place);
            y += sizes[i].want;
            PanelHandle *handle = m_handles[i];
            handle->setVisible(true);
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
            handle->setBetween(i < shown.size() - 1);
            // after the tap is done with: taking the panels apart moves them to another stack
            handle->tapped = [this, target]() {
                QTimer::singleShot(0, this, [this, target]() {
                    if (target && separated) separated(target);
                });
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
        bool eventFilter(QObject *watched, QEvent *event) override
        {
            // a panel since moved to another column, or parked, is that one's
            if (watched->parent() != m_stack) return false;
            switch (event->type()) {
            case QEvent::Show:
            case QEvent::Hide:
            case QEvent::LayoutRequest:
                m_stack->later();
                break;
            case QEvent::ShowToParent:
            case QEvent::HideToParent:
                // also while its group is hidden (it has no other panel shown), when Show doesn't come
                m_stack->later();
                if (m_stack->shownChanged) m_stack->shownChanged();
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
    /// Called when dragged and dropped: the side of the canvas it is on and its place from that side (see shareOf())
    std::function<void(bool left, const QPointF &spot)> moved;
    std::function<void()> tapped;
    /// It was placed somewhere else (see railRectProperty)
    std::function<void()> reachChanged;

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

    void setPlace(bool left, const QPointF &spot)
    {
        m_left = left;
        m_spot = spot;
        place();
    }

    /// When tucked, only the grip shows, as wide as the rail was
    void setTucked(bool tucked)
    {
        if (tucked && !m_tucked) {
            m_grip->setFixedWidth(qMax(m_grip->width(), 30));
            m_openHeight = layout()->sizeHint().height();
        }
        if (!tucked) {
            m_grip->setMinimumWidth(0);
            m_grip->setMaximumWidth(QWIDGETSIZE_MAX);
        }
        m_tucked = tucked;
        for (int i = 0; i < layout()->count(); i++) {
            QWidget *widget = layout()->itemAt(i)->widget();
            if (widget && widget != m_grip) widget->setVisible(!tucked);
        }
        m_grip->setToolTip(tucked ? i18nc("@info:tooltip", "Click: show tools\nDrag: move")
                                  : i18nc("@info:tooltip", "Drag: move\nClick: hide"));
        place();
    }

    void place()
    {
        const QRect room = this->room();
        const int across = offsetOf(m_spot.x(), room.width());
        move(m_left ? room.left() + across : room.left() + room.width() - across, room.top() + offsetOf(m_spot.y(), room.height()));
        KisStudioLayout::keepAbove(this);
        updateReach();
    }

    /// Tells the panels where it is, so they keep off it: where it lands, not on the way while dragged
    void updateReach()
    {
        QWidget *area = parentWidget();
        const QRect at = isVisibleTo(area) ? geometry() : QRect();
        if (area->property(railRectProperty).toRect() == at) return;
        area->setProperty(railRectProperty, at);
        if (!m_moving && reachChanged) reachChanged();
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
            m_pressed = true;
            m_moving = false;
            m_grab = event->pos();
            m_pressPos = globalMousePos(event);
            m_onGrip = m_grip->geometry().contains(event->pos());
            event->accept();
            return;
        }
        QWidget::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (!m_pressed) return;
        const QPoint global = globalMousePos(event);
        if (!m_moving) {
            if ((global - m_pressPos).manhattanLength() < QApplication::startDragDistance()) return;
            m_moving = true;
            // what is carried goes over the other overlays
            raise();
            setCursor(Qt::ClosedHandCursor);
            update();
        }
        // caught by the edges and the middle as it goes, so it shows where it will stay
        const QWidget *area = parentWidget();
        const QPoint at = area->mapFromGlobal(global) - m_grab;
        const QRect room = this->room();
        m_left = at.x() + width() / 2 < area->width() / 2;
        const int across = m_left ? at.x() - room.left() : room.left() + room.width() - at.x();
        m_spot = QPointF(shareOf(across, room.width()), shareOf(at.y() - room.top(), room.height()));
        place();
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (!m_pressed || event->button() != Qt::LeftButton) {
            QWidget::mouseReleaseEvent(event);
            return;
        }
        m_pressed = false;
        if (!m_moving) {
            if (m_onGrip && tapped) tapped();
            return;
        }
        m_moving = false;
        unsetCursor();
        update();
        if (moved) moved(m_left, m_spot);
        if (reachChanged) reachChanged();
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        paintGlass(painter, this, 14, m_moving ? 0.3 : 0.12);
    }

    void showEvent(QShowEvent *event) override
    {
        QWidget::showEvent(event);
        updateReach();
    }

    void resizeEvent(QResizeEvent *event) override
    {
        QWidget::resizeEvent(event);
        updateReach();
    }

    void hideEvent(QHideEvent *event) override
    {
        QWidget::hideEvent(event);
        updateReach();
    }

private:
    /// Where its corner can go, as offset and extent: the whole width (the panels keep off it), clear of the bars
    QRect room() const
    {
        const QWidget *area = parentWidget();
        const int top = area->property(topInsetProperty).toInt();
        const int bottom = area->property(bottomInsetProperty).toInt();
        const int minX = 10;
        const int maxX = area->width() - 10 - width();
        // below the window's handle too, which stays when the top bar goes elsewhere
        const int handle = area->property(handleBottomProperty).toInt();
        const int minY = qMax(top > 0 ? top + 6 : 10, handle > 0 ? handle + 6 : 0);
        // placed as if open, so the grip stays where it was when tucked
        const int maxY = area->height() - (bottom > 0 ? bottom + 6 : 10) - (m_tucked ? qMax(height(), m_openHeight) : height());
        return QRect(minX, minY, qMax(0, maxX - minX), qMax(0, maxY - minY));
    }

    DragGrip *m_grip {nullptr};
    bool m_left {true};
    QPointF m_spot;
    bool m_pressed {false};
    bool m_moving {false};
    bool m_tucked {false};
    int m_openHeight {0};
    bool m_onGrip {false};
    QPoint m_grab;
    QPoint m_pressPos;
};

/**
 * A glass pill over the canvas, sized to its contents: the tool and brush bars
 * (along the top unless dragged elsewhere by its grip), or the window's buttons
 * in the top right corner. Its empty space drags the window like a title bar.
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
            m_grip->pressed = [this](const QPoint &globalPos) { m_grab = mapFromGlobal(globalPos); };
            m_grip->dragged = [this](const QPoint &globalPos) { dragTo(globalPos); };
            m_grip->dropped = [this]() { if (moved) moved(m_spot); };
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
        if (tucked && m_contents->isVisibleTo(this)) m_openWidth = layout()->sizeHint().width();
        m_tucked = tucked;
        m_contents->setVisible(!tucked);
        keepHeight();
        if (m_grip) {
            m_grip->setToolTip(tucked ? i18nc("@info:tooltip", "Click: show the bar\nDrag: move")
                                      : i18nc("@info:tooltip", "Drag: move\nClick: hide"));
        }
    }

    void place()
    {
        QWidget *area = parentWidget();
        if (m_right) {
            move(area->width() - margin - width(), margin);
        } else {
            const int y = margin + offsetOf(m_spot.y(), roomY());
            const QPair<int, int> span = spanAt(y);
            move(span.first + offsetOf(m_spot.x(), span.second - span.first), y);
        }
        KisStudioLayout::keepAbove(this);
    }

    /// Where it was dragged to, as shares of the room it has (see shareOf())
    void setSpot(const QPointF &spot)
    {
        m_spot = spot;
        place();
    }
    QPointF spot() const { return m_spot; }

    /// Whether it sits along the top of the canvas, or else along the bottom
    bool isAtTop() const { return m_spot.y() < 0.5; }

    /// Space in the top row reserved for the window handle on the left and the window's buttons on the right
    void setInsets(int left, int right)
    {
        if (left == m_leftInset && right == m_rightInset) return;
        m_leftInset = left;
        m_rightInset = right;
        place();
    }

    /// Called when resized, moved, shown or hidden, so whatever is placed around it can follow
    std::function<void()> changed;
    /// Called when dropped after a drag by its grip
    std::function<void(const QPointF &spot)> moved;

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
    int roomY() const { return parentWidget()->height() - 2 * margin - height(); }

    /// The span its left edge can take at height \p y; in the top row, between the window handle and the window's buttons
    QPair<int, int> spanAt(int y) const
    {
        const bool topRow = y < margin + height();
        const int from = margin + (topRow ? m_leftInset : 0);
        // placed as if open, so the grip stays where it was when tucked
        const int width = m_tucked ? qMax(this->width(), m_openWidth) : this->width();
        return {from, qMax(from, parentWidget()->width() - margin - (topRow ? m_rightInset : 0) - width)};
    }

    /// Follows the grip, caught by the edges and the middle as it goes
    void dragTo(const QPoint &globalPos)
    {
        const QPoint at = parentWidget()->mapFromGlobal(globalPos) - m_grab;
        const qreal down = shareOf(at.y() - margin, roomY());
        const QPair<int, int> span = spanAt(margin + offsetOf(down, roomY()));
        m_spot = QPointF(shareOf(at.x() - span.first, span.second - span.first), down);
        place();
        if (changed) changed();
    }

    bool m_right {false};
    int m_leftInset {0};
    int m_rightInset {0};
    bool m_tucked {false};
    int m_openWidth {0};
    QPointF m_spot;
    QPoint m_grab;
    TopGrip *m_grip {nullptr};
    QWidget *m_contents {nullptr};
};

/// The application icon in the top-left corner, capping the tool rail: a click goes home, a drag moves the window
class WindowHandle : public QWidget
{
public:
    std::function<void()> clicked;

    explicit WindowHandle(QWidget *canvasArea)
        : QWidget(canvasArea)
    {
        setObjectName(QStringLiteral("studioWindowHandle"));
        setToolTip(i18nc("@info:tooltip the app's icon in the window's corner: your artworks", "Home\nDrag: move\nDouble-click: maximize"));
        hide();
    }

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton) return;
        m_press = event->globalPos();
        m_pressed = true;
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (!m_pressed || (event->globalPos() - m_press).manhattanLength() < 5) return;
        m_pressed = false;
        if (QWindow *window = this->window()->windowHandle()) window->startSystemMove();
    }

    void mouseReleaseEvent(QMouseEvent *) override
    {
        if (!m_pressed) return;
        m_pressed = false;
        if (clicked) clicked();
    }

    void mouseDoubleClickEvent(QMouseEvent *) override
    {
        m_pressed = false;
        QWidget *top = window();
        if (top->isMaximized()) top->showNormal();
        else top->showMaximized();
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        paintGlass(painter, this, 12, 0.12);
        const int side = qMin(20, qMin(width(), height()) - 12);
        painter.setOpacity(0.85);
        KisIconUtils::loadIcon(QStringLiteral("krita-branding")).paint(&painter, QRect(QPoint(), QSize(side, side)).translated(rect().center() - QPoint(side / 2, side / 2)));
    }

private:
    QPoint m_press;
    bool m_pressed {false};
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

/// The line between two lanes of a group of panels: dragged, it sets their widths; a click takes them apart
class LaneDivider : public QWidget
{
public:
    static constexpr int thickness = 7;

    std::function<void()> pressed;
    /// How far it was dragged since the press
    std::function<void(int dx)> dragged;
    std::function<void()> released;
    std::function<void()> clicked;

    explicit LaneDivider(QWidget *parent)
        : QWidget(parent)
    {
        setObjectName(QStringLiteral("studioPanelsLanes"));
        setAttribute(Qt::WA_Hover);
        setCursor(Qt::SizeHorCursor);
        setFixedWidth(thickness);
        setToolTip(i18nc("@info:tooltip the line between groups of panels joined side by side", "Joined\nDrag: resize\nClick: separate"));
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        QColor ink = palette().color(QPalette::WindowText);
        ink.setAlphaF(underMouse() || m_pressed ? 0.5 : 0.22);
        const int x = width() / 2;
        painter.fillRect(QRect(x, KisModernStyle::space2, 1, height() - 2 * KisModernStyle::space2), ink);
    }

    bool event(QEvent *event) override
    {
        if (event->type() == QEvent::HoverEnter || event->type() == QEvent::HoverLeave) update();
        return QWidget::event(event);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton) {
            QWidget::mousePressEvent(event);
            return;
        }
        m_pressed = true;
        m_dragging = false;
        m_pressX = globalMousePos(event).x();
        if (pressed) pressed();
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (!m_pressed) return;
        const int dx = globalMousePos(event).x() - m_pressX;
        if (!m_dragging && qAbs(dx) < QApplication::startDragDistance()) return;
        m_dragging = true;
        if (dragged) dragged(dx);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton || !m_pressed) return;
        m_pressed = false;
        update();
        if (m_dragging) {
            if (released) released();
        } else if (clicked) {
            clicked();
        }
    }

private:
    bool m_pressed {false};
    bool m_dragging {false};
    int m_pressX {0};
};

} // namespace

/**
 * The pinned panels in a glass pill over the canvas, below the strip of panel
 * buttons (or a grip), in lanes side by side, each a stack of panels. Dragging
 * the strip moves it: against an edge it is docked there and the canvas makes
 * room for it, anywhere else it floats; dropped against another group, it
 * joins it (see joining). Dragging the inner edge sets the width; the line
 * between two lanes sets theirs, and a click on it takes them apart.
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
    /// Called when dragged by the strip and dropped, or resized: the side of the canvas it is on and its place from that side
    std::function<void(bool left, const QPointF &spot)> moved;
    std::function<void()> tapped;
    /// Each lane's stack as it is made, to set up
    std::function<void(PanelStack *stack)> stackMade;
    /// Dragged by its strip to \p placed (as if open) and, if \p dropped, let go there: whether it would join
    /// another group there, which it then does
    std::function<bool(KisStudioPanelColumn *column, const QRect &placed, bool dropped)> joining;
    /// The line before \p lane was clicked: the lanes from it on go apart
    std::function<void(int lane)> separated;
    /// The handle under \p above in \p lane was tapped: the panels below it go apart
    std::function<void(int lane, QDockWidget *above)> separatedBelow;
    /// Its group (see groupColumn())
    int group {0};

    KisStudioPanelColumn(QWidget *canvasArea, bool left, const QVector<int> &widths)
        : QWidget(canvasArea)
        , m_settled(++settledCount())
        , m_left(left)
        , m_widths(widths.isEmpty() ? QVector<int>{270} : widths)
    {
        setObjectName(QStringLiteral("studioPanels"));
        m_layout = new QVBoxLayout(this);
        m_layout->setSpacing(0);
        // it is sized by place(): a minimum kept from when its panels showed would keep it from tucking
        m_layout->setSizeConstraint(QLayout::SetNoConstraint);
        m_lanes = new QHBoxLayout();
        m_lanes->setSpacing(0);
        m_layout->addLayout(m_lanes);
        updateMargins();
        setMouseTracking(true);
        // a separate widget, since a cursor set on the column would also apply to the panels
        m_grip = new QWidget(this);
        m_grip->setObjectName(QStringLiteral("studioPanelsEdge"));
        m_grip->setCursor(Qt::SizeHorCursor);
        m_grip->setAttribute(Qt::WA_NoSystemBackground);
        m_grip->installEventFilter(this);
        canvasArea->installEventFilter(this);
        m_glide.setDuration(KisModernStyle::changeDuration);
        m_glide.setEasingCurve(KisModernStyle::motionCurve());
        QObject::connect(&m_glide, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
            setGeometry(m_target.translated(value.toPoint()));
        });
        hide();
    }

    /// Its lanes' stacks, left to right
    QVector<PanelStack *> stacks() const { return m_stacks; }
    /// The width of its first lane (a new group's, until set)
    int columnWidth() const { return m_widths.value(0, 270); }
    /// Each lane's width, left to right, with what is outside its stack
    QVector<int> laneWidths() const { return m_widths; }
    bool isLeft() const { return m_left; }
    /// Against its side's edge, rather than floating over the canvas
    bool isDocked() const { return m_spot.x() <= 0; }
    /// Where it is going, or is when it isn't gliding
    QRect targetGeometry() const { return m_target.isNull() ? geometry() : m_target; }

    /// Where it was dragged to, from its side, as shares of the room it has (see shareOf())
    void setSpot(const QPointF &spot, int refHeight = 0)
    {
        m_spot = spot;
        m_refHeight = refHeight;
        place();
    }
    /// Its height when its place was taken (see place()), 0 if not known
    int refHeight() const { return m_refHeight; }

    /// Takes the place of \p box (canvas area coordinates) from its top and its side's edge, as if dropped there
    void placeAt(const QRect &box)
    {
        const bool left = box.center().x() < parentWidget()->width() / 2;
        setSide(left);
        const QSize size = placedSize(sizeNow());
        m_spot = spotOf(QRect(QPoint(left ? box.left() : box.right() + 1 - size.width(), box.top()), size), left, false);
        m_refHeight = size.height();
        m_settled = ++settledCount();
        m_chosen.start();
        place();
        if (moved) moved(m_left, m_spot);
    }

    void setStrip(QWidget *strip)
    {
        if (m_strip == strip) return;
        m_strip = strip;
        m_layout->insertWidget(0, strip);
    }

    void setLeft(bool left)
    {
        setSide(left);
        place();
    }

    /// Shows the panels below the strip, or only the strip
    void setPanelsShown(bool shown)
    {
        m_shownPanels = shown;
        for (PanelStack *stack : qAsConst(m_stacks)) stack->setVisible(shown);
        for (LaneDivider *divider : qAsConst(m_dividers)) divider->setVisible(shown);
        m_layout->invalidate();
        place();
        placeGrip();
    }
    bool panelsShown() const { return m_shownPanels; }

    QList<QDockWidget *> panels() const
    {
        QList<QDockWidget *> docks;
        for (PanelStack *stack : m_stacks) docks += stack->panels();
        return docks;
    }

    /// Takes \p lanes of panels, left to right, each top to bottom keeping its visibility, \p widths wide
    void setLanes(const QList<QList<QDockWidget *>> &lanes, const QVector<int> &widths)
    {
        // always a lane, perhaps empty: the column with the panel buttons has one without panels
        const int count = qMax(1, int(lanes.size()));
        while (m_stacks.size() < count) {
            auto *stack = new PanelStack(this);
            stack->wantsHeight = [this]() { place(); };
            stack->separated = [this, stack](QDockWidget *above) {
                if (separatedBelow) separatedBelow(m_stacks.indexOf(stack), above);
            };
            if (stackMade) stackMade(stack);
            stack->setVisible(m_shownPanels);
            m_stacks << stack;
        }
        for (int i = 0; i < count; i++) m_stacks[i]->setPanels(lanes.value(i));
        while (m_stacks.size() > count) {
            PanelStack *stack = m_stacks.takeLast();
            // a panel it still holds is on its way to another group's lane (taken later in the same arranging):
            // it stays as shown as it was, rather than going with the stack
            for (QDockWidget *dock : stack->findChildren<QDockWidget *>(QString(), Qt::FindDirectChildrenOnly)) {
                const bool shown = !dock->isHidden();
                dock->setParent(parentWidget());
                dock->setVisible(shown);
            }
            stack->deleteLater();
        }
        m_widths = widths;
        while (m_widths.size() < count) m_widths << columnWidth();
        m_widths.resize(count);
        rebuildLanes();
        place();
    }

    /// The canvas area's part a lane covers
    QRect laneRect(int lane) const
    {
        if (lane < 0 || lane >= m_stacks.size()) return QRect();
        // the first and the last reach the column's edges
        const QRect stack = m_stacks[lane]->geometry();
        const int left = lane == 0 ? 0 : stack.x();
        const int right = lane == m_stacks.size() - 1 ? width() : stack.right() + 1;
        return QRect(mapToParent(QPoint(left, 0)), QSize(right - left, height()));
    }

    /// Places it over the canvas area, as tall as its tallest lane wants within it (just the strip without them)
    void place()
    {
        QWidget *area = parentWidget();
        const QPair<int, int> span = spanY();
        applyWidths();
        // with only the strip showing, placed as if open, so the strip stays where it was; while the group is
        // dragged its panels don't change, so they are measured once rather than at every move
        if (!m_moving || m_movingSize.isEmpty()) m_movingSize = sizeNow();
        const QSize size = m_movingSize;
        const QSize placed = placedSize(size);
        const int across = offsetOf(m_spot.x(), area->width() - 2 * margin - placed.width());
        // the height its place was taken at: as it grows or folds it keeps its top, or against the bottom its bottom
        // a place saved without it gets the height it settled at, kept with it from then on
        const bool learnt = m_refHeight <= 0 && isVisible() && m_shown.isValid() && m_shown.elapsed() >= 500;
        if (learnt) m_refHeight = placed.height();
        const int refHeight = m_refHeight > 0 ? m_refHeight : placed.height();
        int y = span.first + offsetOf(m_spot.y(), span.second - span.first - refHeight);
        if (m_spot.y() >= 1) y = span.second - placed.height();
        y = qMax(span.first, qMin(y, span.second - placed.height()));
        const QRect wanted(QPoint(m_left ? margin + across : area->width() - margin - placed.width() - across, y), placed);
        // while dragged, it makes way for all the others, as it will once dropped: it shows where it will stay
        const QRect at = clearOfOthers(wanted, span, m_moving ? std::numeric_limits<quint64>::max() : m_settled);
        m_placed = at;
        // the group placed last made way by choice: where it went is its place, so the next start agrees; made
        // to make way later (the window smaller), it goes back once there is room
        bool madeWay = false;
        if (!m_moving && at != wanted && m_settled == settledCount() && isVisible() && m_shown.isValid() && m_shown.elapsed() >= 500
            && m_chosen.isValid() && m_chosen.elapsed() < 1500) {
            m_spot = spotOf(at, m_left, false);
            m_refHeight = at.height();
            madeWay = true;
        }
        // a column on the right keeps its right edge when only its strip shows
        const QRect geometry(m_left ? at.x() : at.right() + 1 - size.width(), at.y(), size.width(), size.height());
        const bool changed = geometry != m_target;
        glideTo(geometry, at.topLeft() - wanted.topLeft(), m_moving);
        KisStudioLayout::keepAbove(this);
        if (changed && resized) resized(false);
        // told once it is on its way there: what it sets off (the insets) places the groups again, this one too
        if ((madeWay || learnt) && moved) moved(m_left, m_spot);
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
                    // the outer edge stays where it is
                    m_outer = m_left ? x() : geometry().right() + 1;
                    update();
                    return true;
                }
                break;
            case QEvent::MouseMove:
                if (m_resizing) {
                    const QWidget *area = parentWidget();
                    const int x = area->mapFromGlobal(globalMousePos(static_cast<QMouseEvent *>(event))).x();
                    // the lane at the edge takes the change
                    const int lane = m_left ? m_widths.size() - 1 : 0;
                    const int others = totalWidth() - laneWidth(lane);
                    m_widths[lane] = qBound(minimumColumnWidth, (m_left ? x - m_outer : m_outer - x) - others, qMax(minimumColumnWidth, area->width() / 2));
                    if (!isDocked()) {
                        const int from = m_left ? m_outer - margin : area->width() - margin - m_outer;
                        m_spot.setX(qBound(0.0, qreal(from) / qMax(1, area->width() - 2 * margin - totalWidth()), 1.0));
                    }
                    place();
                    return true;
                }
                break;
            case QEvent::MouseButtonRelease:
                if (m_resizing && static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton) {
                    m_resizing = false;
                    if (resized) resized(true);
                    if (!isDocked() && moved) moved(m_left, m_spot);
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
        m_shown.start();
        place();
        placeGrip();
        // settled in: the height its place is kept with (see place())
        if (m_refHeight <= 0) QTimer::singleShot(600, this, [this]() { place(); });
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
        m_pressed = true;
        m_moving = false;
        m_grab = event->pos();
        m_pressPos = globalMousePos(event);
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (!m_pressed) {
            QWidget::mouseMoveEvent(event);
            return;
        }
        if (!(event->buttons() & Qt::LeftButton)) {
            // its release went elsewhere (the group was hidden or the window lost the pointer): dropped where it is
            if (!m_moving) {
                m_pressed = false;
                return;
            }
            QMouseEvent release(QEvent::MouseButtonRelease, event->localPos(), event->windowPos(), event->screenPos(), Qt::LeftButton,
                                Qt::NoButton, event->modifiers());
            mouseReleaseEvent(&release);
            return;
        }
        const QPoint global = globalMousePos(event);
        if (!m_moving) {
            if ((global - m_pressPos).manhattanLength() < QApplication::startDragDistance()) return;
            m_moving = true;
            m_movingSize = QSize();
            // what is carried goes over the other overlays
            raise();
            setCursor(Qt::ClosedHandCursor);
            update();
        }
        // caught by the edges and the middle as it goes, so it shows where it will stay
        const QWidget *area = parentWidget();
        const QPoint at = area->mapFromGlobal(global) - m_grab;
        const QSize placed = placedSize(size());
        const QRect rect(QPoint(m_left ? at.x() : at.x() + width() - placed.width(), at.y()), placed);
        setSide(rect.center().x() < area->width() / 2);
        m_spot = spotOf(rect, m_left, true);
        m_refHeight = rect.height();
        place();
        if (joining) joining(this, m_placed, false);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton || !m_pressed) {
            QWidget::mouseReleaseEvent(event);
            return;
        }
        m_pressed = false;
        if (!m_moving) {
            if (tapped) tapped();
            return;
        }
        m_moving = false;
        unsetCursor();
        update();
        // the group moved last makes way for the others
        m_settled = ++settledCount();
        m_chosen.start();
        if (joining && joining(this, m_placed, true)) return;
        place();
        if (moved) moved(m_left, m_spot);
    }

private:
    int laneWidth(int lane) const
    {
        return qBound(minimumColumnWidth, m_widths.value(lane, columnWidth()), qMax(minimumColumnWidth, parentWidget()->width() / 2));
    }

    /// All its lanes' widths and the lines between them
    int totalWidth() const
    {
        int total = (m_widths.size() - 1) * LaneDivider::thickness;
        for (int i = 0; i < m_widths.size(); i++) total += laneWidth(i);
        return qMin(total, qMax(minimumColumnWidth, parentWidget()->width() - 2 * margin));
    }

    /// Each stack as wide as its lane, less what is outside the stacks at the column's edges
    void applyWidths()
    {
        const QMargins margins = m_layout->contentsMargins();
        for (int i = 0; i < m_stacks.size(); i++) {
            int width = laneWidth(i);
            if (i == 0) width -= margins.left();
            if (i == m_stacks.size() - 1) width -= margins.right();
            if (m_stacks[i]->width() != width || m_stacks[i]->minimumWidth() != width) m_stacks[i]->setFixedWidth(qMax(0, width));
        }
    }

    /// The stacks side by side with a line between each two
    void rebuildLanes()
    {
        while (m_lanes->count() > 0) delete m_lanes->takeAt(0);
        for (LaneDivider *divider : qAsConst(m_dividers)) {
            divider->hide();
            divider->deleteLater();
        }
        m_dividers.clear();
        for (int i = 0; i < m_stacks.size(); i++) {
            if (i > 0) {
                auto *divider = new LaneDivider(this);
                // the lane on its free side takes the change, so the line stays under the pointer
                const int lane = m_left ? i - 1 : i;
                divider->pressed = [this, lane]() { m_dividerStart = m_widths.value(lane); };
                divider->dragged = [this, lane](int dx) {
                    if (lane >= m_widths.size()) return;
                    m_widths[lane] = qBound(minimumColumnWidth, m_dividerStart + (m_left ? dx : -dx), qMax(minimumColumnWidth, parentWidget()->width() / 2));
                    place();
                };
                divider->released = [this]() {
                    if (resized) resized(true);
                };
                // after the click is done with: taking the lanes apart remakes the lines
                divider->clicked = [this, i]() {
                    QTimer::singleShot(0, this, [this, i]() {
                        if (separated) separated(i);
                    });
                };
                divider->setVisible(m_shownPanels);
                m_dividers << divider;
                m_lanes->addWidget(divider);
            }
            m_lanes->addWidget(m_stacks[i]);
        }
        placeGrip();
    }

    /// How far it still is from where it is going
    QPoint glideNow() const { return m_glide.state() == QAbstractAnimation::Running ? m_glide.currentValue().toPoint() : QPoint(); }

    /**
     * Goes to \p target: at once where the pointer takes it, gliding the rest, so a group making way is seen going
     * there. While \p dragged, \p extra is the part of its place the pointer didn't make.
     */
    void glideTo(const QRect &target, const QPoint &extra, bool dragged)
    {
        const QWidget *area = parentWidget();
        // nothing to glide from: just shown (and settling in), or the window changed size
        const bool fresh = !isVisible() || m_target.isNull() || area->size() != m_areaSize || !m_shown.isValid() || m_shown.elapsed() < 500;
        const QPoint now = glideNow();
        QPoint from = fresh ? QPoint() : pos() - target.topLeft();
        if (dragged && !fresh) {
            // dragged, it follows the pointer or stays put (held off a group) at once, each way apart: sliding
            // along a group it is held off follows the pointer one way and stays the other; only a jump glides
            const QPoint moved = target.topLeft() - m_target.topLeft();
            const QPoint byPointer = moved - (extra - m_extra);
            auto jump = [](int moved, int byPointer) { return qAbs(moved) > 3 && qAbs(moved - byPointer) > 3 ? moved : 0; };
            from = now - QPoint(jump(moved.x(), byPointer.x()), jump(moved.y(), byPointer.y()));
        }
        m_areaSize = area->size();
        m_target = target;
        m_extra = extra;
        // what keeps off it (the zoom pill) keeps off where it is going
        setProperty("studioTarget", target);
        if (from.manhattanLength() < 2) {
            m_glide.stop();
            setGeometry(target);
            return;
        }
        // a new jump starts the glide again; else it keeps going
        if (m_glide.state() != QAbstractAnimation::Running || from != now) {
            m_glide.stop();
            m_glide.setStartValue(from);
            m_glide.setEndValue(QPoint());
            m_glide.start();
        }
        setGeometry(target.translated(glideNow()));
    }

    /// Its size now: as tall as its tallest lane wants within the room, or just the strip without them
    QSize sizeNow()
    {
        const QPair<int, int> span = spanY();
        const QMargins margins = m_layout->contentsMargins();
        // a grip of fixed height has no size hint
        const int strip = m_strip && !m_strip->isHidden() ? qMax(m_strip->sizeHint().height(), m_strip->minimumHeight()) : 0;
        // tucked, just the strip, as wide as when open
        if (!panelsShown()) return QSize(m_openSize.isEmpty() ? totalWidth() : m_openSize.width(), margins.top() + margins.bottom() + strip);
        int tallest = 0;
        for (PanelStack *stack : qAsConst(m_stacks)) tallest = qMax(tallest, stack->wantedHeight());
        const int wanted = margins.top() + margins.bottom() + strip + tallest;
        m_openSize = QSize(totalWidth(), qBound(0, wanted, qMax(0, span.second - span.first)));
        return m_openSize;
    }

    QSize placedSize(const QSize &size) const { return panelsShown() || m_openSize.isEmpty() ? size : m_openSize; }

    /// The top and bottom it can reach, clear of the bars along the canvas's top and bottom
    QPair<int, int> spanY() const
    {
        const QWidget *area = parentWidget();
        return {area->property(topInsetProperty).toInt() + margin, area->height() - area->property(bottomInsetProperty).toInt() - margin};
    }

    /// The place of a box at \p box, from its side, as shares of the room it has; \p snap: caught by the edges and the middle
    QPointF spotOf(const QRect &box, bool left, bool snap) const
    {
        const QWidget *area = parentWidget();
        const QPair<int, int> span = spanY();
        const int across = left ? box.x() - margin : area->width() - margin - box.x() - box.width();
        const int down = box.y() - span.first;
        const int roomX = area->width() - 2 * margin - box.width();
        const int roomY = span.second - span.first - box.height();
        if (snap) return QPointF(shareOf(across, roomX), shareOf(down, roomY));
        return QPointF(roomX > 0 ? qBound(0.0, qreal(across) / roomX, 1.0) : 0.0, roomY > 0 ? qBound(0.0, qreal(down) / roomY, 1.0) : 0.0);
    }

    void setSide(bool left)
    {
        if (left == m_left) return;
        m_left = left;
        updateMargins();
        rebuildLanes();
    }

    /// One more each time a group is made or moved: the one moved last makes way for the others
    static quint64 &settledCount()
    {
        static quint64 count = 0;
        return count;
    }

    /// The columns over the canvas area (it has no meta object, so findChildren() would take any widget)
    QList<KisStudioPanelColumn *> columnsBeside() const
    {
        QList<KisStudioPanelColumn *> columns;
        for (QObject *child : parentWidget()->children()) {
            if (auto *column = dynamic_cast<KisStudioPanelColumn *>(child)) columns << column;
        }
        return columns;
    }

    /// \p wanted moved the least it takes off the tool rail and the groups that settled before \p settled (those it overlaps)
    QRect clearOfOthers(QRect wanted, const QPair<int, int> &span, quint64 settled) const
    {
        QVector<QRect> obstacles;
        const QRect rail = parentWidget()->property(railRectProperty).toRect();
        if (!rail.isEmpty()) obstacles << rail;
        for (KisStudioPanelColumn *other : columnsBeside()) {
            if (other == this || other->isHidden() || other->m_settled > settled) continue;
            obstacles << other->targetGeometry();
        }
        auto clear = [&](const QRect &at, int count) {
            for (int i = 0; i < count; i++) {
                if (obstacles[i].intersects(at)) return false;
            }
            return true;
        };
        if (clear(wanted, obstacles.size())) return wanted;
        // the nearest place clear of them all: beside, above or below any of them, or in a corner two of them make
        const int gap = margin;
        QVector<int> xs{wanted.x()};
        QVector<int> ys{wanted.y()};
        for (const QRect &them : qAsConst(obstacles)) {
            xs << them.right() + 1 + gap << them.left() - gap - wanted.width();
            ys << them.bottom() + 1 + gap << them.top() - gap - wanted.height();
        }
        const int minX = margin;
        const int maxX = parentWidget()->width() - margin - wanted.width();
        // the rail first: when nothing is clear of all, at least off it
        for (int count : {int(obstacles.size()), rail.isEmpty() ? 0 : 1}) {
            QRect best;
            for (int x : qAsConst(xs)) {
                for (int y : qAsConst(ys)) {
                    if (x < minX || x > maxX || y < span.first || y + wanted.height() > span.second) continue;
                    const QRect at(QPoint(x, y), wanted.size());
                    if (!clear(at, count)) continue;
                    if (best.isNull() || (at.topLeft() - wanted.topLeft()).manhattanLength() < (best.topLeft() - wanted.topLeft()).manhattanLength()) best = at;
                }
            }
            if (!best.isNull()) return best;
        }
        return wanted;
    }

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
    QHBoxLayout *m_lanes {nullptr};
    QVector<PanelStack *> m_stacks;
    QVector<LaneDivider *> m_dividers;
    QPointer<QWidget> m_strip;
    QWidget *m_grip {nullptr};
    /// When it was made or last moved (see settledCount())
    quint64 m_settled {0};
    bool m_left {false};
    QPointF m_spot;
    /// Its height when its place was taken (see place())
    int m_refHeight {0};
    QSize m_openSize;
    bool m_shownPanels {true};
    /// Where it was last placed, as if open
    QRect m_placed;
    /// Its size while dragged (see place())
    QSize m_movingSize;
    /// Where it is going (see glideTo()), from what and in which window size
    QRect m_target;
    QVariantAnimation m_glide;
    QPoint m_extra;
    QSize m_areaSize;
    QElapsedTimer m_shown;
    /// Since it was dropped or placed
    QElapsedTimer m_chosen;
    QVector<int> m_widths;
    int m_dividerStart {0};
    bool m_resizing {false};
    int m_outer {0};
    bool m_pressed {false};
    bool m_moving {false};
    QPoint m_grab;
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
    /// When the main menu last closed: a press on its button closes it, and the click that follows mustn't open it again
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

    /// Looked up once: canvas-only mode asks at every pointer move
    QPointer<QAction> canvasOnlyAction;
    QPointer<QMdiArea> mdiArea;
    QSet<QString> pinned;
    /// Pinned panels put away with their button: normalize() leaves them hidden
    QSet<QString> hiddenPinned;
    QHash<QString, int> heights;
    QSet<QString> folded;
    QHash<QString, QPointer<PanelGrip>> grips;
    QPointer<KisStudioPanelColumn> panelColumn;
    /// The groups of pinned panels other than the column with the panel buttons: 1 across the canvas from it,
    /// then floating ones, each a column of its own
    QMap<int, QPointer<KisStudioPanelColumn>> groupColumns;
    /// Shown in place of Krita's welcome page, which is kept aside meanwhile
    QPointer<KisStudioStartPage> startPage;
    QPointer<QWidget> classicWelcome;
    /// Which group each pinned panel is in, where it isn't the column with the panel buttons
    QHash<QString, int> groups;
    QPointer<TopPill> topPill;
    QPointer<TopPill> windowPill;
    /// In canvas-only mode, what the pointer brought back by nearing its edge (see peek())
    QList<QPointer<QWidget>> peeking;
    QPointer<QToolBar> windowBar;
    bool panelsLeft {false};
    bool toolRailLeft {true};
    /// Where the tool rail and the top bar were dragged to (see shareOf())
    QPointF toolRailSpot;
    QPointF topBarSpot;
    bool panelsTucked {false};
    bool toolRailTucked {false};
    bool topBarTucked {false};
    QPointer<QWidget> stripGrip;
    /// The pinned panels' lanes in their groups, for those not in the first (see lanesKey)
    QHash<QString, int> lanes;
    /// Each group's lanes' widths, left to right
    QHash<int, QVector<int>> laneWidths;

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

    int groupOf(const QString &id) const { return groups.value(id, 0); }
    int laneOf(const QString &id) const { return lanes.value(id, 0); }

    /// Every column of pinned panels, the one with the panel buttons first
    QList<KisStudioPanelColumn *> columns() const
    {
        QList<KisStudioPanelColumn *> all;
        if (panelColumn) all << panelColumn;
        for (const QPointer<KisStudioPanelColumn> &column : groupColumns) {
            if (column) all << column;
        }
        return all;
    }

    /// The pinned panels shown in a group (in one of its lanes, unless \p lane is -1), top to bottom, excluding \p without
    QList<QDockWidget *> column(const QString &without, int group = 0, int lane = -1) const
    {
        QList<QDockWidget *> docks;
        Q_FOREACH (const QString &id, pinnedOrder()) {
            QDockWidget *dock = mainWindow->dockWidget(id);
            if (id != without && groupOf(id) == group && (lane < 0 || laneOf(id) == lane) && dock && dock != flyout && !dock->isHidden()
                && !dock->isFloating()) {
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
        // the column with the panel buttons first, then the one across the canvas, then the floating groups
        QList<int> order = {0, 1};
        for (auto it = groupColumns.constBegin(); it != groupColumns.constEnd(); ++it) {
            if (it.key() >= 2 && it.value()) order << it.key();
        }
        for (int group : order) {
            const QList<QDockWidget *> docks = column(moving, group);
            const KisStudioPanelColumn *own = group == 0 ? panelColumn.data() : groupColumns.value(group).data();
            const bool left = group == 0 ? panelsLeft : group == 1 ? !panelsLeft : own && own->isLeft();
            QRect area;
            Q_FOREACH (QDockWidget *dock, docks) {
                area |= placeOf(dock);
            }
            // a group with other panels: all of its pill, the dragged panel's place too
            if (!area.isEmpty() && own && own->isVisible()) {
                const QRect pill = own->targetGeometry();
                area |= QRect(own->parentWidget()->mapTo(mainWindow, pill.topLeft()), pill.size()).adjusted(1, 1, -1, -1);
            }
            // over a group, or near the canvas edge where the two edge columns have no panels
            const bool overColumn = !area.isEmpty() && area.adjusted(-8, -24, 8, 24).contains(pos);
            const bool nearEdge = area.isEmpty() && group <= 1 && canvas.contains(pos)
                                  && (left ? pos.x() < canvas.left() + edgeDropZone : pos.x() > canvas.right() - edgeDropZone);
            if (!overColumn && !nearEdge) continue;

            drop.column = true;
            drop.left = left;
            drop.group = group;
            if (area.isEmpty()) {
                drop.indicator = QRect(left ? canvas.left() + 2 : canvas.right() - 4, canvas.top() + 6, 3, canvas.height() - 12);
                return drop;
            }
            // the lanes it has, in window coordinates; near the group's left or right edge, a new lane there
            QVector<QRect> lanes;
            for (int i = 0; own && i < own->stacks().size(); i++) {
                lanes << QRect(own->parentWidget()->mapTo(mainWindow, own->laneRect(i).topLeft()), own->laneRect(i).size());
            }
            const int side = 20;
            if (pos.x() < area.left() + side || pos.x() > area.right() - side) {
                drop.newLane = true;
                drop.lane = pos.x() < area.left() + side ? 0 : lanes.size();
                drop.indicator = QRect(pos.x() < area.left() + side ? area.left() - 1 : area.right() - 1, area.top() + 6, 3, area.height() - 12);
                return drop;
            }
            for (int i = 0; i < lanes.size(); i++) {
                if (pos.x() >= lanes[i].left()) drop.lane = i;
            }
            const QList<QDockWidget *> inLane = column(moving, group, drop.lane);
            QRect laneArea = lanes.value(drop.lane, area);
            laneArea.setTop(area.top());
            laneArea.setBottom(area.bottom());
            int y = area.bottom() - 1;
            Q_FOREACH (QDockWidget *dock, inLane) {
                if (pos.y() < placeOf(dock).center().y()) {
                    drop.before = dock->objectName();
                    y = placeOf(dock).top() - 1;
                    break;
                }
            }
            if (drop.before.isEmpty() && !inLane.isEmpty()) {
                QRect below;
                Q_FOREACH (QDockWidget *dock, inLane) below |= placeOf(dock);
                y = below.bottom() - 1;
            }
            drop.indicator = QRect(laneArea.left() + 4, qMax(area.top(), y - 1), laneArea.width() - 8, 3);
            return drop;
        }
        // anywhere else on the canvas: a floating group of its own, there
        if (canvas.contains(pos)) {
            const int width = panelColumn ? panelColumn->columnWidth() : 270;
            drop.column = true;
            drop.fresh = true;
            drop.at = pos;
            drop.indicator = QRect(pos.x() - width / 2, pos.y() - 1, width, 3);
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
        if (d->toolRail) d->toolRail->refresh();
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

void KisStudioLayout::keepAbove(QWidget *overlay)
{
    static const char *const overlayProperty = "studioOverlay";
    overlay->setProperty(overlayProperty, true);
    QWidget *parent = overlay->parentWidget();
    if (!parent) return;
    const QObjectList &siblings = parent->children();
    for (int i = siblings.indexOf(overlay) + 1; i < siblings.size(); i++) {
        auto *sibling = qobject_cast<QWidget *>(siblings[i]);
        if (sibling && !sibling->isWindow() && !sibling->isHidden() && !sibling->property(overlayProperty).toBool()) {
            overlay->raise();
            return;
        }
    }
}

qreal KisStudioLayout::spotShare(int offset, int room)
{
    return shareOf(offset, room);
}

int KisStudioLayout::spotOffset(qreal share, int room)
{
    return offsetOf(share, room);
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
    d->groups.clear();
    Q_FOREACH (const QString &id, readSet(farKey)) {
        d->groups.insert(id, 1);
    }
    Q_FOREACH (const QString &entry, cfg.readEntry<QStringList>(groupsKey)) {
        const int separator = entry.lastIndexOf(QLatin1Char('='));
        const int group = separator > 0 ? entry.mid(separator + 1).toInt() : 0;
        if (group >= 2) d->groups.insert(entry.left(separator), group);
    }
    d->lanes.clear();
    Q_FOREACH (const QString &entry, cfg.readEntry<QStringList>(lanesKey)) {
        const int separator = entry.lastIndexOf(QLatin1Char('='));
        const int lane = separator > 0 ? entry.mid(separator + 1).toInt() : 0;
        if (lane > 0) d->lanes.insert(entry.left(separator), lane);
    }
    d->folded = readSet(foldedKey);
    const QString panelSide = cfg.readEntry<QString>(panelSideKey, QString());
    d->panelsLeft = panelSide == QLatin1String("left")
                    || (panelSide.isEmpty() && !d->pinned.isEmpty() && readSet(leftKey).contains(d->pinned));
    d->toolRailLeft = cfg.readEntry<QString>(toolRailSideKey, QStringLiteral("left")) != QLatin1String("right");
    d->toolRailSpot = readSpot(toolRailSpotKey);
    d->topBarSpot = readSpot(topBarSpotKey);
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
        // Krita's panels that run their contents to their edges (the color selectors, the mixer) get the panels' padding,
        // clear of the rounded corners
        static const QStringList edgeToEdge = {QStringLiteral("ColorSelectorNg"), QStringLiteral("WideGamutColorSelector"),
                                               QStringLiteral("DigitalMixer")};
        // one name for the brushes, as the top bar has it
        if (id == QLatin1String("PresetDocker")) dock->setWindowTitle(i18nc("@title:window the brushes panel", "Brushes"));
        if (edgeToEdge.contains(id) && dock->widget()) {
            dock->widget()->setContentsMargins(KisModernStyle::space2, 0, KisModernStyle::space2, KisModernStyle::space2);
        }
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
    // a brush of what is selected (also in the selection's menu)
    addPicker(QStringLiteral("studio_make_brush"), QStringLiteral("krita_tool_freehand"), i18nc("@action", "Make Brush from Selection"), [mainWindow]() {
        KisStudioMakeBrush::fromSelection(mainWindow->viewManager());
    });
    KisSkissaUpdater::addActions(mainWindow);
    // brush tweaks not saved yet come back after a restart
    KisStudioBrushTweaks::watch(mainWindow);
    addPicker(QStringLiteral("studio_home"), QStringLiteral("krita-branding"), i18nc("@action your artworks: the start page", "Home"), [this]() {
        showHome(!d->mainWindow->isWelcomeScreenShown());
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
    // one name for one thing: the brush editors call it so
    if (QAction *reload = mainWindow->actionCollection()->action(QStringLiteral("reload_preset_action"))) {
        reload->setText(i18nc("@action drop the brush's unsaved changes", "Back to the saved brush"));
        reload->setToolTip(reload->text());
    }
        // the commands as they are now; those added or renamed later are tidied as menus get them (eventFilter)
    Q_FOREACH (QAction *action, mainWindow->actionCollection()->actions()) tidyCommand(action);

    qApp->installEventFilter(this);
    KisStudioEmptyStates::install();
    if (KisCanvasResourceProvider *resources = viewManager ? viewManager->canvasResourceProvider() : nullptr) {
        d->connections << connect(resources, &KisCanvasResourceProvider::sigPaintOpPresetChanged, this, [this]() { updateToolChip(); });
    }
    KoToolManager *tools = viewManager ? viewManager->toolManager() : nullptr;
    if (tools) {
        d->connections << connect(tools, &KoToolManager::changedTool, this, [this](KoCanvasController *controller) {
            if (KisStudioToolRail::isOwnController(d->mainWindow, controller)) updateContextBar();
        });
        d->connections << connect(tools, &KoToolManager::toolOptionWidgetsChanged, this,
                                  [this](KoCanvasController *controller, const QList<QPointer<QWidget>> &widgets) {
                                      // ignore tool changes in other windows
                                      if (!KisStudioToolRail::isOwnController(d->mainWindow, controller) || !d->toolStrip) return;
                                      d->toolStrip->setOptionWidgets(widgets);
                                      if (d->toolStripAction) {
                                          d->toolStripAction->setVisible(d->toolStrip->hasOptions() && (!isRailHidden() || d->peeking.contains(QPointer<QWidget>(d->topPill.data()))));
                                      }
                                  });
    }
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
    if (mainWindow->activeView() && tools) {
        tools->initializeCurrentToolForCanvas();
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
    // in a pill, empty space and drag grips act as caption; controls don't, nor does TopGrip, which moves the bar
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
    if (!d->mdiArea) d->mdiArea = d->mainWindow->findChild<QMdiArea *>();
    return d->mdiArea;
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
    Q_FOREACH (KisStudioPanelColumn *column, d->columns()) {
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
        } else if (!panelsHidden && !d->hiddenPinned.contains(dock->objectName())) {
            dock->show();
        }
    }
    // in the rail's order, whatever order a restored state had: a restored state puts panels back in the window's
    // dock areas, otherwise (a setting changed) they are where they were arranged
    bool restored = false;
    Q_FOREACH (QDockWidget *dock, d->panels) {
        if (dock != d->flyout && d->pinned.contains(dock->objectName()) && dock->parentWidget() == mainWindow) restored = true;
    }
    if (restored) d->arrangement.clear();
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
    // the rail's commands only exist once the GUI is built, which happens after the rail; this runs at
    // every setting change, so only then
    if (d->toolRail && d->toolRail->hasMissingItems()) {
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
    if (!d->canvasOnlyAction) d->canvasOnlyAction = d->mainWindow->actionCollection()->action(QStringLiteral("view_show_canvas_only"));
    return d->canvasOnlyAction && d->canvasOnlyAction->isChecked();
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
        d->hiddenPinned.remove(dock->objectName());
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
        if (d->pinned.contains(dock->objectName())) d->hiddenPinned.insert(dock->objectName());
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
    Q_FOREACH (KisStudioPanelColumn *other, d->columns()) {
        if (other == d->panelColumn || !other->isVisible() || other->isLeft()) continue;
        const QRect pill(other->mapToGlobal(QPoint()), other->size());
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
        d->hiddenPinned.remove(id);
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
        d->groups.remove(id);
        d->lanes.remove(id);
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
    // in the rail's order; only redone when the order, a group, a lane or a side has changed
    const QStringList order = d->pinnedOrder();
    QString arrangement = d->panelsLeft ? QStringLiteral("L:") : QStringLiteral("R:");
    Q_FOREACH (const QString &id, order) {
        arrangement += QString::number(d->groupOf(id)) + QLatin1Char('.') + QString::number(d->laneOf(id)) + QLatin1Char(' ') + id + QLatin1Char(',');
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
    // group, then lane, then the panels top to bottom
    QMap<int, QMap<int, QList<QDockWidget *>>> lists;
    lists[0];
    Q_FOREACH (const QString &id, order) {
        QDockWidget *dock = d->mainWindow->dockWidget(id);
        if (dock && dock != d->flyout) lists[d->groupOf(id)][d->laneOf(id)] << dock;
    }
    QMap<int, KisStudioPanelColumn *> columns;
    columns.insert(0, column);
    for (auto it = lists.constBegin(); it != lists.constEnd(); ++it) {
        if (it.key() == 0) continue;
        if (KisStudioPanelColumn *own = groupColumn(it.key())) columns.insert(it.key(), own);
    }
    QList<QDockWidget *> placed;
    for (const auto &lanes : lists) {
        for (const QList<QDockWidget *> &list : lanes) placed += list;
    }
    Q_FOREACH (KisStudioPanelColumn *own, d->columns()) {
        Q_FOREACH (QDockWidget *dock, own->panels()) {
            if (!placed.contains(dock)) parkPanel(dock);
        }
    }
    // take them out of the window's dock areas so the canvas extends under the pills
    Q_FOREACH (QDockWidget *dock, placed) {
        bool inColumn = false;
        for (KisStudioPanelColumn *own : columns) {
            for (PanelStack *stack : own->stacks()) inColumn = inColumn || dock->parentWidget() == stack;
        }
        if (!inColumn) {
            const bool shown = !dock->isHidden();
            if (dock->isFloating()) dockAway(dock);
            d->mainWindow->removeDockWidget(dock);
            dock->setVisible(shown);
        }
    }
    column->setLeft(d->panelsLeft);
    if (columns.contains(1)) columns[1]->setLeft(!d->panelsLeft);
    bool renumbered = false;
    for (auto it = columns.constBegin(); it != columns.constEnd(); ++it) {
        const int group = it.key();
        // lanes numbered from 0 without gaps, each keeping its width
        const QMap<int, QList<QDockWidget *>> lanes = lists.value(group);
        const QVector<int> widths = d->laneWidths.value(group);
        QList<QList<QDockWidget *>> shown;
        QVector<int> kept;
        for (auto lane = lanes.constBegin(); lane != lanes.constEnd(); ++lane) {
            for (QDockWidget *dock : lane.value()) {
                const int now = shown.size();
                if (d->laneOf(dock->objectName()) == now) continue;
                if (now > 0) d->lanes.insert(dock->objectName(), now);
                else d->lanes.remove(dock->objectName());
                renumbered = true;
            }
            shown << lane.value();
            kept << widths.value(lane.key(), widths.value(0, it.value()->columnWidth()));
        }
        if (kept.isEmpty()) kept << widths.value(0, it.value()->columnWidth());
        if (kept != widths) {
            d->laneWidths.insert(group, kept);
            saveWidths(group);
        }
        it.value()->setLanes(shown, kept);
    }
    if (renumbered) {
        savePinned();
        d->arrangement.clear();
    }
    // floating groups left with no panels go; the one across the canvas stays for panels dropped by that edge
    for (auto it = d->groupColumns.begin(); it != d->groupColumns.end();) {
        if (it.key() >= 2 && !lists.contains(it.key())) {
            if (KisStudioPanelColumn *going = it.value()) {
                going->hide();
                going->deleteLater();
            }
            d->laneWidths.remove(it.key());
            it = d->groupColumns.erase(it);
        } else {
            ++it;
        }
    }
    restoreHeights();
    updateColumns();
}

QVector<int> KisStudioLayout::readWidths(int group) const
{
    KisConfig cfg(true);
    const QString key = widthKeyOf(group);
    QVector<int> widths;
    Q_FOREACH (const QString &width, cfg.readEntry<QString>(key + QStringLiteral("Lanes"), QString()).split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        widths << qMax(KisStudioPanelColumn::minimumColumnWidth, width.toInt());
    }
    if (!widths.isEmpty()) return widths;
    // as wide as the column with the panel buttons until its own width is set; that one falls back to the legacy key
    const int fallback = group == 0 ? cfg.readEntry<int>(QStringLiteral("StudioLayoutRightColumnWidth"), 270)
                                    : d->panelColumn ? d->panelColumn->columnWidth() : 270;
    return {cfg.readEntry<int>(key, fallback)};
}

void KisStudioLayout::saveWidths(int group)
{
    const QVector<int> widths = d->laneWidths.value(group);
    if (widths.isEmpty()) return;
    QStringList list;
    for (int width : widths) list << QString::number(width);
    KisConfig cfg(false);
    cfg.writeEntry(widthKeyOf(group), widths.first());
    cfg.writeEntry(widthKeyOf(group) + QStringLiteral("Lanes"), list.join(QLatin1Char(',')));
}

QString KisStudioLayout::widthKeyOf(int group) const
{
    return group == 0 ? columnWidthKey : group == 1 ? farWidthKey : groupKey(group, "Width");
}

bool KisStudioLayout::joinAt(KisStudioPanelColumn *column, const QRect &placed, bool dropped)
{
    QWidget *area = column->parentWidget();
    // within a snap of another group's side (or, for a single lane, its top or bottom), along enough of it
    const int snap = 16;
    const int alongAtLeast = 40;
    KisStudioPanelColumn *to = nullptr;
    Qt::Edge edge = Qt::RightEdge;
    int lane = 0;
    QRect indicator;
    int nearest = snap + 1;
    Q_FOREACH (KisStudioPanelColumn *other, d->columns()) {
        if (other == column || other->isHidden() || !other->panelsShown() || other->panels().isEmpty()) continue;
        // the column with the panel buttons and the one across from it keep to their sides
        if ((column->group <= 1 && other->group <= 1)) continue;
        const QRect them = other->targetGeometry();
        const int upright = qMin(placed.bottom(), them.bottom()) - qMax(placed.top(), them.top());
        const int across = qMin(placed.right(), them.right()) - qMax(placed.left(), them.left());
        auto consider = [&](Qt::Edge side, int distance, int along, int toLane, const QRect &bar) {
            if (qAbs(distance) >= nearest || along < alongAtLeast) return;
            nearest = qAbs(distance);
            to = other;
            edge = side;
            lane = toLane;
            indicator = bar;
        };
        consider(Qt::RightEdge, placed.left() - them.right() - 1, upright, 0, QRect(them.right() - 1, them.top() + 6, 3, them.height() - 12));
        consider(Qt::LeftEdge, them.left() - placed.right() - 1, upright, 0, QRect(them.left() - 1, them.top() + 6, 3, them.height() - 12));
        if (column->stacks().size() == 1) {
            // into the lane it is over
            int over = 0;
            for (int i = 0; i < other->stacks().size(); i++) {
                const QRect laneAt = other->laneRect(i).translated(them.topLeft() - other->geometry().topLeft());
                if (placed.center().x() >= laneAt.left()) over = i;
            }
            const QRect laneAt = other->laneRect(over).translated(them.topLeft() - other->geometry().topLeft());
            consider(Qt::BottomEdge, placed.top() - them.bottom() - 1, across, over, QRect(laneAt.left() + 4, them.bottom() - 1, laneAt.width() - 8, 3));
            consider(Qt::TopEdge, them.top() - placed.bottom() - 1, across, over, QRect(laneAt.left() + 4, them.top() - 1, laneAt.width() - 8, 3));
        }
    }
    if (!dropped) {
        // shown where it would join, as a dragged panel's place is
        if (to) {
            if (!d->panelIndicator) {
                d->panelIndicator = new QFrame(d->mainWindow);
                d->panelIndicator->setAutoFillBackground(true);
                d->panelIndicator->setBackgroundRole(QPalette::Highlight);
            }
            d->panelIndicator->setGeometry(QRect(area->mapTo(d->mainWindow, indicator.topLeft()), indicator.size()));
            d->panelIndicator->show();
            d->panelIndicator->raise();
        } else {
            d->hidePanelDrop();
        }
        return to;
    }
    d->hidePanelDrop();
    if (!to) return false;
    joinGroups(column, to, edge, lane, placed);
    return true;
}

QList<QStringList> KisStudioLayout::lanesOf(const KisStudioPanelColumn *column) const
{
    QList<QStringList> lanes;
    for (PanelStack *stack : column->stacks()) {
        QStringList ids;
        for (QDockWidget *dock : stack->panels()) ids << dock->objectName();
        if (!ids.isEmpty()) lanes << ids;
    }
    return lanes;
}

void KisStudioLayout::joinGroups(KisStudioPanelColumn *from, KisStudioPanelColumn *to, Qt::Edge edge, int lane, const QRect &placed)
{
    const QList<QStringList> moving = lanesOf(from);
    const QList<QStringList> staying = lanesOf(to);
    const QVector<int> movingWidths = from->laneWidths().mid(0, moving.size());
    const QVector<int> stayingWidths = to->laneWidths().mid(0, staying.size());
    QList<QStringList> lanes;
    QVector<int> widths;
    if (edge == Qt::RightEdge || edge == Qt::LeftEdge) {
        lanes = edge == Qt::RightEdge ? staying + moving : moving + staying;
        widths = edge == Qt::RightEdge ? stayingWidths + movingWidths : movingWidths + stayingWidths;
    } else {
        lanes = staying;
        widths = stayingWidths;
        const QStringList ids = moving.value(0);
        if (lane >= 0 && lane < lanes.size()) lanes[lane] = edge == Qt::TopEdge ? ids + lanes[lane] : lanes[lane] + ids;
    }
    // the column with the panel buttons (or the one across from it) stays; a floating group goes into the other
    KisStudioPanelColumn *kept = from->group == 0 || (from->group == 1 && to->group != 0) ? from : to;
    for (int i = 0; i < lanes.size(); i++) {
        Q_FOREACH (const QString &id, lanes[i]) {
            if (kept->group > 0) d->groups.insert(id, kept->group);
            else d->groups.remove(id);
            if (i > 0) d->lanes.insert(id, i);
            else d->lanes.remove(id);
        }
    }
    d->laneWidths.insert(kept->group, widths);
    saveWidths(kept->group);
    savePinned();
    // a lane is its panels in the rail's order: put them in the order the lanes now have, where the first was
    QStringList panels = d->railPanels;
    QStringList joined;
    for (const QStringList &ids : lanes) {
        Q_FOREACH (const QString &id, ids) {
            if (panels.contains(id)) joined << id;
        }
    }
    int at = panels.size();
    Q_FOREACH (const QString &id, joined) at = qMin(at, int(panels.indexOf(id)));
    Q_FOREACH (const QString &id, joined) panels.removeAll(id);
    for (int i = 0; i < joined.size(); i++) panels.insert(qMin(at + i, int(panels.size())), joined[i]);
    // the joined pill where the two were
    const QRect box = placed | to->targetGeometry();
    QPointer<KisStudioPanelColumn> stays = kept;
    d->arrangement.clear();
    setRailPanels(panels);
    if (stays) stays->placeAt(box);
}

void KisStudioLayout::separateLanes(KisStudioPanelColumn *column, int lane)
{
    const QList<QStringList> lanes = lanesOf(column);
    if (lane <= 0 || lane >= lanes.size()) return;
    QWidget *area = column->parentWidget();
    const QVector<int> widths = column->laneWidths();
    const QRect stay = column->laneRect(0) | column->laneRect(lane - 1);
    const QRect go = column->laneRect(lane) | column->laneRect(lanes.size() - 1);
    const int group = freeGroup();
    for (int i = lane; i < lanes.size(); i++) {
        Q_FOREACH (const QString &id, lanes[i]) {
            d->groups.insert(id, group);
            if (i > lane) d->lanes.insert(id, i - lane);
            else d->lanes.remove(id);
        }
    }
    d->laneWidths.insert(group, widths.mid(lane));
    d->laneWidths.insert(column->group, widths.mid(0, lane));
    saveWidths(group);
    saveWidths(column->group);
    // the lanes taken off are a group of their own where they were, a little apart
    writeGroupPlace(group, go.translated(go.center().x() > stay.center().x() ? KisModernStyle::space3 : -KisModernStyle::space3, 0), area);
    savePinned();
    d->arrangement.clear();
    arrangePinned();
    // the lanes left keep their place
    column->placeAt(stay);
}

void KisStudioLayout::separateBelow(KisStudioPanelColumn *column, int lane, QDockWidget *above)
{
    const QList<QStringList> lanes = lanesOf(column);
    if (!above || lane < 0 || lane >= lanes.size()) return;
    const QStringList ids = lanes[lane];
    const int at = ids.indexOf(above->objectName());
    if (at < 0 || at >= ids.size() - 1) return;
    QWidget *area = column->parentWidget();
    // the panels below, as tall as they are and as wide as their lane
    const QRect laneAt = column->laneRect(lane);
    QRect below;
    for (int i = at + 1; i < ids.size(); i++) {
        if (QDockWidget *dock = d->mainWindow->dockWidget(ids[i])) below |= QRect(dock->mapTo(area, QPoint()), dock->size());
    }
    below = QRect(laneAt.left(), below.top(), laneAt.width(), below.height() + 2 * KisStudioPanelColumn::margin);
    const QRect stay = column->targetGeometry();
    const int group = freeGroup();
    for (int i = at + 1; i < ids.size(); i++) {
        d->groups.insert(ids[i], group);
        d->lanes.remove(ids[i]);
    }
    d->laneWidths.insert(group, {column->laneWidths().value(lane, column->columnWidth())});
    saveWidths(group);
    // a group of their own where they were, a little apart
    writeGroupPlace(group, below.translated(0, KisModernStyle::space3), area);
    savePinned();
    d->arrangement.clear();
    arrangePinned();
    // the panels left keep their place
    column->placeAt(stay);
}

void KisStudioLayout::writeGroupPlace(int group, const QRect &rect, QWidget *area)
{
    const int margin = KisStudioPanelColumn::margin;
    const bool left = rect.center().x() < area->width() / 2;
    const int across = left ? rect.x() - margin : area->width() - margin - rect.right() - 1;
    const int top = area->property(topInsetProperty).toInt() + margin;
    const int bottom = area->height() - area->property(bottomInsetProperty).toInt() - margin;
    const int roomX = area->width() - 2 * margin - rect.width();
    const int roomY = bottom - top - rect.height();
    KisSettingsRegistry::writeLater(groupKey(group, "Side"), left ? QStringLiteral("left") : QStringLiteral("right"));
    writeSpot(groupKey(group, "Spot"), QPointF(roomX > 0 ? qBound(0.0, qreal(across) / roomX, 1.0) : 0.0,
                                               roomY > 0 ? qBound(0.0, qreal(rect.y() - top) / roomY, 1.0) : 0.0), rect.height());
}

int KisStudioLayout::freeGroup() const
{
    int group = 2;
    while (d->groupColumns.contains(group) || std::find(d->groups.cbegin(), d->groups.cend(), group) != d->groups.cend()) group++;
    return group;
}

KisStudioPanelColumn *KisStudioLayout::panelColumn()
{
    if (!d->panelColumn) {
        d->panelColumn = createColumn(0, d->panelsLeft);
        if (!d->panelColumn) return nullptr;
        d->panelColumn->setSpot(readSpot(panelSpotKey), readSpotHeight(panelSpotKey));
        d->panelColumn->moved = [this](bool left, const QPointF &spot) {
            writeSpot(panelSpotKey, spot, d->panelColumn ? d->panelColumn->refHeight() : 0);
            if (left != d->panelsLeft) setPanelsSide(left);
            else updateCanvasInsets();
        };
        d->panelColumn->tapped = [this]() { setPanelsTucked(!d->panelsTucked); };
    }
    if (d->rail) d->panelColumn->setStrip(d->rail);
    return d->panelColumn;
}

KisStudioPanelColumn *KisStudioLayout::groupColumn(int group)
{
    if (group <= 0) return panelColumn();
    if (KisStudioPanelColumn *column = d->groupColumns.value(group)) return column;
    KisConfig cfg(true);
    const QString spotKey = group == 1 ? farSpotKey : groupKey(group, "Spot");
    const bool left = group == 1 ? !d->panelsLeft : cfg.readEntry<QString>(groupKey(group, "Side"), QString()) == QLatin1String("left");
    KisStudioPanelColumn *column = createColumn(group, left);
    if (!column) return nullptr;
    column->setObjectName(group == 1 ? QStringLiteral("studioFarPanels") : QStringLiteral("studioPanelGroup%1").arg(group));
    // it has no panel buttons to drag it by; a tap folds its panels
    auto *grip = new DragGrip(column);
    grip->setObjectName(QStringLiteral("studioGroupGrip"));
    grip->setToolTip(i18nc("@info:tooltip a group of panels' grip", "Drag: move\nClick: fold"));
    column->setStrip(grip);
    column->tapped = [this, group]() { foldGroup(group); };
    column->setSpot(readSpot(spotKey), readSpotHeight(spotKey));
    column->moved = [this, group, spotKey](bool left, const QPointF &spot) {
        KisStudioPanelColumn *own = d->groupColumns.value(group);
        writeSpot(spotKey, spot, own ? own->refHeight() : 0);
        if (group == 1) {
            if (left == d->panelsLeft) {
                setPanelsSide(!left);
                return;
            }
        } else {
            KisSettingsRegistry::writeLater(groupKey(group, "Side"), left ? QStringLiteral("left") : QStringLiteral("right"));
        }
        updateCanvasInsets();
    };
    d->groupColumns.insert(group, column);
    return column;
}

int KisStudioLayout::newPanelGroup(const QPoint &windowPos)
{
    const int group = freeGroup();
    QMdiArea *mdi = mdiArea();
    QWidget *area = mdi ? mdi->parentWidget() : nullptr;
    if (!area) return group;
    // where it was dropped, as the shares a column keeps (see shareOf()): its top middle under the pointer
    const QPoint at = area->mapFrom(d->mainWindow, windowPos);
    const bool left = at.x() < area->width() / 2;
    const int width = d->panelColumn ? d->panelColumn->columnWidth() : 270;
    const int margin = KisStudioPanelColumn::margin;
    const int x = at.x() - width / 2;
    const int across = left ? x - margin : area->width() - margin - width - x;
    const int top = area->property(topInsetProperty).toInt() + margin;
    const int bottom = area->height() - area->property(bottomInsetProperty).toInt() - margin;
    // its height is not known yet: a guess, so the share puts its top about where it was dropped
    const int guess = qMin(320, bottom - top);
    KisConfig cfg(false);
    cfg.writeEntry(groupKey(group, "Side"), left ? QStringLiteral("left") : QStringLiteral("right"));
    d->laneWidths.insert(group, {width});
    saveWidths(group);
    writeSpot(groupKey(group, "Spot"), QPointF(shareOf(across, area->width() - 2 * margin - width), shareOf(at.y() - 12 - top, bottom - top - guess)), guess);
    return group;
}

KisStudioPanelColumn *KisStudioLayout::createColumn(int group, bool left)
{
    QMdiArea *area = mdiArea();
    if (!area) return nullptr;
    if (!d->laneWidths.contains(group)) d->laneWidths.insert(group, readWidths(group));
    auto *column = new KisStudioPanelColumn(area->parentWidget(), left, d->laneWidths.value(group));
    column->group = group;
    QPointer<KisStudioPanelColumn> self = column;
    column->resized = [this, self, group](bool done) {
        // refit the views only when a width drag ends; doing it on every move costs most of a frame
        updateCanvasInsets(done);
        if (done && self) {
            d->laneWidths.insert(group, self->laneWidths());
            saveWidths(group);
        }
    };
    column->stackMade = [this](PanelStack *stack) {
        // a group with no panel shown is hidden: one shown again brings it back
        stack->shownChanged = [this]() { QTimer::singleShot(0, this, [this]() { updateColumns(); }); };
        stack->userHeight = [this](const QDockWidget *dock) { return d->heights.value(dock->objectName(), 0); };
        stack->settled = [this](QDockWidget *dock, int height) {
            d->heights.insert(dock->objectName(), height);
            saveHeights();
        };
        stack->resizedByUser = [this](QDockWidget *dock, int height, bool done) {
            if (height > 0) d->heights.insert(dock->objectName(), height);
            else d->heights.remove(dock->objectName());
            if (done) saveHeights();
        };
    };
    column->joining = [this](KisStudioPanelColumn *dragged, const QRect &placed, bool dropped) { return joinAt(dragged, placed, dropped); };
    column->separated = [this, self](int lane) {
        if (self) separateLanes(self, lane);
    };
    column->separatedBelow = [this, self](int lane, QDockWidget *above) {
        if (self) separateBelow(self, lane, above);
    };
    return column;
}

void KisStudioLayout::setPanelsTucked(bool tucked)
{
    d->panelsTucked = tucked;
    KisSettingsRegistry::writeLater(panelsHiddenKey, tucked);
    if (d->stripGrip) {
        d->stripGrip->setToolTip(tucked ? i18nc("@info:tooltip", "Click: show panels\nDrag: move")
                                        : i18nc("@info:tooltip", "Drag: move\nClick: hide"));
    }
    updateColumns();
}

void KisStudioLayout::setToolRailTucked(bool tucked)
{
    d->toolRailTucked = tucked;
    KisSettingsRegistry::writeLater(toolRailHiddenKey, tucked);
    if (auto *rail = static_cast<RailOverlay *>(d->brushRail.data())) rail->setTucked(tucked);
}

void KisStudioLayout::setTopBarTucked(bool tucked)
{
    d->topBarTucked = tucked;
    KisSettingsRegistry::writeLater(topBarHiddenKey, tucked);
    if (d->topPill) d->topPill->setTucked(tucked);
    updateCanvasInsets();
}

void KisStudioLayout::setPanelsSide(bool left)
{
    d->panelsLeft = left;
    KisSettingsRegistry::writeLater(panelSideKey, left ? QStringLiteral("left") : QStringLiteral("right"));
    d->arrangement.clear();
    arrangePinned();
}

void KisStudioLayout::setToolRailSide(bool left)
{
    d->toolRailLeft = left;
    KisSettingsRegistry::writeLater(toolRailSideKey, left ? QStringLiteral("left") : QStringLiteral("right"));
    if (auto *rail = static_cast<RailOverlay *>(d->brushRail.data())) rail->setPlace(left, d->toolRailSpot);
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

void KisStudioLayout::showHome(bool show)
{
    // back only to an image that is open
    if (show == d->mainWindow->isWelcomeScreenShown() || (!show && !d->mainWindow->activeView())) return;
    d->mainWindow->showWelcomeScreen(show);
    if (show && d->startPage) d->startPage->setFocus(Qt::OtherFocusReason);
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

void KisStudioLayout::foldGroup(int group)
{
    KisStudioPanelColumn *column = group == 0 ? d->panelColumn.data() : d->groupColumns.value(group).data();
    if (!column) return;
    // folded all, unless all are folded already
    bool open = false;
    Q_FOREACH (QDockWidget *dock, column->panels()) open = open || (!dock->isHidden() && !d->folded.contains(dock->objectName()));
    Q_FOREACH (QDockWidget *dock, column->panels()) {
        if (!dock->isHidden() && d->folded.contains(dock->objectName()) != open) gripClicked(dock->objectName());
    }
}

void KisStudioLayout::updateColumns()
{
    Q_FOREACH (KisStudioPanelColumn *column, d->columns()) {
        // the only panel of a group needs no title bar of its own: the group's grip moves and folds it; folded,
        // the bar shows its name
        const QList<QDockWidget *> docks = column->panels();
        const bool alone = column != d->panelColumn && docks.size() == 1;
        bool allFolded = !docks.isEmpty();
        Q_FOREACH (QDockWidget *dock, docks) {
            PanelGrip *title = d->grips.value(dock->objectName());
            allFolded = allFolded && d->folded.contains(dock->objectName());
            if (!title || title->isAlone() == alone) continue;
            title->setAlone(alone);
            for (PanelStack *stack : column->stacks()) stack->later();
        }
        if (QWidget *grip = column->findChild<QWidget *>(QStringLiteral("studioGroupGrip"), Qt::FindDirectChildrenOnly)) {
            grip->setToolTip(allFolded ? i18nc("@info:tooltip a group of panels' grip", "Drag: move\nClick: unfold")
                                       : i18nc("@info:tooltip a group of panels' grip", "Drag: move\nClick: fold"));
        }
    }
    Q_FOREACH (KisStudioPanelColumn *column, d->columns()) {
        bool any = false;
        Q_FOREACH (QDockWidget *dock, column->panels()) {
            any = any || !dock->isHidden();
        }
        column->setPanelsShown(any && !arePanelsHidden() && !d->panelsTucked);
        // the other groups are nothing without their panels; the column with the panel buttons keeps its strip
        const bool kept = column == d->panelColumn || column->panelsShown();
        column->setVisible(d->active && kept && (!isRailHidden() || d->peeking.contains(QPointer<QWidget>(column))));
    }
    updateCanvasInsets();
}

void KisStudioLayout::updateCanvasInsets(bool views)
{
    placeWindowHandle();
    // the zoom pill keeps off the panels wherever they go, not only off those docked
    if (d->hud) d->hud->reposition();
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
    Q_FOREACH (KisStudioPanelColumn *column, d->columns()) {
        // one floating over the canvas leaves it be
        if (column->isHidden() || !column->isDocked()) continue;
        const bool left = column->isLeft();
        const int reach = left ? column->targetGeometry().right() + 1 : area->width() - column->targetGeometry().x();
        // the tool rail sits beside the pill, even when only its strip shows
        (left ? pillLeft : pillRight) = qMax(left ? pillLeft : pillRight, reach);
        if (column->panelsShown()) (left ? fullLeft : fullRight) = qMax(left ? fullLeft : fullRight, reach);
    }
    int top = 0;
    int bottom = 0;
    for (TopPill *pill : {d->topPill.data(), d->windowPill.data()}) {
        // a pill only peeking over the canvas doesn't push anything, nor does one dragged away from the edges
        if (!pill || pill->isHidden() || d->peeking.contains(pill)) continue;
        const QRect at = pill->geometry();
        if (at.top() <= TopPill::margin) top = qMax(top, at.bottom() + 1);
        else if (at.bottom() + 1 >= area->height() - TopPill::margin) bottom = qMax(bottom, area->height() - at.top());
    }
    // with its panels shown, the pill covers the canvas's side, so the HUD and the view keep clear of it
    const QMargins insets(fullLeft, top, fullRight, bottom);
    if (canvasInsets(area) != insets || area->property(leftPillProperty).toInt() != pillLeft
        || area->property(rightPillProperty).toInt() != pillRight) {
        area->setProperty(leftInsetProperty, insets.left());
        area->setProperty(rightInsetProperty, insets.right());
        area->setProperty(topInsetProperty, insets.top());
        area->setProperty(bottomInsetProperty, insets.bottom());
        Q_FOREACH (KisStudioPanelColumn *column, d->columns()) {
            column->place();
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
        if (bottom) viewInsets.setBottom(qMax(0, widget.bottom() + 1 - area->mapToGlobal(QPoint(0, area->height() - bottom)).y()));
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
        Q_FOREACH (KisStudioPanelColumn *column, d->columns()) {
            for (PanelStack *stack : column->stacks()) stack->later();
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
    KisSettingsRegistry::writeLater(heightsKey, entries);
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
    KisSettingsRegistry::writeLater(foldedKey, QStringList(d->folded.values()));
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
        updateColumns();
        to = grip->minimumHeight();
    } else {
        applyFold(dock, false);
        updateColumns();
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
    dock->setMaximumHeight(fold && grip ? grip->minimumHeight() : QWIDGETSIZE_MAX);
    updateColumns();
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
    d->hiddenPinned.remove(id);
    const int group = drop.fresh ? newPanelGroup(drop.at) : drop.group;
    if (group > 0) d->groups.insert(id, group);
    else d->groups.remove(id);
    const int lane = drop.fresh ? 0 : drop.lane;
    if (drop.newLane && !drop.fresh) {
        // a new lane: those from there on move one along, and it is as wide as a new group
        Q_FOREACH (const QString &other, d->pinned) {
            if (other != id && d->groupOf(other) == group && d->laneOf(other) >= lane) d->lanes.insert(other, d->laneOf(other) + 1);
        }
        QVector<int> widths = d->laneWidths.value(group);
        widths.insert(qBound(0, lane, int(widths.size())), d->panelColumn ? d->panelColumn->columnWidth() : 270);
        d->laneWidths.insert(group, widths);
        saveWidths(group);
    }
    if (lane > 0) d->lanes.insert(id, lane);
    else d->lanes.remove(id);
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
        Q_FOREACH (QDockWidget *other, d->column(id, group, drop.newLane ? -1 : lane)) {
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
    KisSettingsRegistry::writeLater(pinnedKey, pinned);
    QStringList far;
    QStringList floating;
    for (auto it = d->groups.constBegin(); it != d->groups.constEnd(); ++it) {
        if (!d->pinned.contains(it.key())) continue;
        if (it.value() == 1) far << it.key();
        else if (it.value() >= 2) floating << it.key() + QLatin1Char('=') + QString::number(it.value());
    }
    QStringList lanes;
    for (auto it = d->lanes.constBegin(); it != d->lanes.constEnd(); ++it) {
        if (d->pinned.contains(it.key()) && it.value() > 0) lanes << it.key() + QLatin1Char('=') + QString::number(it.value());
    }
    far.sort();
    floating.sort();
    lanes.sort();
    KisSettingsRegistry::writeLater(farKey, far);
    KisSettingsRegistry::writeLater(groupsKey, floating);
    KisSettingsRegistry::writeLater(lanesKey, lanes);
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
    KisSettingsRegistry::writeLater(panelsKey, panels);
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
    rail->moved = [this](bool left, const QPointF &spot) {
        d->toolRailSpot = spot;
        writeSpot(toolRailSpotKey, spot);
        setToolRailSide(left);
    };
    rail->tapped = [this]() { setToolRailTucked(!d->toolRailTucked); };
    // the panels on its side go beside it
    rail->reachChanged = [this]() {
        Q_FOREACH (KisStudioPanelColumn *column, d->columns()) column->place();
        updateCanvasInsets();
    };
    rail->setPlace(d->toolRailLeft, d->toolRailSpot);
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
        auto *handle = new WindowHandle(area);
        handle->clicked = [this]() { showHome(true); };
        d->windowHandle = handle;
    }
    ensureSettings();
    if (!d->topPill) {
        d->topPill = new TopPill(area, false);
        d->topPill->changed = [self = QPointer<KisStudioLayout>(this)]() { if (self) self->updateCanvasInsets(); };
        d->topPill->grip()->tapped = [this]() { setTopBarTucked(!d->topBarTucked); };
        d->topPill->moved = [this](const QPointF &spot) {
            d->topBarSpot = spot;
            writeSpot(topBarSpotKey, spot);
            updateCanvasInsets();
        };
        d->topPill->setTucked(d->topBarTucked);
        d->topPill->setSpot(d->topBarSpot);
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
    // cap a tool rail in the top left corner, matching its width
    QWidget *rail = d->brushRail;
    QWidget *area = handle->parentWidget();
    if (rail && rail->isVisible() && rail->parentWidget() == area && rail->x() <= area->property(leftPillProperty).toInt() + 16
        && rail->y() < place.bottom() + 24) {
        place.setLeft(rail->x());
        place.setWidth(rail->width());
    }
    if (handle->geometry() != place) handle->setGeometry(place);
    keepAbove(handle);
    area->setProperty(handleBottomProperty, handle->isVisible() ? place.bottom() + 1 : 0);
    TopPill *window = d->windowPill;
    pill->setInsets(handle->isVisible() ? place.right() + 1 : 0, window && window->isVisible() ? area->width() - window->x() : 0);
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
    if (!d->mainMenu) d->mainMenu = new QMenu(d->mainWindow);
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
        if (!top->menu() || !top->isVisible() || !hasContent(top->menu())) continue;
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
    const QString tool = KisStudioTools::activeToolId();
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

    const QString tool = KisStudioTools::activeToolId();
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
    const int reach = 6;
    // most moves are nowhere near an edge, with nothing brought back to keep
    if (d->peeking.isEmpty() && area->rect().adjusted(reach, reach, -reach, -reach).contains(at)) {
        return;
    }
    // what each edge brings back: the bars at the top, the tool rail and the panels at their sides
    const bool toolsLeft = d->brushRail && d->brushRail->x() < area->width() / 2;
    struct Edge {
        QWidget *widget;
        bool near;
    };
    QList<Edge> edges = {
        {d->topPill.data(), d->topPill && !d->topPill->isAtTop() ? at.y() >= area->height() - reach : at.y() < reach},
        {d->brushRail.data(), toolsLeft ? at.x() < reach : at.x() >= area->width() - reach},
        {d->panelColumn.data(), d->panelColumn && d->panelColumn->isLeft() ? at.x() < reach : at.x() >= area->width() - reach},
    };
    Q_FOREACH (KisStudioPanelColumn *other, d->columns()) {
        if (other == d->panelColumn) continue;
        edges.append({other, other->panelsShown() && (other->isLeft() ? at.x() < reach : at.x() >= area->width() - reach)});
    }
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

void KisStudioLayout::tidyCommand(QAction *action)
{
    if (!action || action->isSeparator() || action->objectName().isEmpty()) return;
    // what only the classic layout has goes, also when Krita makes it again (the toolbars' menu, About KDE)
    if (classicOnlyActions().contains(action->objectName())) {
        if (action->isVisible()) {
            action->setVisible(false);
            d->hiddenToolbarActions << action;
        }
        return;
    }
    // commands Krita names in code keep its Title Case ("Gaussian Blur…", "Merge with Layer Below"); items without
    // a name of their own (recent files, workspaces) are the user's and keep theirs
    const QString text = KisSentenceCase::of(action->text());
    if (text != action->text()) action->setText(text);
}

bool KisStudioLayout::eventFilter(QObject *watched, QEvent *event)
{
    if ((event->type() == QEvent::ActionAdded || event->type() == QEvent::ActionChanged) && qobject_cast<QMenu *>(watched)) {
        tidyCommand(static_cast<QActionEvent *>(event)->action());
    }
    // a number box's right click is Qt's menu of Undo, Cut, Paste and Step up: none of it is wanted on a number (the
    // app's own values reset on a right click)
    if (event->type() == QEvent::ContextMenu) {
        auto *widget = qobject_cast<QWidget *>(watched);
        if (widget && (qobject_cast<QAbstractSpinBox *>(widget) || qobject_cast<QAbstractSpinBox *>(widget->parentWidget()))) {
            return true;
        }
    }
    if ((event->type() == QEvent::MouseMove || event->type() == QEvent::TabletMove) && isCanvasOnly()) {
        // only moves over this window
        auto *widget = qobject_cast<QWidget *>(watched);
        const QPoint global = event->type() == QEvent::MouseMove ? static_cast<QMouseEvent *>(event)->globalPos()
                                                                  : static_cast<QTabletEvent *>(event)->globalPos();
        if (widget && widget->window() == d->mainWindow) peek(global);
    }
    // what the pointer brought back goes when it leaves the window (to another screen), unless it went to a menu
    // or popup of the window's own
    if (event->type() == QEvent::Leave && watched == d->mainWindow && isCanvasOnly() && !d->peeking.isEmpty()) {
        QTimer::singleShot(300, this, [this]() {
            if (!isCanvasOnly() || QApplication::activePopupWidget() || QApplication::widgetAt(QCursor::pos())) return;
            for (const QPointer<QWidget> &widget : d->peeking) {
                if (widget) KisStudioMotion::hideFaded(widget);
            }
            d->peeking.clear();
        });
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
