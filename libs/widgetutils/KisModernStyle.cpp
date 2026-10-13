/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisModernStyle.h"

#include "KisTouchMode.h"

#include <QAbstractButton>
#include <QAbstractSpinBox>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDockWidget>
#include <QEvent>
#include <QFontDatabase>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLibrary>
#include <QLineEdit>
#include <QMenu>
#include <QOperatingSystemVersion>
#include <QPainter>
#include <QPolygonF>
#include <QRegularExpression>
#include <QSlider>
#include <QStyleFactory>
#include <QStyleOption>
#include <QTabBar>
#include <QWheelEvent>
#include <QWindow>
#include <QtMath>

#include <cmath>
#include <functional>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {

/// Asks Windows 11 to round a window each time it is shown, since it may get a new native handle.
/// Translucent windows draw their own shape.
class RoundedCorners : public QObject
{
public:
    static RoundedCorners *instance()
    {
        static RoundedCorners *filter = new RoundedCorners();
        return filter;
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::Show || event->type() == QEvent::WinIdChange) {
            QWidget *window = qobject_cast<QWidget *>(watched);
            if (window && window->isWindow() && window->internalWinId() && !window->testAttribute(Qt::WA_TranslucentBackground)) {
                round(window);
            }
        }
        return false;
    }

private:
    static void round(QWidget *window)
    {
#ifdef Q_OS_WIN
        using SetWindowAttribute = HRESULT(WINAPI *)(HWND, DWORD, LPCVOID, DWORD);
        static const auto setAttribute = reinterpret_cast<SetWindowAttribute>(QLibrary::resolve(QStringLiteral("dwmapi"), "DwmSetWindowAttribute"));
        if (!setAttribute) {
            return;
        }
        const DWORD cornerPreference = 33; // DWMWA_WINDOW_CORNER_PREFERENCE
        const int preference = window->windowType() == Qt::ToolTip ? 3 : 2; // DWMWCP_ROUNDSMALL : DWMWCP_ROUND
        setAttribute(reinterpret_cast<HWND>(window->internalWinId()), cornerPreference, &preference, sizeof(preference));
#else
        Q_UNUSED(window);
#endif
    }
};

constexpr qreal radius = KisModernStyle::cornerRadius;

QColor mix(const QColor &a, const QColor &b, qreal t)
{
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t,
                            a.greenF() + (b.greenF() - a.greenF()) * t,
                            a.blueF() + (b.blueF() - a.blueF()) * t,
                            a.alphaF() + (b.alphaF() - a.alphaF()) * t);
}

/// Focus rings use the bright accent; fills use the muted selection color.
QColor focusColor(const QPalette &pal)
{
    return pal.color(QPalette::Link);
}

/// Toolbars and the menu bar, one step darker than the panels.
QColor chromeColor(const QPalette &pal)
{
    const QColor window = pal.color(QPalette::Window);
    return window.lightness() < 128 ? window.darker(136) : window.darker(108);
}

/// Selected rows and menu items: the selection color mixed into the background, stronger on light themes.
QColor selectionTint(const QPalette &pal)
{
    const QColor base = pal.color(QPalette::Base);
    return mix(base, pal.color(QPalette::Highlight), base.lightness() >= 128 ? 0.75 : 0.55);
}

/// A rect whose 1px outline lands exactly on the pixels of \p rect.
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
    painter->drawPath(KisModernStyle::squircle(rect, r));
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

/// Push buttons and non-editable combo boxes: states tint the button color toward the text color.
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

    // Only keyboard focus gets an outline.
    const QColor border = s.keyboardFocus ? focusColor(pal) : QColor();
    drawRounded(painter, strokeRect(option->rect), radius, fill, border);
}

/// The default button, filled with the selection color.
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

