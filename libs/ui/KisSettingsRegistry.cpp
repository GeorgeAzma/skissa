/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisSettingsRegistry.h"
#include <KisProfile.h>

#include <QApplication>
#include <QColor>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QLocale>
#include <QGuiApplication>
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
#include "KisPlatformPluginInterfaceFactory.h"
#include "KisStudioGlass.h"
#include "KisStudioTheme.h"
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
#include <config-hdr.h>

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

/// Moves \p s into its own kritarc group; its id becomes "[group] key"
Setting grouped(Setting s, const QString &group)
{
    s.group = group;
    s.id = QStringLiteral("[%1] %2").arg(group, s.key);
    return s;
}

QString displayrcPath()
{
    return KisProfile::configFile(QStringLiteral("kritadisplayrc"));
}

/// Stores \p s in kritadisplayrc, which is read at startup before kritarc
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

/// Tooltips for settings whose names don't fully explain them
QHash<QString, QString> explanations()
{
    return {
        {QStringLiteral("StudioGlass"), i18nc("@info:tooltip", "On: frosted see-through panels\nOff: solid")},
        {QStringLiteral("StudioGlassBlur"), i18nc("@info:tooltip", "0%: clear → 200% frosted")},
        {QStringLiteral("PresetStrokeThumbnails"), i18nc("@info:tooltip", "Brush lists show a sample stroke, not the icon")},
        {QStringLiteral("colorSamplerPreviewStyle"), i18nc("@info:tooltip", "Ring: around the cursor\nLeft, Right, Above: a swatch beside it")},
        {QStringLiteral("colorSamplerZoomPreviewEnabled"), i18nc("@info:tooltip", "The pixels under the cursor, enlarged")},
        {QStringLiteral("pixelGridDrawingThreshold"), i18nc("@info:tooltip", "Lines between pixels, from this zoom up")},
        {QStringLiteral("selectionOverlayMaskColor"), i18nc("@info:tooltip", "The tint over what's not selected, when shown as a mask")},
        {QStringLiteral("selectionActionBar"), i18nc("@info:tooltip", "Buttons beside a selection: deselect, invert, fill, crop…")},
        {QStringLiteral("zoomHorizontal"), i18nc("@info:tooltip", "Zoom drag: → in, ← out, instead of up and down")},
        {QStringLiteral("useTouchPressureSensitivity"), i18nc("@info:tooltip", "Pressing harder with a finger paints heavier")},
        {QStringLiteral("autoPinLayers"), i18nc("@info:tooltip", "New layers show in the timeline right away")},
        {QStringLiteral("adaptivePlaybackRange"), i18nc("@info:tooltip", "Plays from the first to the last keyframe")},
        {QStringLiteral("autoZoomTimelineToPlaybackRange"), i18nc("@info:tooltip", "The timeline fits the frames that play")},
        {QStringLiteral("CreateBackupFile"), i18nc("@info:tooltip", "Saving keeps the previous version: Ink.kra~")},
        {QStringLiteral("sessionOnStartup"),
         i18nc("@info:tooltip", "Last image: the one you had open\nLast session: every image and window as you left them\nSessions: pick a saved one")},
        {QStringLiteral("saveSessionOnQuit"), i18nc("@info:tooltip", "Reopens these images and windows next time")},
        {QStringLiteral("fpsLimit"), i18nc("@info:tooltip", "The canvas redraws at most this many times a second")},
        {QStringLiteral("[StudioTheme] Lightness"), i18nc("@info:tooltip", "Near black → near white")},
        {QStringLiteral("[StudioTheme] TintAmount"), i18nc("@info:tooltip", "0%: plain gray → 100%: fully the tint's hue")},
        {QStringLiteral("[StudioTheme] UseAccent"), i18nc("@info:tooltip", "Selected items, focus and links in color instead of gray")},
        {QStringLiteral("StudioGlassOpacity"), i18nc("@info:tooltip", "0%: see-through → 100%: solid")},
        {QStringLiteral("StudioGlassRimLight"), i18nc("@info:tooltip", "The line of light along the edges: 0% none → 200% bright")},
        {QStringLiteral("kritadisplayrc EnableHiDPIFractionalScaling"),
         i18nc("@info:tooltip", "On: exactly 125% or 150%, as the screen is set\nOff: rounded to 100% or 200%, crisper but a bit small or large")},
        {QStringLiteral("newOutlineStyle"), i18nc("@info:tooltip", "Circle: the brush's size\nBrush: its real shape\nWith tilt: its shape, leaning with the pen")},
        {QStringLiteral("newCursorStyle"), i18nc("@info:tooltip", "A mark at the pen's tip, with the outline")},
        {QStringLiteral("showSingleChannelAsColor"), i18nc("@info:tooltip", "One channel shown alone in its own color: Red in red, not gray")},
        {QStringLiteral("zoomSteps"), i18nc("@info:tooltip", "Steps from 100% to 200%. 2: 100% → 141% → 200%")},
        {QStringLiteral("OpenGLFilterMode"),
         i18nc("@info:tooltip",
               "Nearest: crisp square pixels, for pixel art\nBilinear: smooth, fastest\nTrilinear: smooth, less shimmer zoomed out\n"
               "High quality: smooth and sharpest")},
        {QStringLiteral("kritadisplayrc useWin8PointerInput"),
         i18nc("@info:tooltip", "On: Windows Ink\nOff: WinTab, your tablet's driver\nPressure or tilt acting up? Try the other")},
        {QStringLiteral("useRightMiddleTabletButtonWorkaround"), i18nc("@info:tooltip", "For pen buttons that do nothing in Skissa")},
        {QStringLiteral("tiltDirectionOffset"), i18nc("@info:tooltip", "Turns the tilt's direction: for a rotated tablet or left hand")},
        {QStringLiteral("touchPainting"), i18nc("@info:tooltip", "Auto: a finger paints when there's no pen\nOn: a finger always paints\nOff: a finger only moves and zooms")},
        {QStringLiteral("longPressEnabled"), i18nc("@info:tooltip", "Hold a finger still = right-click")},
        {QStringLiteral("KineticScrollingEnabled"), i18nc("@info:tooltip", "Lists keep scrolling after a flick, slowing down.")},
        {QStringLiteral("switchSelectionCtrlAlt"), i18nc("@info:tooltip", "Off: Ctrl replaces, Alt subtracts\nOn: Ctrl subtracts, Alt replaces")},
        {QStringLiteral("maximumBrushSize"), i18nc("@info:tooltip", "The top of the brush size slider. Huge brushes paint slowly")},
        {QStringLiteral("useDirtyPresets"), i18nc("@info:tooltip", "On: tweaks stay until you save or reload\nOff: switching away forgets them")},
        {QStringLiteral("useEraserBrushSize"), i18nc("@info:tooltip", "Switching to the eraser and back swaps between the two sizes")},
        {QStringLiteral("useEraserBrushOpacity"), i18nc("@info:tooltip", "Switching to the eraser and back swaps between the two opacities")},
        {QStringLiteral("[filterdialog] forceLodMode"), i18nc("@info:tooltip", "Large images: a rough preview while you adjust, full quality after")},
        {QStringLiteral("[KisToolTransform] forceLodMode"), i18nc("@info:tooltip", "Large images: a rough preview while you transform, full quality after")},
        {QStringLiteral("[KritaTransform/KisToolMove] forceLodMode"), i18nc("@info:tooltip", "Large images: a rough preview while you move, full quality after")},
        {QStringLiteral("useCumulativeUndoRedo"), i18nc("@info:tooltip", "Older quick strokes become one undo step: undo reaches further back")},
        {QStringLiteral("StudioEffectsEditable"), i18nc("@info:tooltip", "On: effects can be changed or removed later\nOff: painted into the layer")},
        {QStringLiteral("ResourceDirectory"), i18nc("@info:tooltip", "Where brushes, palettes, bundles and the rest are kept.")},
        {QStringLiteral("StudioLayersRememberBrushes"), i18nc("@info:tooltip", "Choosing a layer picks the brush last used on it.")},
        {QStringLiteral("StrokeShapeSnap"), i18nc("@info:tooltip", "Hold the pen still at the end of a stroke:\nline, circle, rectangle, arc → clean")},
        {QStringLiteral("ConvertToImageColorSpaceOnImport"), i18nc("@info:tooltip", "Imported layers take the image's colors, e.g. CMYK → RGB")},
        {QStringLiteral("compressLayersInKra"), i18nc("@info:tooltip", "Smaller files, slower to save and open")},
        {QStringLiteral("TrimKra"), i18nc("@info:tooltip", "Paint outside the canvas is deleted on save: smaller files, off-canvas art lost")},
        {QStringLiteral("pasteFormat"),
         i18nc("@info:tooltip", "Images copied from a browser:\nOriginal: the full file, downloaded\nLocal copy: the one the browser kept\nPasted image: exactly what was copied")},
        {QStringLiteral("pasteBehaviour"), i18nc("@info:tooltip", "Pasted images rarely say which colors they use: what to take them as")},
        {QStringLiteral("useDefaultColorSpace"), i18nc("@info:tooltip", "On: new images use this model\nOff: the one used last")},
        {QStringLiteral("renderIntent"),
         i18nc("@info:tooltip",
               "For colors the screen can't show:\n"
               "Perceptual: all shift a little, smooth\n"
               "Relative: exact where possible, the rest to the nearest\n"
               "Saturation: vivid, hues may shift\n"
               "Absolute: as Relative, keeping the image's white")},
        {QStringLiteral("useBlackPointCompensation"), i18nc("@info:tooltip", "Image black → screen black: shadows keep their detail")},
        {QStringLiteral("allowLCMSOptimization"), i18nc("@info:tooltip", "A little faster, a little less exact: may band gradients")},
        {QStringLiteral("defaultProofingProfileName"), i18nc("@info:tooltip", "The printer or paper new images are previewed for (View > Soft Proofing)")},
        {QStringLiteral("defaultProofingConversionIntent"),
         i18nc("@info:tooltip",
               "For colors the printer can't reach:\n"
               "Perceptual: all shift a little, smooth\n"
               "Relative: exact where possible, the rest to the nearest\n"
               "Saturation: vivid, hues may shift\n"
               "Absolute: as Relative, keeping the image's white")},
        {QStringLiteral("defaultProofingBlackpointCompensation"), i18nc("@info:tooltip", "Image black → the printer's darkest black: shadows keep their detail")},
        {QStringLiteral("defaultProofingDisplayMode"),
         i18nc("@info:tooltip", "Paper white: duller whites and blacks, as on a real print\nScreen white: the screen's bright white stands for the paper")},
        {QStringLiteral("defaultProofingGamutwarning"), i18nc("@info:tooltip", "Colors the printer can't reach are marked in this color (View > Out of Gamut Warnings)")},
        {QStringLiteral("colorsettings/forcepalettecolors"), i18nc("@info:tooltip", "Picked colors snap to the nearest color of the palette in use.")},
        {QStringLiteral("enableCanvasSurfaceColorSpaceManagement"),
         i18nc("@info:tooltip", "The system manages the canvas' colors: colors beyond sRGB, paper white proofing. Can be slower")},
        {QStringLiteral("canvasSurfaceColorSpaceManagementMode"), i18nc("@info:tooltip", "As the system prefers suits most screens")},
        {QStringLiteral("canvasSurfaceBitDepthMode"), i18nc("@info:tooltip", "10 bit is needed for HDR.")},
        {QStringLiteral("kritadisplayrc rootSurfaceFormat"), i18nc("@info:tooltip", "sRGB: any screen\nHDR: an HDR screen with HDR turned on in the system")},
        {QStringLiteral("ExrDefaultColorProfile"), i18nc("@info:tooltip", "EXR files rarely carry a profile: this one is used to open them")},
        {QStringLiteral("kritadisplayrc OpenGLRenderer"), i18nc("@info:tooltip", "Auto suits most. Slow, flickering or wrong colors? Try another")},
        {QStringLiteral("memoryHardLimitPercent"), i18nc("@info:tooltip", "The share of memory images may use. Past it, Skissa uses the disk: slower")},
        {QStringLiteral("maxNumberOfThreads"), i18nc("@info:tooltip", "More: faster painting and effects, less left for other apps")},
        {QStringLiteral("frameRenderingClones"), i18nc("@info:tooltip", "More: animations render faster, using more memory")},
        {QStringLiteral("detectFpsLimit"), i18nc("@info:tooltip", "The canvas redraws as often as the screen refreshes: 60 Hz → 60 a second")},
    };
}

