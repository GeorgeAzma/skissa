/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioTheme.h"

#include "KisColorMath.h"
#include "KisSettingsRegistry.h"

#include <cmath>

#include <QFile>
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTimer>

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

using KisColorMath::colorOf;
using KisColorMath::Oklab;
using KisColorMath::oklabOf;

const char *const group = "StudioTheme";

struct Colors {
    QColor window, windowAlt, view, viewAlt, button, buttonAlt, header, tooltip;
    QColor text, inactive, selection, selectionText, decoration;
};

Colors colorsOf(const Params &p)
{
    const bool dark = p.lightness < 0.5;
    // At full strength the tint clearly colors the backgrounds but stays quieter than the accent
    const Oklab tint = oklabOf(p.tint.isValid() ? p.tint : QColor(Qt::gray));
    const qreal chroma = std::hypot(tint.a, tint.b);
    const qreal reach = 0.11 * qBound<qreal>(0, p.tintAmount, 1);
    const qreal ta = chroma > 1e-4 ? tint.a / chroma * reach : 0;
    const qreal tb = chroma > 1e-4 ? tint.b / chroma * reach : 0;
    auto bg = [&](qreal l) { return colorOf({qBound<qreal>(0, l, 1), ta, tb}); };

    const qreal w = qBound<qreal>(0.05, p.lightness, 1.0);
    // a surface raised off the window is lighter, or darker by as much near white, where there is no lighter
    auto raised = [w](qreal by) { return w + by <= 1.0 ? w + by : w - by; };
    Colors c;
    c.window = bg(w);
    c.view = bg(w);
    c.windowAlt = bg(raised(dark ? 0.045 : 0.03));
    c.viewAlt = bg(raised(0.02));
    c.button = bg(raised(dark ? 0.045 : 0.03));
    c.buttonAlt = bg(w + (dark ? 0.09 : -0.04));
    c.header = bg(w - (dark ? 0.05 : 0.06));
    c.tooltip = bg(raised(dark ? 0.13 : 0.03));
    const qreal textL = dark ? 0.94 : 0.2;
    c.text = colorOf({qBound<qreal>(0, textL, 1), ta * 0.3, tb * 0.3});
    c.inactive = colorOf({w + (textL - w) * 0.5, ta * 0.5, tb * 0.5});
    if (p.accent.isValid()) {
        c.selection = p.accent;
        // links and focus lines are ink on the window: the accent's hue at a lightness that reads
        Oklab ink = oklabOf(p.accent);
        ink.l = dark ? qMax<qreal>(ink.l, 0.78) : qMin<qreal>(ink.l, 0.45);
        c.decoration = colorOf(ink);
    } else {
        // A gray that contrasts with the window. It fills sliders, checks and lines, so it takes
        // the faint tint of the text rather than the stronger one of the backgrounds.
        c.selection = colorOf({dark ? w + 0.21 : 0.31, ta * 0.3, tb * 0.3});
        c.decoration = c.text;
    }
    c.selectionText = oklabOf(c.selection).l < 0.68 ? QColor(250, 250, 250) : QColor(24, 24, 24);
    return c;
}

QString schemePath()
{
    return KoResourcePaths::saveLocation("data", QStringLiteral("color-schemes/"), true) + QStringLiteral("SkissaTheme.colors");
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

namespace
{

using R = KisSettingsRegistry;

const QString listId = QStringLiteral("[StudioThemes] List");
const QString currentId = QStringLiteral("[StudioThemes] Current");

struct Shipped {
    QString id;
    QString name;
    Params params;
};

QVector<Shipped> shipped()
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
        {QStringLiteral("dark"), i18nc("theme preset", "Dark"), params(0.252, Qt::gray, 0, QColor())},
        {QStringLiteral("darker"), i18nc("theme preset", "Darker"), params(0.17, Qt::gray, 0, QColor())},
        {QStringLiteral("night"), i18nc("theme preset", "Night"), params(0.22, QColor(60, 90, 200), 0.4, QColor(92, 124, 250))},
        {QStringLiteral("moss"), i18nc("theme preset", "Moss"), params(0.24, QColor(80, 140, 90), 0.33, QColor(104, 170, 116))},
        {QStringLiteral("light"), i18nc("theme preset", "Light"), params(0.97, Qt::gray, 0, QColor())},
        {QStringLiteral("paper"), i18nc("theme preset", "Paper"), params(0.93, QColor(214, 170, 120), 0.3, QColor(176, 106, 50))},
        {QStringLiteral("sky"), i18nc("theme preset", "Sky"), params(0.94, QColor(110, 150, 220), 0.25, QColor(50, 110, 210))},
        {QStringLiteral("blossom"), i18nc("theme preset", "Blossom"), params(0.94, QColor(240, 120, 175), 0.45, QColor(226, 78, 150))},
    };
}

