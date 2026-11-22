/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIOLAYOUTPARTS_P_H
#define KISSTUDIOLAYOUTPARTS_P_H

#include "KisStudioLayout.h"
#include "KisStudioGlide.h"
#include "KisStudioTools.h"

#include <KisSentenceCase.h>
#include <KisResourceItemChooser.h>
#include "KisColorDrop.h"

#include <memory>
#include <QScopedValueRollback>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <ksharedconfig.h>
#include <kconfiggroup.h>
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
#include <QLabel>
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
#include <QScreen>
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
#include <kxmlguifactory.h>

#include "KisDocument.h"
#include "KisMainWindow.h"
#include "KisModernStyle.h"
#include "KisResourceServerProvider.h"
#include "KisSettingsRegistry.h"
#include "KisSkissaUpdater.h"
#include "KisStudioBarButtons.h"
#include "KisStudioBrushTweaks.h"
#include "KisSettingsTransfer.h"
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
#include "KisStudioPanelPages.h"
#include "KisStudioWindowFrame.h"
#include "KisTouchMode.h"
#include "KisView.h"
#include "KisViewManager.h"
#include "kis_node_manager.h"
#include <kis_image.h>
#include <kis_image_animation_interface.h>
#include <kis_layer.h>
#include <kis_paint_layer.h>
#include <QSharedPointer>
#include "KisWelcomePageWidget.h"
#include "KisStudioStartPage.h"
#include "widgets/KisStudioPopup.h"
#include "widgets/KisStudioColorField.h"
#include "canvas/kis_canvas2.h"
#include "canvas/kis_coordinates_converter.h"
#include "canvas/kis_canvas_controller.h"
#include "kis_canvas_resource_provider.h"
#include "kis_config.h"
#include "kis_filter_manager.h"
#include "kis_icon_utils.h"
#include "kis_paintop_box.h"
#include "kis_selection_actions_panel_button.h"
#include "kis_slider_spin_box.h"
#include "kis_workspace_resource.h"


// The Studio layout's keys, small helpers, grips, handles and chips; private to KisStudioLayout*.cpp