/// Color models available for proofing, print models first
const QVector<KoID> &proofingModels()
{
    static const QVector<KoID> models {CMYKAColorModelID, GrayAColorModelID, RGBAColorModelID, LABAColorModelID};
    return models;
}

/// Profiles new images can be proofed with, grouped by color model
QVector<Choice> proofingProfiles()
{
    QVector<Choice> profiles;
    QStringList seen;
    KoColorSpaceRegistry *registry = KoColorSpaceRegistry::instance();
    Q_FOREACH (const KoID &model, proofingModels()) {
        QStringList names;
        Q_FOREACH (const KoColorProfile *profile, registry->profilesFor(registry->colorSpaceId(model, Integer16BitsColorDepthID))) {
            if (profile && !seen.contains(profile->name())) names << profile->name();
        }
        names.removeDuplicates();
        names.sort(Qt::CaseInsensitive);
        seen << names;
        Q_FOREACH (const QString &name, names) profiles << Choice{name, name};
    }
    return profiles;
}

/// Also stores the profile's color model, switching to 16-bit if the model lacks the current depth
void proofWith(const QString &profileName)
{
    KoColorSpaceRegistry *registry = KoColorSpaceRegistry::instance();
    KConfigGroup g = KSharedConfig::openConfig()->group(QString());
    Q_FOREACH (const KoID &model, proofingModels()) {
        Q_FOREACH (const KoColorProfile *profile, registry->profilesFor(registry->colorSpaceId(model, Integer16BitsColorDepthID))) {
            if (!profile || profile->name() != profileName) continue;
            const QString depth = g.readEntry("defaultProofingProfileDepth", Integer8BitsColorDepthID.id());
            g.writeEntry("defaultProofingProfileModel", model.id());
            if (registry->colorSpaceId(model.id(), depth).isEmpty()) g.writeEntry("defaultProofingProfileDepth", Integer16BitsColorDepthID.id());
            return;
        }
    }
}