const Shipped *shippedOf(const QString &id)
{
    static const QVector<Shipped> all = shipped();
    for (const Shipped &s : all) {
        if (s.id == id) return &s;
    }
    return nullptr;
}

/// A shipped theme as it ships: its colors, and the defaults for the rest
Theme themeOf(const Shipped &s)
{
    Theme theme {s.id, QString(), {}};
    Q_FOREACH (const QString &id, held()) theme.values.insert(id, R::instance()->defaultValue(id));
    theme.values.insert(QStringLiteral("[StudioTheme] Lightness"), s.params.lightness);
    theme.values.insert(QStringLiteral("[StudioTheme] Tint"), s.params.tint);
    theme.values.insert(QStringLiteral("[StudioTheme] TintAmount"), s.params.tintAmount);
    theme.values.insert(QStringLiteral("[StudioTheme] UseAccent"), s.params.accent.isValid());
    if (s.params.accent.isValid()) theme.values.insert(QStringLiteral("[StudioTheme] Accent"), s.params.accent);
    return theme;
}

QVector<Theme> parse(const QString &text)
{
    QVector<Theme> list;
    const QJsonDocument document = QJsonDocument::fromJson(text.toUtf8());
    Q_FOREACH (const QJsonValue &item, document.array()) {
        const QJsonObject object = item.toObject();
        Theme theme {object.value(QStringLiteral("id")).toString(), object.value(QStringLiteral("name")).toString(), {}};
        if (theme.id.isEmpty()) continue;
        const QJsonObject values = object.value(QStringLiteral("values")).toObject();
        Q_FOREACH (const QString &id, held()) {
            const R::Setting *setting = R::instance()->setting(id);
            if (!setting || !values.contains(id)) continue;
            QVariant raw = values.value(id).toVariant();
            if (setting->kind == R::Kind::Color) raw = QColor(raw.toString());
            const QVariant value = R::instance()->normalized(*setting, raw);
            if (value.isValid()) theme.values.insert(id, value);
        }
        list << theme;
    }
    // never stored, or not readable: the shipped ones
    if (list.isEmpty()) {
        Q_FOREACH (const Shipped &s, shipped()) list << themeOf(s);
    }
    return list;
}

QString serialized(const QVector<Theme> &list)
{
    QJsonArray array;
    for (const Theme &theme : list) {
        QJsonObject values;
        for (auto it = theme.values.constBegin(); it != theme.values.constEnd(); ++it) {
            values.insert(it.key(), it.value().userType() == QMetaType::QColor ? QJsonValue(it.value().value<QColor>().name(QColor::HexArgb))
                                                                                : QJsonValue::fromVariant(it.value()));
        }
        QJsonObject object {{QStringLiteral("id"), theme.id}, {QStringLiteral("values"), values}};
        if (!theme.name.isEmpty()) object.insert(QStringLiteral("name"), theme.name);
        array << object;
    }
    return QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact));
}

int indexOf(const QVector<Theme> &list, const QString &id)
{
    for (int i = 0; i < list.size(); i++) {
        if (list[i].id == id) return i;
    }
    return -1;
}

