/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioTheme.h"

#include "KisSettingsRegistry.h"

#include <cmath>

#include <QFile>

#include <KConfig>
#include <KConfigGroup>
#include <KSharedConfig>
#include <klocalizedstring.h>

#include <KoResourcePaths.h>

#include "kis_config_notifier.h"

namespace KisStudioTheme
{

namespace
{

struct Lab {
    qreal l, a, b;
};

qreal toLinear(qreal c)
{
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

qreal toSrgb(qreal c)
{
    c = qBound<qreal>(0, c, 1);
    return c <= 0.0031308 ? 12.92 * c : 1.055 * std::pow(c, 1 / 2.4) - 0.055;
}

Lab toOklab(const QColor &color)
{
    const qreal r = toLinear(color.redF()), g = toLinear(color.greenF()), b = toLinear(color.blueF());
    const qreal l = std::cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b);
    const qreal m = std::cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b);
    const qreal s = std::cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b);
    return {0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s, 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
            0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s};
}

QColor fromOklab(Lab lab)
{
    lab.l = qBound<qreal>(0, lab.l, 1);
    const qreal l = std::pow(lab.l + 0.3963377774 * lab.a + 0.2158037573 * lab.b, 3);
    const qreal m = std::pow(lab.l - 0.1055613458 * lab.a - 0.0638541728 * lab.b, 3);
    const qreal s = std::pow(lab.l - 0.0894841775 * lab.a - 1.2914855480 * lab.b, 3);
    return QColor::fromRgbF(toSrgb(4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s),
                            toSrgb(-1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s),
                            toSrgb(-0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s));
}

const char *const group = "StudioTheme";

struct Colors {
    QColor window, windowAlt, view, viewAlt, button, buttonAlt, header, tooltip;
    QColor text, inactive, selection, selectionText, decoration;
};

Colors colorsOf(const Params &p)
{
    const bool dark = p.lightness < 0.5;
    // At full strength the tint clearly colors the backgrounds but stays quieter than the accent
    const Lab tint = toOklab(p.tint.isValid() ? p.tint : QColor(Qt::gray));
    const qreal chroma = std::hypot(tint.a, tint.b);
    const qreal reach = 0.11 * qBound<qreal>(0, p.tintAmount, 1);
    const qreal ta = chroma > 1e-4 ? tint.a / chroma * reach : 0;
    const qreal tb = chroma > 1e-4 ? tint.b / chroma * reach : 0;
    auto bg = [&](qreal l) { return fromOklab({qBound<qreal>(0, l, 1), ta, tb}); };

    const qreal w = qBound<qreal>(0.05, p.lightness, 0.98);
    Colors c;
    c.window = bg(w);
    c.view = bg(w);
    c.windowAlt = bg(w + (dark ? 0.045 : 0.03));
    c.viewAlt = bg(w + 0.02);
    c.button = bg(w + (dark ? 0.045 : 0.03));
    c.buttonAlt = bg(w + (dark ? 0.09 : -0.04));
    c.header = bg(w - (dark ? 0.05 : 0.06));
    c.tooltip = bg(w + (dark ? 0.13 : 0.03));
    const qreal textL = dark ? 0.94 : 0.2;
    c.text = fromOklab({qBound<qreal>(0, textL, 1), ta * 0.3, tb * 0.3});
    c.inactive = fromOklab({w + (textL - w) * 0.5, ta * 0.5, tb * 0.5});
    if (p.accent.isValid()) {
        c.selection = p.accent;
        c.decoration = p.accent;
    } else {
        // A gray that contrasts with the window. It fills sliders, checks and lines, so it takes
        // the faint tint of the text rather than the stronger one of the backgrounds.
        c.selection = fromOklab({dark ? w + 0.21 : 0.31, ta * 0.3, tb * 0.3});
        c.decoration = c.text;
    }
    c.selectionText = toOklab(c.selection).l < 0.68 ? QColor(250, 250, 250) : QColor(24, 24, 24);
    return c;
}

QString schemePath()
{
    return KoResourcePaths::saveLocation("data", QStringLiteral("color-schemes/"), true) + QStringLiteral("KritaStudioTheme.colors");
}

