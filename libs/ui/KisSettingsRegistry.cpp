/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisSettingsRegistry.h"

#include <QApplication>
#include <QColor>
#include <QDir>
#include <QFileInfo>
#include <QLocale>
#include <QPointer>
#include <QSettings>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>

#include <KConfig>
#include <KConfigGroup>
#include <KSharedConfig>
#include <klocalizedstring.h>

#include <KoColorSpaceRegistry.h>
#include <KoColorProfile.h>
#include <KoColorModelStandardIds.h>
#include <KoResourcePaths.h>

#include "KisImportExportManager.h"
#include "KisMainWindow.h"
#include "KisMimeDatabase.h"
#include "KisPart.h"
#include "KisStudioGlass.h"
#include "KisView.h"
#include "KisViewManager.h"
#include "KisImageConfigNotifier.h"
#include "kis_config.h"
#include "kis_config_notifier.h"
#include "kis_cubic_curve.h"
#include "kis_image.h"
#include "kis_group_layer.h"
#include "kis_image_config.h"
#include "opengl/kis_opengl.h"
#include "widgets/KisPresetStrokeThumbnails.h"

#include "config-high-dpi-scale-factor-rounding-policy.h"

#include <cmath>

namespace {

using Setting = KisSettingsRegistry::Setting;
using Kind = KisSettingsRegistry::Kind;
using Choice = KisSettingsRegistry::Choice;

Setting make(Kind kind, const QString &key, const QVariant &fallback, const QString &label, int flags)
{
    Setting s;
    s.id = key;
    s.key = key;
    s.kind = kind;
    s.fallback = fallback;
    s.label = label;
    s.flags = flags;
    return s;
}

Setting toggle(const QString &key, bool fallback, const QString &label, int flags = 0)
{
    return make(Kind::Toggle, key, fallback, label, flags);
}

Setting number(const QString &key, const QVariant &fallback, qreal min, qreal max, const QString &label, int flags = 0)
{
    Setting s = make(Kind::Number, key, fallback, label, flags);
    s.min = min;
    s.max = max;
    return s;
}

Setting choice(const QString &key, const QVariant &fallback, const QVector<Choice> &choices, const QString &label, int flags = 0)
{
    Setting s = make(Kind::Choice, key, fallback, label, flags);
    s.choices = choices;
    return s;
}

Setting color(const QString &key, const QColor &fallback, const QString &label, int flags = 0)
{
    return make(Kind::Color, key, fallback, label, flags);
}

Setting text(const QString &key, const QString &fallback, const QString &label, int flags = 0)
{
    return make(Kind::Text, key, fallback, label, flags);
}

/// In a group of kritarc of its own
Setting grouped(Setting s, const QString &group)
{
    s.group = group;
    s.id = QStringLiteral("[%1] %2").arg(group, s.key);
    return s;
}

QString displayrcPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/kritadisplayrc");
}

/// Kept in kritadisplayrc (read before Krita's own settings are, at the start)
Setting inDisplayrc(Setting s)
{
    const QString key = s.key;
    const QVariant fallback = s.fallback;
    s.id = QStringLiteral("kritadisplayrc ") + key;
    s.read = [key, fallback]() {
        QSettings settings(displayrcPath(), QSettings::IniFormat);
        return settings.value(key, fallback);
    };
    s.write = [key](const QVariant &value) {
        QSettings settings(displayrcPath(), QSettings::IniFormat);
        settings.setValue(key, value);
    };
    return s;
}

bool writableFolder(const QVariant &value)
{
    const QString path = value.toString();
    if (path.isEmpty()) return false;
    const QFileInfo info(path);
    return info.isAbsolute() && info.isDir() && info.isWritable();
}

QVector<Choice> exportTypes()
{
    QVector<Choice> types;
    types << Choice{QStringLiteral("all/mime"), i18nc("default export file type", "All supported")};
    QMap<QString, QString> byDescription;
    Q_FOREACH (const QString &mime, KisImportExportManager::supportedMimeTypes(KisImportExportManager::Export)) {
        byDescription.insert(KisMimeDatabase::descriptionForMimeType(mime), mime);
    }
    QStringList descriptions = byDescription.keys();
    descriptions.sort(Qt::CaseInsensitive);
    Q_FOREACH (const QString &description, descriptions) {
        types << Choice{byDescription.value(description), description};
    }
    return types;
}

QVector<Choice> colorSpaces()
{
    QVector<Choice> spaces;
    KoColorSpaceRegistry *registry = KoColorSpaceRegistry::instance();
    Q_FOREACH (const KoID &id, registry->listKeys()) {
        if (registry->colorSpaceColorModelId(id.id()) == AlphaColorModelID) continue;
        spaces << Choice{id.id(), id.name()};
    }
    return spaces;
}

QVector<Choice> rgbProfiles()
{
    QVector<Choice> profiles;
    QStringList names;
    Q_FOREACH (const KoColorProfile *profile, KoColorSpaceRegistry::instance()->profilesFor(QStringLiteral("RGBAF16"))) {
        if (profile) names << profile->name();
    }
    names.removeDuplicates();
    names.sort(Qt::CaseInsensitive);
    Q_FOREACH (const QString &name, names) profiles << Choice{name, name};
    return profiles;
}

QVector<Choice> renderers()
{
    QVector<Choice> renderers;
    const KisOpenGL::OpenGLRenderers supported = KisOpenGL::getSupportedOpenGLRenderers();
    auto add = [&](KisOpenGL::OpenGLRenderer renderer, const QString &text) {
        renderers << Choice{KisOpenGL::convertOpenGLRendererToConfig(renderer), text};
    };
    add(KisOpenGL::RendererAuto, i18nc("canvas renderer", "Auto"));
    if (supported & KisOpenGL::RendererDesktopGL) add(KisOpenGL::RendererDesktopGL, i18nc("canvas renderer", "OpenGL"));
#ifdef Q_OS_WIN
    if (supported & KisOpenGL::RendererOpenGLES) add(KisOpenGL::RendererOpenGLES, i18nc("canvas renderer", "Direct3D (ANGLE)"));
#else
    if (supported & KisOpenGL::RendererOpenGLES) add(KisOpenGL::RendererOpenGLES, i18nc("canvas renderer", "OpenGL ES"));
#endif
    if (supported & KisOpenGL::RendererSoftware) add(KisOpenGL::RendererSoftware, i18nc("canvas renderer", "Software (slow)"));
    add(KisOpenGL::RendererNone, i18nc("canvas renderer: no acceleration", "Off"));
    return renderers;
}

