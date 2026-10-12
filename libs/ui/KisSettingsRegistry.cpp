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
#include <QGuiApplication>
#include <QPixmapCache>
#include <QPointer>
#include <QScreen>
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
#include "kis_canvas2.h"
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

/// What the settings that aren't plain from their names do: shown on hover
QHash<QString, QString> explanations()
{
    return {
        {QStringLiteral("StudioGlassRefraction"), i18nc("@info:tooltip", "How much a panel's edge bends the picture under it, as the rim of a lens does.")},
        {QStringLiteral("kritadisplayrc EnableHiDPIFractionalScaling"),
         i18nc("@info:tooltip",
               "At a screen scale like 125% or 150%, the interface is drawn at exactly that size. Off, it is rounded to 100% or 200%: "
               "crisper lines, but a little too small or too large.")},
        {QStringLiteral("newOutlineStyle"), i18nc("@info:tooltip", "The outline shows where the brush paints: a circle of its size, or its actual shape.")},
        {QStringLiteral("newCursorStyle"), i18nc("@info:tooltip", "A mark at the pen's tip, besides the outline.")},
        {QStringLiteral("colorSamplerPreviewCircleExtraCirclesEnabled"), i18nc("@info:tooltip", "The ring shows the color you had before next to the one under the pointer.")},
        {QStringLiteral("pixelGridDrawingThreshold"), i18nc("@info:tooltip", "Lines between the pixels appear from this zoom on.")},
        {QStringLiteral("showSingleChannelAsColor"), i18nc("@info:tooltip", "When the Channels panel shows one channel alone, it is shown in its color instead of gray.")},
        {QStringLiteral("zoomSteps"), i18nc("@info:tooltip", "How many zoom steps it takes to double the size.")},
        {QStringLiteral("OpenGLFilterMode"),
         i18nc("@info:tooltip",
               "How the image is scaled on screen when zoomed. Nearest: sharp square pixels, best for pixel art. Bilinear: smooth, "
               "fastest. Trilinear: smooth, and calmer when zoomed out. High quality: the sharpest smooth result.")},
        {QStringLiteral("kritadisplayrc useWin8PointerInput"),
         i18nc("@info:tooltip",
               "How Krita reads the pen. On: Windows Ink, built into Windows. Off: WinTab, the tablet maker's driver. If pressure "
               "or tilt misbehave, try the other.")},
        {QStringLiteral("useRightMiddleTabletButtonWorkaround"), i18nc("@info:tooltip", "For tablets whose pen buttons do nothing in Krita: their clicks are read as mouse clicks.")},
        {QStringLiteral("tiltDirectionOffset"), i18nc("@info:tooltip", "Turns the direction the pen's tilt is read in, e.g. for a tablet used rotated or left-handed.")},
        {QStringLiteral("touchPainting"), i18nc("@info:tooltip", "Auto: a finger paints only when there is no pen. On: a finger always paints. Off: a finger only moves and zooms the canvas.")},
        {QStringLiteral("longPressEnabled"), i18nc("@info:tooltip", "Holding a finger still opens the context menu, as a right-click does.")},
        {QStringLiteral("KineticScrollingEnabled"), i18nc("@info:tooltip", "Lists keep scrolling after a flick, slowing down.")},
        {QStringLiteral("switchSelectionCtrlAlt"), i18nc("@info:tooltip", "Ctrl and Alt change how a new selection combines with the old one (add or subtract): this swaps which does which.")},
        {QStringLiteral("maximumBrushSize"), i18nc("@info:tooltip", "The largest size the brush size slider reaches. Very large brushes paint slowly.")},
        {QStringLiteral("[filterdialog] forceLodMode"), i18nc("@info:tooltip", "While you adjust an effect on a large image, a quick lower-resolution preview; the full result follows.")},
        {QStringLiteral("[KisToolTransform] forceLodMode"), i18nc("@info:tooltip", "While you transform on a large image, a quick lower-resolution preview; the full result follows.")},
        {QStringLiteral("[KritaTransform/KisToolMove] forceLodMode"), i18nc("@info:tooltip", "While you move on a large image, a quick lower-resolution preview; the full result follows.")},
        {QStringLiteral("useCumulativeUndoRedo"), i18nc("@info:tooltip", "Older quick strokes are merged into one undo step each, so the undo history reaches further back.")},
        {QStringLiteral("StudioLayersRememberBrushes"), i18nc("@info:tooltip", "Choosing a layer picks the brush last used on it.")},
        {QStringLiteral("ConvertToImageColorSpaceOnImport"), i18nc("@info:tooltip", "Layers imported from files are converted to the image's color space instead of keeping their own.")},
        {QStringLiteral("compressLayersInKra"), i18nc("@info:tooltip", "Layers are compressed harder in .kra files: smaller files that take longer to save and open.")},
        {QStringLiteral("UseZip64"), i18nc("@info:tooltip", "Needed to save .kra files over 4 GB. Krita before 4.2 can't open them.")},
        {QStringLiteral("TrimKra"), i18nc("@info:tooltip", "Paint outside the canvas is cut off when saving: smaller files, but what was moved off the canvas is lost.")},
        {QStringLiteral("pasteFormat"), i18nc("@info:tooltip", "An image pasted from a browser can be the original file (downloaded again), the copy the browser kept, or the picture as pasted.")},
        {QStringLiteral("pasteBehaviour"), i18nc("@info:tooltip", "Images pasted from other apps usually say nothing of their colors: they are taken as sRGB, as your screen's colors, or you are asked.")},
        {QStringLiteral("useDefaultColorSpace"), i18nc("@info:tooltip", "New images start in the color model chosen here, not in the one used last.")},
        {QStringLiteral("renderIntent"),
         i18nc("@info:tooltip",
               "How colors the screen can't show are brought into what it can.\n"
               "Perceptual: all colors move together, keeping how they relate; smooth, a little less vivid.\n"
               "Relative: colors the screen can show stay exact; the rest move to the nearest it can.\n"
               "Saturation: keeps colors vivid; hues may shift. For charts more than pictures.\n"
               "Absolute: as Relative, but white stays the image's white (for proofing paper).")},
        {QStringLiteral("useBlackPointCompensation"),
         i18nc("@info:tooltip",
               "The image's darkest black is shown as the screen's darkest black: shadows keep their detail instead of being crushed "
               "or looking gray.")},
        {QStringLiteral("allowLCMSOptimization"),
         i18nc("@info:tooltip",
               "Converts colors a little faster and a little less exactly. Turn it off for linear light and wide color spaces, where "
               "it can show as banding.")},
        {QStringLiteral("colorsettings/forcepalettecolors"), i18nc("@info:tooltip", "Picked colors snap to the nearest color of the palette in use.")},
        {QStringLiteral("ExrDefaultColorProfile"), i18nc("@info:tooltip", "EXR files usually say nothing of their colors: the profile they are opened in.")},
        {QStringLiteral("kritadisplayrc OpenGLRenderer"),
         i18nc("@info:tooltip",
               "What draws the canvas on the graphics card. Auto picks the best for this computer; change it only if the canvas is "
               "slow, flickers or shows wrong colors.")},
        {QStringLiteral("memoryHardLimitPercent"), i18nc("@info:tooltip", "The share of this computer's memory images may use. Past it, Krita works from the swap file on disk.")},
        {QStringLiteral("memorySoftLimitPercent"), i18nc("@info:tooltip", "Past this share of memory, older undo steps move to disk.")},
        {QStringLiteral("maxSwapSize"), i18nc("@info:tooltip", "The most disk space Krita takes when images don't fit in memory.")},
        {QStringLiteral("swaplocation"), i18nc("@info:tooltip", "Where on disk Krita keeps what doesn't fit in memory. A fast drive is best.")},
        {QStringLiteral("maxNumberOfThreads"), i18nc("@info:tooltip", "How many processor threads Krita paints and runs effects with.")},
        {QStringLiteral("frameRenderingClones"), i18nc("@info:tooltip", "Animation frames rendered at once: more is faster, and takes more memory.")},
        {QStringLiteral("detectFpsLimit"), i18nc("@info:tooltip", "The canvas redraws as often as the screen refreshes.")},
        {QStringLiteral("calculateAnimationCacheInBackground"), i18nc("@info:tooltip", "Frames are made ready for playback while you work.")},
        {QStringLiteral("useOnDiskAnimationCacheSwapping"), i18nc("@info:tooltip", "Cached frames that don't fit in memory go to disk instead of being dropped.")},
        {QStringLiteral("disableAVXOptimizations"), i18nc("@info:tooltip", "Only if Krita crashes when painting: paints without this processor's AVX instructions, slower.")},
        {QStringLiteral("amdDisableVectorWorkaround"), i18nc("@info:tooltip", "Only if Krita crashes when painting: paints without vector instructions, much slower.")},
    };
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
        Setting refraction = number(QStringLiteral("StudioGlassRefraction"), 0.5, 0, 1, i18nc("setting", "Glass refraction"), R::Fork);
        refraction.read = []() { return KisStudioGlass::refraction(); };
        refraction.write = [](const QVariant &value) { KisStudioGlass::setRefraction(value.toReal()); };
        all << refraction;

        Setting strokes = toggle(QStringLiteral("PresetStrokeThumbnails"), false, i18nc("setting", "Brushes as strokes"), R::Fork);
        strokes.read = []() { return KisPresetStrokeThumbnails::isEnabled(); };
        strokes.write = [](const QVariant &value) { KisPresetStrokeThumbnails::setEnabled(value.toBool()); };
        all << strokes;

        all << color(QStringLiteral("canvasBorderColor"), QColor(0, 0, 0), i18nc("setting", "Around the image"));
        Setting ownFont = toggle(QStringLiteral("use_custom_system_font"), false, i18nc("setting", "Own interface font"));
        // on: the font used now, to change from; off: the system's again, nothing left set
        ownFont.written = [](const QVariant &value) {
            KisSettingsRegistry *r = KisSettingsRegistry::instance();
            if (!value.toBool()) {
                r->reset(QStringLiteral("custom_system_font"));
                r->reset(QStringLiteral("custom_font_size"));
                return;
            }
            if (r->value(QStringLiteral("custom_system_font")).toString().isEmpty()) {
                r->setValue(QStringLiteral("custom_system_font"), qApp->font().family());
            }
            if (r->value(QStringLiteral("custom_font_size")).toInt() < 7) {
                r->setValue(QStringLiteral("custom_font_size"), qBound(7, qApp->font().pointSize(), 30));
            }
        };
        all << ownFont;
        all << text(QStringLiteral("custom_system_font"), QLatin1String(""), i18nc("setting", "Interface font"), R::Machine);
        all << number(QStringLiteral("custom_font_size"), -1, -1, 72, i18nc("setting", "Interface font size"));
        all << toggle(QStringLiteral("showOnCanvasMessages"), true, i18nc("setting", "Messages on the canvas"), R::NewViews);
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
        all << color(QStringLiteral("eraserCursorMaincColor"), QColor(128, 255, 128), i18nc("setting", "Eraser outline color"));

        Setting sampler = choice(QStringLiteral("colorSamplerPreviewStyle"), 1,
                                 {{0, i18nc("color sampler preview", "None")}, {1, i18nc("color sampler preview", "Ring")},
                                  {2, i18nc("color sampler preview", "Left")}, {3, i18nc("color sampler preview", "Right")},
                                  {4, i18nc("color sampler preview", "Above")}},
                                 i18nc("setting", "Color sampler preview"));
        sampler.written = [](const QVariant &) { KisConfigNotifier::instance()->notifyColorSamplerPreviewStyleChanged(); };
        all << sampler;
        all << number(QStringLiteral("colorSamplerPreviewCircleDiameter"), 180, 1, 400, i18nc("setting", "Sampler ring size"));
        all << toggle(QStringLiteral("colorSamplerPreviewCircleExtraCirclesEnabled"), true, i18nc("setting", "Sampler ring comparison"));
        all << toggle(QStringLiteral("colorSamplerZoomPreviewEnabled"), false, i18nc("setting", "Sampler magnifier"));
        all << number(QStringLiteral("colorSamplerZoomPreviewScale"), 10.0, 1, 50, i18nc("setting", "Sampler magnification"));

        all << color(QStringLiteral("checkerscolor"), QColor(220, 220, 220), i18nc("setting", "Checkerboard squares"));
        all << color(QStringLiteral("checkerscolor2"), QColor(255, 255, 255), i18nc("setting", "Checkerboard other squares"));
        all << number(QStringLiteral("checksize"), 32, 1, 256, i18nc("setting", "Checkerboard size"));
        all << color(QStringLiteral("pixelGridColor"), QColor(255, 255, 255), i18nc("setting", "Pixel grid color"));
        Setting grid = number(QStringLiteral("pixelGridDrawingThreshold"), 24.0, 0, 99.99, i18nc("setting", "Pixel grid from"));
        grid.format = [](const QVariant &value) { return i18nc("zoom", "%1%", qRound(value.toDouble() * 100)); };
        all << grid;
        all << number(QStringLiteral("selectionOutlineOpacity"), 1.0, 0, 1, i18nc("setting", "Selection outline opacity"));
        Setting mask = color(QStringLiteral("selectionOverlayMaskColor"), QColor(255, 0, 0, 128), i18nc("setting", "Selection mask color"));
        mask.alpha = true;
        all << mask;
        all << toggle(QStringLiteral("selectionActionBar"), true, i18nc("setting", "Selection actions bar"));
        Setting channels = toggle(QStringLiteral("showSingleChannelAsColor"), false, i18nc("setting", "Channels in color"));
        // read as the canvas is drawn: what is drawn already is drawn again
        channels.written = [](const QVariant &) {
            Q_FOREACH (QPointer<KisView> view, KisPart::instance()->views()) {
                if (view && view->canvasBase()) view->canvasBase()->refetchDataFromImage();
            }
        };
        all << channels;
        all << number(QStringLiteral("zoomSteps"), 2, 1, 32, i18nc("setting", "Zoom steps"), R::NewViews);
        all << toggle(QStringLiteral("zoomHorizontal"), false, i18nc("setting", "Zoom by dragging sideways"));
        all << choice(QStringLiteral("OpenGLFilterMode"), 3,
                      {{0, i18nc("canvas scaling", "Nearest")}, {1, i18nc("canvas scaling", "Bilinear")},
                       {2, i18nc("canvas scaling", "Trilinear")}, {3, i18nc("canvas scaling", "High quality")}},
                      i18nc("setting", "Scaling"), R::Machine);
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
        all << inDisplayrc(toggle(QStringLiteral("useWin8PointerInput"), false, i18nc("setting", "Windows Ink"), R::Machine | R::Restart));
#endif
        all << number(QStringLiteral("tiltDirectionOffset"), 0, -180, 180, i18nc("setting", "Tilt direction offset"));
        all << toggle(QStringLiteral("useRightMiddleTabletButtonWorkaround"), false, i18nc("setting", "Pen buttons as mouse clicks"), R::Restart);
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
    }

    // tools, layers, undo
    {
        all << toggle(QStringLiteral("switchSelectionCtrlAlt"), false, i18nc("setting", "Swap Ctrl and Alt for selections"));
        all << toggle(QStringLiteral("activateTransformToolAfterPaste"), false, i18nc("setting", "Transform after pasting"));
        all << number(QStringLiteral("maximumBrushSize"), 1000, 100, 10000, i18nc("setting", "Largest brush"), R::Restart);
        all << grouped(toggle(QStringLiteral("forceLodMode"), true, i18nc("setting", "Instant preview in effects")), QStringLiteral("filterdialog"));
        all << grouped(toggle(QStringLiteral("forceLodMode"), true, i18nc("setting", "Instant preview in Transform")), QStringLiteral("KisToolTransform"));
        all << grouped(toggle(QStringLiteral("forceLodMode"), false, i18nc("setting", "Instant preview in Move")), QStringLiteral("KritaTransform/KisToolMove"));
        all << number(QStringLiteral("undoStackLimit"), 200, 0, 1000, i18nc("setting", "Undo steps"));
        all << toggle(QStringLiteral("useCumulativeUndoRedo"), false, i18nc("setting", "Merge strokes in undo"));
        all << toggle(QStringLiteral("ConvertToImageColorSpaceOnImport"), false, i18nc("setting", "Imported layers take the image's colors"));
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
        all << toggle(QStringLiteral("CreateBackupFile"), true, i18nc("setting", "Backup when saving"));
        all << choice(QStringLiteral("backupfilelocation"), 0,
                      {{0, i18nc("backup location", "Beside the file")}, {1, i18nc("backup location", "Home folder")},
                       {2, i18nc("backup location", "Temporary folder")}},
                      i18nc("setting", "Backups go"));
        all << number(QStringLiteral("numberofbackupfiles"), 1, 1, 99, i18nc("setting", "Backups kept"));
        all << toggle(QStringLiteral("compressLayersInKra"), false, i18nc("setting", "Smaller .kra files"));
        all << toggle(QStringLiteral("UseZip64"), false, i18nc("setting", "Files over 4 GB"));
        all << toggle(QStringLiteral("TrimKra"), false, i18nc("setting", "Trim layers when saving"));
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
        all << number(QStringLiteral("memoryHardLimitPercent"), 50.0, 1, 100, i18nc("setting", "Memory for images"), R::Machine | R::Restart);
        all << number(QStringLiteral("memorySoftLimitPercent"), 2.0, 0, 50, i18nc("setting", "Undo goes to disk after"), R::Machine | R::Restart);
        all << number(QStringLiteral("maxSwapSize"), 4096, 1024, 65536, i18nc("setting", "Swap file limit"), R::Machine | R::Restart);
        Setting swap = text(QStringLiteral("swaplocation"), KisImageConfig(true).swapDir(true), i18nc("setting", "Swap folder"), R::Machine | R::Restart);
        swap.valid = writableFolder;
        all << swap;
        const int threads = QThread::idealThreadCount();
        all << number(QStringLiteral("maxNumberOfThreads"), threads, 1, threads, i18nc("setting", "Processor threads"), R::Machine);
        // as Krita takes it while unset: half the threads it may use
        auto halfTheThreads = []() { return QVariant(qMax(1, KisImageConfig(true).maxNumberOfThreads() / 2)); };
        Setting clones = number(QStringLiteral("frameRenderingClones"), halfTheThreads(), 1, threads, i18nc("setting", "Frames rendered at once"), R::Machine);
        clones.fallbackNow = halfTheThreads;
        all << clones;
        all << toggle(QStringLiteral("detectFpsLimit"), true, i18nc("setting", "Match the screen's frame rate"), R::NewViews);
        all << number(QStringLiteral("fpsLimit"), 100, 1, 1000, i18nc("setting", "Frame rate limit"), R::NewViews);
        all << toggle(QStringLiteral("useOnDiskAnimationCacheSwapping"), true, i18nc("setting", "Animation cache on disk"));
        all << toggle(QStringLiteral("calculateAnimationCacheInBackground"), true, i18nc("setting", "Cache in the background"));
#ifdef Q_OS_WIN
        all << toggle(QStringLiteral("disableAVXOptimizations"), false, i18nc("setting", "No AVX"), R::Machine | R::Restart);
#endif
        all << toggle(QStringLiteral("amdDisableVectorWorkaround"), false, i18nc("setting", "No vector instructions"), R::Machine | R::Restart);
    }
    const QHash<QString, QString> help = explanations();
    for (Setting &s : all) s.help = help.value(s.id);
    return all;
}

