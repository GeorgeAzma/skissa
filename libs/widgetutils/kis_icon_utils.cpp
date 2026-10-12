/*
 *  SPDX-FileCopyrightText: 2015 Dmitry Kazakov <dimula73@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "kis_icon_utils.h"

#include <QApplication>
#include <QAction>
#include <QAbstractButton>
#include <QComboBox>
#include <QTabBar>
#include <QTabWidget>
#include <QIcon>
#include <QFile>
#include <QPair>
#include <QHash>
#include <QDebug>

#include <KoIcon.h>
#include "kis_debug.h"

namespace KisIconUtils
{

static QMap<QString, QIcon> s_cache;
static QMap<qint64, QString> s_icons;

/**
 * Krita's icons drawn again in the Studio icons' one language (see
 * krita/pics/svg/make_studio_icons.py): the ones in sight all the time,
 * the tools, the top bar's switches, a layer's switches. Every place that
 * asks for one of these names gets the new drawing.
 */
static QString studioIcon(const QString &name)
{
    static const QHash<QString, QString> icons = {
        // tools
        {QStringLiteral("krita_tool_freehand"), QStringLiteral("studio_paint")},
        {QStringLiteral("krita_tool_dyna"), QStringLiteral("studio_dyna")},
        {QStringLiteral("krita_tool_multihand"), QStringLiteral("studio_multihand")},
        {QStringLiteral("krita_tool_line"), QStringLiteral("studio_line")},
        {QStringLiteral("krita_tool_rectangle"), QStringLiteral("studio_rectangle")},
        {QStringLiteral("krita_tool_ellipse"), QStringLiteral("studio_ellipse")},
        {QStringLiteral("krita_tool_polygon"), QStringLiteral("studio_polygon")},
        {QStringLiteral("polyline"), QStringLiteral("studio_polyline")},
        {QStringLiteral("krita_draw_path"), QStringLiteral("studio_path")},
        {QStringLiteral("krita_tool_freehandvector"), QStringLiteral("studio_freehand_path")},
        {QStringLiteral("krita_tool_color_fill"), QStringLiteral("studio_fill")},
        {QStringLiteral("krita_tool_gradient"), QStringLiteral("studio_gradient")},
        {QStringLiteral("krita_tool_enclose_and_fill"), QStringLiteral("studio_enclose")},
        {QStringLiteral("krita_tool_color_sampler"), QStringLiteral("studio_sampler")},
        {QStringLiteral("krita_tool_smart_patch"), QStringLiteral("studio_patch")},
        {QStringLiteral("krita_tool_move"), QStringLiteral("studio_move")},
        {QStringLiteral("krita_tool_transform"), QStringLiteral("studio_transform")},
        {QStringLiteral("tool_crop"), QStringLiteral("studio_crop")},
        {QStringLiteral("tool_zoom"), QStringLiteral("studio_zoom")},
        {QStringLiteral("krita_tool_measure"), QStringLiteral("studio_measure")},
        {QStringLiteral("krita_tool_assistant"), QStringLiteral("studio_guides")},
        {QStringLiteral("krita_tool_reference_images"), QStringLiteral("studio_reference")},
        {QStringLiteral("draw-text"), QStringLiteral("studio_text")},
        {QStringLiteral("select"), QStringLiteral("studio_pointer")},
        {QStringLiteral("calligraphy"), QStringLiteral("studio_calligraphy")},
        {QStringLiteral("tool_rect_selection"), QStringLiteral("studio_select")},
        {QStringLiteral("tool_elliptical_selection"), QStringLiteral("studio_select_ellipse")},
        {QStringLiteral("tool_polygonal_selection"), QStringLiteral("studio_select_polygon")},
        {QStringLiteral("tool_outline_selection"), QStringLiteral("studio_lasso")},
        {QStringLiteral("tool_contiguous_selection"), QStringLiteral("studio_wand")},
        // the top bar
        {QStringLiteral("draw-eraser"), QStringLiteral("studio_erase")},
        {QStringLiteral("transform_icons_penPressure"), QStringLiteral("studio_pressure")},
        {QStringLiteral("transform_icons_penPressure_locked"), QStringLiteral("studio_pressure_off")},
        {QStringLiteral("paintop_settings_02"), QStringLiteral("studio_brush_settings")},
        {QStringLiteral("bar-transparency-locked"), QStringLiteral("studio_alpha_lock")},
        {QStringLiteral("bar-transparency-unlocked"), QStringLiteral("studio_alpha_unlock")},
        {QStringLiteral("reload-preset"), QStringLiteral("studio_reload")},
        {QStringLiteral("symmetry-horizontal"), QStringLiteral("studio_mirror_x")},
        {QStringLiteral("symmetry-vertical"), QStringLiteral("studio_mirror_y")},
        {QStringLiteral("wraparound"), QStringLiteral("studio_wrap")},
        {QStringLiteral("edit-find"), QStringLiteral("studio_find")},
        // a layer's switches
        {QStringLiteral("visible"), QStringLiteral("studio_visible")},
        {QStringLiteral("novisible"), QStringLiteral("studio_hidden")},
        {QStringLiteral("layer-locked"), QStringLiteral("studio_locked")},
        {QStringLiteral("layer-unlocked"), QStringLiteral("studio_unlocked")},
        {QStringLiteral("transparency-locked"), QStringLiteral("studio_alpha_lock")},
        {QStringLiteral("transparency-unlocked"), QStringLiteral("studio_alpha_unlock")},
        // inherit alpha on (Krita's names are the other way round)
        {QStringLiteral("transparency-disabled"), QStringLiteral("studio_clip_on")},
        {QStringLiteral("transparency-enabled"), QStringLiteral("studio_clip")},
        // commands
        {QStringLiteral("list-add"), QStringLiteral("studio_add")},
        {QStringLiteral("addlayer"), QStringLiteral("studio_add")},
        {QStringLiteral("edit-delete"), QStringLiteral("studio_delete")},
        {QStringLiteral("view-refresh"), QStringLiteral("studio_refresh")},
        {QStringLiteral("edit-undo"), QStringLiteral("studio_undo")},
        {QStringLiteral("view-filter"), QStringLiteral("studio_filter")},
    };
    return icons.value(name);
}