QVector<Choice> themes()
{
    QVector<Choice> themes;
    QStringList files = KoResourcePaths::findAllAssets("data", QStringLiteral("color-schemes/*.colors"));
    files += KoResourcePaths::findAllAssets("genericdata", QStringLiteral("color-schemes/*.colors"));
    QStringList names;
    Q_FOREACH (const QString &file, files) {
        KConfig scheme(file, KConfig::SimpleConfig);
        names << scheme.group("General").readEntry("Name", QFileInfo(file).completeBaseName());
    }
    names.removeDuplicates();
    names.sort(Qt::CaseInsensitive);
    Q_FOREACH (const QString &name, names) themes << Choice{name, name};
    return themes;
}

QVector<Setting> describeAll()
{
    using R = KisSettingsRegistry;
    QVector<Setting> all;

    // the window, its looks
    {
        Setting theme = grouped(choice(QStringLiteral("Theme"), QStringLiteral("Krita Studio Dark"), {}, i18nc("setting", "Theme"), R::Fork), QStringLiteral("theme"));
        theme.dynamicChoices = themes;
        all << theme;

        Setting glass = toggle(QStringLiteral("StudioGlass"), true, i18nc("setting", "Glass"), R::Fork);
        glass.read = []() { return KisStudioGlass::isEnabled(); };
        glass.write = [](const QVariant &value) { KisStudioGlass::setEnabled(value.toBool()); };
        all << glass;
        Setting style = choice(QStringLiteral("StudioGlassStyle"), 0,
                               {{0, i18nc("glass style", "Frosted")}, {1, i18nc("glass style", "Liquid")}},
                               i18nc("setting", "Glass style"), R::Fork);
        style.read = []() { return int(KisStudioGlass::style()); };
        style.write = [](const QVariant &value) { KisStudioGlass::setStyle(KisStudioGlass::Style(value.toInt())); };
        all << style;
        Setting refraction = number(QStringLiteral("StudioGlassRefraction"), 0.5, 0, 1, i18nc("setting", "Glass refraction"), R::Fork);
        refraction.read = []() { return KisStudioGlass::refraction(); };
        refraction.write = [](const QVariant &value) { KisStudioGlass::setRefraction(value.toReal()); };
        all << refraction;

        Setting strokes = toggle(QStringLiteral("PresetStrokeThumbnails"), false, i18nc("setting", "Brushes as strokes"), R::Fork);
        strokes.read = []() { return KisPresetStrokeThumbnails::isEnabled(); };
        strokes.write = [](const QVariant &value) { KisPresetStrokeThumbnails::setEnabled(value.toBool()); };
        all << strokes;

        all << color(QStringLiteral("canvasBorderColor"), QColor(0, 0, 0), i18nc("setting", "Around the image"));
        all << toggle(QStringLiteral("use_custom_system_font"), false, i18nc("setting", "Own interface font"));
        Setting family = text(QStringLiteral("custom_system_font"), QLatin1String(""), i18nc("setting", "Interface font"), R::Machine);
        all << family;
        all << number(QStringLiteral("custom_font_size"), -1, -1, 72, i18nc("setting", "Interface font size"));
        all << toggle(QStringLiteral("showOnCanvasMessages"), true, i18nc("setting", "Messages on the canvas"), R::NewViews);
        all << choice(QStringLiteral("iconsInMenu"), 0,
                      {{0, i18nc("icons in menus", "Default")}, {1, i18nc("icons in menus", "Shown")}, {2, i18nc("icons in menus", "Hidden")}},
                      i18nc("setting", "Icons in menus"), R::Restart);
        all << inDisplayrc(toggle(QStringLiteral("EnableHiDPI"), true, i18nc("setting", "Sharp on high-DPI screens"), R::Machine | R::Restart));
#if defined(Q_OS_WIN) && defined(HAVE_HIGH_DPI_SCALE_FACTOR_ROUNDING_POLICY)
        all << inDisplayrc(toggle(QStringLiteral("EnableHiDPIFractionalScaling"), false, i18nc("setting", "Fractional scaling"), R::Machine | R::Restart));
#endif
    }

    // the canvas
    {
        all << choice(QStringLiteral("newCursorStyle"), 0,
                      {{0, i18nc("cursor", "None")}, {1, i18nc("cursor", "Tool icon")}, {2, i18nc("cursor", "Arrow")},
                       {3, i18nc("cursor", "Small circle")}, {4, i18nc("cursor", "Crosshair")}, {5, i18nc("cursor", "Triangle, right-handed")},
                       {6, i18nc("cursor", "Triangle, left-handed")}, {7, i18nc("cursor", "Black pixel")}, {8, i18nc("cursor", "White pixel")}},
                      i18nc("setting", "Brush cursor"));
        const QVector<Choice> outlines = {{0, i18nc("brush outline", "None")}, {1, i18nc("brush outline", "Circle")},
                                          {2, i18nc("brush outline", "Brush")}, {3, i18nc("brush outline", "With tilt")}};
        all << choice(QStringLiteral("newOutlineStyle"), 2, outlines, i18nc("setting", "Brush outline"));
        all << toggle(QStringLiteral("ShowOutlineWhilePainting"), true, i18nc("setting", "Outline while painting"));
        all << toggle(QStringLiteral("forceAlwaysFullSizedOutline"), false, i18nc("setting", "Outline at full size"));
        all << color(QStringLiteral("cursorMaincColor"), QColor(255, 0, 255), i18nc("setting", "Outline color"));
        all << toggle(QStringLiteral("separateEraserCursor"), false, i18nc("setting", "Eraser has its own cursor"));
        all << choice(QStringLiteral("eraserCursorStyle"), 9,
                      {{0, i18nc("cursor", "None")}, {1, i18nc("cursor", "Tool icon")}, {2, i18nc("cursor", "Arrow")},
                       {3, i18nc("cursor", "Small circle")}, {4, i18nc("cursor", "Crosshair")}, {5, i18nc("cursor", "Triangle, right-handed")},
                       {6, i18nc("cursor", "Triangle, left-handed")}, {7, i18nc("cursor", "Black pixel")}, {8, i18nc("cursor", "White pixel")},
                       {9, i18nc("cursor", "Eraser")}},
                      i18nc("setting", "Eraser cursor"));
        all << choice(QStringLiteral("eraserOutlineStyle"), 2, outlines, i18nc("setting", "Eraser outline"));
        all << toggle(QStringLiteral("ShowEraserOutlineWhilePainting"), true, i18nc("setting", "Eraser outline while painting"));
        all << toggle(QStringLiteral("forceAlwaysFullSizedEraserOutline"), false, i18nc("setting", "Eraser outline at full size"));
        all << color(QStringLiteral("eraserCursorMaincColor"), QColor(128, 255, 128), i18nc("setting", "Eraser outline color"));

        Setting sampler = choice(QStringLiteral("colorSamplerPreviewStyle"), 1,
                                 {{0, i18nc("color sampler preview", "None")}, {1, i18nc("color sampler preview", "Ring")},
                                  {2, i18nc("color sampler preview", "Left")}, {3, i18nc("color sampler preview", "Right")},
                                  {4, i18nc("color sampler preview", "Above")}},
                                 i18nc("setting", "Color sampler preview"));
        sampler.written = [](const QVariant &) { KisConfigNotifier::instance()->notifyColorSamplerPreviewStyleChanged(); };
        all << sampler;
        all << number(QStringLiteral("colorSamplerPreviewCircleDiameter"), 180, 1, 400, i18nc("setting", "Sampler ring size"));
        all << number(QStringLiteral("colorSamplerPreviewCircleThickness"), 12.0, 1, 50, i18nc("setting", "Sampler ring thickness"));
        all << toggle(QStringLiteral("colorSamplerPreviewCircleOutlineEnabled"), true, i18nc("setting", "Sampler ring outline"));
        all << toggle(QStringLiteral("colorSamplerPreviewCircleExtraCirclesEnabled"), true, i18nc("setting", "Sampler ring comparison"));
        all << toggle(QStringLiteral("colorSamplerZoomPreviewEnabled"), false, i18nc("setting", "Sampler magnifier"));
        all << number(QStringLiteral("colorSamplerZoomPreviewScale"), 10.0, 1, 50, i18nc("setting", "Sampler magnification"));
        all << choice(QStringLiteral("colorSamplerPreviewCirclePosition"), 0,
                      {{0, i18nc("sampler ring position", "Center")}, {1, i18nc("sampler ring position", "Above")},
                       {2, i18nc("sampler ring position", "Above left")}, {3, i18nc("sampler ring position", "Above right")}},
                      i18nc("setting", "Sampler ring position"));

        all << color(QStringLiteral("checkerscolor"), QColor(220, 220, 220), i18nc("setting", "Checkerboard light squares"));
        all << color(QStringLiteral("checkerscolor2"), QColor(255, 255, 255), i18nc("setting", "Checkerboard dark squares"));
        all << number(QStringLiteral("checksize"), 32, 1, 256, i18nc("setting", "Checkerboard size"));
        all << toggle(QStringLiteral("scrollingcheckers"), false, i18nc("setting", "Checkerboard moves with the image"));
        all << color(QStringLiteral("pixelGridColor"), QColor(255, 255, 255), i18nc("setting", "Pixel grid color"));
        Setting grid = number(QStringLiteral("pixelGridDrawingThreshold"), 24.0, 0, 99.99, i18nc("setting", "Pixel grid from"));
        grid.format = [](const QVariant &value) { return i18nc("zoom", "%1%", qRound(value.toDouble() * 100)); };
        all << grid;
        all << number(QStringLiteral("selectionOutlineOpacity"), 1.0, 0, 1, i18nc("setting", "Selection outline opacity"));
        Setting mask = color(QStringLiteral("selectionOverlayMaskColor"), QColor(255, 0, 0, 128), i18nc("setting", "Selection mask color"));
        mask.alpha = true;
        all << mask;
        all << toggle(QStringLiteral("AntialiasSelectionOutline"), false, i18nc("setting", "Smooth selection outline"));
        all << toggle(QStringLiteral("selectionActionBar"), true, i18nc("setting", "Selection actions bar"));
        all << toggle(QStringLiteral("antialiascurves"), true, i18nc("setting", "Smooth curves"));
        all << toggle(QStringLiteral("hidePopups"), false, i18nc("setting", "No layer thumbnail popup"));
        all << toggle(QStringLiteral("showSingleChannelAsColor"), false, i18nc("setting", "Channels in color"));
        all << number(QStringLiteral("zoomSteps"), 2, 1, 32, i18nc("setting", "Zoom steps"), R::NewViews);
        all << toggle(QStringLiteral("zoomHorizontal"), false, i18nc("setting", "Zoom by dragging sideways"));
        all << number(QStringLiteral("zoomMarginSize"), 0, 0, 1024, i18nc("setting", "Margin when fit"));
        all << choice(QStringLiteral("OpenGLFilterMode"), 3,
                      {{0, i18nc("canvas scaling", "Nearest")}, {1, i18nc("canvas scaling", "Bilinear")},
                       {2, i18nc("canvas scaling", "Trilinear")}, {3, i18nc("canvas scaling", "High quality")}},
                      i18nc("setting", "Scaling"), R::Machine);
        for (const auto &hide : {std::make_pair("hideMenuFullScreen", i18nc("setting", "Canvas only hides the menu")),
                                 std::make_pair("hideToolbarFullscreen", i18nc("setting", "Canvas only hides toolbars")),
                                 std::make_pair("hideDockersFullScreen", i18nc("setting", "Canvas only hides panels")),
                                 std::make_pair("hideScrollbarsFullScreen", i18nc("setting", "Canvas only hides scrollbars")),
                                 std::make_pair("hideTitleBarFullscreen", i18nc("setting", "Canvas only is full screen")),
                                 std::make_pair("hideStatusbarFullScreen", i18nc("setting", "Canvas only hides the status bar"))}) {
            all << toggle(QLatin1String(hide.first), true, hide.second);
        }
    }

    // the pen
    {
        Setting curve = text(QStringLiteral("tabletPressureCurve"), QStringLiteral("0,0;1,1;"), i18nc("setting", "Pressure"));
        curve.format = [](const QVariant &value) {
            return value.toString() == QLatin1String("0,0;1,1;") ? i18nc("pressure curve", "Straight") : i18nc("pressure curve", "Curved");
        };
        curve.valid = [](const QVariant &value) {
            const QString string = value.toString();
            if (string.isEmpty() || string.size() > 4000) return false;
            const QList<KisCubicCurvePoint> points = KisCubicCurve(string).curvePoints();
            if (points.size() < 2) return false;
            qreal last = -1;
            Q_FOREACH (const KisCubicCurvePoint &point, points) {
                const QPointF p = point.position();
                if (!(p.x() >= 0 && p.x() <= 1 && p.y() >= 0 && p.y() <= 1) || p.x() < last) return false;
                last = p.x();
            }
            return true;
        };
        all << curve;
#ifdef Q_OS_WIN
        Setting api = toggle(QStringLiteral("useWin8PointerInput"), false, i18nc("setting", "Windows Ink"), R::Machine | R::Restart);
        all << inDisplayrc(api);
#endif
        all << toggle(QStringLiteral("useTimestampsForBrushSpeed"), false, i18nc("setting", "Driver timestamps for speed"));
        all << number(QStringLiteral("maxAllowedSpeedValue"), 30, 1, 100, i18nc("setting", "Fastest brush speed"));
        all << number(QStringLiteral("speedValueSmoothing"), 3, 3, 100, i18nc("setting", "Speed smoothing"));
        all << number(QStringLiteral("tiltDirectionOffset"), 0, -180, 180, i18nc("setting", "Tilt direction offset"));
        all << toggle(QStringLiteral("useRightMiddleTabletButtonWorkaround"), false, i18nc("setting", "Pen buttons as mouse clicks"), R::Restart);
#ifdef Q_OS_WIN
        all << toggle(QStringLiteral("ignoreHighFunctionKeys"), true, i18nc("setting", "Ignore F13 to F24"));
#endif
        Setting touch = choice(QStringLiteral("touchPainting"), 0,
                               {{0, i18nc("touch painting", "Auto")}, {1, i18nc("touch painting", "On")}, {2, i18nc("touch painting", "Off")}},
                               i18nc("setting", "Painting with a finger"));
        touch.written = [](const QVariant &) { KisConfigNotifier::instance()->notifyTouchPaintingChanged(); };
        all << touch;
        all << toggle(QStringLiteral("useTouchPressureSensitivity"), true, i18nc("setting", "Finger pressure"));
        Setting longPress = toggle(QStringLiteral("longPressEnabled"), KisConfig(true).longPressEnabled(true), i18nc("setting", "Hold for the context menu"));
        longPress.written = [](const QVariant &value) { KisConfigNotifier::instance()->notifyLongPressChanged(value.toBool()); };
        all << longPress;
        all << toggle(QStringLiteral("KineticScrollingEnabled"), true, i18nc("setting", "Kinetic scrolling"), R::Restart);
        all << choice(QStringLiteral("KineticScrollingGesture"), KisConfig(true).kineticScrollingGesture(true),
                      {{0, i18nc("kinetic scrolling gesture", "Finger")}, {1, i18nc("kinetic scrolling gesture", "Drag")},
                       {2, i18nc("kinetic scrolling gesture", "Middle drag")}},
                      i18nc("setting", "Kinetic scrolling with"), R::Restart);
        all << number(QStringLiteral("KineticScrollingSensitivity"), 75, 0, 100, i18nc("setting", "Kinetic scrolling sensitivity"), R::Restart);
        all << toggle(QStringLiteral("KineticScrollingHideScrollbar"), false, i18nc("setting", "Kinetic scrolling hides scrollbars"), R::Restart);
    }

    // tools, layers, undo
    {
        all << toggle(QStringLiteral("switchSelectionCtrlAlt"), false, i18nc("setting", "Swap Ctrl and Alt for selections"));
        all << toggle(QStringLiteral("activateTransformToolAfterPaste"), false, i18nc("setting", "Transform after pasting"));
        all << number(QStringLiteral("maximumBrushSize"), 1000, 100, 10000, i18nc("setting", "Largest brush"), R::Restart);
        all << toggle(QStringLiteral("useCreamyAlphaDarken"), true, i18nc("setting", "Creamy flow"), R::Restart);
        all << toggle(QStringLiteral("useSubtractiveBlendingForCmykColorSpaces"), true, i18nc("setting", "Subtractive CMYK blending"), R::Restart);
        all << grouped(toggle(QStringLiteral("forceLodMode"), true, i18nc("setting", "Instant preview in effects")), QStringLiteral("filterdialog"));
        all << grouped(toggle(QStringLiteral("forceLodMode"), true, i18nc("setting", "Instant preview in Transform")), QStringLiteral("KisToolTransform"));
        all << grouped(toggle(QStringLiteral("useOverlayPreviewStyle"), false, i18nc("setting", "Transform previews over the layers")), QStringLiteral("KisToolTransform"));
        all << grouped(toggle(QStringLiteral("forceLodMode"), false, i18nc("setting", "Instant preview in Move")), QStringLiteral("KritaTransform/KisToolMove"));
        all << number(QStringLiteral("undoStackLimit"), 200, 0, 1000, i18nc("setting", "Undo steps"));
        all << toggle(QStringLiteral("useCumulativeUndoRedo"), false, i18nc("setting", "Merge strokes in undo"));
        all << number(QStringLiteral("cumulativeUndoMergeTimeout"), 5000, 3000, 600000, i18nc("setting", "Merge strokes older than"));
        all << number(QStringLiteral("cumulativeUndoExcludeFromMerge"), 10, 1, 1000, i18nc("setting", "Keep the last strokes apart"));
        all << number(QStringLiteral("cumulativeUndoMaxGroupSeparation"), 1000, 300, 60000, i18nc("setting", "Merge strokes closer than"));
        all << number(QStringLiteral("cumulativeUndoMaxGroupDuration"), 5000, 300, 600000, i18nc("setting", "Longest merged group"));
        all << toggle(QStringLiteral("renameMergedLayers"), true, i18nc("setting", "Name merged groups \"Merged\""));
        all << toggle(QStringLiteral("renamePastedLayers"), true, i18nc("setting", "Name pasted layers \"(pasted)\""));
        all << toggle(QStringLiteral("renameDuplicatedLayers"), true, i18nc("setting", "Name copies \"Copy of\""));
        all << toggle(QStringLiteral("ConvertToImageColorSpaceOnImport"), false, i18nc("setting", "Imported layers take the image's colors"));
        all << toggle(QStringLiteral("ShowRootLayer"), false, i18nc("setting", "Show the root layer"));
        all << toggle(QStringLiteral("StudioLayersRememberBrushes"), true, i18nc("setting", "Layers remember their brush"), R::Fork);
        all << toggle(QStringLiteral("StudioEffectsEditable"), true, i18nc("setting", "Effects stay editable"), R::Fork);
        all << toggle(QStringLiteral("autoPinLayers"), true, i18nc("setting", "Pin new layers to the timeline"));
        all << toggle(QStringLiteral("adaptivePlaybackRange"), true, i18nc("setting", "Playback range follows keyframes"));
        all << toggle(QStringLiteral("autoZoomTimelineToPlaybackRange"), true, i18nc("setting", "Timeline zooms to the playback range"));
    }

    // files
    {
        Setting autosave = number(QStringLiteral("AutoSaveInterval"), 420, 0, 86400, i18nc("setting", "Autosave"));
        autosave.format = [](const QVariant &value) {
            return value.toInt() <= 0 ? i18nc("autosave", "Off") : i18nc("autosave interval", "Every %1 min", qMax(1, value.toInt() / 60));
        };
        all << autosave;
        all << toggle(QStringLiteral("autosavefileshidden"), true, i18nc("setting", "Hidden autosave files"));
        all << toggle(QStringLiteral("CreateBackupFile"), true, i18nc("setting", "Backup when saving"));
        all << choice(QStringLiteral("backupfilelocation"), 0,
                      {{0, i18nc("backup location", "Beside the file")}, {1, i18nc("backup location", "Home folder")},
                       {2, i18nc("backup location", "Temporary folder")}},
                      i18nc("setting", "Backups go"));
        Setting suffix = text(QStringLiteral("backupfilesuffix"), QStringLiteral("~"), i18nc("setting", "Backup suffix"));
        suffix.valid = [](const QVariant &value) {
            const QString s = value.toString();
            if (s.isEmpty() || s.size() > 10) return false;
            for (const QChar c : s) {
                if (c.isDigit() || c == QLatin1Char('/') || c == QLatin1Char('\\') || c == QLatin1Char(':') || c == QLatin1Char(';') || c.isSpace()) return false;
            }
            return true;
        };
        all << suffix;
        all << number(QStringLiteral("numberofbackupfiles"), 1, 1, 99, i18nc("setting", "Backups kept"));
        all << toggle(QStringLiteral("compressLayersInKra"), false, i18nc("setting", "Smaller .kra files"));
        all << toggle(QStringLiteral("UseZip64"), false, i18nc("setting", "Files over 4 GB"));
        all << toggle(QStringLiteral("TrimKra"), false, i18nc("setting", "Trim layers when saving"));
        all << toggle(QStringLiteral("TrimFramesImport"), false, i18nc("setting", "Trim imported frames"));
        // none chosen yet is PNG, as Krita takes it
        Setting exportType = choice(QStringLiteral("defaultExportMimeType"), QStringLiteral("image/png"), {}, i18nc("setting", "Export as"));
        exportType.dynamicChoices = exportTypes;
        all << exportType;
        all << choice(QStringLiteral("pasteFormat"), 0,
                      {{0, i18nc("paste format", "Ask")}, {1, i18nc("paste format", "Download the original")},
                       {2, i18nc("paste format", "A local copy")}, {3, i18nc("paste format", "The pasted image")}},
                      i18nc("setting", "Pasting from other apps"));
        all << choice(QStringLiteral("pasteBehaviour"), 2,
                      {{0, i18nc("pasted colors", "sRGB")}, {1, i18nc("pasted colors", "The screen's")}, {2, i18nc("pasted colors", "Ask")}},
                      i18nc("setting", "Pasted colors are"));
        all << choice(QStringLiteral("sessionOnStartup"), 0,
                      {{0, i18nc("on start", "New window")}, {1, i18nc("on start", "Last session")}, {2, i18nc("on start", "Sessions")}},
                      i18nc("setting", "On start"));
        all << toggle(QStringLiteral("saveSessionOnQuit"), false, i18nc("setting", "Save the session on quit"));
        bool nativeDialogs = false;
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS) || defined(Q_OS_ANDROID)
        nativeDialogs = true;
#else
        nativeDialogs = qgetenv("XDG_CURRENT_DESKTOP") == "KDE";
#endif
        all << grouped(toggle(QStringLiteral("DontUseNativeFileDialog"), !nativeDialogs, i18nc("setting", "Krita's own file dialogs"), R::Machine),
                       QStringLiteral("File Dialogs"));
        Setting resources = text(QStringLiteral("ResourceDirectory"), QStandardPaths::writableLocation(QStandardPaths::AppDataLocation),
                                 i18nc("setting", "Resource folder"), R::Machine | R::Restart);
        resources.valid = writableFolder;
        all << resources;
    }

    // color
    {
        auto newImageColors = [](const QString &space) {
            // what the new image dialog reads
            KoColorSpaceRegistry *registry = KoColorSpaceRegistry::instance();
            KisConfig cfg(false);
            cfg.defColorModel(registry->colorSpaceColorModelId(space).id());
            cfg.setDefaultColorDepth(registry->colorSpaceColorDepthId(space).id());
        };
        Setting fixed = toggle(QStringLiteral("useDefaultColorSpace"), false, i18nc("setting", "Fixed color model for new images"), R::NewViews);
        fixed.written = [newImageColors](const QVariant &value) {
            if (value.toBool()) newImageColors(KisConfig(true).workingColorSpace());
        };
        all << fixed;
        Setting space = choice(QStringLiteral("workingColorSpace"), QStringLiteral("RGBA"), {}, i18nc("setting", "Color model for new images"), R::NewViews);
        space.dynamicChoices = colorSpaces;
        space.written = [newImageColors](const QVariant &value) {
            if (KisConfig(true).useDefaultColorSpace()) newImageColors(value.toString());
        };
        all << space;
        all << choice(QStringLiteral("renderIntent"), 1,
                      {{0, i18nc("rendering intent", "Perceptual")}, {1, i18nc("rendering intent", "Relative")},
                       {2, i18nc("rendering intent", "Saturation")}, {3, i18nc("rendering intent", "Absolute")}},
                      i18nc("setting", "Rendering intent"));
        all << toggle(QStringLiteral("useBlackPointCompensation"), true, i18nc("setting", "Black point compensation"));
        all << toggle(QStringLiteral("allowLCMSOptimization"), true, i18nc("setting", "Faster color conversion"));
        all << toggle(QStringLiteral("colorsettings/forcepalettecolors"), false, i18nc("setting", "Only the palette's colors"));
        Setting exr = choice(QStringLiteral("ExrDefaultColorProfile"), KoColorSpaceRegistry::instance()->defaultProfileForColorSpace(QStringLiteral("RGBAF16")),
                             {}, i18nc("setting", "Profile of EXR files"));
        exr.dynamicChoices = rgbProfiles;
        all << exr;
    }

    // speed
    {
        Setting renderer = choice(QStringLiteral("OpenGLRenderer"), KisOpenGL::convertOpenGLRendererToConfig(KisOpenGL::RendererAuto), {},
                                  i18nc("setting", "Renderer"), R::Machine | R::Restart);
        renderer.dynamicChoices = renderers;
        renderer.id = QStringLiteral("kritadisplayrc OpenGLRenderer");
        renderer.read = []() { return KisOpenGL::convertOpenGLRendererToConfig(KisOpenGL::getUserPreferredOpenGLRendererConfig()); };
        renderer.write = [](const QVariant &value) {
            const KisOpenGL::OpenGLRenderer before = KisOpenGL::getUserPreferredOpenGLRendererConfig();
            const KisOpenGL::OpenGLRenderer after = KisOpenGL::convertConfigToOpenGLRenderer(value.toString());
            // on again: the canvas tries OpenGL once more, even if it failed before
            if (before == KisOpenGL::RendererNone && after != KisOpenGL::RendererNone) KisConfig(false).setCanvasState(QStringLiteral("TRY_OPENGL"));
            KisOpenGL::setUserPreferredOpenGLRendererConfig(after);
        };
        // off works at once (the canvas is drawn without OpenGL); another one at the next start
        renderer.waitsForRestart = [](const QVariant &startup, const QVariant &now) {
            const QString none = KisOpenGL::convertOpenGLRendererToConfig(KisOpenGL::RendererNone);
            return now.toString() != none && now.toString() != startup.toString();
        };
        all << renderer;
        all << toggle(QStringLiteral("useOpenGLTextureBuffer"), true, i18nc("setting", "Texture buffer"), R::Machine);
        all << choice(QStringLiteral("assistantsDrawMode"), 0,
                      {{0, i18nc("assistants drawing", "Direct")}, {1, i18nc("assistants drawing", "Cached")}, {2, i18nc("assistants drawing", "Large cache")}},
                      i18nc("setting", "Assistants are drawn"), R::Machine);
        all << number(QStringLiteral("memoryHardLimitPercent"), 50.0, 1, 100, i18nc("setting", "Memory for images"), R::Machine | R::Restart);
        all << number(QStringLiteral("memorySoftLimitPercent"), 2.0, 0, 50, i18nc("setting", "Undo goes to disk after"), R::Machine | R::Restart);
        all << number(QStringLiteral("maxSwapSize"), 4096, 1024, 65536, i18nc("setting", "Swap file limit"), R::Machine | R::Restart);
        Setting swap = text(QStringLiteral("swaplocation"), KisImageConfig(true).swapDir(true), i18nc("setting", "Swap folder"), R::Machine | R::Restart);
        swap.valid = writableFolder;
        all << swap;
        const int threads = QThread::idealThreadCount();
        all << number(QStringLiteral("maxNumberOfThreads"), threads, 1, threads, i18nc("setting", "Processor threads"), R::Machine);
        // as Krita takes it while unset: half the threads it may use
        all << number(QStringLiteral("frameRenderingClones"), qMax(1, KisImageConfig(true).maxNumberOfThreads() / 2), 1, threads,
                      i18nc("setting", "Frames rendered at once"), R::Machine);
        all << number(QStringLiteral("frameRenderingTimeout"), 30000, 5000, 600000, i18nc("setting", "Frame rendering timeout"));
        all << toggle(QStringLiteral("detectFpsLimit"), true, i18nc("setting", "Frame rate of the screen"), R::NewViews);
        all << number(QStringLiteral("fpsLimit"), 100, 1, 1000, i18nc("setting", "Frame rate limit"), R::NewViews);
        all << toggle(QStringLiteral("useOnDiskAnimationCacheSwapping"), true, i18nc("setting", "Animation cache on disk"));
        all << toggle(QStringLiteral("useAnimationCacheFrameSizeLimit"), true, i18nc("setting", "Smaller cached frames"));
        all << number(QStringLiteral("animationCacheFrameSizeLimit"), 2500, 256, 10000, i18nc("setting", "Cached frame size"));
        all << toggle(QStringLiteral("useAnimationCacheRegionOfInterest"), true, i18nc("setting", "Cache only what's shown"));
        all << number(QStringLiteral("animationCacheRegionOfInterestMargin"), 0.25, 0.01, 1, i18nc("setting", "Cached margin"));
        all << toggle(QStringLiteral("calculateAnimationCacheInBackground"), true, i18nc("setting", "Cache in the background"));
        all << toggle(QStringLiteral("enableOpenGLFramerateLogging"), false, i18nc("setting", "Log the frame rate"));
        all << toggle(QStringLiteral("enableBrushSpeedLogging"), false, i18nc("setting", "Log brush speed"));
        all << toggle(QStringLiteral("enableProgressReporting"), true, i18nc("setting", "Report progress"), R::NewViews);
        all << toggle(QStringLiteral("enablePerfLog"), false, i18nc("setting", "Performance log"), R::Restart);
#ifdef Q_OS_WIN
        all << toggle(QStringLiteral("disableAVXOptimizations"), false, i18nc("setting", "No AVX"), R::Machine | R::Restart);
#endif
        all << toggle(QStringLiteral("amdDisableVectorWorkaround"), false, i18nc("setting", "No vector instructions"), R::Machine | R::Restart);
        all << inDisplayrc(toggle(QStringLiteral("LogUsage"), true, i18nc("setting", "Log for bug reports"), R::Restart));
    }
    return all;
}

} // namespace