void writeScheme(const Params &p)
{
    const Colors c = colorsOf(p);
    QFile::remove(schemePath());
    KConfig scheme(schemePath(), KConfig::SimpleConfig);
    auto colors = [&](const char *name, const QColor &background, const QColor &alternate, const QColor &text, const QColor &inactive) {
        KConfigGroup g = scheme.group(name);
        g.writeEntry("BackgroundNormal", background);
        g.writeEntry("BackgroundAlternate", alternate);
        g.writeEntry("ForegroundNormal", text);
        g.writeEntry("ForegroundActive", text);
        g.writeEntry("ForegroundInactive", inactive);
        g.writeEntry("ForegroundLink", c.decoration);
        g.writeEntry("ForegroundVisited", c.decoration);
        g.writeEntry("ForegroundNegative", QColor(230, 110, 104));
        g.writeEntry("ForegroundNeutral", QColor(228, 183, 80));
        g.writeEntry("ForegroundPositive", QColor(118, 199, 136));
        g.writeEntry("DecorationFocus", c.decoration);
        g.writeEntry("DecorationHover", c.inactive);
    };
    colors("Colors:Window", c.window, c.windowAlt, c.text, c.inactive);
    colors("Colors:View", c.view, c.viewAlt, c.text, c.inactive);
    colors("Colors:Button", c.button, c.buttonAlt, c.text, c.inactive);
    colors("Colors:Selection", c.selection, c.selection, c.selectionText, c.selectionText);
    colors("Colors:Tooltip", c.tooltip, c.button, c.text, c.inactive);
    colors("Colors:Header", c.header, c.window, c.text, c.inactive);
    colors("Colors:Complementary", c.header, c.window, c.text, c.inactive);
    KConfigGroup disabled = scheme.group("ColorEffects:Disabled");
    disabled.writeEntry("Color", c.window);
    disabled.writeEntry("ColorAmount", 0);
    disabled.writeEntry("ColorEffect", 0);
    disabled.writeEntry("ContrastAmount", 0.55);
    disabled.writeEntry("ContrastEffect", 1);
    disabled.writeEntry("IntensityAmount", 0);
    disabled.writeEntry("IntensityEffect", 0);
    KConfigGroup inactive = scheme.group("ColorEffects:Inactive");
    inactive.writeEntry("ChangeSelectionColor", false);
    inactive.writeEntry("Enable", false);
    KConfigGroup general = scheme.group("General");
    general.writeEntry("Name", schemeName());
    general.writeEntry("ColorScheme", schemeName());
    general.writeEntry("shadeSortColumn", false);
    scheme.group("KDE").writeEntry("contrast", 4);
    KConfigGroup wm = scheme.group("WM");
    wm.writeEntry("activeBackground", c.header);
    wm.writeEntry("activeForeground", c.text);
    wm.writeEntry("activeBlend", c.text);
    wm.writeEntry("inactiveBackground", c.header);
    wm.writeEntry("inactiveForeground", c.inactive);
    wm.writeEntry("inactiveBlend", c.inactive);
    scheme.sync();
}

} // namespace

bool Params::operator==(const Params &other) const
{
    return qFuzzyCompare(lightness + 1, other.lightness + 1) && tint == other.tint && qFuzzyCompare(tintAmount + 1, other.tintAmount + 1)
        && accent == other.accent;
}

QVector<Preset> presets()
{
    auto params = [](qreal lightness, const QColor &tint, qreal amount, const QColor &accent) {
        Params p;
        p.lightness = lightness;
        p.tint = tint;
        p.tintAmount = amount;
        p.accent = accent;
        return p;
    };
    return {
        {i18nc("theme preset", "Dark"), params(0.252, Qt::gray, 0, QColor())},
        {i18nc("theme preset", "Darker"), params(0.17, Qt::gray, 0, QColor())},
        {i18nc("theme preset", "Graphite"), params(0.36, QColor(70, 110, 170), 0.18, QColor())},
        {i18nc("theme preset", "Night"), params(0.22, QColor(60, 90, 200), 0.4, QColor(92, 124, 250))},
        {i18nc("theme preset", "Moss"), params(0.24, QColor(80, 140, 90), 0.33, QColor(104, 170, 116))},
        {i18nc("theme preset", "Light"), params(0.955, Qt::gray, 0, QColor())},
        {i18nc("theme preset", "Paper"), params(0.93, QColor(214, 170, 120), 0.3, QColor(176, 106, 50))},
        {i18nc("theme preset", "Sky"), params(0.94, QColor(110, 150, 220), 0.25, QColor(50, 110, 210))},
    };
}

Params current()
{
    const KConfigGroup g = KSharedConfig::openConfig()->group(group);
    Params p;
    p.lightness = qBound<qreal>(0.05, g.readEntry("Lightness", p.lightness), 0.98);
    p.tint = g.readEntry("Tint", QColor(Qt::gray));
    p.tintAmount = qBound<qreal>(0, g.readEntry("TintAmount", p.tintAmount), 1);
    p.accent = g.readEntry("UseAccent", false) ? g.readEntry("Accent", QColor(92, 124, 250)) : QColor();
    return p;
}

void apply(const Params &params)
{
    KConfigGroup g = KSharedConfig::openConfig()->group(group);
    g.writeEntry("Lightness", params.lightness);
    g.writeEntry("Tint", params.tint);
    g.writeEntry("TintAmount", params.tintAmount);
    g.writeEntry("UseAccent", params.accent.isValid());
    if (params.accent.isValid()) g.writeEntry("Accent", params.accent);
    writeScheme(params);
    // The scheme file may be new, so the list of themes has to be scanned again
    KisSettingsRegistry::instance()->refreshChoices();
    // Bumping the revision makes the window reload the scheme even when its name is unchanged
    KConfigGroup theme = KSharedConfig::openConfig()->group("theme");
    theme.writeEntry("Theme", schemeName());
    theme.writeEntry("Revision", theme.readEntry("Revision", 0) + 1);
    KisConfigNotifier::instance()->notifyConfigChanged();
}

QString schemeName()
{
    return QStringLiteral("Krita Studio");
}

void remake()
{
    static bool done = false;
    if (done) return;
    done = true;
    if (KSharedConfig::openConfig()->group("theme").readEntry("Theme", QString()) == schemeName()) writeScheme(current());
}

Look look(const Params &params)
{
    const Colors c = colorsOf(params);
    return {c.window, c.view, c.text, c.selection, c.button};
}

} // namespace KisStudioTheme
