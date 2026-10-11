/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISMODERNSTYLE_H
#define KISMODERNSTYLE_H

#include "kritawidgetutils_export.h"
#include <QPainterPath>
#include <QProxyStyle>
#include <QKeySequence>
#include <QFont>

class QAction;

/**
 * A flat, rounded widget style built on top of Fusion.
 *
 * Everything that is not overridden here is still painted by Fusion, so
 * metrics and layouts stay close to what Krita's widgets were designed for.
 * All colors come from the palette (i.e. the active color theme), this
 * style only changes shapes, spacing and interaction states.
 */
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

    /**
     * The name of the style that actually paints the widgets, i.e. "fusion"
     * for KisModernStyle. Some widgets adjust their geometry based on it.
     */
    static QString underlyingStyleName(const QStyle *style);

    /// Corner radius of rounded controls, for widgets that paint themselves
    static constexpr int cornerRadius = 5;

    /**
     * A frameless line edit with this property set true is a quiet field:
     * no box until it is pointed at or typed in (a search over a list, on a
     * panel of its own)
     */
    static constexpr const char *quietFieldProperty = "studioQuietField";

    /**
     * A checkable tool button with this property set true is a tab: no fill
     * when checked, its icon at full ink and a short bar under it; unchecked,
     * its icon faint (a strip of panels' buttons)
     */
    static constexpr const char *tabButtonProperty = "studioTabButton";

    /**
     * \p rect with squircle corners: each a quarter of a superellipse,
     * |x|^4 + |y|^4 = 1, whose bend eases in from the sides (a circular
     * corner's starts all at once where the side ends). A corner is as round
     * as a circular one of \p radius (it comes as far into the corner) and
     * reaches further along the sides. Where the sides are too short for
     * that, the exponent is less, down to 2, a circle: a pill stays a pill.
     */
    static QPainterPath squircle(const QRectF &rect, qreal radius);

    /**
     * The corners of squircle() for \p radius, the sides \p room long
     * either way from the middle: how far along the sides a corner reaches,
     * and the exponent of its superellipse.
     */
    static void squircleCorner(qreal radius, qreal room, qreal *reach, qreal *exponent);

    /**
     * Whether the system rounds popup windows (menus, lists, tooltips,
     * floating panels) and draws their border and shadow: Windows 11. The
     * style asks it to for every such window; they then draw no frame of
     * their own.
     */
    static bool systemRoundsWindows();

    /**
     * Shortcuts look the same wherever they are listed (menus, Find, a
     * button's card, a button's corner): a key cap, its text short ("Ctrl ⇧E").
     */
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
    /// The key cap at the right of \p row, in the middle of its height
    static QRectF keyCapRect(const QRectF &row, const QFont &font, const QString &text, KeyCap state = KeyCap::Shown);
    static void drawKeyCap(QPainter *painter, const QRectF &rect, const QFont &font, const QString &text,
                           const QPalette &palette, KeyCap state = KeyCap::Shown);
    /// The smaller font of key caps in lists, from the list's font
    static QFont keyCapFont(const QFont &font);

    /**
     * \p font at \p scale times its size and of \p weight, as type is set:
     * Segoe UI Variable (the Windows 11 face) cut for the size, small text
     * from its Small cut (wider, more open), large from Display, and a
     * semibold its own (not the bold Qt would take for it)
     */
    static QFont textFont(const QFont &font, qreal scale, QFont::Weight weight = QFont::Normal);

    /**
     * \p ink as secondary text is set (hints, captions, units, column
     * labels): faint, but still readable on glass over a bright canvas
     */
    static QColor secondaryInk(QColor ink);

    /**
     * The action whose shortcut a menu entry shows: the entry itself, or
     * the action it stands in for (its "kisShortcutTarget" property: menus
     * may name an action in fewer words). None for what has no shortcut of
     * its own (a submenu, a separator, an action with no name to keep it by).
     */
    static QAction *shortcutTarget(QAction *entry);

    /// After a drag that nothing took (Qt::IgnoreAction): given up on with Esc, the button still held, not let go of somewhere
    static bool dragCancelled();

    using QProxyStyle::polish;
    void polish(QWidget *widget) override;

    int pixelMetric(PixelMetric metric, const QStyleOption *option = nullptr, const QWidget *widget = nullptr) const override;
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