struct KisSettingsRegistry::Private
{
    QVector<Setting> settings;
    QHash<QString, int> index;
    QHash<QString, QVariant> startup;
    /// Choices known only at run time (themes, file types, profiles): found once, they don't change while Krita runs
    QHash<QString, QVector<Choice>> foundChoices;
    QTimer notify;
    QTimer sync;
};

KisSettingsRegistry *KisSettingsRegistry::instance()
{
    static QPointer<KisSettingsRegistry> registry;
    if (!registry) {
        registry = new KisSettingsRegistry();
        registry->setParent(qApp);
    }
    return registry;
}

KisSettingsRegistry::KisSettingsRegistry()
    : d(new Private)
{
    d->settings = describeAll();
    for (int i = 0; i < d->settings.size(); i++) {
        d->index.insert(d->settings[i].id, i);
    }
    // what Krita runs with: a change from it to a setting that waits for a restart is pending
    Q_FOREACH (const Setting &s, d->settings) {
        if (s.flags & Restart) d->startup.insert(s.id, value(s.id));
    }

    // Krita is told once for what is written together (a slider dragged), soon enough to follow it live
    d->notify.setSingleShot(true);
    d->notify.setInterval(30);
    connect(&d->notify, &QTimer::timeout, this, &KisSettingsRegistry::notifyNow);
    // and kritarc is written once the changes stop: rewriting it is not free, nor safe to do often
    d->sync.setSingleShot(true);
    d->sync.setInterval(800);
    connect(&d->sync, &QTimer::timeout, this, []() { KSharedConfig::openConfig()->sync(); });
}

