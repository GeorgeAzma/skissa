/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisModernStyle.h"

#include <QAbstractButton>
#include <QAbstractSpinBox>
#include <QComboBox>
#include <QHeaderView>
#include <QPainter>
#include <QSlider>
#include <QStyleFactory>
#include <QStyleOption>
#include <QLabel>
#include <functional>
#include <QRegularExpression>
#include <QGridLayout>
#include <QFormLayout>
#include <QTabBar>

namespace {

constexpr qreal radius = KisModernStyle::cornerRadius;

QColor mix(const QColor &a, const QColor &b, qreal t)
{
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t,
                            a.greenF() + (b.greenF() - a.greenF()) * t,
                            a.blueF() + (b.blueF() - a.blueF()) * t,
                            a.alphaF() + (b.alphaF() - a.alphaF()) * t);
}

/// Focus rings and active indicators use the bright accent, fills the muted selection color
QColor focusColor(const QPalette &pal)
{
    return pal.color(QPalette::Link);
}

/**
 * The frame of the window (toolbars, rails, the menu bar) is one step darker
 * than the panels, so the panels and the canvas read as the content.
 */
QColor chromeColor(const QPalette &pal)
{
    const QColor window = pal.color(QPalette::Window);
    return window.lightness() < 128 ? window.darker(136) : window.darker(108);
}

/// Selected rows and menu items: the selection color mixed into the background
QColor selectionTint(const QPalette &pal)
{
    return mix(pal.color(QPalette::Base), pal.color(QPalette::Highlight), 0.55);
}

/// A rect whose 1px outline lands exactly on the pixels of \p rect
QRectF strokeRect(const QRect &rect)
{
    return QRectF(rect).adjusted(0.5, 0.5, -0.5, -0.5);
}

void drawRounded(QPainter *painter, const QRectF &rect, qreal r, const QColor &fill, const QColor &border = QColor())
{
    if (rect.isEmpty() || (!fill.isValid() && !border.isValid())) {
        return;
    }

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setPen(border.isValid() ? QPen(border, 1.0) : QPen(Qt::NoPen));
    painter->setBrush(fill.isValid() ? QBrush(fill) : QBrush(Qt::NoBrush));
    r = qMin(r, qMin(rect.width(), rect.height()) / 2.0);
    painter->drawRoundedRect(rect, r, r);
    painter->restore();
}