namespace KisStudioLayoutParts
{

/// Menu items only the classic layout has: toolbars, styles and themes of their own, a status bar, tiled windows; and
/// the resource database's explorer, a developer's tool
inline const QStringList &classicOnlyActions()
{
    static const QStringList names = {QStringLiteral("options_configure_toolbars"), QStringLiteral("lock_toolbars"), QStringLiteral("dbexplorer"),
                                      QStringLiteral("toolbars_submenu_action"), QStringLiteral("style_menu"),
                                      QStringLiteral("theme_menu"), QStringLiteral("showStatusBar"), QStringLiteral("windows_tile"),
                                      QStringLiteral("windows_cascade"), QStringLiteral("windows_next"),
                                      QStringLiteral("windows_previous"), QStringLiteral("help_about_kde")};
    return names;
}

/// Whether \p menu shows anything: a menu whose submenus are all empty (Scripts without scripts) leads nowhere
inline bool hasContent(const QMenu *menu)
{
    Q_FOREACH (QAction *action, menu->actions()) {
        if (!action->isVisible() || action->isSeparator()) continue;
        if (!action->menu() || hasContent(action->menu())) return true;
    }
    return false;
}

inline const QString toolBoxId = QStringLiteral("ToolBox");

inline const QString activeKey = QStringLiteral("StudioLayoutActive");
inline const QString panelsKey = QStringLiteral("StudioLayoutPanels");
inline const QString pinnedKey = QStringLiteral("StudioLayoutPinnedPanels");
/// Legacy key: the pinned panels on the left of the canvas, read when no panel side is saved
inline const QString leftKey = QStringLiteral("StudioLayoutLeftPanels");
/// A panel dropped this close to the canvas edge is pinned there
inline const int edgeDropZone = 140;
/// Heights set by the handles between pinned panels, as "id=height" entries; the others fit their contents
inline const QString heightsKey = QStringLiteral("StudioLayoutPanelSizes");
inline const QString foldedKey = QStringLiteral("StudioLayoutFoldedPanels");
/// Pinned panels put away with their button, which stay away
inline const QString hiddenPinnedKey = QStringLiteral("StudioLayoutHiddenPanels");
inline const QString columnWidthKey = QStringLiteral("StudioLayoutPanelWidth");
/// The pinned panels docked on the far side of the canvas, away from the panel buttons
inline const QString farKey = QStringLiteral("StudioLayoutFarPanels");
inline const QString farWidthKey = QStringLiteral("StudioLayoutFarPanelWidth");
/// The pinned panels in floating groups of their own, as "id=group" entries (groups from 2 on)
inline const QString groupsKey = QStringLiteral("StudioLayoutPanelGroups");
/// The pinned panels in a lane of their group's other than the first, as "id=lane" entries
inline const QString lanesKey = QStringLiteral("StudioLayoutPanelLanes");
/// A floating group's own setting: Spot, Width or Side
inline QString groupKey(int group, const char *what)
{
    return QStringLiteral("StudioLayoutPanelGroup%1%2").arg(group).arg(QLatin1String(what));
}
/// "left" or "right"
inline const QString panelSideKey = QStringLiteral("StudioLayoutPanelSide");
inline const QString toolRailSideKey = QStringLiteral("StudioLayoutToolRailSide");
inline const QString panelsHiddenKey = QStringLiteral("StudioLayoutPanelsTucked");
inline const QString toolRailHiddenKey = QStringLiteral("StudioLayoutToolRailTucked");
/// The tool rail lies flat along the top or bottom edge
inline const QString toolRailFlatKey = QStringLiteral("StudioLayoutToolRailFlat");
inline const QString topBarHiddenKey = QStringLiteral("StudioLayoutTopBarTucked");
/// Where each pill was dragged to: "across,down", as shares of the room it has (see shareOf()); across is from its own side
inline const QString toolRailSpotKey = QStringLiteral("StudioLayoutToolRailSpot");
inline const QString panelSpotKey = QStringLiteral("StudioLayoutPanelSpot");
inline const QString farSpotKey = QStringLiteral("StudioLayoutFarPanelSpot");
inline const QString topBarSpotKey = QStringLiteral("StudioLayoutTopBarSpot");
inline const QString classicStateKey = QStringLiteral("StudioLayoutClassicState");

/// The arrangement as of the last step of the undo history (see KisStudioLayout::arrangementNow())
inline QString &committedStore()
{
    static QString committed;
    return committed;
}

/// Every window's layout: an undo of the arrangement arranges them all
inline QList<QPointer<KisStudioLayout>> &everyLayout()
{
    static QList<QPointer<KisStudioLayout>> layouts;
    return layouts;
}

/// Whether \p key holds part of the arrangement undone as one: not whether the Studio is on, the classic layout to go
/// back to, or the panel buttons (a setting of their own, written in the same step when a gesture changes them)
inline bool isArrangementKey(const QString &key)
{
    return key.startsWith(QLatin1String("StudioLayout")) && key != activeKey && key != classicStateKey && key != panelsKey;
}

/// The panels on the rail by default, what painting needs (the palette and History are one ⋯ away); the tool options
/// open from the tool chip instead
inline QStringList defaultRailPanels()
{
    return {
        QStringLiteral("KisLayerBox"),
        QStringLiteral("PresetDocker"),
        QStringLiteral("StudioColor"),
    };
}

inline QPoint globalMousePos(const QMouseEvent *event)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return event->globalPosition().toPoint();
#else
    return event->globalPos();
#endif
}