const QVector<Setting> &KisSettingsRegistry::settings() const
{
    return d->settings;
}

const Setting *KisSettingsRegistry::setting(const QString &id) const
{
    const auto it = d->index.constFind(id);
    return it == d->index.constEnd() ? nullptr : &d->settings[*it];
}

QVector<Choice> KisSettingsRegistry::choices(const Setting &setting) const
{
    if (!setting.dynamicChoices) return setting.choices;
    auto it = d->foundChoices.find(setting.id);
    if (it == d->foundChoices.end()) it = d->foundChoices.insert(setting.id, setting.dynamicChoices());
    return *it;
}

QString KisSettingsRegistry::display(const Setting &setting, const QVariant &value) const
{
    if (!value.isValid()) return i18nc("a setting's value: not set", "Default");
    if (setting.format) return setting.format(value);
    switch (setting.kind) {
    case Kind::Toggle:
        return value.toBool() ? i18nc("a setting that is on", "On") : i18nc("a setting that is off", "Off");
    case Kind::Number:
        return QLocale().toString(value.toDouble(), 'g', 6);
    case Kind::Choice:
        Q_FOREACH (const Choice &c, choices(setting)) {
            if (c.value == value) return c.text;
        }
        return value.toString();
    case Kind::Color:
        return value.value<QColor>().name(value.value<QColor>().alpha() < 255 ? QColor::HexArgb : QColor::HexRgb);
    case Kind::Text:
        return value.toString();
    }
    return QString();
}