void drawChevron(QPainter *painter, const QRectF &rect, Qt::ArrowType direction, const QColor &color)
{
    const qreal size = qBound(4.0, qMin(rect.width(), rect.height()) * 0.5, 8.0);
    const qreal half = size / 2.0;
    const qreal depth = size / 4.0;
    const QPointF c = rect.center();

    QPolygonF points;
    switch (direction) {
    case Qt::UpArrow:
        points << QPointF(c.x() - half, c.y() + depth) << QPointF(c.x(), c.y() - depth) << QPointF(c.x() + half, c.y() + depth);
        break;
    case Qt::DownArrow:
        points << QPointF(c.x() - half, c.y() - depth) << QPointF(c.x(), c.y() + depth) << QPointF(c.x() + half, c.y() - depth);
        break;
    case Qt::LeftArrow:
        points << QPointF(c.x() + depth, c.y() - half) << QPointF(c.x() - depth, c.y()) << QPointF(c.x() + depth, c.y() + half);
        break;
    case Qt::RightArrow:
        points << QPointF(c.x() - depth, c.y() - half) << QPointF(c.x() + depth, c.y()) << QPointF(c.x() - depth, c.y() + half);
        break;
    default:
        return;
    }

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setPen(QPen(color, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter->setBrush(Qt::NoBrush);
    painter->drawPolyline(points);
    painter->restore();
}

struct Interaction {
    explicit Interaction(const QStyleOption *option)
        : enabled(option->state.testFlag(QStyle::State_Enabled))
        , hovered(enabled && option->state.testFlag(QStyle::State_MouseOver))
        , pressed(enabled && option->state.testFlag(QStyle::State_Sunken))
        , checked(option->state.testFlag(QStyle::State_On))
        , keyboardFocus(option->state.testFlag(QStyle::State_HasFocus) && option->state.testFlag(QStyle::State_KeyboardFocusChange))
    {
    }

    bool enabled;
    bool hovered;
    bool pressed;
    bool checked;
    bool keyboardFocus;
};

/**
 * Background of push buttons and non-editable combo boxes. States are made by
 * tinting the button color towards the text color, so the same code works
 * for both dark and light themes.
 */
void drawButtonPanel(QPainter *painter, const QStyleOption *option, bool flat)
{
    const Interaction s(option);
    const QPalette &pal = option->palette;
    const QColor bg = pal.color(QPalette::Button);
    const QColor fg = pal.color(QPalette::ButtonText);

    QColor fill;
    if (s.pressed || s.checked) {
        fill = mix(bg, fg, 0.16);
    } else if (s.hovered) {
        fill = mix(bg, fg, 0.08);
    } else if (!flat) {
        fill = bg;
    }

    // surfaces are separated by lightness, not outlines; only keyboard focus gets a ring
    QColor border;
    if (s.keyboardFocus) {
        border = focusColor(pal);
    }

    drawRounded(painter, strokeRect(option->rect), radius, fill, border);
}

/// The button that Enter activates is the one filled with the selection color
void drawPrimaryButtonPanel(QPainter *painter, const QStyleOption *option)
{
    const Interaction s(option);
    const QPalette &pal = option->palette;
    QColor fill = pal.color(QPalette::Highlight);
    if (s.pressed) {
        fill = fill.darker(115);
    } else if (s.hovered) {
        fill = fill.lighter(112);
    }
    drawRounded(painter, strokeRect(option->rect), radius, fill,
                s.keyboardFocus ? focusColor(pal) : QColor());
}

/// Background of text fields: line edits, spin boxes and editable combo boxes
void drawField(QPainter *painter, const QStyleOption *option, bool fill)
{
    const Interaction s(option);
    const QPalette &pal = option->palette;
    // fields are filled one step above the panel, like buttons, and only get
    // an outline while hovered or focused
    const QColor bg = pal.color(QPalette::Button);
    const QColor fg = pal.color(QPalette::Text);

    QColor border;
    if (s.enabled && option->state.testFlag(QStyle::State_HasFocus)) {
        border = focusColor(pal);
    } else if (s.hovered) {
        border = mix(bg, fg, 0.18);
    }

    drawRounded(painter, strokeRect(option->rect), radius, fill ? bg : QColor(), border);
}

/// A 2px line in the middle of splitter and dock separator handles, only while hovered
void drawHandleHighlight(QPainter *painter, const QStyleOption *option)
{
    if (!option->state.testFlag(QStyle::State_MouseOver) || !option->state.testFlag(QStyle::State_Enabled)) {
        return;
    }

    QColor color = option->palette.color(QPalette::Highlight);
    color.setAlphaF(0.6);

    QRectF line = option->rect;
    const QPointF center = line.center();
    if (line.width() < line.height()) {
        line.setLeft(center.x() - 1.0);
        line.setWidth(2.0);
    } else {
        line.setTop(center.y() - 1.0);
        line.setHeight(2.0);
    }
    painter->fillRect(line, color);
}

} // namespace

KisModernStyle::KisModernStyle()
    : QProxyStyle(QStyleFactory::create(QStringLiteral("fusion")))
{
    setObjectName(styleName());
}

QString KisModernStyle::styleName()
{
    return QStringLiteral("Krita Modern");
}

QStyle *KisModernStyle::create(const QString &name)
{
    if (name.compare(styleName(), Qt::CaseInsensitive) == 0) {
        return new KisModernStyle();
    }
    return QStyleFactory::create(name);
}

QStringList KisModernStyle::availableStyles()
{
    QStringList styles = QStyleFactory::keys();
    styles.prepend(styleName());
    return styles;
}

QString KisModernStyle::underlyingStyleName(const QStyle *style)
{
    if (qobject_cast<const KisModernStyle *>(style)) {
        return QStringLiteral("fusion");
    }
    return style ? style->objectName() : QString();
}

namespace {
/// Whether a label names the field next to it, rather than being a sentence of its own
bool labelsAField(QLabel *label)
{
    if (label->buddy()) {
        return true;
    }
    QWidget *parent = label->parentWidget();
    if (!parent || !parent->layout()) {
        return false;
    }
    std::function<QLayout *(QLayout *)> holding = [&](QLayout *layout) -> QLayout * {
        if (layout->indexOf(label) >= 0) return layout;
        for (int i = 0; i < layout->count(); i++) {
            if (QLayout *child = layout->itemAt(i)->layout()) {
                if (QLayout *found = holding(child)) return found;
            }
        }
        return nullptr;
    };
    QLayout *layout = holding(parent->layout());
    if (auto *form = qobject_cast<QFormLayout *>(layout)) {
        int row = 0;
        QFormLayout::ItemRole role = QFormLayout::FieldRole;
        form->getWidgetPosition(label, &row, &role);
        return role == QFormLayout::LabelRole;
    }
    if (auto *grid = qobject_cast<QGridLayout *>(layout)) {
        int row = 0, column = 0, rowSpan = 0, columnSpan = 0;
        grid->getItemPosition(grid->indexOf(label), &row, &column, &rowSpan, &columnSpan);
        for (int c = column + columnSpan; c < grid->columnCount(); c++) {
            if (QLayoutItem *item = grid->itemAtPosition(row, c)) {
                if (item->widget()) return true;
            }
        }
        return false;
    }
    if (auto *box = qobject_cast<QBoxLayout *>(layout)) {
        const int at = box->indexOf(label);
        const bool horizontal = box->direction() == QBoxLayout::LeftToRight || box->direction() == QBoxLayout::RightToLeft;
        return horizontal && at + 1 < box->count() && box->itemAt(at + 1)->widget();
    }
    return false;
}
} // namespace

void KisModernStyle::polish(QWidget *widget)
{
    QProxyStyle::polish(widget);

    // State_MouseOver is only set for widgets that receive hover events.
    // Fusion already enables them for buttons, sliders and scroll bars.
    if (qobject_cast<QTabBar *>(widget) || qobject_cast<QHeaderView *>(widget)) {
        widget->setAttribute(Qt::WA_Hover, true);
    }

    // "Size:" next to its field says nothing the layout doesn't: the label and the field are a pair
    if (QLabel *label = qobject_cast<QLabel *>(widget)) {
        if (label->textFormat() != Qt::RichText && labelsAField(label)) {
            static const QRegularExpression colon(QStringLiteral("\\s*[:\\x{FF1A}]\\s*$"));
            QString text = label->text();
            if (text.contains(colon)) {
                label->setText(text.remove(colon));
            }
        }
    }
}

int KisModernStyle::pixelMetric(PixelMetric metric, const QStyleOption *option, const QWidget *widget) const
{
    switch (metric) {
    case PM_ScrollBarExtent:
        return 10;
    case PM_ScrollBarSliderMin:
        return 32;
    case PM_ButtonShiftHorizontal:
    case PM_ButtonShiftVertical:
        return 0;
    case PM_MenuHMargin:
    case PM_MenuVMargin:
        return 4;
    case PM_MenuBarItemSpacing:
    case PM_ToolBarItemSpacing:
        return 2;
    case PM_TabBarTabHSpace:
        return QProxyStyle::pixelMetric(metric, option, widget) + 6;
    default:
        break;
    }
    return QProxyStyle::pixelMetric(metric, option, widget);
}

int KisModernStyle::styleHint(StyleHint hint, const QStyleOption *option, const QWidget *widget, QStyleHintReturn *returnData) const
{
    switch (hint) {
    case SH_ComboBox_Popup:
        // drop the list down below the box instead of covering it
        return 0;
    case SH_DitherDisabledText:
    case SH_EtchDisabledText:
        return 0;
    default:
        break;
    }
    return QProxyStyle::styleHint(hint, option, widget, returnData);
}

QSize KisModernStyle::sizeFromContents(ContentsType type, const QStyleOption *option, const QSize &contentsSize, const QWidget *widget) const
{
    QSize size = QProxyStyle::sizeFromContents(type, option, contentsSize, widget);

    switch (type) {
    case CT_MenuItem:
        if (const auto *item = qstyleoption_cast<const QStyleOptionMenuItem *>(option)) {
            if (item->menuItemType != QStyleOptionMenuItem::Separator) {
                size.rheight() += 4;
            }
        }
        break;
    case CT_MenuBarItem:
    case CT_PushButton:
        size.rheight() += 2;
        break;
    case CT_ComboBox:
        // Fusion reserves this space for the check mark of its menu-like
        // popup, which is disabled by SH_ComboBox_Popup
        size.rwidth() += 10;
        break;
    default:
        break;
    }
    return size;
}

QRect KisModernStyle::subControlRect(ComplexControl control, const QStyleOptionComplex *option, SubControl subControl, const QWidget *widget) const
{
    if (control == CC_ScrollBar) {
        // Same layout as QCommonStyle, but without the arrow buttons
        if (const auto *bar = qstyleoption_cast<const QStyleOptionSlider *>(option)) {
            const QRect rect = bar->rect;
            const bool horizontal = bar->orientation == Qt::Horizontal;
            const int length = horizontal ? rect.width() : rect.height();

            int sliderLength = length;
            if (bar->maximum != bar->minimum) {
                const qint64 range = qint64(bar->maximum) - bar->minimum;
                sliderLength = int(qint64(bar->pageStep) * length / (range + bar->pageStep));
                sliderLength = qMax(sliderLength, proxy()->pixelMetric(PM_ScrollBarSliderMin, bar, widget));
                sliderLength = qMin(sliderLength, length);
            }
            const int sliderStart = sliderPositionFromValue(bar->minimum, bar->maximum, bar->sliderPosition,
                                                            length - sliderLength, bar->upsideDown);

            auto span = [&](int start, int size) {
                return horizontal ? QRect(start, 0, size, rect.height()) : QRect(0, start, rect.width(), size);
            };

            QRect result;
            switch (subControl) {
            case SC_ScrollBarGroove:
                result = span(0, length);
                break;
            case SC_ScrollBarSlider:
                result = span(sliderStart, sliderLength);
                break;
            case SC_ScrollBarSubPage:
                result = span(0, sliderStart);
                break;
            case SC_ScrollBarAddPage:
                result = span(sliderStart + sliderLength, length - sliderStart - sliderLength);
                break;
            default:
                // SC_ScrollBarAddLine, SC_ScrollBarSubLine, SC_ScrollBarFirst, SC_ScrollBarLast
                return QRect();
            }
            return visualRect(bar->direction, rect, result);
        }
    }

    return QProxyStyle::subControlRect(control, option, subControl, widget);
}

void KisModernStyle::drawPrimitive(PrimitiveElement element, const QStyleOption *option, QPainter *painter, const QWidget *widget) const
{
    switch (element) {
    case PE_PanelButtonCommand:
    case PE_PanelButtonBevel: {
        const auto *button = qstyleoption_cast<const QStyleOptionButton *>(option);
        const bool flat = button && button->features.testFlag(QStyleOptionButton::Flat);
        if (button && button->features.testFlag(QStyleOptionButton::DefaultButton) && !flat
            && option->state.testFlag(State_Enabled)) {
            drawPrimaryButtonPanel(painter, option);
        } else {
            drawButtonPanel(painter, option, flat);
        }
        return;
    }

    case PE_PanelButtonTool: {
        const Interaction s(option);
        const QPalette &pal = option->palette;
        const QColor bg = pal.color(QPalette::Button);
        const QColor fg = pal.color(QPalette::ButtonText);
        const QColor accent = pal.color(QPalette::Highlight);

        QColor fill;
        QColor border;
        if (s.checked) {
            // e.g. the active tool in the toolbox
            fill = mix(bg, accent, s.hovered ? 0.55 : 0.45);
            border = mix(bg, accent, 0.8);
        } else if (s.pressed) {
            fill = mix(bg, fg, 0.16);
        } else if (s.hovered) {
            fill = mix(bg, fg, 0.08);
        } else if (!option->state.testFlag(State_AutoRaise)) {
            fill = bg;
            border = mix(bg, fg, 0.14);
        }
        if (s.keyboardFocus) {
            border = accent;
        }
        drawRounded(painter, strokeRect(option->rect), radius, fill, border);
        return;
    }

    case PE_FrameFocusRect:
        // Buttons and combo boxes show keyboard focus with their border
        if (qobject_cast<const QAbstractButton *>(widget) || qobject_cast<const QComboBox *>(widget)) {
            return;
        }
        if (option->state.testFlag(State_KeyboardFocusChange)) {
            drawRounded(painter, strokeRect(option->rect), radius, QColor(), option->palette.color(QPalette::Highlight));
        }
        return;

    case PE_PanelLineEdit:
        if (const auto *frame = qstyleoption_cast<const QStyleOptionFrame *>(option)) {
            // Editors embedded into spin boxes and combo boxes have no frame,
            // their parent paints the field for them
            if (frame->lineWidth > 0) {
                drawField(painter, option, true);
            }
            return;
        }
        break;

    case PE_FrameLineEdit:
        drawField(painter, option, false);
        return;

    case PE_IndicatorCheckBox: {
        const Interaction s(option);
        const QPalette &pal = option->palette;
        const QColor bg = pal.color(QPalette::Base);
        const QColor fg = pal.color(QPalette::Text);
        const QColor accent = pal.color(QPalette::Highlight);
        const bool partial = option->state.testFlag(State_NoChange);
        const bool marked = s.checked || partial;

        const qreal size = qMin(option->rect.width(), option->rect.height());
        QRectF box(0, 0, size, size);
        box.moveCenter(QRectF(option->rect).center());
        box.adjust(0.5, 0.5, -0.5, -0.5);

        QColor border = marked ? accent : mix(bg, fg, s.hovered ? 0.5 : 0.35);
        if (s.keyboardFocus) {
            border = marked ? fg : accent;
        }
        drawRounded(painter, box, 3, marked ? accent : bg, border);

        auto at = [&](qreal x, qreal y) {
            return QPointF(box.left() + box.width() * x, box.top() + box.height() * y);
        };

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->setPen(QPen(pal.color(QPalette::HighlightedText), qMax(1.5, size / 9.0), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        if (partial) {
            painter->drawLine(at(0.28, 0.5), at(0.72, 0.5));
        } else if (s.checked) {
            painter->drawPolyline(QPolygonF() << at(0.26, 0.52) << at(0.43, 0.69) << at(0.75, 0.33));
        }
        painter->restore();
        return;
    }

    case PE_IndicatorRadioButton: {
        const Interaction s(option);
        const QPalette &pal = option->palette;
        const QColor bg = pal.color(QPalette::Base);
        const QColor fg = pal.color(QPalette::Text);
        const QColor accent = pal.color(QPalette::Highlight);

        const qreal size = qMin(option->rect.width(), option->rect.height());
        QRectF circle(0, 0, size, size);
        circle.moveCenter(QRectF(option->rect).center());
        circle.adjust(0.5, 0.5, -0.5, -0.5);

        QColor border = s.checked ? accent : mix(bg, fg, s.hovered ? 0.5 : 0.35);
        if (s.keyboardFocus) {
            border = s.checked ? fg : accent;
        }

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->setPen(QPen(border, 1.0));
        painter->setBrush(s.checked ? accent : bg);
        painter->drawEllipse(circle);
        if (s.checked) {
            const qreal dot = size * 0.2;
            painter->setPen(Qt::NoPen);
            painter->setBrush(pal.color(QPalette::HighlightedText));
            painter->drawEllipse(circle.center(), dot, dot);
        }
        painter->restore();
        return;
    }

    case PE_IndicatorArrowUp:
    case PE_IndicatorArrowDown:
    case PE_IndicatorArrowLeft:
    case PE_IndicatorArrowRight: {
        const Qt::ArrowType direction =
            element == PE_IndicatorArrowUp ? Qt::UpArrow :
            element == PE_IndicatorArrowDown ? Qt::DownArrow :
            element == PE_IndicatorArrowLeft ? Qt::LeftArrow : Qt::RightArrow;
        drawChevron(painter, option->rect, direction, option->palette.color(QPalette::ButtonText));
        return;
    }

    case PE_FrameGroupBox:
        // no boxes around boxes: the title and the spacing group the contents
        return;

    case PE_FrameTabWidget: {
        const QColor bg = option->palette.color(QPalette::Window);
        const QColor fg = option->palette.color(QPalette::WindowText);
        drawRounded(painter, strokeRect(option->rect), radius + 1, QColor(), mix(bg, fg, 0.12));
        return;
    }

    case PE_FrameTabBarBase:
    case PE_FrameDockWidget:
        return;

    case PE_IndicatorDockWidgetResizeHandle:
        drawHandleHighlight(painter, option);
        return;

    case PE_FrameMenu: {
        const QColor bg = option->palette.color(QPalette::Window);
        const QColor fg = option->palette.color(QPalette::WindowText);
        painter->save();
        painter->setPen(mix(bg, fg, 0.18));
        painter->setBrush(Qt::NoBrush);
        painter->drawRect(QRectF(option->rect).adjusted(0.5, 0.5, -0.5, -0.5));
        painter->restore();
        return;
    }

    case PE_PanelMenuBar:
        painter->fillRect(option->rect, chromeColor(option->palette));
        return;

    case PE_IndicatorToolBarSeparator: {
        const QColor bg = option->palette.color(QPalette::Window);
        const QColor fg = option->palette.color(QPalette::WindowText);
        const QRect r = option->rect;
        painter->save();
        painter->setPen(mix(bg, fg, 0.18));
        if (option->state.testFlag(State_Horizontal)) {
            // a separator in a horizontal toolbar is a vertical line
            const int x = r.center().x();
            painter->drawLine(x, r.top() + 4, x, r.bottom() - 4);
        } else {
            const int y = r.center().y();
            painter->drawLine(r.left() + 4, y, r.right() - 4, y);
        }
        painter->restore();
        return;
    }

    case PE_PanelItemViewRow:
        if (const auto *item = qstyleoption_cast<const QStyleOptionViewItem *>(option)) {
            if (item->state.testFlag(State_Selected)) {
                // The selection is drawn as rounded pills by PE_PanelItemViewItem,
                // don't fill the branch/indentation area of tree views around them
                QStyleOptionViewItem row = *item;
                row.state.setFlag(State_Selected, false);
                QProxyStyle::drawPrimitive(element, &row, painter, widget);
                return;
            }
        }
        break;

    case PE_PanelItemViewItem:
        if (const auto *item = qstyleoption_cast<const QStyleOptionViewItem *>(option)) {
            const bool selected = item->state.testFlag(State_Selected);
            const bool hovered = item->state.testFlag(State_MouseOver) && item->state.testFlag(State_Enabled);
            if (!selected && !hovered) {
                break;
            }

            if (item->backgroundBrush.style() != Qt::NoBrush) {
                painter->fillRect(item->rect, item->backgroundBrush);
            }

            const QColor fill = selected
                ? selectionTint(item->palette)
                : mix(item->palette.color(QPalette::Base), item->palette.color(QPalette::Text), 0.06);

            // A row spanning several columns is drawn as one pill: only
            // round the outer ends and let the inner edges run past the clip
            const qreal extend = radius + 1;
            QStyleOptionViewItem::ViewItemPosition position = item->viewItemPosition;
            if (item->direction == Qt::RightToLeft) {
                if (position == QStyleOptionViewItem::Beginning) {
                    position = QStyleOptionViewItem::End;
                } else if (position == QStyleOptionViewItem::End) {
                    position = QStyleOptionViewItem::Beginning;
                }
            }

            QRectF shape = QRectF(item->rect).adjusted(1, 1, -1, -1);
            switch (position) {
            case QStyleOptionViewItem::Beginning:
                shape.adjust(0, 0, extend, 0);
                break;
            case QStyleOptionViewItem::Middle:
                shape.adjust(-extend, 0, extend, 0);
                break;
            case QStyleOptionViewItem::End:
                shape.adjust(-extend, 0, 0, 0);
                break;
            default:
                break;
            }

            painter->save();
            painter->setClipRect(item->rect, Qt::IntersectClip);
            drawRounded(painter, shape, radius - 1, fill);
            painter->restore();
            return;
        }
        break;

    default:
        break;
    }

    QProxyStyle::drawPrimitive(element, option, painter, widget);
}

void KisModernStyle::drawControl(ControlElement element, const QStyleOption *option, QPainter *painter, const QWidget *widget) const
{
    switch (element) {
    case CE_PushButtonBevel:
        if (const auto *button = qstyleoption_cast<const QStyleOptionButton *>(option)) {
            proxy()->drawPrimitive(PE_PanelButtonCommand, button, painter, widget);
            if (button->features.testFlag(QStyleOptionButton::HasMenu)) {
                const int indicator = proxy()->pixelMetric(PM_MenuButtonIndicator, button, widget);
                QStyleOptionButton arrow = *button;
                arrow.rect = visualRect(button->direction, button->rect,
                                        QRect(button->rect.right() - indicator - 4, button->rect.top(),
                                              indicator, button->rect.height()));
                proxy()->drawPrimitive(PE_IndicatorArrowDown, &arrow, painter, widget);
            }
            return;
        }
        break;

    case CE_PushButtonLabel:
        if (const auto *button = qstyleoption_cast<const QStyleOptionButton *>(option)) {
            if (button->features.testFlag(QStyleOptionButton::DefaultButton)
                && !button->features.testFlag(QStyleOptionButton::Flat)
                && button->state.testFlag(State_Enabled)) {
                QStyleOptionButton label = *button;
                label.palette.setColor(QPalette::ButtonText, button->palette.color(QPalette::HighlightedText));
                QProxyStyle::drawControl(element, &label, painter, widget);
                return;
            }
        }
        break;

    case CE_MenuItem:
        if (const auto *item = qstyleoption_cast<const QStyleOptionMenuItem *>(option)) {
            const bool selected = item->state.testFlag(State_Selected) && item->state.testFlag(State_Enabled);
            if (item->menuItemType == QStyleOptionMenuItem::Separator || !selected) {
                break;
            }

            drawRounded(painter, QRectF(item->rect).adjusted(3, 1, -3, -1), radius - 1,
                        selectionTint(item->palette));

            // Let Fusion draw the contents as if unselected, but in the selected text color
            QStyleOptionMenuItem contents = *item;
            contents.state.setFlag(State_Selected, false);
            const QColor text = item->palette.color(QPalette::HighlightedText);
            contents.palette.setColor(QPalette::Text, text);
            contents.palette.setColor(QPalette::ButtonText, text);
            contents.palette.setColor(QPalette::WindowText, text);
            QProxyStyle::drawControl(element, &contents, painter, widget);
            return;
        }
        break;

    case CE_MenuBarItem:
        if (const auto *item = qstyleoption_cast<const QStyleOptionMenuItem *>(option)) {
            const QPalette &pal = item->palette;
            const bool enabled = item->state.testFlag(State_Enabled);
            const bool selected = enabled && item->state.testFlag(State_Selected);
            const bool open = selected && item->state.testFlag(State_Sunken);

            painter->fillRect(item->rect, chromeColor(pal));
            const QRectF shape = QRectF(item->rect).adjusted(1, 2, -1, -2);
            if (open) {
                drawRounded(painter, shape, radius - 1, pal.color(QPalette::Highlight));
            } else if (selected) {
                drawRounded(painter, shape, radius - 1, mix(pal.color(QPalette::Window), pal.color(QPalette::WindowText), 0.08));
            }

            int alignment = Qt::AlignCenter | Qt::TextShowMnemonic | Qt::TextDontClip | Qt::TextSingleLine;
            if (!proxy()->styleHint(SH_UnderlineShortcut, item, widget)) {
                alignment |= Qt::TextHideMnemonic;
            }
            proxy()->drawItemText(painter, item->rect, alignment, pal, enabled, item->text,
                                  open ? QPalette::HighlightedText : QPalette::WindowText);
            return;
        }
        break;

    case CE_MenuBarEmptyArea:
    case CE_ToolBar:
        // flat, Fusion paints a gradient here
        painter->fillRect(option->rect, chromeColor(option->palette));
        return;

    case CE_Splitter:
        drawHandleHighlight(painter, option);
        return;

    case CE_HeaderSection:
        if (const auto *header = qstyleoption_cast<const QStyleOptionHeader *>(option)) {
            const Interaction s(option);
            const QColor bg = header->palette.color(QPalette::Window);
            const QColor fg = header->palette.color(QPalette::WindowText);
            const QRect r = header->rect;

            painter->fillRect(r, s.pressed ? mix(bg, fg, 0.12) : s.hovered ? mix(bg, fg, 0.06) : bg);

            painter->save();
            painter->setPen(mix(bg, fg, 0.15));
            const bool last = header->position == QStyleOptionHeader::End
                || header->position == QStyleOptionHeader::OnlyOneSection;
            if (header->orientation == Qt::Horizontal) {
                painter->drawLine(r.bottomLeft(), r.bottomRight());
                if (!last) {
                    painter->drawLine(QPoint(r.right(), r.top() + 4), QPoint(r.right(), r.bottom() - 4));
                }
            } else {
                painter->drawLine(r.topRight(), r.bottomRight());
                if (!last) {
                    painter->drawLine(QPoint(r.left() + 4, r.bottom()), QPoint(r.right() - 4, r.bottom()));
                }
            }
            painter->restore();
            return;
        }
        break;

    case CE_HeaderEmptyArea:
        painter->fillRect(option->rect, option->palette.window());
        return;

    case CE_TabBarTabShape:
        if (const auto *tab = qstyleoption_cast<const QStyleOptionTab *>(option)) {
            const Interaction s(option);
            const QPalette &pal = tab->palette;
            const QColor bg = pal.color(QPalette::Window);
            const QColor fg = pal.color(QPalette::WindowText);
            const bool selected = tab->state.testFlag(State_Selected);

            const QRectF shape = QRectF(tab->rect).adjusted(1.5, 2, -1.5, -2);
            if (selected) {
                drawRounded(painter, shape, radius, mix(bg, fg, 0.13));

                // accent marker on the edge that faces the tab's contents
                const qreal inset = radius;
                QRectF marker;
                switch (tab->shape) {
                case QTabBar::RoundedSouth:
                case QTabBar::TriangularSouth:
                    marker = QRectF(shape.left() + inset, shape.top(), shape.width() - 2 * inset, 2);
                    break;
                case QTabBar::RoundedWest:
                case QTabBar::TriangularWest:
                    marker = QRectF(shape.right() - 2, shape.top() + inset, 2, shape.height() - 2 * inset);
                    break;
                case QTabBar::RoundedEast:
                case QTabBar::TriangularEast:
                    marker = QRectF(shape.left(), shape.top() + inset, 2, shape.height() - 2 * inset);
                    break;
                default:
                    marker = QRectF(shape.left() + inset, shape.bottom() - 2, shape.width() - 2 * inset, 2);
                    break;
                }
                drawRounded(painter, marker, 1, pal.color(QPalette::Highlight));
            } else if (s.hovered) {
                drawRounded(painter, shape, radius, mix(bg, fg, 0.06));
            }
            return;
        }
        break;

    default:
        break;
    }

    QProxyStyle::drawControl(element, option, painter, widget);
}

void KisModernStyle::drawComplexControl(ComplexControl control, const QStyleOptionComplex *option, QPainter *painter, const QWidget *widget) const
{
    switch (control) {
    case CC_ScrollBar:
        if (const auto *bar = qstyleoption_cast<const QStyleOptionSlider *>(option)) {
            const QColor bg = bar->palette.color(QPalette::Window);
            const QColor fg = bar->palette.color(QPalette::WindowText);
            const bool horizontal = bar->orientation == Qt::Horizontal;
            const bool enabled = bar->state.testFlag(State_Enabled);
            const bool hovered = enabled && bar->state.testFlag(State_MouseOver);
            const bool dragging = enabled && bar->state.testFlag(State_Sunken)
                && bar->activeSubControls.testFlag(SC_ScrollBarSlider);

            // A thin bar that grows to the full width while hovered
            const qreal margin = 2;
            const qreal extent = horizontal ? bar->rect.height() : bar->rect.width();
            const qreal thickness = (hovered || dragging) ? extent - 2 * margin : 4;
            const bool alignLeft = !horizontal && bar->direction == Qt::RightToLeft;

            auto thin = [&](QRectF r) {
                if (horizontal) {
                    r.setTop(r.bottom() - margin - thickness);
                    r.setHeight(thickness);
                    r.adjust(margin, 0, -margin, 0);
                } else {
                    if (alignLeft) {
                        r.setLeft(r.left() + margin);
                    } else {
                        r.setLeft(r.right() - margin - thickness);
                    }
                    r.setWidth(thickness);
                    r.adjust(0, margin, 0, -margin);
                }
                return r;
            };

            if (hovered || dragging) {
                const QRect groove = proxy()->subControlRect(CC_ScrollBar, bar, SC_ScrollBarGroove, widget);
                drawRounded(painter, thin(groove), thickness / 2, mix(bg, fg, 0.06));
            }

            const QRect slider = proxy()->subControlRect(CC_ScrollBar, bar, SC_ScrollBarSlider, widget);
            drawRounded(painter, thin(slider), thickness / 2,
                        mix(bg, fg, dragging ? 0.55 : hovered ? 0.45 : 0.3));
            return;
        }
        break;

    case CC_Slider:
        if (const auto *slider = qstyleoption_cast<const QStyleOptionSlider *>(option)) {
            if (slider->tickPosition != QSlider::NoTicks) {
                break;
            }

            const QPalette &pal = slider->palette;
            const QColor bg = pal.color(QPalette::Window);
            const QColor fg = pal.color(QPalette::WindowText);
            const QColor button = pal.color(QPalette::Button);
            const QColor accent = pal.color(QPalette::Highlight);
            const bool horizontal = slider->orientation == Qt::Horizontal;
            const QRect groove = proxy()->subControlRect(CC_Slider, slider, SC_SliderGroove, widget);
            const QRect handle = proxy()->subControlRect(CC_Slider, slider, SC_SliderHandle, widget);
            const QPointF handleCenter = QRectF(handle).center();

            if (slider->subControls.testFlag(SC_SliderGroove)) {
                const qreal track = 4;
                QRectF full = groove;
                if (horizontal) {
                    full.setTop(QRectF(groove).center().y() - track / 2);
                    full.setHeight(track);
                } else {
                    full.setLeft(QRectF(groove).center().x() - track / 2);
                    full.setWidth(track);
                }
                drawRounded(painter, full, track / 2, mix(bg, fg, 0.2));

                // fill from the minimum to the handle
                QRectF filled = full;
                if (horizontal) {
                    if (slider->upsideDown) {
                        filled.setLeft(handleCenter.x());
                    } else {
                        filled.setRight(handleCenter.x());
                    }
                } else {
                    if (slider->upsideDown) {
                        filled.setTop(handleCenter.y());
                    } else {
                        filled.setBottom(handleCenter.y());
                    }
                }
                drawRounded(painter, filled, track / 2, accent);
            }

            if (slider->subControls.testFlag(SC_SliderHandle)) {
                const Interaction s(option);
                const bool active = slider->activeSubControls.testFlag(SC_SliderHandle);
                const qreal diameter = qMax(8, qMin(handle.width(), handle.height()) - 2);
                QRectF knob(0, 0, diameter, diameter);
                knob.moveCenter(handleCenter);

                QColor fill = button;
                if (active && s.pressed) {
                    fill = accent;
                } else if (active && s.hovered) {
                    fill = mix(button, fg, 0.1);
                }
                const QColor border = s.keyboardFocus ? accent : mix(button, fg, 0.3);

                painter->save();
                painter->setRenderHint(QPainter::Antialiasing, true);
                painter->setPen(QPen(border, 1.0));
                painter->setBrush(fill);
                painter->drawEllipse(knob.adjusted(0.5, 0.5, -0.5, -0.5));
                painter->restore();
            }
            return;
        }
        break;

    case CC_ComboBox:
        if (const auto *combo = qstyleoption_cast<const QStyleOptionComboBox *>(option)) {
            if (combo->editable) {
                drawField(painter, option, true);
            } else {
                drawButtonPanel(painter, option, !combo->frame);
            }

            if (combo->subControls.testFlag(SC_ComboBoxArrow)) {
                const QRect arrow = proxy()->subControlRect(CC_ComboBox, combo, SC_ComboBoxArrow, widget);
                drawChevron(painter, arrow, Qt::DownArrow,
                            combo->palette.color(combo->editable ? QPalette::Text : QPalette::ButtonText));
            }
            return;
        }
        break;

    case CC_SpinBox:
        if (const auto *spin = qstyleoption_cast<const QStyleOptionSpinBox *>(option)) {
            if (spin->frame) {
                drawField(painter, option, true);
            }

            if (spin->buttonSymbols != QAbstractSpinBox::NoButtons) {
                const QPalette &pal = spin->palette;
                const QColor bg = pal.color(QPalette::Base);
                const QColor fg = pal.color(QPalette::Text);

                for (const SubControl sc : {SC_SpinBoxUp, SC_SpinBoxDown}) {
                    if (!spin->subControls.testFlag(sc)) {
                        continue;
                    }

                    const bool up = sc == SC_SpinBoxUp;
                    const bool stepEnabled = spin->state.testFlag(State_Enabled)
                        && spin->stepEnabled.testFlag(up ? QAbstractSpinBox::StepUpEnabled : QAbstractSpinBox::StepDownEnabled);
                    const bool active = stepEnabled && spin->activeSubControls.testFlag(sc);
                    const bool sunken = active && spin->state.testFlag(State_Sunken);
                    const QRect r = proxy()->subControlRect(CC_SpinBox, spin, sc, widget);

                    if (sunken || (active && spin->state.testFlag(State_MouseOver))) {
                        drawRounded(painter, QRectF(r).adjusted(1, 1, -1, -1), radius - 2, mix(bg, fg, sunken ? 0.16 : 0.08));
                    }

                    const QColor color = stepEnabled ? pal.color(QPalette::Text) : pal.color(QPalette::Disabled, QPalette::Text);
                    if (spin->buttonSymbols == QAbstractSpinBox::PlusMinus) {
                        const QPointF c = QRectF(r).center();
                        const qreal half = qBound(2.0, qMin(r.width(), r.height()) * 0.25, 4.0);
                        painter->save();
                        painter->setRenderHint(QPainter::Antialiasing, true);
                        painter->setPen(QPen(color, 1.5, Qt::SolidLine, Qt::RoundCap));
                        painter->drawLine(QPointF(c.x() - half, c.y()), QPointF(c.x() + half, c.y()));
                        if (up) {
                            painter->drawLine(QPointF(c.x(), c.y() - half), QPointF(c.x(), c.y() + half));
                        }
                        painter->restore();
                    } else {
                        drawChevron(painter, r, up ? Qt::UpArrow : Qt::DownArrow, color);
                    }
                }
            }
            return;
        }
        break;

    default:
        break;
    }

    QProxyStyle::drawComplexControl(control, option, painter, widget);
}