inline QString railIconName(const QString &id)
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
        {QStringLiteral("TimelineDocker"), QStringLiteral("studio_timeline")},
        {QStringLiteral("AnimationCurvesDocker"), QStringLiteral("studio_anim_curves")},
        {QStringLiteral("OnionSkinsDocker"), QStringLiteral("studio_onion")},
        {QStringLiteral("StoryboardDocker"), QStringLiteral("studio_storyboard")},
        {QStringLiteral("ArrangeDocker"), QStringLiteral("studio_arrange")},
        {QStringLiteral("ChannelDocker"), QStringLiteral("studio_channels")},
        {QStringLiteral("CompositionDocker"), QStringLiteral("studio_compositions")},
        {QStringLiteral("GridDocker"), QStringLiteral("studio_grid")},
        {QStringLiteral("HistogramDocker"), QStringLiteral("studio_histogram")},
        {QStringLiteral("LutDocker"), QStringLiteral("studio_lut")},
        {QStringLiteral("OverviewDocker"), QStringLiteral("studio_overview")},
        {QStringLiteral("PatternDocker"), QStringLiteral("studio_patterns")},
        {QStringLiteral("Snapshot"), QStringLiteral("studio_snapshots")},
        {QStringLiteral("SvgSymbolCollectionDocker"), QStringLiteral("studio_symbols")},
        {QStringLiteral("TextProperties"), QStringLiteral("studio_text_properties")},
    };
    return icons.value(id);
}

/// A panel's title bar: drag to move the panel, click to fold it, right-click to close it
class PanelGrip : public QWidget
{
public:
    std::function<void(const QPoint &globalPos)> dragged;
    std::function<void(const QPoint &globalPos)> dropped;
    std::function<void()> closeRequested;
    std::function<void()> clicked;

    explicit PanelGrip(QWidget *parent) : QWidget(parent)
    {
        setAttribute(Qt::WA_Hover);
        setCursor(Qt::OpenHandCursor);
        setToolTip(i18nc("@info:tooltip a panel's title bar", "Drag: move\nClick: fold\nRight-click: close"));
        QObject::connect(KisTouchMode::instance(), &KisTouchMode::changed, this, [this]() { fit(); });
        fit();
    }