QVariant KisSettingsRegistry::normalized(const Setting &setting, const QVariant &value) const
{
    if (!value.isValid()) return QVariant();
    // an empty text is one (a Qt 5 QString() is "null"), nothing else is
    if (value.isNull() && setting.kind != Kind::Text) return QVariant();
    QVariant result;
    switch (setting.kind) {
    case Kind::Toggle: {
        if (value.userType() == QMetaType::Bool) {
            result = value.toBool();
        } else {
            const QString s = value.toString().trimmed().toLower();
            if (s == QLatin1String("true") || s == QLatin1String("1") || s == QLatin1String("yes") || s == QLatin1String("on")) result = true;
            else if (s == QLatin1String("false") || s == QLatin1String("0") || s == QLatin1String("no") || s == QLatin1String("off")) result = false;
        }
        break;
    }
    case Kind::Number: {
        bool ok = false;
        const double number = value.userType() == QMetaType::QString ? value.toString().trimmed().toDouble(&ok) : value.toDouble(&ok);
        if (!ok || !std::isfinite(number) || number < setting.min || number > setting.max) break;
        if (setting.fallback.userType() == QMetaType::Int) {
            if (std::floor(number) != number) break;
            result = int(number);
        } else {
            result = number;
        }
        break;
    }
    case Kind::Choice: {
        const bool numeric = setting.fallback.userType() == QMetaType::Int;
        QVariant wanted;
        if (numeric) {
            bool ok = false;
            const int n = value.userType() == QMetaType::QString ? value.toString().trimmed().toInt(&ok) : value.toInt(&ok);
            if (ok && double(n) == value.toDouble()) wanted = n;
        } else if (value.userType() == QMetaType::QString) {
            wanted = value.toString();
        }
        if (!wanted.isValid()) break;
        // its default is valid also when it isn't listed (yet)
        if (wanted == setting.fallback) {
            result = wanted;
            break;
        }
        Q_FOREACH (const Choice &c, choices(setting)) {
            if (c.value == wanted) {
                result = wanted;
                break;
            }
        }
        break;
    }
    case Kind::Color: {
        QColor c;
        if (value.userType() == QMetaType::QColor) {
            c = value.value<QColor>();
        } else {
            const QString s = value.toString().trimmed();
            const QStringList parts = s.split(QLatin1Char(','));
            if (parts.size() == 3 || parts.size() == 4) {
                int channels[4] = {0, 0, 0, 255};
                bool ok = true;
                for (int i = 0; i < parts.size() && ok; i++) {
                    channels[i] = parts[i].trimmed().toInt(&ok);
                    ok = ok && channels[i] >= 0 && channels[i] <= 255;
                }
                if (ok) c = QColor(channels[0], channels[1], channels[2], channels[3]);
            } else if (s.startsWith(QLatin1Char('#'))) {
                c = QColor(s);
            }
        }
        if (c.isValid()) {
            if (!setting.alpha) c.setAlpha(255);
            result = c;
        }
        break;
    }
    case Kind::Text:
        if (value.userType() == QMetaType::QString) result = value.toString().isNull() ? QString(QLatin1String("")) : value.toString();
        break;
    }
    if (result.isValid() && setting.valid && !setting.valid(result)) return QVariant();
    return result;
}