QIcon loadIcon(const QString &requestedName)
{
    QMap<QString, QIcon>::const_iterator cached = s_cache.constFind(requestedName);
    if (cached != s_cache.constEnd()) {
        return cached.value();
    }
    const QString studio = studioIcon(requestedName);
    if (!studio.isEmpty()) {
        // kept under the name it was asked by, so that a theme change finds it again
        QIcon icon = loadIcon(studio);
        s_icons.insert(icon.cacheKey(), requestedName);
        s_cache.insert(requestedName, icon);
        return icon;
    }
    const QString &name = requestedName;

    // try load themed icon
    const char * const prefix = useDarkIcons() ? "dark_" : "light_";

    QString  realName = QLatin1String(prefix) + name;

    // Dark and light, no size specified
    const QStringList names = { ":/pics/" + realName + ".png",
                                ":/pics/" + realName + ".svg",
                                ":/pics/" + realName + ".svgz",
                                ":/pics/" + name + ".png",
                                ":/pics/" + name + ".svg",
                                ":/pics/" + name + ".svgz",
                                ":/" + realName + ".png",
                                ":/" + realName + ".svg",
                                ":/" + realName + ".svgz",
                                ":/" + name,
                                ":/" + name + ".png",
                                ":/" + name + ".svg",
                                ":/" + name + ".svgz"};

    for (const QString &resname : names) {
        if (QFile(resname).exists()) {
            QIcon icon(resname);
            s_icons.insert(icon.cacheKey(), name);
            s_cache.insert(name, icon);
            return icon;
        }
    }

    // Now check for icons with sizes
    QStringList sizes = QStringList() << "16_" << "22_" << "32_" << "48_" << "64_" << "128_" << "256_" << "512_" << "1024_";
    QVector<QPair<QString, QString> > icons;
    Q_FOREACH (const QString &size, sizes) {
        const QStringList names = { ":/pics/" + size + realName + ".png",
                                    ":/pics/" + size + realName + ".svg",
                                    ":/pics/" + size + realName + ".svgz",
                                    ":/pics/" + size + name + ".png",
                                    ":/pics/" + size + name + ".svg",
                                    ":/pics/" + size + name + ".svgz",
                                    ":/" + size + realName + ".png",
                                    ":/" + size + realName + ".svg",
                                    ":/" + size + realName + ".svgz",
                                    ":/" + size + name,
                                    ":/" + size + name + ".png",
                                    ":/" + size + name + ".svg",
                                    ":/" + size + name + ".svgz"};

        for (const QString &resname : names) {
            if (QFile(resname).exists()) {
                icons << qMakePair(size, resname);
            }
        }
    }

    if (!icons.isEmpty()) {
        QIcon icon;
        Q_FOREACH (auto p, icons) {
            QString sz = p.first;
            sz.chop(1);
            int size = sz.toInt();
            icon.addFile(p.second, QSize(size, size));
        }
        s_icons.insert(icon.cacheKey(), name);
        s_cache.insert(name, icon);
        return icon;
    }

    QIcon icon = QIcon::fromTheme(name);
    //qDebug() << "falling back on QIcon::FromTheme:" << name;
    s_icons.insert(icon.cacheKey(), name);
    s_cache.insert(name, icon);
    return icon;
}




