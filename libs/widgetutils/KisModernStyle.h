/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISMODERNSTYLE_H
#define KISMODERNSTYLE_H

#include "kritawidgetutils_export.h"

#include <QEasingCurve>
#include <QFont>
#include <QKeySequence>
#include <QPainterPath>
#include <QProxyStyle>

class QAction;
class QWheelEvent;

/// A flat, rounded style built on Fusion. All colors come from the palette.
class KRITAWIDGETUTILS_EXPORT KisModernStyle : public QProxyStyle
{
    Q_OBJECT
public:
    KisModernStyle();

    /// The name shown in Settings->Styles and stored in kritarc.
    static QString styleName();

    /// Like QStyleFactory::create(), but also knows about KisModernStyle.
    static QStyle *create(const QString &name);

    /// QStyleFactory::keys() with KisModernStyle prepended.
    static QStringList availableStyles();

    /// The style that actually paints the widgets ("fusion" for KisModernStyle).
    static QString underlyingStyleName(const QStyle *style);

    /// Corner radius of rounded controls, for widgets that paint themselves.
    static constexpr int cornerRadius = 5;

    /// The spacing scale: a hairline between buttons in a group, then steps of four. Use only these for
    /// margins and spacing.
    static constexpr int hairline = 2;
    static constexpr int space1 = 4;
    static constexpr int space2 = 8;
    static constexpr int space3 = 12;
    static constexpr int space4 = 16;
    /// Corner radii by element size: control (cornerRadius), tile or chip, card, panel.
    static constexpr int tileRadius = 8;
    static constexpr int cardRadius = 10;
    static constexpr int panelRadius = 14;

    /// A frameless line edit with this property shows no box until hovered or focused.
    static constexpr const char *quietFieldProperty = "studioQuietField";

    /// A checkable tool button with this property is drawn as a tab: full opacity when checked, faint otherwise.
    static constexpr const char *tabButtonProperty = "studioTabButton";

    /**
     * \p rect with squircle corners (quarters of |x|^4 + |y|^4 = 1) as deep as circular corners of \p radius.
     * On short sides the exponent drops toward 2, so a pill stays a pill.
     */
    static QPainterPath squircle(const QRectF &rect, qreal radius);
    /// The radius at which squircle() of a square with sides \p side has no straight edges.
    static qreal fullSquircleRadius(qreal side);

    /**
     * The easing curve for all motion: an exponential decay that starts fast and settles without overshoot.
     * Durations in ms: enterDuration for appearing, exitDuration for disappearing, changeDuration for state changes.
     */
    static QEasingCurve motionCurve();
    static constexpr int enterDuration = 180;
    static constexpr int exitDuration = 120;
    static constexpr int changeDuration = 160;

    /// The corner reach along the sides and the exponent squircle() uses, given half-sides of length \p room.
    static void squircleCorner(qreal radius, qreal room, qreal *reach, qreal *exponent);

    /// Whether the system rounds and frames popup windows (Windows 11), so they need no frame of their own.
    static bool systemRoundsWindows();

    /// Shortcuts are drawn as key caps with abbreviated text ("Ctrl ⇧E").
    enum class KeyCap {
        Shown,     ///< the shortcut
        Pointed,   ///< the shortcut under the pointer; a click records a new one
        Add,       ///< no shortcut yet, a faint "+"; a click records one
        Recording, ///< waiting for the new shortcut to be pressed
    };
    static QString keyCapText(const QKeySequence &sequence);
    /// \p nativeText is in QKeySequence::NativeText form ("Ctrl+Shift+E").
    static QString keyCapText(const QString &nativeText);
    static QSizeF keyCapSize(const QFont &font, const QString &text, KeyCap state = KeyCap::Shown);
    /// The key cap rect at the right of \p row, vertically centered.
    static QRectF keyCapRect(const QRectF &row, const QFont &font, const QString &text, KeyCap state = KeyCap::Shown);
    static void drawKeyCap(QPainter *painter, const QRectF &rect, const QFont &font, const QString &text,
                           const QPalette &palette, KeyCap state = KeyCap::Shown);
    /// The smaller key cap font for lists, derived from the list's font.
    static QFont keyCapFont(const QFont &font);

    /// \p font scaled and weighted. Segoe UI Variable uses the matching optical size and a real semibold.
    static QFont textFont(const QFont &font, qreal scale, QFont::Weight weight = QFont::Normal);

    /// \p ink faded for secondary text (hints, captions, units), still readable on glass.
    static QColor secondaryInk(QColor ink);
    /// \p a toward \p b by \p t, alpha too
    static QColor mix(const QColor &a, const QColor &b, qreal t);

    /// A selected row or menu item's fill, as the style draws them; a shade of the text on themes with no accent
    static QColor selectedRow(const QPalette &palette);
    /// The text on selectedRow()
    static QColor selectedRowInk(const QPalette &palette);

    /// A color swatch as a button shows it: a squircle of \p color (over checks where it is see-through) with a thin edge;
    /// an invalid color draws the empty swatch
    static void paintSwatch(QPainter &painter, const QRectF &rect, const QColor &color, const QPalette &palette, bool hovered,
                            bool pressed, bool focused);

    /// The action whose shortcut a menu entry shows: the entry or its "kisShortcutTarget". Null for submenus, separators and unnamed actions.
    static QAction *shortcutTarget(QAction *entry);

    /// Whole wheel notches in \p event. Smaller deltas (touchpads, fine wheels) accumulate on \p receiver.
    static int wheelSteps(const QWheelEvent *event, QObject *receiver);

    /// Whether the system reports the left button (or pen) down, even if Qt missed the release in a drag loop.
    static bool pointerHeld();

    using QProxyStyle::polish;
    void polish(QWidget *widget) override;

    int pixelMetric(PixelMetric metric, const QStyleOption *option = nullptr, const QWidget *widget = nullptr) const override;
    QRect subElementRect(SubElement element, const QStyleOption *option, const QWidget *widget = nullptr) const override;
    int styleHint(StyleHint hint, const QStyleOption *option = nullptr, const QWidget *widget = nullptr, QStyleHintReturn *returnData = nullptr) const override;
    /// Draws message box icons as flat round marks.
    QIcon standardIcon(StandardPixmap standardIcon, const QStyleOption *option = nullptr, const QWidget *widget = nullptr) const override;
    QSize sizeFromContents(ContentsType type, const QStyleOption *option, const QSize &size, const QWidget *widget) const override;
    QRect subControlRect(ComplexControl control, const QStyleOptionComplex *option, SubControl subControl, const QWidget *widget) const override;

    void drawPrimitive(PrimitiveElement element, const QStyleOption *option, QPainter *painter, const QWidget *widget = nullptr) const override;
    void drawControl(ControlElement element, const QStyleOption *option, QPainter *painter, const QWidget *widget = nullptr) const override;
    void drawComplexControl(ComplexControl control, const QStyleOptionComplex *option, QPainter *painter, const QWidget *widget = nullptr) const override;
};

#endif // KISMODERNSTYLE_H