/// \p first, then the values that make \p theme the one in use
QVector<QPair<QString, QVariant>> choosing(const Theme &theme, QVector<QPair<QString, QVariant>> first)
{
    first << qMakePair(currentId, QVariant(theme.id));
    Q_FOREACH (const QString &id, held()) {
        if (theme.values.contains(id)) first << qMakePair(id, theme.values.value(id));
    }
    return first;
}

void write(const QVector<QPair<QString, QVariant>> &values)
{
    R::instance()->setValues(values);
    // the same colors as now, from a shipped scheme: the generated one in its place
    if (R::instance()->value(QStringLiteral("[theme] Theme")).toString() != schemeName()) apply(current());
}

/**
 * The first time themes are kept: the look in use stays with the shipped theme of its colors, or
 * becomes a theme of its own. Written straight to kritarc, as nothing the user did.
 */
void settle()
{
    static bool done = false;
    if (done) return;
    done = true;
    KConfigGroup g = KSharedConfig::openConfig()->group("StudioThemes");
    if (g.hasKey("Current")) return;
    QVector<Theme> list = parse(g.readEntry("List", QString()));
    const Params now = current();
    int match = -1;
    for (int i = 0; i < list.size() && match < 0; i++) {
        if (paramsOf(list[i]) == now) match = i;
    }
    if (match < 0) {
        list << Theme {QStringLiteral("own1"), i18nc("theme name", "Mine"), {}};
        match = list.size() - 1;
    }
    Q_FOREACH (const QString &id, held()) list[match].values.insert(id, R::instance()->value(id));
    g.writeEntry("List", serialized(list));
    g.writeEntry("Current", list[match].id);
    R::instance()->scheduleSync();
}

} // namespace

QStringList held()
{
    // the font's switch before its family and size, which turning it on or off fills in or clears
    return {QStringLiteral("[StudioTheme] Lightness"), QStringLiteral("[StudioTheme] Tint"), QStringLiteral("[StudioTheme] TintAmount"),
            QStringLiteral("[StudioTheme] UseAccent"), QStringLiteral("[StudioTheme] Accent"), QStringLiteral("StudioGlass"),
            QStringLiteral("StudioGlassBlur"), QStringLiteral("StudioGlassOpacity"), QStringLiteral("StudioGlassRimLight"),
            QStringLiteral("StudioGlassVibrancy"), QStringLiteral("StudioGlassGrain"),
            QStringLiteral("canvasBorderColor"), QStringLiteral("use_custom_system_font"), QStringLiteral("custom_system_font"),
            QStringLiteral("custom_font_size")};
}

QVector<Theme> themes()
{
    settle();
    return parse(R::instance()->value(listId).toString());
}

QString nameOf(const Theme &theme)
{
    if (!theme.name.isEmpty()) return theme.name;
    const Shipped *s = shippedOf(theme.id);
    return s ? s->name : theme.id;
}

Params paramsOf(const Theme &theme)
{
    Params p;
    p.lightness = theme.values.value(QStringLiteral("[StudioTheme] Lightness"), p.lightness).toReal();
    p.tint = theme.values.value(QStringLiteral("[StudioTheme] Tint"), p.tint).value<QColor>();
    p.tintAmount = theme.values.value(QStringLiteral("[StudioTheme] TintAmount"), p.tintAmount).toReal();
    if (theme.values.value(QStringLiteral("[StudioTheme] UseAccent")).toBool()) {
        p.accent = theme.values.value(QStringLiteral("[StudioTheme] Accent"), QColor(92, 124, 250)).value<QColor>();
    }
    return p;
}

QString currentTheme()
{
    const QVector<Theme> list = themes();
    const QString id = R::instance()->value(currentId).toString();
    return indexOf(list, id) >= 0 ? id : QString();
}

void pick(const QString &id)
{
    const QVector<Theme> list = themes();
    const int i = indexOf(list, id);
    if (i >= 0) write(choosing(list[i], {}));
}