bool KisSettingsRegistry::sameValue(const Setting &setting, const QVariant &a, const QVariant &b) const
{
    if (!a.isValid() || !b.isValid()) return a.isValid() == b.isValid();
    switch (setting.kind) {
    case Kind::Number:
        return std::abs(a.toDouble() - b.toDouble()) <= 1e-9 * qMax(1.0, std::abs(a.toDouble()));
    case Kind::Color:
        return a.value<QColor>().rgba() == b.value<QColor>().rgba();
    default:
        return a == b;
    }
}

QVariant KisSettingsRegistry::value(const QString &id) const
{
    const Setting *s = setting(id);
    if (!s) return QVariant();
    QVariant raw;
    if (s->read) {
        raw = s->read();
    } else {
        KConfigGroup group = KSharedConfig::openConfig()->group(s->group);
        raw = group.hasKey(s->key) ? QVariant(group.readEntry(s->key, QString())) : s->fallback;
    }
    // a value Krita can't have written (an old version's, a hand edit): as Krita reads it, its default
    const QVariant value = normalized(*s, raw);
    return value.isValid() ? value : s->fallback;
}

QVariant KisSettingsRegistry::defaultValue(const QString &id) const
{
    const Setting *s = setting(id);
    return s ? s->fallback : QVariant();
}