bool useDarkIcons() {
    QColor background = qApp->palette().window().color();
    return  background.value() > 100;
}

bool adjustIcon(QIcon *icon)
{
    bool result = false;

    QString iconName = icon->name();
    if (iconName.isNull()) {
        if (s_icons.contains(icon->cacheKey())) {
            iconName = s_icons[icon->cacheKey()];
        } else {
            return false;
        }
    }

    QString realIconName = iconName;

    if (iconName.startsWith("dark_")) {
        realIconName = iconName.mid(5);
    }

    if (iconName.startsWith("light_")) {
        realIconName = iconName.mid(6);
    }

    if (!realIconName.isNull()) {
        *icon = loadIcon(realIconName);
        result = !icon->isNull();
        s_icons.insert(icon->cacheKey(), iconName);
    }

    return result;
}

void updateIconCommon(QObject *object)
{
    QAbstractButton* button = qobject_cast<QAbstractButton*>(object);
    if (button) {
        updateIcon(button);
        return;
    }

    QComboBox* comboBox = qobject_cast<QComboBox*>(object);
    if (comboBox) {
        updateIcon(comboBox);
        return;
    }

    QAction* action = qobject_cast<QAction*>(object);
    if (action) {
        updateIcon(action);
        return;
    }

    QTabBar* tabBar = qobject_cast<QTabBar*>(object);
    if (tabBar) {
        updateIcon(tabBar);
        return;
    }
}

void clearIconCache() {
    s_cache.clear();
}

void updateIcon(QAbstractButton *button)
{
    KIS_SAFE_ASSERT_RECOVER_RETURN(button);

    QIcon icon = button->icon();

    if (adjustIcon(&icon)) {
        button->setIcon(icon);
    }
}

void updateIcon(QComboBox *comboBox)
{
    for (int i = 0; i < comboBox->count(); i++) {
        QIcon icon = comboBox->itemIcon(i);
        if (adjustIcon(&icon)) {
            comboBox->setItemIcon(i, icon);
        }
    }
}

void updateIcon(QAction *action)
{
    QIcon icon = action->icon();

    if (adjustIcon(&icon)) {
        action->setIcon(icon);
    }
}

void updateIcon(QTabBar *tabBar)
{
    for (int i = 0; i < tabBar->count(); i++) {
        QIcon icon = tabBar->tabIcon(i);
        if (adjustIcon(&icon)) {
            tabBar->setTabIcon(i, icon);
        }
    }
}

QStringList allUniqueLoadedIconNames()
{
    // Take a snapshot of current values to avoid modification during iteration
    const auto valuesCopy = s_icons.values();

    QSet<QString> uniq(valuesCopy.cbegin(), valuesCopy.cend());
    QStringList list = QStringList(uniq.cbegin(), uniq.cend());
    list.sort();

    return list;
}

}
