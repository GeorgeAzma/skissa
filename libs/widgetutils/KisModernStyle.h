/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISMODERNSTYLE_H
#define KISMODERNSTYLE_H

#include "kritawidgetutils_export.h"
#include <QProxyStyle>

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
     * Whether the system rounds popup windows (menus, lists, tooltips,
     * floating panels) and draws their border and shadow: Windows 11. The
     * style asks it to for every such window; they then draw no frame of
     * their own.
     */
    static bool systemRoundsWindows();

    using QProxyStyle::polish;
    void polish(QWidget *widget) override;

    int pixelMetric(PixelMetric metric, const QStyleOption *option = nullptr, const QWidget *widget = nullptr) const override;
    int styleHint(StyleHint hint, const QStyleOption *option = nullptr, const QWidget *widget = nullptr, QStyleHintReturn *returnData = nullptr) const override;
    QSize sizeFromContents(ContentsType type, const QStyleOption *option, const QSize &size, const QWidget *widget) const override;
    QRect subControlRect(ComplexControl control, const QStyleOptionComplex *option, SubControl subControl, const QWidget *widget) const override;

    void drawPrimitive(PrimitiveElement element, const QStyleOption *option, QPainter *painter, const QWidget *widget = nullptr) const override;
    void drawControl(ControlElement element, const QStyleOption *option, QPainter *painter, const QWidget *widget = nullptr) const override;
    void drawComplexControl(ComplexControl control, const QStyleOptionComplex *option, QPainter *painter, const QWidget *widget = nullptr) const override;
};

#endif // KISMODERNSTYLE_H