/// Text fields: filled like buttons, outlined only while hovered or focused.
void drawField(QPainter *painter, const QStyleOption *option, bool fill)
{
    const Interaction s(option);
    const QPalette &pal = option->palette;
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

/// A 2px line in the middle of splitter and dock separator handles, shown only while hovered.
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
    // Touch mode changes metrics, so every widget has to re-measure itself.
    connect(KisTouchMode::instance(), &KisTouchMode::changed, this, []() {
        Q_FOREACH (QWidget *widget, QApplication::allWidgets()) {
            QEvent change(QEvent::StyleChange);
            QApplication::sendEvent(widget, &change);
            widget->updateGeometry();
        }
    });
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
/// Whether a label names the field next to it, rather than being standalone text.
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

void KisModernStyle::squircleCorner(qreal radius, qreal room, qreal *reach, qreal *exponent)
{
    // How far a superellipse of exponent n and reach 1 comes into the corner: sqrt(2) (1 - 2^(-1/n)).
    auto depth = [](qreal n) { return M_SQRT2 * (1 - std::pow(2.0, -1 / n)); };
    const qreal r = qBound<qreal>(0, radius, room);
    const qreal circle = (M_SQRT2 - 1) * r;
    // Match the circle's depth into the corner, reaching further along the sides.
    constexpr qreal smooth = 4;
    if (r <= 0 || circle / depth(smooth) <= room) {
        *reach = r > 0 ? circle / depth(smooth) : 0;
        *exponent = smooth;
        return;
    }
    // Not enough room: use all of it with a lower exponent, down to a circle so a pill stays a pill.
    *reach = room;
    *exponent = qBound<qreal>(2, -1 / std::log2(1 - circle / (room * M_SQRT2)), smooth);
}

qreal KisModernStyle::fullSquircleRadius(qreal side)
{
    // Solves squircleCorner() for a reach of half the side.
    return side / 2 * M_SQRT2 * (1 - std::pow(2.0, -1 / 4.0)) / (M_SQRT2 - 1);
}

QEasingCurve KisModernStyle::motionCurve()
{
    // 1 - e^(-kt), normalized to end at 1; covers most of the distance in the first third.
    QEasingCurve curve;
    curve.setCustomType([](qreal t) {
        constexpr qreal k = 5.5;
        return (1 - std::exp(-k * t)) / (1 - std::exp(-k));
    });
    return curve;
}

QPainterPath KisModernStyle::squircle(const QRectF &rect, qreal radius)
{
    QPainterPath path;
    if (rect.isEmpty()) {
        return path;
    }
    qreal reach = 0;
    qreal n = 2;
    squircleCorner(radius, qMin(rect.width(), rect.height()) / 2, &reach, &n);
    if (reach <= 0) {
        path.addRect(rect);
        return path;
    }
    // Each corner is a quarter of |x|^n + |y|^n = 1, scaled to the reach.
    const int steps = qBound(6, qCeil(reach), 32);
    QPolygonF corner;
    for (int i = 0; i <= steps; i++) {
        const qreal t = M_PI / 2 * i / steps;
        // Clamped, as t may round past a right angle.
        corner << QPointF(std::pow(qMax<qreal>(0, std::sin(t)), 2 / n), std::pow(qMax<qreal>(0, std::cos(t)), 2 / n)) * reach;
    }
    // Clockwise; a corner point's (x, y) maps to (across, along) its sides.
    struct Corner { QPointF center; QPointF across; QPointF along; };
    const Corner corners[] = {
        {rect.topRight() + QPointF(-reach, reach), {1, 0}, {0, -1}},
        {rect.bottomRight() + QPointF(-reach, -reach), {0, 1}, {1, 0}},
        {rect.bottomLeft() + QPointF(reach, -reach), {-1, 0}, {0, 1}},
        {rect.topLeft() + QPointF(reach, reach), {0, -1}, {-1, 0}},
    };
    QPolygonF outline;
    outline.reserve(4 * corner.size());
    for (const Corner &c : corners) {
        for (const QPointF &p : corner) {
            outline << c.center + p.x() * c.across + p.y() * c.along;
        }
    }
    path.addPolygon(outline);
    path.closeSubpath();
    return path;
}

QString KisModernStyle::keyCapText(const QKeySequence &sequence)
{
    return sequence.isEmpty() ? QString() : keyCapText(sequence.toString(QKeySequence::NativeText));
}

QString KisModernStyle::keyCapText(const QString &nativeText)
{
    // Only the first sequence, with short modifiers: "Ctrl+Shift+E" -> "Ctrl ⇧E".
    QString text = nativeText.section(QLatin1String(", "), 0, 0);
    text.replace(QStringLiteral("Shift+"), QStringLiteral("⇧"));
    text.replace(QStringLiteral("Ctrl+"), QStringLiteral("Ctrl "));
    text.replace(QStringLiteral("Alt+"), QStringLiteral("Alt "));
    text.replace(QStringLiteral("Meta+"), QStringLiteral("Win "));
    return text;
}

QFont KisModernStyle::keyCapFont(const QFont &font)
{
    return textFont(font, 0.86);
}

QColor KisModernStyle::secondaryInk(QColor ink)
{
    ink.setAlphaF(ink.alphaF() * 0.62);
    return ink;
}

QFont KisModernStyle::textFont(const QFont &font, qreal scale, QFont::Weight weight)
{
    QFont sized = font;
    if (sized.pointSizeF() > 0) {
        sized.setPointSizeF(sized.pointSizeF() * scale);
    } else {
        sized.setPixelSize(qMax(8, qRound(sized.pixelSize() * scale)));
    }
    sized.setWeight(weight);

    static const QString variable = QStringLiteral("Segoe UI Variable");
    if (!font.family().startsWith(variable)) {
        return sized;
    }
    // Windows ships optical sizes Small (up to 9 pt), Text (body) and Display (from 15 pt).
    const qreal points = sized.pointSizeF() > 0 ? sized.pointSizeF() : sized.pixelSize() * 0.75;
    QString family = variable + (points < 9.5 ? QStringLiteral(" Small") : points >= 15 ? QStringLiteral(" Display") : QStringLiteral(" Text"));
    // Weights between regular and bold are separate families.
    if (weight == QFont::DemiBold || weight == QFont::Medium) {
        family += QStringLiteral(" Semibold");
        sized.setWeight(QFont::Normal);
    } else if (weight == QFont::Light || weight == QFont::ExtraLight) {
        family += QStringLiteral(" Light");
        sized.setWeight(QFont::Normal);
    }
    static const QStringList installed = QFontDatabase().families();
    // Windows may truncate family names to 31 characters.
    for (const QString &name : {family, family.left(31)}) {
        if (installed.contains(name)) {
            sized.setFamily(name);
            break;
        }
    }
    return sized;
}

namespace {
QString keyCapShown(const QString &text, KisModernStyle::KeyCap state)
{
    if (state == KisModernStyle::KeyCap::Recording) return QStringLiteral("…");
    if (state == KisModernStyle::KeyCap::Add || text.isEmpty()) return QStringLiteral("+");
    return text;
}
} // namespace

QSizeF KisModernStyle::keyCapSize(const QFont &font, const QString &text, KeyCap state)
{
    const QFontMetricsF metrics(font);
    const QString shown = keyCapShown(text, state);
    // Never narrower than it is tall.
    const qreal height = metrics.height() + 4 + 1.5;
    return QSizeF(qMax(height + 2, metrics.horizontalAdvance(shown) + 12), height);
}

QRectF KisModernStyle::keyCapRect(const QRectF &row, const QFont &font, const QString &text, KeyCap state)
{
    const QSizeF size = keyCapSize(font, text, state);
    return QRectF(row.right() - size.width(), row.center().y() - size.height() / 2, size.width(), size.height());
}

void KisModernStyle::drawKeyCap(QPainter *painter, const QRectF &rect, const QFont &font, const QString &text,
                                const QPalette &palette, KeyCap state)
{
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    const QColor window = palette.color(QPalette::Window);
    const QColor ink = palette.color(QPalette::ButtonText);
    const QRectF face = rect.adjusted(0, 0, 0, -1.5);
    const qreal r = qMin<qreal>(cornerRadius, face.height() / 2);
    painter->setPen(Qt::NoPen);
    if (state != KeyCap::Add) {
        // A darker edge below the face makes it read as a key rather than a button.
        painter->setBrush(mix(window, Qt::black, 0.3));
        painter->drawPath(squircle(face.translated(0, 1.5), r));
    }
    const qreal lift = state == KeyCap::Pointed || state == KeyCap::Recording ? 0.24 : state == KeyCap::Add ? 0.1 : 0.15;
    painter->setBrush(mix(window, ink, lift));
    painter->drawPath(squircle(face, r));
    if (state == KeyCap::Recording) {
        painter->setPen(QPen(palette.color(QPalette::Link), 1.25));
        painter->setBrush(Qt::NoBrush);
        painter->drawPath(squircle(face.adjusted(0.6, 0.6, -0.6, -0.6), r - 0.6));
    }
    QColor textColor = ink;
    textColor.setAlphaF(state == KeyCap::Add ? 0.55 : state == KeyCap::Shown ? 0.85 : 1.0);
    painter->setPen(textColor);
    painter->setFont(font);
    painter->drawText(face, Qt::AlignCenter, keyCapShown(text, state));
    painter->restore();
}

int KisModernStyle::wheelSteps(const QWheelEvent *event, QObject *receiver)
{
    const char *const restProperty = "studioWheelRest";
    const int delta = event->angleDelta().y();
    if (!delta) return 0;
    int rest = receiver->property(restProperty).toInt();
    // Changing direction drops the remainder.
    if ((rest < 0) != (delta < 0)) rest = 0;
    rest += delta;
    const int steps = rest / 120;
    receiver->setProperty(restProperty, rest - steps * 120);
    return steps;
}

bool KisModernStyle::pointerHeld()
{
#ifdef Q_OS_WIN
    // Ask Windows directly: during a system drag loop, Qt's button state is stale.
    return GetAsyncKeyState(GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON) & 0x8000;
#else
    return QGuiApplication::mouseButtons() & Qt::LeftButton;
#endif
}

QAction *KisModernStyle::shortcutTarget(QAction *entry)
{
    if (!entry || entry->isSeparator() || entry->menu()) {
        return nullptr;
    }
    QAction *target = entry;
    if (QObject *named = entry->property("kisShortcutTarget").value<QObject *>()) {
        target = qobject_cast<QAction *>(named);
    }
    return target && !target->objectName().isEmpty() ? target : nullptr;
}

bool KisModernStyle::systemRoundsWindows()
{
#ifdef Q_OS_WIN
    static const bool rounds = QOperatingSystemVersion::current() >= QOperatingSystemVersion(QOperatingSystemVersion::Windows, 10, 0, 22000);
    return rounds;
#else
    return false;
#endif
}

void KisModernStyle::polish(QWidget *widget)
{
    QProxyStyle::polish(widget);

    if (systemRoundsWindows()) {
        const Qt::WindowType type = widget->windowType();
        const bool popup = widget->isWindow() && (type == Qt::Popup || type == Qt::ToolTip || type == Qt::Tool);
        // A dock widget only becomes a window once it floats.
        if (popup || qobject_cast<QDockWidget *>(widget)) {
            widget->installEventFilter(RoundedCorners::instance());
        }
    }

    // State_MouseOver needs hover events; Fusion only enables them for buttons, sliders, scroll bars and spin boxes.
    if (qobject_cast<QTabBar *>(widget) || qobject_cast<QHeaderView *>(widget) || qobject_cast<QLineEdit *>(widget)) {
        widget->setAttribute(Qt::WA_Hover, true);
    }

    // A label beside its field needs no colon.
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

QRect KisModernStyle::subElementRect(SubElement element, const QStyleOption *option, const QWidget *widget) const
{
    QRect r = QProxyStyle::subElementRect(element, option, widget);
    if (element == SE_LineEditContents) {
        // Keep the text and cursor clear of the rounded border.
        const auto *frame = qstyleoption_cast<const QStyleOptionFrame *>(option);
        if (frame && frame->lineWidth > 0) r.adjust(4, 0, -4, 0);
    }
    return r;
}

int KisModernStyle::pixelMetric(PixelMetric metric, const QStyleOption *option, const QWidget *widget) const
{
    const bool touch = KisTouchMode::isActive();
    switch (metric) {
    case PM_ToolTipLabelFrameWidth:
        return 5;
    case PM_ScrollBarExtent:
        return touch ? 16 : 10;
    case PM_IndicatorWidth:
    case PM_IndicatorHeight:
    case PM_ExclusiveIndicatorWidth:
    case PM_ExclusiveIndicatorHeight:
        return QProxyStyle::pixelMetric(metric, option, widget) + (touch ? 6 : 0);
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
        // Drop the list below the box instead of covering it.
        return 0;
    case SH_ComboBox_PopupFrameStyle:
        // The system draws the rounded popup's border.
        if (systemRoundsWindows()) {
            return QFrame::NoFrame;
        }
        break;
    case SH_DitherDisabledText:
    case SH_EtchDisabledText:
    case SH_UnderlineShortcut:
        return 0;
    default:
        break;
    }
    return QProxyStyle::styleHint(hint, option, widget, returnData);
}

QIcon KisModernStyle::standardIcon(StandardPixmap standardIcon, const QStyleOption *option, const QWidget *widget) const
{
    QColor fill;
    QString glyph;
    switch (standardIcon) {
    case SP_MessageBoxWarning:
        fill = QColor(232, 168, 56);
        glyph = QStringLiteral("!");
        break;
    case SP_MessageBoxCritical:
        fill = QColor(220, 80, 72);
        glyph = QStringLiteral("!");
        break;
    case SP_MessageBoxInformation:
        fill = QColor(74, 136, 232);
        glyph = QStringLiteral("i");
        break;
    case SP_MessageBoxQuestion:
        fill = QColor(74, 136, 232);
        glyph = QStringLiteral("?");
        break;
    default:
        return QProxyStyle::standardIcon(standardIcon, option, widget);
    }
    // A filled circle with the glyph in it.
    QIcon icon;
    for (int size : {16, 24, 32, 48, 64, 96}) {
        QPixmap pixmap(size, size);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::TextAntialiasing, true);
        painter.setPen(Qt::NoPen);
        painter.setBrush(fill);
        painter.drawEllipse(QRectF(0.5, 0.5, size - 1, size - 1));
        QFont font = QApplication::font();
        font.setBold(true);
        font.setPixelSize(qRound(size * 0.62));
        painter.setFont(font);
        painter.setPen(Qt::white);
        painter.drawText(QRect(0, 0, size, size), Qt::AlignCenter, glyph);
        painter.end();
        icon.addPixmap(pixmap);
    }
    return icon;
}

QSize KisModernStyle::sizeFromContents(ContentsType type, const QStyleOption *option, const QSize &contentsSize, const QWidget *widget) const
{
    QSize size = QProxyStyle::sizeFromContents(type, option, contentsSize, widget);

    // In touch mode, make everything pressable a bit bigger.
    if (KisTouchMode::isActive()) {
        switch (type) {
        case CT_PushButton:
        case CT_ComboBox:
        case CT_LineEdit:
        case CT_SpinBox:
        case CT_TabBarTab:
            size.rheight() += 10;
            break;
        case CT_ToolButton:
            size += QSize(8, 8);
            break;
        case CT_CheckBox:
        case CT_RadioButton:
        case CT_ItemViewItem:
            size.rheight() += 8;
            break;
        case CT_MenuItem:
            if (const auto *item = qstyleoption_cast<const QStyleOptionMenuItem *>(option)) {
                if (item->menuItemType != QStyleOptionMenuItem::Separator) size.rheight() += 12;
            }
            break;
        default:
            break;
        }
    }

    switch (type) {
    case CT_MenuItem:
        if (const auto *item = qstyleoption_cast<const QStyleOptionMenuItem *>(option)) {
            if (item->menuItemType != QStyleOptionMenuItem::Separator) {
                size.rheight() += 4;
                // Room for the shortcut's key cap.
                size.rwidth() += 26;
            }
        }
        break;
    case CT_MenuBarItem:
    case CT_PushButton:
        size.rheight() += 2;
        break;
    case CT_ComboBox:
        // Room for the check mark of Fusion's menu-like popup, which SH_ComboBox_Popup turns off.
        size.rwidth() += 10;
        // A flat combo box has no chevron (see subControlRect).
        if (const auto *combo = qstyleoption_cast<const QStyleOptionComboBox *>(option)) {
            if (!combo->frame && !combo->editable) {
                QStyleOptionComboBox sized = *combo;
                sized.rect = QRect(QPoint(), size);
                size.rwidth() -= QProxyStyle::subControlRect(CC_ComboBox, &sized, SC_ComboBoxArrow, widget).width();
            }
        }
        break;
    default:
        break;
    }
    return size;
}

QRect KisModernStyle::subControlRect(ComplexControl control, const QStyleOptionComplex *option, SubControl subControl, const QWidget *widget) const
{
    if (control == CC_ComboBox) {
        // A flat combo box shows only its text, no chevron.
        const auto *combo = qstyleoption_cast<const QStyleOptionComboBox *>(option);
        if (combo && !combo->frame && !combo->editable) {
            if (subControl == SC_ComboBoxArrow) return QRect();
            if (subControl == SC_ComboBoxEditField) {
                const QRect field = QProxyStyle::subControlRect(control, option, subControl, widget);
                const QRect arrow = QProxyStyle::subControlRect(control, option, SC_ComboBoxArrow, widget);
                return QRect(field.left(), field.top(), qMax(field.right(), arrow.right()) - field.left() + 1, field.height());
            }
        }
    }
    if (control == CC_ScrollBar) {
        // QCommonStyle's layout without the arrow buttons.
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
    // A glass menu (see KisStudioGlass::installMenuGlass) paints its own background and border.
    if ((element == PE_PanelMenu || element == PE_FrameMenu) && widget && widget->property("studioGlassMenu").toBool()) {
        return;
    }

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
        if (option->state.testFlag(State_AutoRaise) && (s.checked || s.pressed || s.hovered)) {
            // A flat button on glass: a translucent wash of its text color.
            fill = fg;
            fill.setAlphaF(s.checked ? (s.hovered ? 0.24 : 0.2) : s.pressed ? 0.16 : 0.08);
        } else if (s.checked) {
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
        // Buttons and combo boxes show keyboard focus with their border.
        if (qobject_cast<const QAbstractButton *>(widget) || qobject_cast<const QComboBox *>(widget)) {
            return;
        }
        if (option->state.testFlag(State_KeyboardFocusChange)) {
            drawRounded(painter, strokeRect(option->rect), radius, QColor(), option->palette.color(QPalette::Highlight));
        }
        return;

    case PE_PanelLineEdit:
        if (const auto *frame = qstyleoption_cast<const QStyleOptionFrame *>(option)) {
            // An editor inside a spin box or combo box has no frame; its parent paints the field.
            if (frame->lineWidth > 0
                || (widget && widget->property(quietFieldProperty).toBool() && option->state & (State_MouseOver | State_HasFocus))) {
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
        // A full squircle, which reads well at this small, square size.
        drawRounded(painter, box, fullSquircleRadius(box.width()), marked ? accent : bg, border);

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
        // No frame: the title and spacing group the contents.
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
        // The system draws the rounded menu's border.
        if (systemRoundsWindows() && widget && widget->isWindow()) {
            return;
        }
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

    case PE_PanelTipLabel:
        // A translucent tooltip has its glass painted underneath (see KisStudioGlass).
        if (widget && widget->testAttribute(Qt::WA_TranslucentBackground)) {
            return;
        }
        // The system draws the rounded tooltip's border.
        if (systemRoundsWindows()) {
            painter->fillRect(option->rect, option->palette.brush(QPalette::ToolTipBase));
            return;
        }
        break;

    case PE_IndicatorToolBarSeparator: {
        const QColor bg = option->palette.color(QPalette::Window);
        const QColor fg = option->palette.color(QPalette::WindowText);
        const QRect r = option->rect;
        painter->save();
        painter->setPen(mix(bg, fg, 0.18));
        if (option->state.testFlag(State_Horizontal)) {
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
                // PE_PanelItemViewItem draws the selection as a pill, so don't fill the tree's branch area.
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

            // A row spanning several columns forms one pill: inner ends extend past the clip.
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
            if (item->menuItemType == QStyleOptionMenuItem::Separator) {
                break;
            }
            const bool enabled = item->state.testFlag(State_Enabled);
            const bool selected = item->state.testFlag(State_Selected) && enabled;
            if (selected) {
                drawRounded(painter, QRectF(item->rect).adjusted(3, 1, -3, -1), radius - 1,
                            selectionTint(item->palette));
            }

            // Let Fusion draw the rest as unselected (but in the selected text color), without the shortcut.
            QStyleOptionMenuItem contents = *item;
            const int tab = item->text.indexOf(QLatin1Char('\t'));
            const QString shortcut = tab >= 0 ? keyCapText(item->text.mid(tab + 1)) : QString();
            if (tab >= 0) contents.text = item->text.left(tab);
            if (selected) {
                contents.state.setFlag(State_Selected, false);
                const QColor text = item->palette.color(QPalette::HighlightedText);
                contents.palette.setColor(QPalette::Text, text);
                contents.palette.setColor(QPalette::ButtonText, text);
                contents.palette.setColor(QPalette::WindowText, text);
            }
            QProxyStyle::drawControl(element, &contents, painter, widget);

            // The shortcut as a key cap, highlighted when it can be edited.
            const QMenu *menu = qobject_cast<const QMenu *>(widget);
            QAction *action = nullptr;
            if (menu) {
                Q_FOREACH (QAction *candidate, menu->actions()) {
                    if (menu->actionGeometry(candidate) == item->rect) {
                        action = candidate;
                        break;
                    }
                }
            }
            const bool editable = enabled && qApp->property("kisMenuShortcutsEditable").toBool() && shortcutTarget(action);
            KeyCap state = KeyCap::Shown;
            bool shown = !shortcut.isEmpty();
            if (action && menu->property("kisRecordingShortcut").value<QObject *>() == action) {
                state = KeyCap::Recording;
                shown = true;
            } else if (!shortcut.isEmpty()) {
                state = selected && editable ? KeyCap::Pointed : KeyCap::Shown;
            } else if (selected && editable) {
                state = KeyCap::Add;
                shown = true;
            }
            if (shown) {
                const QFont font = keyCapFont(item->font);
                painter->save();
                if (!enabled) painter->setOpacity(0.5);
                drawKeyCap(painter, keyCapRect(QRectF(item->rect).adjusted(0, 0, -10, 0), font, shortcut, state),
                           font, shortcut, item->palette, state);
                painter->restore();
            }
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
        // Flat instead of Fusion's gradient.
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
            } else if (s.hovered) {
                drawRounded(painter, shape, radius, mix(bg, fg, 0.06));
            }
            return;
        }
        break;

    case CE_ProgressBarGroove:
        drawRounded(painter, QRectF(option->rect).adjusted(0.5, 0.5, -0.5, -0.5),
                    qMin<qreal>(radius, qMin(option->rect.width(), option->rect.height()) / 2.0),
                    mix(option->palette.color(QPalette::Base), option->palette.color(QPalette::Text), 0.1));
        return;

    case CE_ProgressBarContents:
        if (const auto *bar = qstyleoption_cast<const QStyleOptionProgressBar *>(option)) {
            if (bar->minimum == bar->maximum) {
                break; // Busy indicator: use the base style's animation.
            }
            const qreal amount = qBound(0.0, qreal(bar->progress - bar->minimum) / qreal(bar->maximum - bar->minimum), 1.0);
            if (amount <= 0.0) {
                return;
            }
            const bool vertical = !(bar->state & QStyle::State_Horizontal);
            const bool reversed = bar->invertedAppearance != (bar->direction == Qt::RightToLeft && !vertical);
            QRectF filled = QRectF(option->rect).adjusted(0.5, 0.5, -0.5, -0.5);
            if (vertical) {
                const qreal h = filled.height() * amount;
                filled = reversed ? QRectF(filled.left(), filled.top(), filled.width(), h)
                                  : QRectF(filled.left(), filled.bottom() - h, filled.width(), h);
            } else {
                const qreal w = filled.width() * amount;
                filled = reversed ? QRectF(filled.right() - w, filled.top(), w, filled.height())
                                  : QRectF(filled.left(), filled.top(), w, filled.height());
            }
            const qreal r = qMin<qreal>(radius, qMin(option->rect.width(), option->rect.height()) / 2.0);
            drawRounded(painter, filled, qMin(r, qMin(filled.width(), filled.height()) / 2.0),
                        option->palette.color(QPalette::Highlight));
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
    case CC_ToolButton:
        if (const auto *tool = qstyleoption_cast<const QStyleOptionToolButton *>(option);
            tool && widget && widget->property(tabButtonProperty).toBool()) {
            const Interaction s(option);
            const QColor fg = tool->palette.color(QPalette::ButtonText);
            const bool on = tool->state.testFlag(State_On);
            if (s.hovered || s.pressed) {
                QColor fill = fg;
                fill.setAlphaF(s.pressed ? 0.14 : 0.08);
                drawRounded(painter, QRectF(tool->rect).adjusted(1, 1, -1, -1), radius, fill);
            }
            // Draw the icon plainly, not pushed in or highlighted.
            QStyleOptionToolButton label = *tool;
            label.state &= ~(State_On | State_Sunken | State_MouseOver);
            label.rect = proxy()->subControlRect(CC_ToolButton, tool, SC_ToolButton, widget);
            painter->save();
            painter->setOpacity(on ? 1.0 : (s.hovered ? 0.8 : 0.5));
            if (on && !tool->icon.isNull() && tool->toolButtonStyle == Qt::ToolButtonIconOnly) {
                // Use the On pixmap, the solid variant (see KisIconUtils).
                QWindow *window = widget->window()->windowHandle();
                const QPixmap pixmap = window ? tool->icon.pixmap(window, tool->iconSize, QIcon::Normal, QIcon::On)
                                              : tool->icon.pixmap(tool->iconSize, QIcon::Normal, QIcon::On);
                proxy()->drawItemPixmap(painter, label.rect, Qt::AlignCenter, pixmap);
            } else {
                proxy()->drawControl(CE_ToolButtonLabel, &label, painter, widget);
            }
            painter->restore();
            return;
        }
        break;

    case CC_ScrollBar:
        if (const auto *bar = qstyleoption_cast<const QStyleOptionSlider *>(option)) {
            const QColor bg = bar->palette.color(QPalette::Window);
            const QColor fg = bar->palette.color(QPalette::WindowText);
            const bool horizontal = bar->orientation == Qt::Horizontal;
            const bool enabled = bar->state.testFlag(State_Enabled);
            const bool hovered = enabled && bar->state.testFlag(State_MouseOver);
            const bool dragging = enabled && bar->state.testFlag(State_Sunken)
                && bar->activeSubControls.testFlag(SC_ScrollBarSlider);

            // A thin bar that widens while hovered.
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

            const QRect arrow = proxy()->subControlRect(CC_ComboBox, combo, SC_ComboBoxArrow, widget);
            if (combo->subControls.testFlag(SC_ComboBoxArrow) && !arrow.isEmpty()) {
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