void add()
{
    QVector<Theme> list = themes();
    const int from = indexOf(list, currentTheme());
    int number = 1;
    for (const Theme &theme : list) {
        if (theme.id.startsWith(QLatin1String("own"))) number = qMax(number, theme.id.mid(3).toInt() + 1);
    }
    // "Dark" → "Dark 2", "Dark 2" → "Dark 3"
    QString base = from >= 0 ? nameOf(list[from]) : i18nc("theme name", "Mine");
    const QRegularExpressionMatch numbered = QRegularExpression(QStringLiteral("^(.*\\S) (\\d+)$")).match(base);
    if (numbered.hasMatch()) base = numbered.captured(1);
    QStringList names;
    for (const Theme &theme : list) names << nameOf(theme);
    int n = 2;
    while (names.contains(QStringLiteral("%1 %2").arg(base).arg(n))) n++;
    Theme theme {QStringLiteral("own%1").arg(number), QStringLiteral("%1 %2").arg(base).arg(n), {}};
    Q_FOREACH (const QString &setting, held()) theme.values.insert(setting, R::instance()->value(setting));
    list.insert(from >= 0 ? from + 1 : list.size(), theme);
    R::instance()->setValues({{listId, serialized(list)}, {currentId, theme.id}});
}

void rename(const QString &id, const QString &name)
{
    QVector<Theme> list = themes();
    const int i = indexOf(list, id);
    const QString typed = name.simplified();
    if (i < 0 || typed.isEmpty()) return;
    const Shipped *s = shippedOf(id);
    list[i].name = s && s->name == typed ? QString() : typed;
    R::instance()->setValue(listId, serialized(list));
}

void remove(const QString &id)
{
    QVector<Theme> list = themes();
    const int i = indexOf(list, id);
    if (i < 0 || list.size() < 2) return;
    const bool inUse = id == currentTheme();
    list.remove(i);
    const QVector<QPair<QString, QVariant>> stored {{listId, serialized(list)}};
    if (inUse) write(choosing(list[qMin(i, int(list.size()) - 1)], stored));
    else R::instance()->setValues(stored);
}

void reset(const QString &id)
{
    QVector<Theme> list = themes();
    const int i = indexOf(list, id);
    const Shipped *s = shippedOf(id);
    if (i < 0 || !s) return;
    list[i] = themeOf(*s);
    const QVector<QPair<QString, QVariant>> stored {{listId, serialized(list)}};
    if (id == currentTheme()) write(choosing(list[i], stored));
    else R::instance()->setValues(stored);
}

bool isChanged(const QString &id)
{
    const Shipped *s = shippedOf(id);
    const QVector<Theme> list = themes();
    const int i = indexOf(list, id);
    if (!s || i < 0) return false;
    if (!list[i].name.isEmpty()) return true;
    const Theme original = themeOf(*s);
    Q_FOREACH (const QString &setting, held()) {
        const R::Setting *described = R::instance()->setting(setting);
        if (described && list[i].values.contains(setting)
            && !R::instance()->sameValue(*described, list[i].values.value(setting), original.values.value(setting))) {
            return true;
        }
    }
    return false;
}

bool anyDeleted()
{
    const QVector<Theme> list = themes();
    Q_FOREACH (const Shipped &s, shipped()) {
        if (indexOf(list, s.id) < 0) return true;
    }
    return false;
}

void restoreDeleted()
{
    QVector<Theme> list = themes();
    QString before;
    Q_FOREACH (const Shipped &s, shipped()) {
        if (indexOf(list, s.id) < 0) list.insert(before.isEmpty() ? 0 : indexOf(list, before) + 1, themeOf(s));
        before = s.id;
    }
    R::instance()->setValue(listId, serialized(list));
}

void remember(const QString &id, const QVariant &value)
{
    // an undo puts the list back by itself
    if (R::instance()->replaying()) return;
    QVector<Theme> list = themes();
    const int i = indexOf(list, R::instance()->value(currentId).toString());
    const R::Setting *setting = R::instance()->setting(id);
    if (i < 0 || !setting) return;
    if (list[i].values.contains(id) && R::instance()->sameValue(*setting, list[i].values.value(id), value)) return;
    list[i].values.insert(id, value);
    R::instance()->setValue(listId, serialized(list));
}