bool KisSettingsRegistry::isDefault(const QString &id) const
{
    const Setting *s = setting(id);
    return !s || sameValue(*s, value(id), s->fallback);
}

bool KisSettingsRegistry::setValue(const QString &id, const QVariant &newValue)
{
    const Setting *s = setting(id);
    if (!s) return false;
    const QVariant v = normalized(*s, newValue);
    if (!v.isValid()) return false;
    if (sameValue(*s, v, value(id))) return true;
    if (s->write) {
        s->write(v);
    } else {
        KConfigGroup group = KSharedConfig::openConfig()->group(s->group);
        if (s->kind == Kind::Color) {
            group.writeEntry(s->key, v.value<QColor>());
        } else {
            group.writeEntry(s->key, v);
        }
        d->sync.start();
    }
    if (s->written) s->written(v);
    schedule();
    Q_EMIT valueChanged(id);
    return true;
}

void KisSettingsRegistry::reset(const QString &id)
{
    const Setting *s = setting(id);
    if (s) setValue(id, s->fallback);
}

QHash<QString, QVariant> KisSettingsRegistry::snapshot() const
{
    QHash<QString, QVariant> values;
    Q_FOREACH (const Setting &s, d->settings) values.insert(s.id, value(s.id));
    return values;
}

void KisSettingsRegistry::restore(const QHash<QString, QVariant> &snapshot)
{
    for (auto it = snapshot.constBegin(); it != snapshot.constEnd(); ++it) {
        setValue(it.key(), it.value());
    }
}