/**
 * Settings there are no more of: Krita's defaults always. Once, what was set of them is let go, so that none
 * is left changed where it can no longer be seen.
 */
void forgetRetired()
{
    constexpr int version = 1;
    KConfigGroup top = KSharedConfig::openConfig()->group(QString());
    if (top.readEntry("StudioRetiredSettings", 0) >= version) return;
    const char *const keys[] = {
        "StudioGlassStyle", "iconsInMenu", "forceAlwaysFullSizedOutline", "forceAlwaysFullSizedEraserOutline",
        "colorSamplerPreviewCircleThickness", "colorSamplerPreviewCircleOutlineEnabled", "colorSamplerPreviewCirclePosition",
        "scrollingcheckers", "AntialiasSelectionOutline", "antialiascurves", "hidePopups", "zoomMarginSize", "hideMenuFullScreen",
        "hideToolbarFullscreen", "hideDockersFullScreen", "hideScrollbarsFullScreen", "hideTitleBarFullscreen", "hideStatusbarFullScreen",
        "useTimestampsForBrushSpeed", "maxAllowedSpeedValue", "speedValueSmoothing", "ignoreHighFunctionKeys", "KineticScrollingGesture",
        "KineticScrollingSensitivity", "KineticScrollingHideScrollbar", "useCreamyAlphaDarken", "useSubtractiveBlendingForCmykColorSpaces",
        "cumulativeUndoMergeTimeout", "cumulativeUndoExcludeFromMerge", "cumulativeUndoMaxGroupSeparation", "cumulativeUndoMaxGroupDuration",
        "renameMergedLayers", "renamePastedLayers", "renameDuplicatedLayers", "ShowRootLayer", "autosavefileshidden", "backupfilesuffix",
        "TrimFramesImport", "useOpenGLTextureBuffer", "assistantsDrawMode", "frameRenderingTimeout", "useAnimationCacheFrameSizeLimit",
        "animationCacheFrameSizeLimit", "useAnimationCacheRegionOfInterest", "animationCacheRegionOfInterestMargin",
        "enableOpenGLFramerateLogging", "enableBrushSpeedLogging", "enableProgressReporting", "enablePerfLog",
    };
    for (const char *key : keys) top.deleteEntry(key);
    KSharedConfig::openConfig()->group(QStringLiteral("KisToolTransform")).deleteEntry("useOverlayPreviewStyle");
    QSettings display(displayrcPath(), QSettings::IniFormat);
    display.remove(QStringLiteral("EnableHiDPI"));
    display.remove(QStringLiteral("LogUsage"));
    top.writeEntry("StudioRetiredSettings", version);
}

QVariant fallbackOf(const Setting &s)
{
    return s.fallbackNow ? s.fallbackNow() : s.fallback;
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
    forgetRetired();
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
    d->notify.setInterval(60);
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
        raw = group.hasKey(s->key) ? QVariant(group.readEntry(s->key, QString())) : fallbackOf(*s);
    }
    // a value Krita can't have written (an old version's, a hand edit): as Krita reads it, its default
    const QVariant value = normalized(*s, raw);
    return value.isValid() ? value : fallbackOf(*s);
}

QVariant KisSettingsRegistry::defaultValue(const QString &id) const
{
    const Setting *s = setting(id);
    return s ? fallbackOf(*s) : QVariant();
}

bool KisSettingsRegistry::isDefault(const QString &id) const
{
    const Setting *s = setting(id);
    return !s || sameValue(*s, value(id), fallbackOf(*s));
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
        if (s->fallbackNow && sameValue(*s, v, s->fallbackNow())) {
            KSharedConfig::openConfig()->group(s->group).deleteEntry(s->key); // to keep following its default
        } else {
            writeIn(*s, *KSharedConfig::openConfig(), v);
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
    if (s) setValue(id, fallbackOf(*s));
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