    /// A folded panel shows only this grip, with the panel's name
    void setFolded(bool folded, const QString &title)
    {
        m_folded = folded;
        m_title = title;
        setToolTip(folded ? i18nc("@info:tooltip a panel's title bar", "Drag: move\nClick: unfold\nRight-click: close")
                          : i18nc("@info:tooltip a panel's title bar", "Drag: move\nClick: fold\nRight-click: close"));
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
        if (!m_dragging && (pos - m_pressPos).manhattanLength() >= KisModernStyle::dragDistance(event)) {
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
        // deferred: closing rearranges the panel and its grip
        if (closeRequested) QTimer::singleShot(0, this, [this]() { if (closeRequested) closeRequested(); });
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
            if (!m_scrubbing && qAbs(event->pos().x() - m_pressX) > KisModernStyle::dragDistance(event)) {
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
                // the fill's edge follows the pointer, as a drawn bar's does; Shift a quarter as fast
                const qreal span = qMax(1, width() - 2) * (fine ? 4.0 : 1.0);
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
    /// A right click: the picker of the color under it (the back one or the front one)
    std::function<void(bool background)> pickColor;

    explicit ColorChip(QWidget *parent) : QWidget(parent)
    {
        setFixedSize(40, 40);
        setCursor(Qt::PointingHandCursor);
        setToolTip(i18nc("@info:tooltip the color chip on the tool rail", "Colors\nDrag onto the image: fill\nClick the back one: swap\nRight-click: change it"));
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
        if (!m_pressed || (event->pos() - m_pressAt).manhattanLength() < KisModernStyle::dragDistance(event)) return;
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

    void contextMenuEvent(QContextMenuEvent *event) override
    {
        event->accept();
        if (pickColor) pickColor(onBackground(event->pos()));
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

inline QMargins canvasInsets(const QWidget *canvasArea)
{
    if (!canvasArea) return QMargins();
    return QMargins(canvasArea->property(leftInsetProperty).toInt(), canvasArea->property(topInsetProperty).toInt(),
                    canvasArea->property(rightInsetProperty).toInt(), canvasArea->property(bottomInsetProperty).toInt());
}

/**
 * How much more room the canvas area would have with the window filling its screen. Places are shares of
 * that room, so a smaller window keeps each pill as far from its nearer edge as it is there (see offsetOf()).
 */
inline QSize roomGrowth(const QWidget *widget)
{
    // not by the window's state: a resize arrives before its new state
    const QWidget *window = widget ? widget->window() : nullptr;
    if (!window || !window->screen()) return QSize();
    return window->screen()->availableGeometry().size() - window->size();
}

/// The share of the room at \p offset, measured from the nearer edge in a room \p grown larger (see roomGrowth())
inline qreal shareAt(int offset, int room, int grown = 0)
{
    if (room <= 0) return 0;
    const int whole = room + grown;
    if (grown == 0 || whole <= 0) return qBound(0.0, qreal(offset) / room, 1.0);
    if (2 * offset < room) return qBound(0.0, qreal(offset) / whole, 0.499);
    return qBound(0.501, 1 - qreal(room - offset) / whole, 1.0);
}

/**
 * Where a pill that can be dragged anywhere sits along one axis: the share of the room it can move in,
 * 0 against the near edge and 1 against the far one. Dropped near an edge or the middle, it is caught
 * there (so corners are easy to hit), and it stays there as the window changes size; elsewhere it keeps
 * its distance from the nearer edge.
 */
inline qreal shareOf(int offset, int room, int grown = 0)
{
    const int snap = 20;
    if (room <= 0 || offset < snap) return 0;
    if (offset > room - snap) return 1;
    if (qAbs(offset - room / 2) < snap / 2) return 0.5;
    return shareAt(offset, room, grown);
}

inline int offsetOf(qreal share, int room, int grown = 0)
{
    share = qBound(0.0, share, 1.0);
    room = qMax(0, room);
    if (grown == 0 || share <= 0 || share >= 1 || share == 0.5) return qRound(share * room);
    const int whole = qMax(0, room + grown);
    const int offset = share < 0.5 ? qRound(share * whole) : room - qRound((1 - share) * whole);
    return qBound(0, offset, room);
}

/// A pill's place as "across,down" shares (see shareOf())
inline QPointF readSpot(const QString &key)
{
    const QStringList parts = KisConfig(true).readEntry<QString>(key, QString()).split(QLatin1Char(','));
    if (parts.size() < 2) return QPointF();
    return QPointF(qBound(0.0, parts[0].toDouble(), 1.0), qBound(0.0, parts[1].toDouble(), 1.0));
}

/// The height a group of panels had when its place was taken, written after its spot; 0 if not
inline int readSpotHeight(const QString &key)
{
    const QStringList parts = KisConfig(true).readEntry<QString>(key, QString()).split(QLatin1Char(','));
    return parts.size() >= 3 ? qMax(0, parts[2].toInt()) : 0;
}

/// \p height: the height of a group of panels when its place was taken, so a folded one keeps its top
inline void writeSpot(const QString &key, const QPointF &spot, int height = 0)
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
inline void paintGlass(QPainter &painter, QWidget *widget, qreal radius, qreal rim)
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
    /// A right click on it; without one the parent gets it
    std::function<void()> rightClicked;

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

    void contextMenuEvent(QContextMenuEvent *event) override
    {
        if (!rightClicked) {
            event->ignore();
            return;
        }
        event->accept();
        // deferred: what it does may rearrange or remove the grip
        QTimer::singleShot(0, this, [this]() { if (rightClicked) rightClicked(); });
    }
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
        if (!m_dragging && (pos - m_pressPos).manhattanLength() < KisModernStyle::dragDistance(event)) return;
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
        if (!(event->buttons() & Qt::LeftButton)) {
            // let go in another window, whose release never comes here: done where it is
            m_pressed = false;
            update();
            if (m_moved && dragged) dragged(dy, true);
            return;
        }
        // a tap (or a pen's wobble) isn't a resize
        if (!m_moved && qAbs(dy) < KisModernStyle::dragDistance(event)) return;
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
inline void keepUpright(QDockWidget *dock, bool upright)
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
inline QAbstractScrollArea *mainScrollArea(const QDockWidget *dock)
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
inline int contentsHeight(QAbstractScrollArea *area, int enough = std::numeric_limits<int>::max())
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

} // namespace KisStudioLayoutParts

#endif // KISSTUDIOLAYOUTPARTS_P_H