QStringList KisSettingsRegistry::waitingForRestart() const
{
    QStringList waiting;
    Q_FOREACH (const Setting &s, d->settings) {
        if (!(s.flags & Restart)) continue;
        const QVariant startup = d->startup.value(s.id);
        const QVariant now = value(s.id);
        const bool waits = s.waitsForRestart ? s.waitsForRestart(startup, now) : !sameValue(s, startup, now);
        if (waits) waiting << s.id;
    }
    return waiting;
}

void KisSettingsRegistry::notifyNow()
{
    KisConfigNotifier::instance()->notifyConfigChanged();
    KisConfigNotifier::instance()->notifyPixelGridModeChanged();
    KisImageConfigNotifier::instance()->notifyConfigChanged();
    // layers keep some settings for compositing, without listening for changes
    Q_FOREACH (QPointer<KisView> view, KisPart::instance()->views()) {
        if (view && view->image()) view->image()->rootLayer()->updateSettings();
    }
    Q_FOREACH (QPointer<KisMainWindow> window, KisPart::instance()->mainWindows()) {
        if (window && window->viewManager()) window->viewManager()->showHideScrollbars();
    }
}

void KisSettingsRegistry::schedule()
{
    if (!d->notify.isActive()) d->notify.start();
}

void KisSettingsRegistry::flush()
{
    if (d->notify.isActive()) {
        d->notify.stop();
        notifyNow();
    }
    if (d->sync.isActive()) {
        d->sync.stop();
        KSharedConfig::openConfig()->sync();
    }
}

QVariant KisSettingsRegistry::valueIn(const Setting &setting, KConfig &config) const
{
    if (!setting.storedInKritarc()) return QVariant();
    const KConfigGroup group = config.group(setting.group);
    if (!group.hasKey(setting.key)) return QVariant();
    return normalized(setting, group.readEntry(setting.key, QString()));
}

void KisSettingsRegistry::writeIn(const Setting &setting, KConfig &config, const QVariant &value) const
{
    if (!setting.storedInKritarc()) return;
    KConfigGroup group = config.group(setting.group);
    if (setting.kind == Kind::Color) {
        group.writeEntry(setting.key, value.value<QColor>());
    } else {
        group.writeEntry(setting.key, value);
    }
}
