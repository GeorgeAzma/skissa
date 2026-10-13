/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISMODERNSTYLE_H
#define KISMODERNSTYLE_H

#include "kritawidgetutils_export.h"
#include <QPainterPath>
#include <QEasingCurve>
#include <QProxyStyle>
#include <QKeySequence>
#include <QFont>

class QAction;
class QWheelEvent;

/// A flat, rounded style on top of Fusion; colors all come from the palette
class KRITAWIDGETUTILS_EXPORT KisModernStyle : public QProxyStyle
{
    Q_OBJECT
public:
    KisModernStyle();

    /// The name shown in Settings->Styles and stored in kritarc
    static QString styleName();

    /// Like QStyleFactory::create(), but also knows about KisModernStyle
    static QStyle *create(const QString &name);

    /// QStyleFactory::keys() with KisModernStyle prepended
    static QStringList availableStyles();

    /// The style that actually paints the widgets ("fusion" for KisModernStyle)
    static QString underlyingStyleName(const QStyle *style);

    /// Corner radius of rounded controls, for widgets that paint themselves
    static constexpr int cornerRadius = 5;

    /**
     * The one spacing scale: a hairline between the buttons of one group, then steps of four. Margins and
     * spacings are these and nothing else.
     */
    static constexpr int hairline = 2;
    static constexpr int space1 = 4;
    static constexpr int space2 = 8;
    static constexpr int space3 = 12;
    static constexpr int space4 = 16;
    /// Corner radii by size: a control's (cornerRadius), a tile's or chip's, a card's, a panel's
    static constexpr int tileRadius = 8;
    static constexpr int cardRadius = 10;
    static constexpr int panelRadius = 14;

    /// A frameless line edit with this property is a quiet field: no box until hovered or focused
    static constexpr const char *quietFieldProperty = "studioQuietField";

    /// A checkable tool button with this property is a tab: checked, full ink and a bar under it; else faint
    static constexpr const char *tabButtonProperty = "studioTabButton";

    /**
     * \p rect with squircle corners (quarters of |x|^4 + |y|^4 = 1), as deep as circular ones of \p radius;
     * on short sides the exponent drops toward 2, so a pill stays a pill
     */
    static QPainterPath squircle(const QRectF &rect, qreal radius);
    /// The radius that makes squircle() of a square \p side long the whole |x|^4 + |y|^4 = 1, no straight part
    static qreal fullSquircleRadius(qreal side);

    /**
     * The one way things move: an exponential decay toward where they go, quick at first and settling
     * softly, never past it. Appearing takes enterDuration, going away exitDuration (ms); a change of
     * state (a switch, a highlight) changeDuration.
     */
    static QEasingCurve motionCurve();
    static constexpr int enterDuration = 180;
    static constexpr int exitDuration = 120;
    static constexpr int changeDuration = 160;

    /// squircle()'s corner reach along the sides and exponent, for half-sides \p room long
    static void squircleCorner(qreal radius, qreal room, qreal *reach, qreal *exponent);

    /// Whether the system rounds and frames popup windows (Windows 11); they then draw no frame of their own
    static bool systemRoundsWindows();

    /// Shortcuts are drawn as key caps everywhere, their text short ("Ctrl ⇧E")
    enum class KeyCap {
        Shown,     ///< the shortcut
        Pointed,   ///< the shortcut, under the pointer (a click records another)
        Add,       ///< none yet, a faint "+" (a click records one)
        Recording, ///< waiting for the new one to be pressed
    };
    static QString keyCapText(const QKeySequence &sequence);
    /// \p nativeText as QKeySequence::NativeText writes it ("Ctrl+Shift+E")
    static QString keyCapText(const QString &nativeText);
    static QSizeF keyCapSize(const QFont &font, const QString &text, KeyCap state = KeyCap::Shown);
    /// The key cap at the right of \p row, vertically centered
    static QRectF keyCapRect(const QRectF &row, const QFont &font, const QString &text, KeyCap state = KeyCap::Shown);
    static void drawKeyCap(QPainter *painter, const QRectF &rect, const QFont &font, const QString &text,
                           const QPalette &palette, KeyCap state = KeyCap::Shown);
    /// The smaller font of key caps in lists, from the list's font
    static QFont keyCapFont(const QFont &font);

    /// \p font scaled and weighted; Segoe UI Variable uses its optical cut (Small, Text, Display) and real semibold
    static QFont textFont(const QFont &font, qreal scale, QFont::Weight weight = QFont::Normal);

    /// \p ink for secondary text (hints, captions, units): faint but readable on glass
    static QColor secondaryInk(QColor ink);

    /// The action whose shortcut a menu entry shows: itself or its "kisShortcutTarget"; none for submenus, separators, unnamed actions
    static QAction *shortcutTarget(QAction *entry);

    /// Whole wheel notches in \p event; smaller steps (touchpads, fine wheels) accumulate on \p receiver
    static int wheelSteps(const QWheelEvent *event, QObject *receiver);

    /// Whether the left button (or pen) is down as the system has it, even where Qt missed the release (a drag's own loop)
    static bool pointerHeld();

    using QProxyStyle::polish;
    void polish(QWidget *widget) override;

    int pixelMetric(PixelMetric metric, const QStyleOption *option = nullptr, const QWidget *widget = nullptr) const override;
    QRect subElementRect(SubElement element, const QStyleOption *option, const QWidget *widget = nullptr) const override;
    int styleHint(StyleHint hint, const QStyleOption *option = nullptr, const QWidget *widget = nullptr, QStyleHintReturn *returnData = nullptr) const override;
    /// Message boxes' icons: a flat round mark instead of the glossy triangle
    QIcon standardIcon(StandardPixmap standardIcon, const QStyleOption *option = nullptr, const QWidget *widget = nullptr) const override;
    QSize sizeFromContents(ContentsType type, const QStyleOption *option, const QSize &size, const QWidget *widget) const override;
    QRect subControlRect(ComplexControl control, const QStyleOptionComplex *option, SubControl subControl, const QWidget *widget) const override;

    void drawPrimitive(PrimitiveElement element, const QStyleOption *option, QPainter *painter, const QWidget *widget = nullptr) const override;
    void drawControl(ControlElement element, const QStyleOption *option, QPainter *painter, const QWidget *widget = nullptr) const override;
    void drawComplexControl(ComplexControl control, const QStyleOptionComplex *option, QPainter *painter, const QWidget *widget = nullptr) const override;
};

#endif // KISMODERNSTYLE_H