Params current()
{
    const KConfigGroup g = KSharedConfig::openConfig()->group(group);
    Params p;
    p.lightness = qBound<qreal>(0.05, g.readEntry("Lightness", p.lightness), 1.0);
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
    // through the registry, so the switch to the generated theme is undone with the change that made it;
    // an undo puts back the theme it recorded
    if (!KisSettingsRegistry::instance()->replaying()) {
        KisSettingsRegistry::instance()->setValue(QStringLiteral("[theme] Theme"), schemeName());
    }
    // Bumping the revision makes the window reload the scheme even when its name is unchanged
    KConfigGroup theme = KSharedConfig::openConfig()->group("theme");
    theme.writeEntry("Revision", theme.readEntry("Revision", 0) + 1);
    // once for all the values a preset changes at a time, and at most every few frames while a lightness or
    // tint is dragged: each notice reloads the theme into every widget
    static QTimer *notify = []() {
        auto *timer = new QTimer(qApp);
        timer->setSingleShot(true);
        timer->setInterval(40);
        QObject::connect(timer, &QTimer::timeout, []() { KisConfigNotifier::instance()->notifyConfigChanged(); });
        return timer;
    }();
    if (!notify->isActive()) notify->start();
}

QString schemeName()
{
    return QStringLiteral("Skissa");
}

void remake()
{
    static bool done = false;
    if (done) return;
    done = true;
    // shipped themes still as they first shipped take the defaults they ship with now: glass at 85%, not 70%,
    // and Light at 97%, not 95.5%. The values in use follow when the theme is the current one
    KConfigGroup themes = KSharedConfig::openConfig()->group("StudioThemes");
    if (themes.readEntry("Defaults", 0) < 1 && themes.hasKey("List")) {
        QVector<Theme> list = parse(themes.readEntry("List", QString()));
        const QString current = themes.readEntry("Current", QString());
        const QString glass = QStringLiteral("StudioGlassOpacity");
        const QString lightness = QStringLiteral("[StudioTheme] Lightness");
        KConfigGroup top = KSharedConfig::openConfig()->group(QString());
        KConfigGroup studioTheme = KSharedConfig::openConfig()->group("StudioTheme");
        for (Theme &t : list) {
            if (!shippedOf(t.id)) continue;
            const bool inUse = t.id == current;
            if (t.values.contains(glass) && qFuzzyCompare(t.values.value(glass).toDouble(), 0.7)) {
                t.values.insert(glass, 0.85);
                if (inUse && qFuzzyCompare(top.readEntry(glass, 0.7), 0.7)) top.writeEntry(glass, 0.85);
            }
            if (t.id == QLatin1String("light") && t.values.contains(lightness) && qFuzzyCompare(t.values.value(lightness).toDouble(), 0.955)) {
                t.values.insert(lightness, 0.97);
                if (inUse && qFuzzyCompare(studioTheme.readEntry("Lightness", 0.955), 0.955)) studioTheme.writeEntry("Lightness", 0.97);
            }
        }
        themes.writeEntry("List", serialized(list));
    }
    themes.writeEntry("Defaults", 1);
    KConfigGroup theme = KSharedConfig::openConfig()->group("theme");
    // themes from builds named Krita Studio
    const QString stored = theme.readEntry("Theme", QString());
    if (stored.startsWith(QLatin1String("Krita Studio"))) theme.writeEntry("Theme", QString(schemeName() + stored.mid(12)));
    QFile::remove(KoResourcePaths::saveLocation("data", QStringLiteral("color-schemes/"), true) + QStringLiteral("KritaStudioTheme.colors"));
    if (theme.readEntry("Theme", QString()) == schemeName()) writeScheme(current());
}

Look look(const Params &params)
{
    const Colors c = colorsOf(params);
    return {c.window, c.view, c.text, c.selection, c.button};
}

} // namespace KisStudioTheme