/// Profiles suitable for a display
QVector<Choice> displayProfiles()
{
    QVector<Choice> profiles;
    Q_FOREACH (const KoColorProfile *profile, KoColorSpaceRegistry::instance()->profilesFor(QStringLiteral("RGBA"))) {
        if (profile->isSuitableForDisplay()) profiles << Choice{profile->name(), profile->name()};
    }
    std::sort(profiles.begin(), profiles.end(), [](const Choice &a, const Choice &b) { return a.text.compare(b.text, Qt::CaseInsensitive) < 0; });
    return profiles;
}

QVector<Setting> describeAll()
{
    using R = KisSettingsRegistry;
    QVector<Setting> all;

    // Appearance
    {
        Setting theme = grouped(choice(QStringLiteral("Theme"), QStringLiteral("Skissa Dark"), {}, i18nc("setting", "Theme"), R::Fork), QStringLiteral("theme"));
        theme.dynamicChoices = themes;
        all << theme;
        {
            // Theme parameters (KisStudioTheme): changing any of them regenerates and applies the theme
            const QString themeGroup = QStringLiteral("StudioTheme");
            auto made = [](Setting s) {
                s.written = [](const QVariant &) { KisStudioTheme::apply(KisStudioTheme::current()); };
                return s;
            };
            all << made(grouped(number(QStringLiteral("Lightness"), 0.252, 0.05, 0.98, i18nc("setting", "Theme lightness"), R::Fork), themeGroup));
            all << made(grouped(color(QStringLiteral("Tint"), QColor(Qt::gray), i18nc("setting", "Theme tint"), R::Fork), themeGroup));
            all << made(grouped(number(QStringLiteral("TintAmount"), 0.0, 0, 1, i18nc("setting", "Theme tint strength"), R::Fork), themeGroup));
            all << made(grouped(toggle(QStringLiteral("UseAccent"), false, i18nc("setting", "Theme accent"), R::Fork), themeGroup));
            all << made(grouped(color(QStringLiteral("Accent"), QColor(92, 124, 250), i18nc("setting", "Theme accent color"), R::Fork), themeGroup));
        }

        Setting glass = toggle(QStringLiteral("StudioGlass"), true, i18nc("setting", "Glass"), R::Fork);
        glass.read = []() { return KisStudioGlass::isEnabled(); };
        glass.write = [](const QVariant &value) { KisStudioGlass::setEnabled(value.toBool()); };
        all << glass;
        Setting blur = number(QStringLiteral("StudioGlassBlur"), 1.0, 0, 2.0, i18nc("setting", "Glass blur"), R::Fork);
        blur.read = []() { return KisStudioGlass::blur(); };
        blur.write = [](const QVariant &value) { KisStudioGlass::setBlur(value.toReal()); };
        all << blur;
        Setting rimLight = number(QStringLiteral("StudioGlassRimLight"), 1.0, 0, 2.0, i18nc("setting", "Glass rim light"), R::Fork);
        rimLight.read = []() { return KisStudioGlass::rimLight(); };
        rimLight.write = [](const QVariant &value) { KisStudioGlass::setRimLight(value.toReal()); };
        all << rimLight;
        Setting opacity = number(QStringLiteral("StudioGlassOpacity"), 0.7, 0, 1, i18nc("setting", "Glass opacity"), R::Fork);
        opacity.read = []() { return KisStudioGlass::opacity(); };
        opacity.write = [](const QVariant &value) { KisStudioGlass::setOpacity(value.toReal()); };
        all << opacity;

        Setting strokes = toggle(QStringLiteral("PresetStrokeThumbnails"), false, i18nc("setting", "Brushes as strokes"), R::Fork);
        strokes.read = []() { return KisPresetStrokeThumbnails::isEnabled(); };
        strokes.write = [](const QVariant &value) { KisPresetStrokeThumbnails::setEnabled(value.toBool()); };
        all << strokes;

        all << color(QStringLiteral("canvasBorderColor"), QColor(0, 0, 0), i18nc("setting", "Around the image"));
        Setting ownFont = toggle(QStringLiteral("use_custom_system_font"), false, i18nc("setting", "Own interface font"));
        // Turning it on starts from the current font; turning it off clears both and uses the system font
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

    // Canvas
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
        all << toggle(QStringLiteral("colorSamplerZoomPreviewEnabled"), true, i18nc("setting", "Sampler magnifier"));
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
        // The canvas reads this while drawing, so redraw what is already there
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

    // Pen
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

    // Tools, layers and undo
    {
        all << toggle(QStringLiteral("switchSelectionCtrlAlt"), false, i18nc("setting", "Swap Ctrl and Alt for selections"));
        all << toggle(QStringLiteral("activateTransformToolAfterPaste"), false, i18nc("setting", "Transform after pasting"));
        all << number(QStringLiteral("maximumBrushSize"), 1000, 100, 10000, i18nc("setting", "Largest brush"), R::Restart);
        all << toggle(QStringLiteral("useDirtyPresets"), true, i18nc("setting", "Keep brush tweaks"));
        all << toggle(QStringLiteral("useEraserBrushSize"), false, i18nc("setting", "Eraser keeps its own size"));
        all << toggle(QStringLiteral("useEraserBrushOpacity"), false, i18nc("setting", "Eraser keeps its own opacity"));
        all << toggle(QStringLiteral("StrokeShapeSnap"), false, i18nc("setting", "Hold to make a shape"), R::Fork);
        all << toggle(QStringLiteral("StudioViewPill"), true, i18nc("setting", "Zoom pill"), R::Fork);
        all << grouped(toggle(QStringLiteral("forceLodMode"), true, i18nc("setting", "Instant preview in effects")), QStringLiteral("filterdialog"));
        all << grouped(toggle(QStringLiteral("forceLodMode"), true, i18nc("setting", "Instant preview in Transform")), QStringLiteral("KisToolTransform"));
        all << grouped(toggle(QStringLiteral("forceLodMode"), false, i18nc("setting", "Instant preview in Move")), QStringLiteral("KritaTransform/KisToolMove"));
        all << number(QStringLiteral("undoStackLimit"), 200, 0, 1000, i18nc("setting", "Undo steps"));
        all << toggle(QStringLiteral("useCumulativeUndoRedo"), false, i18nc("setting", "Merge strokes in undo"));
        all << toggle(QStringLiteral("ConvertToImageColorSpaceOnImport"), false, i18nc("setting", "Imported layers take the image's colors"));
        {
            using C = KisImageConfig;
            const QString labels[] = {i18nc("setting", "Copies' names"), i18nc("setting", "Pasted layers' names"), i18nc("setting", "Merged layers' names")};
            for (C::LayerNaming kind : {C::CopiedLayer, C::PastedLayer, C::MergedLayer}) {
                for (bool before : {true, false}) {
                    Setting words = text(C::layerNameKey(kind, before), C::defaultLayerNameAffix(kind, before),
                                         labels[kind] + QStringLiteral(" · ") + (before ? i18nc("words of a layer name", "before") : i18nc("words of a layer name", "after")));
                    words.valid = [](const QVariant &value) { return value.toString().size() <= 40; };
                    all << words;
                }
            }
        }
        all << toggle(QStringLiteral("StudioLayersRememberBrushes"), true, i18nc("setting", "Layers remember their brush"), R::Fork);
        all << toggle(QStringLiteral("StudioEffectsEditable"), true, i18nc("setting", "Effects stay editable"), R::Fork);
        all << toggle(QStringLiteral("autoPinLayers"), true, i18nc("setting", "Pin new layers to the timeline"));
        all << toggle(QStringLiteral("adaptivePlaybackRange"), true, i18nc("setting", "Playback range follows keyframes"));
        all << toggle(QStringLiteral("autoZoomTimelineToPlaybackRange"), true, i18nc("setting", "Timeline zooms to the playback range"));
    }

    // Files
    {
        Setting autosave = number(QStringLiteral("AutoSaveInterval"), 120, 0, 86400, i18nc("setting", "Autosave"));
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
        all << toggle(QStringLiteral("TrimKra"), false, i18nc("setting", "Trim layers when saving"));
        // Krita treats an unset export type as PNG
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
        // 3 (Last image) exists only in this fork (see KisApplication::start)
        all << choice(QStringLiteral("sessionOnStartup"), 3,
                      {{3, i18nc("on start", "Last image")}, {0, i18nc("on start", "Start page")}, {1, i18nc("on start", "Last session")},
                       {2, i18nc("on start", "Sessions")}},
                      i18nc("setting", "On start"));
        all << toggle(QStringLiteral("saveSessionOnQuit"), false, i18nc("setting", "Save the session on quit"));
        bool nativeDialogs = false;
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS) || defined(Q_OS_ANDROID)
        nativeDialogs = true;
#else
        nativeDialogs = qgetenv("XDG_CURRENT_DESKTOP") == "KDE";
#endif
        all << grouped(toggle(QStringLiteral("DontUseNativeFileDialog"), !nativeDialogs, i18nc("setting", "Skissa's own file dialogs"), R::Machine),
                       QStringLiteral("File Dialogs"));
        Setting resources = text(QStringLiteral("ResourceDirectory"), QStandardPaths::writableLocation(QStandardPaths::AppDataLocation),
                                 i18nc("setting", "Resource folder"), R::Machine | R::Restart);
        resources.valid = writableFolder;
        all << resources;
    }

    // Color
    {
        auto newImageColors = [](const QString &space) {
            // Stored where the new image dialog reads them
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

        // Proofing defaults for new images
        {
            auto proofingChanged = [](const QVariant &) { KisImageConfigNotifier::instance()->notifyGlobalProofingConfigChanged(); };
            Setting profile = choice(QStringLiteral("defaultProofingProfileName"), QStringLiteral("Chemical proof"), {}, i18nc("setting", "Proofing profile"));
            profile.dynamicChoices = proofingProfiles;
            profile.written = [proofingChanged](const QVariant &value) {
                proofWith(value.toString());
                proofingChanged(value);
            };
            all << profile;
            Setting intent = choice(QStringLiteral("defaultProofingConversionIntent"), 1,
                                    {{0, i18nc("rendering intent", "Perceptual")}, {1, i18nc("rendering intent", "Relative")},
                                     {2, i18nc("rendering intent", "Saturation")}, {3, i18nc("rendering intent", "Absolute")}},
                                    i18nc("setting", "Proofing intent"));
            intent.written = proofingChanged;
            all << intent;
            Setting black = toggle(QStringLiteral("defaultProofingBlackpointCompensation"), true, i18nc("setting", "Proofing black point compensation"));
            black.written = proofingChanged;
            all << black;
            // The old dialog's custom mode shows as Paper white
            Setting paper = choice(QStringLiteral("defaultProofingDisplayMode"), 1,
                                   {{0, i18nc("proofing display", "Screen white")}, {1, i18nc("proofing display", "Paper white")}},
                                   i18nc("setting", "Proof's white"));
            paper.written = proofingChanged;
            all << paper;
            Setting warning = color(QStringLiteral("defaultProofingGamutwarning"), Qt::green, i18nc("setting", "Out of gamut"));
            warning.written = proofingChanged;
            all << warning;
        }

        // A profile per screen, or the canvas surface options when the system manages color
        const bool osManaged = KisPlatformPluginInterfaceFactory::instance()->surfaceColorManagedByOS();
        if (!osManaged) {
            const QList<QScreen *> screens = QGuiApplication::screens();
            for (int i = 0; i < screens.size(); i++) {
                const QString name = screens[i]->model().trimmed().isEmpty() ? screens[i]->name() : screens[i]->model().trimmed();
                Setting monitor = choice(QStringLiteral("monitorProfile"), QStringLiteral("sRGB-elle-V2-srgbtrc.icc"), {},
                                         i18nc("setting: a screen's color profile, %1 its number, %2 its name", "Screen %1 (%2)", i + 1, name), R::Machine);
                monitor.id = QStringLiteral("monitorProfile screen %1").arg(i);
                monitor.dynamicChoices = displayProfiles;
                monitor.read = [i]() { return KisConfig(true).monitorProfile(i); };
                monitor.write = [i](const QVariant &value) { KisConfig(false).setMonitorProfile(i, value.toString()); };
                all << monitor;
            }
        } else {
            Setting managed = toggle(QStringLiteral("enableCanvasSurfaceColorSpaceManagement"), false, i18nc("setting", "Color-managed canvas"), R::Machine);
            managed.read = []() { return KisConfig(true).enableCanvasSurfaceColorSpaceManagement(); };
            managed.write = [](const QVariant &value) { KisConfig(false).setEnableCanvasSurfaceColorSpaceManagement(value.toBool()); };
            all << managed;
            using Mode = KisConfig::CanvasSurfaceMode;
            Setting mode = choice(QStringLiteral("canvasSurfaceColorSpaceManagementMode"), int(Mode::Preferred),
                                  {{int(Mode::Preferred), i18nc("canvas surface", "As the system prefers")}, {int(Mode::Rec709g22), i18nc("canvas surface", "Rec. 709, gamma 2.2")},
                                   {int(Mode::Rec709g10), i18nc("canvas surface", "Rec. 709, linear")}, {int(Mode::Rec2020pq), i18nc("canvas surface", "Rec. 2020 PQ")},
                                   {int(Mode::Unmanaged), i18nc("canvas surface", "Unmanaged (testing)")}},
                                  i18nc("setting", "Canvas color space"), R::Machine);
            mode.read = []() { return int(KisConfig(true).canvasSurfaceColorSpaceManagementMode()); };
            mode.write = [](const QVariant &value) { KisConfig(false).setCanvasSurfaceColorSpaceManagementMode(Mode(value.toInt())); };
            all << mode;
            using Depth = KisConfig::CanvasSurfaceBitDepthMode;
            Setting depth = choice(QStringLiteral("canvasSurfaceBitDepthMode"), int(Depth::DepthAuto),
                                   {{int(Depth::DepthAuto), i18nc("canvas surface depth", "Auto")}, {int(Depth::Depth8Bit), i18nc("canvas surface depth", "8 bit")},
                                    {int(Depth::Depth10Bit), i18nc("canvas surface depth", "10 bit")}},
                                   i18nc("setting", "Canvas bit depth"), R::Machine | R::Restart);
            depth.read = []() { return int(KisConfig(true).canvasSurfaceBitDepthMode()); };
            depth.write = [](const QVariant &value) { KisConfig(false).setCanvasSurfaceBitDepthMode(Depth(value.toInt())); };
            all << depth;
        }
#ifdef HAVE_HDR
        // When the system manages color, it sets the window format too
        if (!osManaged) {
            Setting format = choice(QStringLiteral("rootSurfaceFormat"), int(KisConfig::BT709_G22),
                                    {{int(KisConfig::BT709_G22), i18nc("screen format", "sRGB, 8 bit")},
                                     {int(KisConfig::BT2020_PQ), i18nc("screen format", "HDR: Rec. 2020 PQ, 10 bit")},
                                     {int(KisConfig::BT709_G10), i18nc("screen format", "HDR: Rec. 709 linear, 16 bit")}},
                                    i18nc("setting", "Window format"), R::Machine | R::Restart);
            format.id = QStringLiteral("kritadisplayrc rootSurfaceFormat");
            format.read = []() {
                QSettings settings(displayrcPath(), QSettings::IniFormat);
                return int(KisConfig::rootSurfaceFormat(&settings));
            };
            format.write = [](const QVariant &value) {
                QSettings settings(displayrcPath(), QSettings::IniFormat);
                KisConfig::setRootSurfaceFormat(&settings, KisConfig::RootSurfaceFormat(value.toInt()));
            };
            all << format;
        }
#endif
    }

    // Performance
    {
        Setting renderer = choice(QStringLiteral("OpenGLRenderer"), KisOpenGL::convertOpenGLRendererToConfig(KisOpenGL::RendererAuto), {},
                                  i18nc("setting", "Renderer"), R::Machine | R::Restart);
        renderer.dynamicChoices = renderers;
        renderer.id = QStringLiteral("kritadisplayrc OpenGLRenderer");
        renderer.read = []() { return KisOpenGL::convertOpenGLRendererToConfig(KisOpenGL::getUserPreferredOpenGLRendererConfig()); };
        renderer.write = [](const QVariant &value) {
            const KisOpenGL::OpenGLRenderer before = KisOpenGL::getUserPreferredOpenGLRendererConfig();
            const KisOpenGL::OpenGLRenderer after = KisOpenGL::convertConfigToOpenGLRenderer(value.toString());
            // Re-enabling acceleration makes the canvas try OpenGL again, even if it failed before
            if (before == KisOpenGL::RendererNone && after != KisOpenGL::RendererNone) KisConfig(false).setCanvasState(QStringLiteral("TRY_OPENGL"));
            KisOpenGL::setUserPreferredOpenGLRendererConfig(after);
        };
        // Turning acceleration off takes effect at once; any other switch needs a restart
        renderer.waitsForRestart = [](const QVariant &startup, const QVariant &now) {
            const QString none = KisOpenGL::convertOpenGLRendererToConfig(KisOpenGL::RendererNone);
            return now.toString() != none && now.toString() != startup.toString();
        };
        all << renderer;
        all << number(QStringLiteral("memoryHardLimitPercent"), 50.0, 1, 100, i18nc("setting", "Memory for images"), R::Machine | R::Restart);
        const int threads = QThread::idealThreadCount();
        all << number(QStringLiteral("maxNumberOfThreads"), threads, 1, threads, i18nc("setting", "Processor threads"), R::Machine);
        // Krita's default while unset: half the allowed threads
        auto halfTheThreads = []() { return QVariant(qMax(1, KisImageConfig(true).maxNumberOfThreads() / 2)); };
        Setting clones = number(QStringLiteral("frameRenderingClones"), halfTheThreads(), 1, threads, i18nc("setting", "Frames rendered at once"), R::Machine);
        clones.fallbackNow = halfTheThreads;
        all << clones;
        all << toggle(QStringLiteral("detectFpsLimit"), true, i18nc("setting", "Match the screen's frame rate"), R::NewViews);
        all << number(QStringLiteral("fpsLimit"), 100, 1, 1000, i18nc("setting", "Frame rate limit"), R::NewViews);
    }
    const QHash<QString, QString> help = explanations();
    for (Setting &s : all) s.help = help.value(s.id);
    return all;
}

/**
 * Deletes the stored values of settings that are no longer shown, so they fall back to Krita's
 * defaults instead of staying changed out of sight. Runs once per \c version.
 */
void forgetRetired()
{
    constexpr int version = 7;
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
        "forcedDpiForQtFontBugWorkaround", "memorySoftLimitPercent", "maxSwapSize", "swaplocation", "useOnDiskAnimationCacheSwapping",
        "calculateAnimationCacheInBackground", "disableAVXOptimizations", "amdDisableVectorWorkaround",
        // pinned panels now fit their contents; only heights set by hand are kept, under another key
        "StudioLayoutPanelHeights",
        // the glass's refraction is its shape now, with no strength of its own
        "StudioGlassRefraction", "StudioGlassEdge", "StudioGlassClarity",
    };
    // Krita's "don't rename copied/pasted/merged layers" options become empty name affixes
    const struct { const char *old; KisImageConfig::LayerNaming kind; } renames[] = {
        {"renameDuplicatedLayers", KisImageConfig::CopiedLayer},
        {"renamePastedLayers", KisImageConfig::PastedLayer},
        {"renameMergedLayers", KisImageConfig::MergedLayer},
    };
    for (const auto &rename : renames) {
        if (!top.hasKey(rename.old) || top.readEntry(rename.old, true)) continue;
        for (bool before : {true, false}) {
            const QString key = KisImageConfig::layerNameKey(rename.kind, before);
            if (!top.hasKey(key)) top.writeEntry(key, QString());
        }
    }
    for (const char *key : keys) top.deleteEntry(key);
    // Krita always wrote the start page choice, chosen or not; drop it so the last image opens instead
    if (top.readEntry("StudioRetiredSettings", 0) < 4 && top.readEntry("sessionOnStartup", 0) == 0) top.deleteEntry("sessionOnStartup");
    // Likewise for the sampler magnifier, which upstream Krita defaults to off
    if (top.readEntry("StudioRetiredSettings", 0) < 5 && !top.readEntry("colorSamplerZoomPreviewEnabled", true)) top.deleteEntry("colorSamplerZoomPreviewEnabled");
    KSharedConfig::openConfig()->group(QStringLiteral("StudioTheme")).deleteEntry("Contrast");
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
    /// Cached run-time choices (themes, file types, profiles), cleared by refreshChoices()
    QHash<QString, QVector<Choice>> foundChoices;
    QTimer notify;
    QTimer sync;

    /// A setting another one's change wrote (a font with its switch): part of that change's step
    struct Follower {
        QString id;
        QVariant before;
        QVariant after;
    };
    struct Change {
        QString id;
        QVariant before;
        QVariant after;
        QElapsedTimer made;
        QVector<Follower> followers;
    };
    QVector<Change> undo;
    QVector<Change> redo;
    /// Set while undoing or redoing, so the write isn't recorded as a new change
    bool replaying {false};
    /// How deep in writing settings: a write made while another is written follows it
    int writing {0};
    /// Inside setValues(): after the first, the writes follow it
    bool batch {false};
    bool batchStarted {false};
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
    // Values at startup, to tell which restart-only changes are still pending
    Q_FOREACH (const Setting &s, d->settings) {
        if (s.flags & Restart) d->startup.insert(s.id, value(s.id));
    }

    // Notify Krita once per burst of changes (a slider drag), still soon enough to follow live
    d->notify.setSingleShot(true);
    d->notify.setInterval(60);
    connect(&d->notify, &QTimer::timeout, this, &KisSettingsRegistry::notifyNow);
    // Write kritarc only once the changes stop: rewriting it is costly and not safe to do often
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

void KisSettingsRegistry::refreshChoices()
{
    d->foundChoices.clear();
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
    // Only Text accepts a null value, since Qt 5 treats an empty QString() as null
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
        // The default is valid even when it isn't among the choices (yet)
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
    // An invalid stored value (an old version's, a hand edit) reads as the default, as in Krita
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
    const QVariant before = value(id);
    if (sameValue(*s, v, before)) return true;
    if (!d->replaying && (d->writing > 0 || (d->batch && d->batchStarted)) && !d->undo.isEmpty()) {
        d->undo.last().followers << Private::Follower{id, before, v};
    } else if (!d->replaying) {
        if (d->batch) d->batchStarted = true;
        // Merge quick successive changes of one setting (a slider drag, typing) into one step
        if (!d->undo.isEmpty() && d->undo.last().id == id && d->undo.last().made.elapsed() < 1500) {
            d->undo.last().after = v;
        } else {
            Private::Change change{id, before, v, QElapsedTimer(), {}};
            d->undo << change;
            if (d->undo.size() > 200) d->undo.removeFirst();
        }
        d->undo.last().made.restart();
        d->redo.clear();
        // dragged there and back: no step at all
        if (d->undo.last().followers.isEmpty() && sameValue(*s, d->undo.last().after, d->undo.last().before)) d->undo.removeLast();
    }
    d->writing++;
    if (s->write) {
        s->write(v);
    } else {
        if (s->fallbackNow && sameValue(*s, v, s->fallbackNow())) {
            KSharedConfig::openConfig()->group(s->group).deleteEntry(s->key); // so it keeps following its default
        } else {
            writeIn(*s, *KSharedConfig::openConfig(), v);
        }
        d->sync.start();
    }
    if (s->written) s->written(v);
    d->writing--;
    schedule();
    Q_EMIT valueChanged(id);
    return true;
}

void KisSettingsRegistry::setValues(const QVector<QPair<QString, QVariant>> &values)
{
    d->batch = true;
    d->batchStarted = false;
    for (const auto &value : values) setValue(value.first, value.second);
    d->batch = false;
}

qint64 KisSettingsRegistry::undoAge() const
{
    return d->undo.isEmpty() ? -1 : d->undo.last().made.elapsed();
}

QString KisSettingsRegistry::undoName() const
{
    const Setting *s = d->undo.isEmpty() ? nullptr : setting(d->undo.last().id);
    return s ? s->label : QString();
}

bool KisSettingsRegistry::undoChange()
{
    if (d->undo.isEmpty()) return false;
    Private::Change change = d->undo.takeLast();
    d->replaying = true;
    const bool done = setValue(change.id, change.before);
    for (int i = change.followers.size() - 1; i >= 0; i--) setValue(change.followers[i].id, change.followers[i].before);
    d->replaying = false;
    // a value that can't be gone back to (a choice since removed): on to the step before
    if (!done) return undoChange();
    change.made.restart();
    d->redo << change;
    return true;
}

bool KisSettingsRegistry::replaying() const
{
    return d->replaying;
}

bool KisSettingsRegistry::canRedoChange() const
{
    return !d->redo.isEmpty();
}

QString KisSettingsRegistry::redoName() const
{
    const Setting *s = d->redo.isEmpty() ? nullptr : setting(d->redo.last().id);
    return s ? s->label : QString();
}

bool KisSettingsRegistry::redoChange()
{
    if (d->redo.isEmpty()) return false;
    Private::Change change = d->redo.takeLast();
    d->replaying = true;
    setValue(change.id, change.after);
    for (const Private::Follower &follower : change.followers) setValue(follower.id, follower.after);
    d->replaying = false;
    // A redone change counts as new when Undo weighs it against the document's steps
    change.made.restart();
    d->undo << change;
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
    // Layers cache some settings for compositing and don't listen for changes
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
