/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioGlass.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include <QAbstractItemView>
#include <QAbstractScrollArea>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QEvent>
#include <QFrame>
#include <QGraphicsOpacityEffect>
#include <QGroupBox>
#include <QHash>
#include <QLabel>
#include <QLayout>
#include <QLinearGradient>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QElapsedTimer>
#include <QPointer>
#include <QRegion>
#include <QSet>
#include <QThreadPool>
#include <QTimer>
#include <QWidget>
#include <QtMath>

#include "KisMainWindow.h"
#include "KisModernStyle.h"
#include "KisPart.h"
#include "KisPopupButton.h"
#include "KisUpdatePacer.h"
#include "KisViewManager.h"
#include "canvas/kis_canvas2.h"
#include "KisSettingsRegistry.h"
#include "kis_config.h"
#include "kis_config_notifier.h"
#include "kis_coordinates_converter.h"
#include "kis_image.h"
#include "kis_paint_device.h"
#include <KisFrameRate.h>
#include <KoConfig.h>

#ifdef HAVE_X11
#include <QGuiApplication>
#include <qpa/qplatformnativeinterface.h>
#endif

namespace {

const QString settingKey = QStringLiteral("StudioGlass");
const QString blurKey = QStringLiteral("StudioGlassBlur");
const QString opacityKey = QStringLiteral("StudioGlassOpacity");
const QString rimLightKey = QStringLiteral("StudioGlassRimLight");

constexpr qreal blurRadius = 26;    ///< widget pixels
constexpr qreal sampleScale = 0.25; ///< CPU glass renders at quarter resolution; the blur hides it

const char *const drawnByCanvasProperty = "studioGlassDrawnByCanvas";
const char *const maskedProperty = "studioGlassMasked";
const char *const overOverlaysProperty = "studioGlassOverOverlays";
const char *const savedPaletteProperty = "studioGlassSavedPalette";
const char *const seeThroughProperty = "studioGlassSeeThrough";
const char *const unfilledViewProperty = "studioGlassUnfilledView";
const char *const glassMenuProperty = "studioGlassMenu";
const char *const glassDropdownProperty = "studioGlassDropdown";
const char *const glassDialogProperty = "studioGlassDialog";
const char *const glassDialogDoneProperty = "studioGlassDialogDone";
const char *const movableProperty = "studioMovableByBackground";

// ------------------------------------------------------------ color

const std::array<float, 256> &toLinear()
{
    static const std::array<float, 256> table = []() {
        std::array<float, 256> t {};
        for (int i = 0; i < 256; i++) {
            const float c = i / 255.0f;
            t[i] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
        }
        return t;
    }();
    return table;
}

quint8 toSrgb(float linear)
{
    static const std::array<quint8, 4096> table = []() {
        std::array<quint8, 4096> t {};
        for (int i = 0; i < 4096; i++) {
            const float c = i / 4095.0f;
            const float s = c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
            t[i] = quint8(qBound(0, int(std::lround(s * 255.0f)), 255));
        }
        return t;
    }();
    return table[qBound(0, int(linear * 4095.0f + 0.5f), 4095)];
}

struct Lab {
    float l, a, b;
};

Lab toOklab(float r, float g, float b)
{
    const float l = std::cbrt(qMax(0.0f, 0.4122214708f * r + 0.5363325363f * g + 0.0514459929f * b));
    const float m = std::cbrt(qMax(0.0f, 0.2119034982f * r + 0.6806995451f * g + 0.1073969566f * b));
    const float s = std::cbrt(qMax(0.0f, 0.0883024619f * r + 0.2817188376f * g + 0.6299787005f * b));
    return {0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s,
            1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s,
            0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s};
}

std::array<float, 3> fromOklab(const Lab &c)
{
    float l = c.l + 0.3963377774f * c.a + 0.2158037573f * c.b;
    float m = c.l - 0.1055613458f * c.a - 0.0638541728f * c.b;
    float s = c.l - 0.0894841775f * c.a - 1.2914855480f * c.b;
    l = l * l * l;
    m = m * m * m;
    s = s * s * s;
    return {4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s,
            -1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s,
            -0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s};
}

/// \p c with its chroma reduced until it fits in the sRGB gamut.
std::array<float, 3> inGamut(const Lab &c)
{
    auto inside = [](const std::array<float, 3> &rgb) {
        return rgb[0] >= -1e-4f && rgb[1] >= -1e-4f && rgb[2] >= -1e-4f && rgb[0] <= 1.0001f && rgb[1] <= 1.0001f && rgb[2] <= 1.0001f;
    };
    const std::array<float, 3> rgb = fromOklab(c);
    if (inside(rgb)) return rgb;
    float lo = 0, hi = 1;
    for (int i = 0; i < 10; i++) {
        const float mid = 0.5f * (lo + hi);
        if (inside(fromOklab({c.l, c.a * mid, c.b * mid}))) lo = mid;
        else hi = mid;
    }
    return fromOklab({c.l, c.a * lo, c.b * lo});
}

/// \p image (sRGB) seen through glass tinted with \p tint at \p strength, matching the GPU glass.
QImage tinted(const QImage &image, const QColor &tint, qreal strength)
{
    const auto &linear = toLinear();
    const Lab t = toOklab(linear[tint.red()], linear[tint.green()], linear[tint.blue()]);
    const float s = float(strength);
    const float veil = float(KisStudioGlass::veil);
    QImage result(image.size(), QImage::Format_RGB32);
    for (int y = 0; y < image.height(); y++) {
        const QRgb *in = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        QRgb *out = reinterpret_cast<QRgb *>(result.scanLine(y));
        for (int x = 0; x < image.width(); x++) {
            const Lab b = toOklab(linear[qRed(in[x])], linear[qGreen(in[x])], linear[qBlue(in[x])]);
            const float l = qBound(0.0f, b.l + (t.l - b.l) * s, 1.0f);
            // Keep the chroma, veiled by light the glass scatters (fully where the glass lightens).
            const float lighter = qBound(0.0f, (l - b.l + 0.02f) / 0.08f, 1.0f);
            // all of it at full strength, where the glass is solid
            const float m = veil + (1.0f - veil) * lighter * lighter * (3 - 2 * lighter);
            const float scattered = s * m + (1 - m) * s * s * s;
            const std::array<float, 3> c = inGamut({l, b.a * (1 - scattered) + t.a * scattered, b.b * (1 - scattered) + t.b * scattered});
            out[x] = qRgb(toSrgb(c[0]), toSrgb(c[1]), toSrgb(c[2]));
        }
    }
    return result;
}

/// One box blur pass over \p count values \p stride apart, clamping at the edges.
void boxLine(float *data, int count, int stride, int radius, std::vector<float> &scratch)
{
    if (radius <= 0 || count <= 1) return;
    scratch.resize(count);
    const float scale = 1.0f / (2 * radius + 1);
    float sum = 0;
    for (int i = -radius; i <= radius; i++) sum += data[qBound(0, i, count - 1) * stride];
    for (int i = 0; i < count; i++) {
        scratch[i] = sum * scale;
        sum += data[qMin(i + radius + 1, count - 1) * stride] - data[qMax(i - radius, 0) * stride];
    }
    for (int i = 0; i < count; i++) data[i * stride] = scratch[i];
}

/// Approximates a gaussian blur of \p radius with three box passes, mixing in Oklab like the GPU glass.
QImage blurred(const QImage &source, int radius)
{
    const QImage image = source.convertToFormat(QImage::Format_RGB32);
    const int w = image.width(), h = image.height();
    // Weighted lightness power, the weight, weighted a and b, and weighted chroma.
    enum { L, Weight, A, Bb, Chroma, Count };
    const float power = float(KisStudioGlass::lightnessPower);
    std::vector<float> channels[Count];
    for (auto &channel : channels) channel.resize(size_t(w) * h);
    const auto &linear = toLinear();
    for (int y = 0; y < h; y++) {
        const QRgb *line = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < w; x++) {
            const size_t i = size_t(y) * w + x;
            const Lab lab = toOklab(linear[qRed(line[x])], linear[qGreen(line[x])], linear[qBlue(line[x])]);
            const float chroma = std::hypot(lab.a, lab.b);
            const float weight = 1.0f + float(KisStudioGlass::salience) * chroma;
            channels[L][i] = std::pow(qMax(0.0f, lab.l), power) * weight;
            channels[Weight][i] = weight;
            channels[A][i] = lab.a * weight;
            channels[Bb][i] = lab.b * weight;
            channels[Chroma][i] = chroma * weight;
        }
    }
    // Three passes share the variance.
    const int box = qMax(1, int(std::lround(radius / std::sqrt(3.0))));
    std::vector<float> scratch;
    for (int c = 0; c < Count; c++) {
        for (int pass = 0; pass < 3; pass++) {
            for (int y = 0; y < h; y++) boxLine(channels[c].data() + size_t(y) * w, w, 1, box, scratch);
            for (int x = 0; x < w; x++) boxLine(channels[c].data() + x, h, w, box, scratch);
        }
    }
    const float restore = float(KisStudioGlass::restore);
    QImage result(w, h, QImage::Format_RGB32);
    for (int y = 0; y < h; y++) {
        QRgb *line = reinterpret_cast<QRgb *>(result.scanLine(y));
        for (int x = 0; x < w; x++) {
            const size_t i = size_t(y) * w + x;
            const float weight = qMax(1e-6f, channels[Weight][i]);
            const float l = std::pow(qMax(0.0f, channels[L][i] / weight), 1.0f / power);
            const float a = channels[A][i] / weight, b = channels[Bb][i] / weight;
            const float agree = qBound(0.0f, std::hypot(a, b) / qMax(1e-4f, channels[Chroma][i] / weight), 1.0f);
            const float gain = 1.0f + restore * (1.0f - agree);
            const std::array<float, 3> c = inGamut({qBound(0.0f, l, 1.0f), a * gain, b * gain});
            line[x] = qRgb(toSrgb(c[0]), toSrgb(c[1]), toSrgb(c[2]));
        }
    }
    return result;
}

/// Mean perceived lightness of \p image, 0 to 1.
qreal lightness(const QImage &image)
{
    if (image.isNull()) return 0;
    qreal sum = 0;
    int count = 0;
    const QImage small = image.scaled(16, 16, Qt::IgnoreAspectRatio, Qt::FastTransformation).convertToFormat(QImage::Format_RGB32);
    for (int y = 0; y < small.height(); y++) {
        const QRgb *line = reinterpret_cast<const QRgb *>(small.constScanLine(y));
        for (int x = 0; x < small.width(); x++, count++) sum += qGray(line[x]) / 255.0;
    }
    return count ? sum / count : 0;
}

/// \p image tinted by \p tint just strongly enough for text on it to stay readable.
QImage tintedForText(const QImage &image, const QColor &tint)
{
    const qreal contrast = qBound(0.0, qAbs(lightness(image) - tint.lightnessF()), 1.0);
    return tinted(image, tint, KisStudioGlass::tintOverDark() + (KisStudioGlass::tintOverBright() - KisStudioGlass::tintOverDark()) * contrast);
}

// ------------------------------------------------------------ where the canvas is

KisMainWindow *mainWindowOf(QWidget *widget)
{
    for (QWidget *w = widget; w; w = w->parentWidget()) {
        if (auto *mainWindow = qobject_cast<KisMainWindow *>(w)) return mainWindow;
    }
    return KisPart::instance()->currentMainwindow();
}

KisCanvas2 *canvasOf(QWidget *widget)
{
    KisMainWindow *mainWindow = mainWindowOf(widget);
    KisViewManager *view = mainWindow ? mainWindow->viewManager() : nullptr;
    return view ? view->canvasBase() : nullptr;
}

QWidget *canvasWidgetOf(QWidget *widget)
{
    KisCanvas2 *canvas = canvasOf(widget);
    return canvas ? canvas->canvasWidget() : nullptr;
}

bool drawnByCanvas(const QWidget *canvasWidget)
{
    return canvasWidget && canvasWidget->property(drawnByCanvasProperty).toBool();
}

// ------------------------------------------------------------ glass made on the CPU

/// Inputs for a backdrop: captured on the GUI thread, rendered on any thread.
struct Scene {
    KisPaintDeviceSP projection;
    QRect imageBounds;
    QTransform toWidget;
    qreal zoom {1};
    QSize canvasSize;
    QColor border;      ///< the canvas color around the image
    QColor transparent; ///< the average checker color, for transparent image areas
};

/// What is behind \p canvasRect (canvas widget pixels), blurred.
QImage render(const Scene &scene, const QRect &canvasRect, const QColor &outside)
{
    const qreal radius = blurRadius * KisStudioGlass::blur() * KisStudioGlass::blurScale(canvasRect.size());
    // Include the blur's reach so the edges blur like the middle.
    const int pad = qCeil(radius);
    const QRect area = canvasRect.adjusted(-pad, -pad, pad, pad);
    const QSize size(qMax(1, qCeil(area.width() * sampleScale)), qMax(1, qCeil(area.height() * sampleScale)));
    QImage image(size, QImage::Format_RGB32);
    image.fill(outside);

    QPainter painter(&image);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.scale(sampleScale, sampleScale);
    painter.translate(-area.topLeft());
    painter.fillRect(QRect(QPoint(0, 0), scene.canvasSize), scene.border);

    if (scene.projection) {
        const QRect imageArea = scene.toWidget.inverted().mapRect(QRectF(area)).toAlignedRect() & scene.imageBounds;
        if (!imageArea.isEmpty()) {
            // At roughly the resolution it is drawn at here.
            const QSize thumbSize(qBound(1, qCeil(imageArea.width() * scene.zoom * sampleScale), imageArea.width()),
                                  qBound(1, qCeil(imageArea.height() * scene.zoom * sampleScale), imageArea.height()));
            KisPaintDeviceSP projection = scene.projection;
            const QImage thumbnail = projection->createThumbnailUncached(thumbSize.width(), thumbSize.height(), imageArea, 2);
            painter.setTransform(scene.toWidget, true);
            painter.fillRect(imageArea, scene.transparent);
            painter.drawImage(QRectF(imageArea), thumbnail);
        }
    }
    painter.end();

    const QImage soft = blurred(image, qMax(1, qRound(radius * sampleScale)));
    const int cut = qRound(pad * sampleScale);
    return soft.copy(cut, cut, qMax(1, soft.width() - 2 * cut), qMax(1, soft.height() - 2 * cut));
}

/**
 * Caches the backdrop of each glass widget that is not over an OpenGL canvas, and re-renders it when
 * the canvas under it changes. The first backdrop is rendered immediately, later ones in the background.
 */
class Engine : public QObject
{
public:
    struct Entry {
        QRect rect;       ///< the part of the widget that is glass
        QRect globalRect; ///< where it was on screen when rendered
        QImage image;
    };

    static Engine &instance()
    {
        static Engine *engine = new Engine();
        return *engine;
    }

    /// The canvas behind \p rect of \p widget, blurred and tinted; a null image if there is none.
    Entry backdrop(QWidget *widget, const QRect &rect);

    /// Drops all backdrops after the glass settings change.
    void clear()
    {
        m_cache.clear();
        m_again.clear();
        m_epoch++;
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::Resize && !m_refresh.isActive()) m_refresh.start();
        return QObject::eventFilter(watched, event);
    }

private:
    Engine()
    {
        m_refresh.setSingleShot(true);
        m_refresh.setInterval(KisFrameRate::frameMs());
        connect(&m_refresh, &QTimer::timeout, this, [this]() { refresh(); });
        m_pool.setMaxThreadCount(1);
    }

    void follow(KisCanvas2 *canvas);
    void watch(QWidget *widget);
    void refresh();
    Scene scene() const;
    void renderLater(QWidget *widget);
    void rendered(QWidget *widget, const QPointer<QWidget> &guard, int epoch, const Entry &entry);

    QPointer<KisCanvas2> m_canvas;
    QPointer<QWidget> m_canvasWidget;
    KisImageWSP m_image;
    QMetaObject::Connection m_imageConnection;
    QMetaObject::Connection m_viewConnection;
    /// Image area changed since the last refresh, in image pixels.
    QRect m_imageChanged;
    QTransform m_transform;
    QSize m_canvasSize;

    QTimer m_refresh;
    QHash<QWidget *, Entry> m_cache;
    QSet<QWidget *> m_watched;
    QThreadPool m_pool;
    QSet<QWidget *> m_rendering;
    /// Widgets that changed again while rendering.
    QSet<QWidget *> m_again;
    /// Bumped whenever the backdrops become stale, so renders in flight are dropped.
    int m_epoch {0};
};

void Engine::follow(KisCanvas2 *canvas)
{
    if (canvas == m_canvas) {
        return;
    }
    if (m_canvasWidget) m_canvasWidget->removeEventFilter(this);
    disconnect(m_imageConnection);
    disconnect(m_viewConnection);
    m_canvas = canvas;
    m_canvasWidget = canvas ? canvas->canvasWidget() : nullptr;
    m_image = canvas ? canvas->image() : KisImageWSP();
    if (m_canvasWidget) m_canvasWidget->installEventFilter(this);
    if (canvas) {
        m_viewConnection = connect(canvas, &KisCanvas2::sigCanvasStateChanged, this, [this]() {
            if (!m_refresh.isActive()) m_refresh.start();
        });
    }
    if (m_image) {
        m_imageConnection = connect(m_image.data(), &KisImage::sigImageUpdated, this, [this](const QRect &rect) {
            m_imageChanged |= rect;
            if (!m_refresh.isActive()) m_refresh.start();
        });
    }
    clear();
}

void Engine::watch(QWidget *widget)
{
    if (m_watched.contains(widget)) {
        return;
    }
    m_watched.insert(widget);
    connect(widget, &QObject::destroyed, this, [this, widget]() {
        m_watched.remove(widget);
        m_cache.remove(widget);
        m_again.remove(widget);
    });
}

void Engine::refresh()
{
    // If the view moved, re-render all glass; if the image changed, only the glass near the change.
    bool moved = false;
    if (m_canvas && m_canvasWidget) {
        const QTransform transform = m_canvas->coordinatesConverter()->imageToWidgetTransform();
        const QSize size = m_canvasWidget->size();
        if (transform != m_transform || size != m_canvasSize) {
            m_transform = transform;
            m_canvasSize = size;
            moved = true;
        }
    }
    QRect changed;
    if (!m_imageChanged.isEmpty() && m_canvasWidget) {
        const QRect inWidget = m_transform.mapRect(QRectF(m_imageChanged)).toAlignedRect();
        const int reach = qCeil(blurRadius * KisStudioGlass::blur() * KisStudioGlass::maxBlurScale);
        changed = QRect(m_canvasWidget->mapToGlobal(inWidget.topLeft()), inWidget.size()).adjusted(-reach, -reach, reach, reach);
    }
    m_imageChanged = QRect();
    if (!moved && changed.isEmpty()) {
        return;
    }
    const QList<QWidget *> widgets = m_watched.values();
    for (QWidget *widget : widgets) {
        auto it = m_cache.find(widget);
        if (it == m_cache.end() || (!moved && !it->globalRect.intersects(changed))) {
            continue;
        }
        if (widget->isVisible()) {
            renderLater(widget);
        } else {
            // Re-rendered when shown again.
            m_cache.erase(it);
        }
    }
}

Scene Engine::scene() const
{
    Scene scene;
    if (!m_canvas || !m_canvasWidget) {
        return scene;
    }
    const KisConfig cfg(true);
    scene.border = cfg.canvasBorderColor();
    const QColor c1 = cfg.checkersColor1(), c2 = cfg.checkersColor2();
    scene.transparent = QColor((c1.red() + c2.red()) / 2, (c1.green() + c2.green()) / 2, (c1.blue() + c2.blue()) / 2);
    scene.canvasSize = m_canvasWidget->size();
    const KisCoordinatesConverter *converter = m_canvas->coordinatesConverter();
    scene.toWidget = converter->imageToWidgetTransform();
    scene.zoom = qMax(1e-3, converter->effectiveZoom());
    KisImageSP image = m_canvas->image();
    if (image) {
        scene.projection = image->projection();
        scene.imageBounds = image->bounds();
    }
    return scene;
}

Engine::Entry makeEntry(const Scene &scene, const QRect &rect, const QRect &globalRect, const QRect &canvasRect, const QColor &outside)
{
    Engine::Entry entry {rect, globalRect, QImage()};
    if (canvasRect.intersects(QRect(QPoint(), scene.canvasSize))) {
        entry.image = tintedForText(render(scene, canvasRect, outside), outside);
    }
    return entry;
}

void Engine::renderLater(QWidget *widget)
{
    if (m_rendering.contains(widget)) {
        m_again.insert(widget);
        return;
    }
    auto it = m_cache.constFind(widget);
    if (it == m_cache.constEnd() || !m_canvasWidget) {
        return;
    }
    const QRect rect = it->rect;
    const QRect globalRect(widget->mapToGlobal(rect.topLeft()), rect.size());
    const QRect canvasRect(m_canvasWidget->mapFromGlobal(globalRect.topLeft()), globalRect.size());
    const QColor outside = widget->palette().color(QPalette::Window);
    const QPointer<QWidget> guard(widget);
    const int epoch = m_epoch;
    m_rendering.insert(widget);
    m_pool.start([this, scene = scene(), rect, globalRect, canvasRect, outside, widget, guard, epoch]() mutable {
        const Entry entry = makeEntry(scene, rect, globalRect, canvasRect, outside);
        // Release the projection on the GUI thread.
        QMetaObject::invokeMethod(this, [this, widget, guard, epoch, entry, projection = std::move(scene.projection)]() {
            rendered(widget, guard, epoch, entry);
        }, Qt::QueuedConnection);
    });
}

void Engine::rendered(QWidget *widget, const QPointer<QWidget> &guard, int epoch, const Entry &entry)
{
    m_rendering.remove(widget);
    const bool again = m_again.remove(widget);
    auto it = m_cache.find(widget);
    // Drop it if the widget is gone, the canvas changed, or it was resized meanwhile.
    if (!guard || epoch != m_epoch || it == m_cache.end() || it->rect.size() != entry.rect.size()) {
        return;
    }
    *it = entry;
    if (widget->isVisible()) widget->update();
    if (again) renderLater(widget);
}

Engine::Entry Engine::backdrop(QWidget *widget, const QRect &rect)
{
    KisCanvas2 *canvas = canvasOf(widget);
    follow(canvas);
    if (!canvas || !m_canvasWidget || !m_canvasWidget->isVisible()) {
        return Entry();
    }
    watch(widget);

    const QRect globalRect(widget->mapToGlobal(rect.topLeft()), rect.size());
    auto it = m_cache.find(widget);
    if (it != m_cache.end() && it->rect.size() == rect.size()) {
        // Moved: re-render in the background and show the old one meanwhile.
        if (it->globalRect != globalRect) {
            it->rect = rect;
            renderLater(widget);
        }
        return *it;
    }
    // New or resized: render now.
    const QRect canvasRect(m_canvasWidget->mapFromGlobal(globalRect.topLeft()), globalRect.size());
    const Entry entry = makeEntry(scene(), rect, globalRect, canvasRect, widget->palette().color(QPalette::Window));
    m_cache.insert(widget, entry);
    return entry;
}

/// The solid dialog that \p widget's window (a menu or tooltip) is over, if any.
QWidget *windowBehind(const QWidget *widget)
{
    for (QWidget *w = widget->window()->parentWidget(); w; w = w->parentWidget()) {
        if (!w->isWindow()) continue;
        if (qobject_cast<const KisMainWindow *>(w)) return nullptr;
        if (!w->testAttribute(Qt::WA_TranslucentBackground)) return w;
    }
    // A parentless popup (Qt's tooltips on Windows) is over the active dialog it overlaps.
    const QWidget *window = widget->window();
    if (window->isWindow() && (window->windowType() == Qt::ToolTip || window->windowType() == Qt::Popup || window->windowType() == Qt::Tool)) {
        QWidget *active = QApplication::activeModalWidget();
        if (!active) active = QApplication::activeWindow();
        if (active && active != window && !qobject_cast<KisMainWindow *>(active) && !active->testAttribute(Qt::WA_TranslucentBackground)
            && active->frameGeometry().intersects(QRect(window->mapToGlobal(QPoint()), window->size()))) {
            return active;
        }
    }
    return nullptr;
}

/// The glass for \p rect of \p widget over the solid window \p behind; null if they don't overlap.
QImage glassOverWindow(QWidget *widget, const QRect &rect, QWidget *behind)
{
    const int reach = qCeil(blurRadius * KisStudioGlass::blur());
    const QRect global(widget->mapToGlobal(rect.topLeft()), rect.size());
    // kept a moment while it stays put over the same window: a menu repaints at every item the pointer passes,
    // and grabbing and blurring the window behind takes milliseconds
    struct Made {
        QPointer<QWidget> widget;
        QPointer<QWidget> behind;
        QRect global;
        QColor tint;
        qreal blur {0};
        QElapsedTimer age;
        QImage glass;
    };
    static QHash<QWidget *, Made> made;
    const QColor tint = widget->palette().color(QPalette::Window);
    auto known = made.constFind(widget);
    if (known != made.constEnd() && known->widget == widget && known->behind == behind && known->global == global
        && known->tint == tint && known->blur == KisStudioGlass::blur() && known->age.elapsed() < 1000) {
        return known->glass;
    }
    if (made.size() > 32) {
        for (auto it = made.begin(); it != made.end();) it = it->widget ? std::next(it) : made.erase(it);
    }
    auto keep = [&](const QImage &glass) {
        Made entry {widget, behind, global, tint, KisStudioGlass::blur(), QElapsedTimer(), glass};
        entry.age.start();
        made.insert(widget, entry);
        return glass;
    };
    const QRect inside(behind->mapFromGlobal(global.topLeft()), global.size());
    const QRect around = inside.adjusted(-reach, -reach, reach, reach) & behind->rect();
    if (around.isEmpty()) return QImage();
    QImage seen = behind->grab(around).toImage().convertToFormat(QImage::Format_RGB32);
    if (seen.isNull()) return QImage();
    const QSize small = (QSizeF(around.size()) * sampleScale).toSize().expandedTo(QSize(1, 1));
    seen = blurred(seen.scaled(small, Qt::IgnoreAspectRatio, Qt::SmoothTransformation), qMax(1, qRound(blurRadius * KisStudioGlass::blur() * sampleScale)));
    seen = tintedForText(seen, tint);
    const QRectF part(QPointF(inside.topLeft() - around.topLeft()) * sampleScale, QSizeF(inside.size()) * sampleScale);
    return keep(seen.copy(part.toAlignedRect()).scaled(rect.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
}

// ------------------------------------------------------------ glass drawn by the canvas

/// Whether \p a paints before (under) \p b; both must be in the same window.
bool paintedBefore(const QWidget *a, const QWidget *b)
{
    if (a == b || b->isAncestorOf(a)) return false;
    if (a->isAncestorOf(b)) return true;
    for (const QWidget *x = a; x; x = x->parentWidget()) {
        for (const QWidget *y = b; y; y = y->parentWidget()) {
            if (x != y && x->parentWidget() && x->parentWidget() == y->parentWidget()) {
                const QObjectList &siblings = x->parentWidget()->children();
                return siblings.indexOf(const_cast<QWidget *>(x)) < siblings.indexOf(const_cast<QWidget *>(y));
            }
        }
    }
    return false;
}

/// The widgets over \p canvas in its window, such as the tool rail and the zoom pill.
QList<QWidget *> overlaysOf(QWidget *canvas)
{
    QList<QWidget *> overlays;
    const QRect canvasRect(canvas->mapToGlobal(QPoint()), canvas->size());
    auto add = [&](QObject *object) {
        QWidget *w = qobject_cast<QWidget *>(object);
        if (w && !w->isWindow() && canvasRect.intersects(QRect(w->mapToGlobal(QPoint()), w->size()))) overlays << w;
    };
    for (QObject *child : canvas->children()) add(child);
    // Siblings stacked above the canvas or above its ancestors.
    for (QWidget *w = canvas; !w->isWindow() && w->parentWidget(); w = w->parentWidget()) {
        const QObjectList &siblings = w->parentWidget()->children();
        for (int i = siblings.indexOf(w) + 1; i < siblings.size(); i++) add(siblings[i]);
    }
    return overlays;
}

bool isTooltip(const QWidget *widget);

/// Tracks the glass panels over canvases that draw the glass themselves.
class Panels : public QObject
{
public:
    struct Panel {
        QRectF rect;
        qreal radius {0};
        /// The canvas has drawn its glass since it was shown; until then the panel is solid.
        bool drawn {false};
        /// The part covered by a glass window above it, in its own coordinates; not painted.
        QRegion covered;
        /// Where its glass may still be on a canvas, in that canvas' coordinates.
        QRect shownAt;
        QPointer<QWidget> shownOn;
    };

    static Panels &instance()
    {
        static Panels *panels = new Panels();
        return *panels;
    }

    /// Registers \p rect of \p widget as glass. Returns whether the canvas has drawn its glass yet.
    bool paint(QWidget *widget, const QRectF &rect, qreal radius)
    {
        auto it = m_panels.find(widget);
        if (it == m_panels.end()) {
            m_panels.insert(widget, {rect, radius, false, QRegion(), QRect(), QPointer<QWidget>()});
            if (!shownOrder(widget->window())) noteShown(widget->window());
            widget->installEventFilter(this);
            if (widget->window() != widget) widget->window()->installEventFilter(this);
            connect(widget, &QObject::destroyed, this, [this, widget]() {
                m_panels.remove(widget);
                m_shown.remove(widget);
                showPlainAgain();
            });
            changedAfterPainting(widget);
            return false;
        }
        if (it->rect != rect || it->radius != radius) {
            it->rect = rect;
            it->radius = radius;
            changedAfterPainting(widget);
        }
        return it->drawn;
    }

    /// Unregisters \p widget.
    void forget(QWidget *widget)
    {
        if (!m_panels.remove(widget)) {
            return;
        }
        m_shown.remove(widget);
        disconnect(widget, &QObject::destroyed, this, nullptr);
        widget->removeEventFilter(this);
        QWidget *window = widget->window();
        if (window != widget && std::none_of(m_panels.keyBegin(), m_panels.keyEnd(), [window](QWidget *panel) { return panel->window() == window; })) {
            window->removeEventFilter(this);
        }
        showPlainAgain();
    }

    /**
     * Calls changed() once painting is done. Qt drops canvas updates requested while the window
     * paints, so a panel shown with its window would otherwise get its glass only where the canvas
     * happens to redraw next.
     */
    void changedAfterPainting(QWidget *widget)
    {
        QMetaObject::invokeMethod(this, [this, guard = QPointer<QWidget>(widget)]() {
            if (guard) changed(guard);
        }, Qt::QueuedConnection);
    }

    void showPlainAgain()
    {
        for (QWidget *canvas : qAsConst(m_canvases)) {
            if (!canvas) continue;
            canvas->update();
            hideUnderWindows(canvas);
        }
    }

    /// Hides the glass tooltips \p menu overlaps: Qt paints a window's content unclipped, so a tooltip under
    /// a menu would show through the menu's glass (a layer's picture stays up after a right-click)
    void hideTooltipsUnder(QWidget *menu)
    {
        const QRect area(menu->mapToGlobal(QPoint()), menu->size());
        QList<QWidget *> under;
        for (auto it = m_panels.constBegin(); it != m_panels.constEnd(); ++it) {
            QWidget *widget = it.key();
            if (widget != menu && widget->isVisible() && isTooltip(widget) && area.intersects(QRect(widget->mapToGlobal(QPoint()), widget->size()))) {
                under << widget;
            }
        }
        Q_FOREACH (QWidget *tooltip, under) tooltip->hide();
    }

    bool isDrawn(QWidget *widget) const
    {
        return m_panels.value(widget).drawn;
    }

    void repaintAll()
    {
        for (auto it = m_panels.constBegin(); it != m_panels.constEnd(); ++it) it.key()->update();
        for (QWidget *canvas : qAsConst(m_canvases)) {
            if (canvas) canvas->update();
        }
    }

    QRegion covered(QWidget *widget) const
    {
        return m_panels.value(widget).covered;
    }

    /**
     * Redraws the canvases under \p widget and the other panels in its window: where their glass is now
     * and where it was drawn before, or \p everywhere.
     */
    void changed(QWidget *widget, bool everywhere = false)
    {
        if (KisUpdatePacer::logging()) KisUpdatePacer::log(QStringLiteral("glass changed ") + QString::fromLatin1(widget->metaObject()->className()) + QLatin1Char(' ') + widget->objectName());
        QWidget *window = widget->window();
        QHash<QWidget *, QRegion> canvases;
        for (auto it = m_panels.constBegin(); it != m_panels.constEnd(); ++it) {
            QWidget *panel = it.key();
            if (panel != widget && panel->window() != window) {
                continue;
            }
            QWidget *canvas = canvasWidgetOf(panel);
            if (drawnByCanvas(canvas)) {
                const QRect now = it->rect.translated(panel->mapToGlobal(QPoint()) - canvas->mapToGlobal(QPoint())).toAlignedRect();
                canvases[canvas] += now.adjusted(-2, -2, 2, 2) & canvas->rect();
            }
            QWidget *before = it->shownOn.data();
            if (drawnByCanvas(before)) canvases[before] += it->shownAt;
        }
        for (auto it = canvases.constBegin(); it != canvases.constEnd(); ++it) {
            if (everywhere) {
                it.key()->update();
            } else {
                it.key()->update(it.value());
            }
            hideUnderWindows(it.key());
        }
    }

    /**
     * The canvas draws the glass of all panels at once, so a lower panel's content would show through
     * the glass of a window above it. Masks the canvas' overlays and the content of glass windows
     * wherever a higher glass window (or a panel set over the overlays) covers them.
     */
    void hideUnderWindows(QWidget *canvas)
    {
        // another image's canvas has no panels over it, and would unmask what the current one masked
        if (canvasWidgetOf(canvas) != canvas) {
            return;
        }
        // Glass above the overlays, lowest first, and their shapes on screen.
        QList<QWidget *> windows;
        for (auto it = m_panels.constBegin(); it != m_panels.constEnd(); ++it) {
            QWidget *panel = it.key();
            const bool over = panel->window() != canvas->window() || panel->property(overOverlaysProperty).toBool();
            if (over && panel->isVisible() && canvasWidgetOf(panel) == canvas) {
                windows << panel;
            }
        }
        std::sort(windows.begin(), windows.end(), [this, canvas](QWidget *a, QWidget *b) { return below(a, b, canvas); });
        QVector<QRegion> shapes;
        for (QWidget *window : qAsConst(windows)) {
            const Panel &panel = m_panels[window];
            const QPolygon shape = KisModernStyle::squircle(panel.rect, panel.radius).toFillPolygon().toPolygon();
            shapes << QRegion(shape).translated(window->mapToGlobal(QPoint()));
        }

        Q_FOREACH (QWidget *overlay, overlaysOf(canvas)) {
            QRegion above;
            for (int i = 0; i < windows.size(); i++) {
                QWidget *glass = windows[i];
                if (glass->window() != canvas->window()
                    || (glass != overlay && !glass->isAncestorOf(overlay) && !overlay->isAncestorOf(glass) && paintedBefore(overlay, glass))) {
                    above |= shapes[i];
                }
            }
            hide({overlay}, above);
        }

        for (int i = 0; i < windows.size(); i++) {
            QRegion above;
            for (int j = i + 1; j < windows.size(); j++) above |= shapes[j];
            QWidget *window = windows[i];
            Panel &panel = m_panels[window];
            const QRegion covered = above.translated(-window->mapToGlobal(QPoint())) & window->rect();
            if (panel.covered != covered) {
                panel.covered = covered;
                window->update();
            }
            QList<QWidget *> content;
            for (QObject *child : window->children()) {
                QWidget *w = qobject_cast<QWidget *>(child);
                if (w && !w->isWindow()) content << w;
            }
            hide(content, above);
        }
        // Unmask panels that are no longer covered.
        for (auto it = m_panels.begin(); it != m_panels.end(); ++it) {
            if (!windows.contains(it.key()) && !it->covered.isEmpty()) {
                it->covered = QRegion();
                hide(it.key()->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly), QRegion());
                it.key()->update();
            }
        }
    }

    /// Masks \p widgets out where they intersect \p region (global coordinates).
    static void hide(const QList<QWidget *> &widgets, const QRegion &region)
    {
        for (QWidget *w : widgets) {
            const bool masked = w->property(maskedProperty).toBool();
            if (!masked && !w->mask().isEmpty()) {
                // Leave widgets with their own mask alone.
                continue;
            }
            const QRegion hidden = region.translated(-w->mapToGlobal(QPoint())) & w->rect();
            if (hidden.isEmpty() || !w->isVisible()) {
                if (masked) {
                    w->clearMask();
                    w->setProperty(maskedProperty, false);
                }
                continue;
            }
            QRegion shown = QRegion(w->rect()) - hidden;
            if (shown.isEmpty()) {
                // An empty mask means no mask, so use one pixel outside instead.
                shown = QRegion(-1, -1, 1, 1);
            }
            if (!masked || w->mask() != shown) {
                w->setMask(shown);
                w->setProperty(maskedProperty, true);
            }
        }
    }

    /// The panels over \p canvasWidget in stacking order.
    QVector<KisStudioGlass::Surface> over(const QWidget *canvasWidget)
    {
        QVector<KisStudioGlass::Surface> surfaces;
        if (!KisStudioGlass::isEnabled()) {
            return surfaces;
        }
        const QPoint canvasOrigin = canvasWidget->mapToGlobal(QPoint());
        // Hidden menus and tooltips stay registered, so leave them out before sorting.
        QList<QPair<QWidget *, qreal>> shown;
        for (auto it = m_panels.constBegin(); it != m_panels.constEnd(); ++it) {
            QWidget *widget = it.key();
            if (!widget->isVisible() || canvasWidgetOf(widget) != canvasWidget) {
                continue;
            }
            const qreal opacity = opacityOf(widget);
            if (opacity <= 0) {
                // A panel fading in from zero stays solid until its glass is drawn.
                continue;
            }
            shown.append(qMakePair(widget, opacity));
        }
        std::sort(shown.begin(), shown.end(), [this, canvasWidget](const QPair<QWidget *, qreal> &a, const QPair<QWidget *, qreal> &b) {
            return below(a.first, b.first, canvasWidget);
        });
        for (const QPair<QWidget *, qreal> &entry : qAsConst(shown)) {
            QWidget *widget = entry.first;
            const Panel &panel = m_panels[widget];
            const QPointF offset = widget->mapToGlobal(QPoint()) - canvasOrigin;
            surfaces.append({panel.rect.translated(offset), panel.radius, widget->palette().color(QPalette::Window), entry.second});
        }
        return surfaces;
    }

    /**
     * \p canvasWidget drew a frame over \p painted (empty means all of it). Panels it covered fully become
     * see-through. A panel drawn only in part (e.g. a brush outline update) stays solid and the canvas is
     * asked to redraw all of it; otherwise it would show the canvas without glass until the next redraw.
     */
    void drewOver(QWidget *canvasWidget, const QRect &painted)
    {
        const QPoint canvasOrigin = canvasWidget->mapToGlobal(QPoint());
        for (auto it = m_panels.begin(); it != m_panels.end(); ++it) {
            QWidget *widget = it.key();
            Panel &panel = it.value();
            const bool on = widget->isVisible() && canvasWidgetOf(widget) == canvasWidget && opacityOf(widget) > 0;
            if (!on && panel.shownOn != canvasWidget) {
                continue;
            }
            const QRect area = on ? panel.rect.translated(widget->mapToGlobal(QPoint()) - canvasOrigin).toAlignedRect() & canvasWidget->rect() : QRect();
            // Its glass is now where it was drawn, and still where it was before but not drawn over.
            const bool kept = panel.shownOn == canvasWidget && !painted.isEmpty() && !painted.contains(panel.shownAt);
            panel.shownAt = (kept ? panel.shownAt : QRect()) | (area.isEmpty() ? QRect() : area.adjusted(-1, -1, 1, 1));
            panel.shownOn = panel.shownAt.isEmpty() ? nullptr : canvasWidget;
            if (!on || panel.drawn) {
                continue;
            }
            if (painted.isEmpty() || painted.contains(area)) {
                panel.drawn = true;
                // Turn see-through only after the frame with its glass is presented.
                QMetaObject::invokeMethod(widget, QOverload<>::of(&QWidget::update), Qt::QueuedConnection);
            } else if (!area.isEmpty()) {
                canvasWidget->update(area);
            }
        }
    }

    void addCanvas(QWidget *canvas)
    {
        if (!m_canvases.contains(canvas)) m_canvases.append(canvas);
    }

    void repaintOver(QWidget *canvas)
    {
        for (auto it = m_panels.constBegin(); it != m_panels.constEnd(); ++it) {
            if (canvasWidgetOf(it.key()) == canvas) it.key()->update();
        }
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        QWidget *widget = static_cast<QWidget *>(watched);
        switch (event->type()) {
        case QEvent::Hide:
            // When shown again it is solid until the canvas draws its glass.
            for (auto it = m_panels.begin(); it != m_panels.end(); ++it) {
                if (it.key() == watched || it.key()->window() == watched) it->drawn = false;
            }
            changed(widget);
            break;
        case QEvent::Show:
            // A window shown later stacks above those shown before.
            if (widget->isWindow()) noteShown(widget);
            changed(widget);
            break;
        case QEvent::Move:
        case QEvent::Resize:
        case QEvent::PaletteChange:
            changed(widget);
            break;
        case QEvent::WindowStateChange:
            changed(widget, true);
            break;
        default:
            break;
        }
        return false;
    }

private:
    /// The window opacity multiplied by any opacity effects.
    static qreal opacityOf(QWidget *widget)
    {
        qreal opacity = widget->window()->windowOpacity();
        for (QWidget *w = widget; w; w = w->parentWidget()) {
            if (auto *effect = qobject_cast<QGraphicsOpacityEffect *>(w->graphicsEffect())) {
                if (effect->isEnabled()) opacity *= effect->opacity();
            }
            if (w->isWindow()) break;
        }
        return opacity;
    }

    /// Stacking order: panels in the canvas' window first, then other windows in the order they were shown;
    /// paint order within one window.
    bool below(QWidget *a, QWidget *b, const QWidget *canvas) const
    {
        const bool aOwn = a->window() != canvas->window(), bOwn = b->window() != canvas->window();
        if (aOwn != bOwn) return bOwn;
        if (a->window() != b->window()) return shownOrder(a->window()) < shownOrder(b->window());
        return paintedBefore(a, b);
    }

    void noteShown(QWidget *window)
    {
        m_shown.insert(window, {window, ++m_shownCount});
    }

    /// When \p window was shown, counting up; 0 if unknown. An entry left by a destroyed window at the
    /// same address does not count.
    quint64 shownOrder(QWidget *window) const
    {
        const Shown shown = m_shown.value(window);
        return shown.window == window ? shown.order : 0;
    }

    struct Shown {
        QPointer<QWidget> window;
        quint64 order {0};
    };

    QHash<QWidget *, Panel> m_panels;
    QList<QPointer<QWidget>> m_canvases;
    QHash<QWidget *, Shown> m_shown;
    quint64 m_shownCount {0};
};

// ------------------------------------------------------------ see-through panels

/// White or black, just opaque enough over \p under to reach \p color's lightness (minimum 0.04).
QColor veilFor(const QColor &color, const QColor &under)
{
    const qreal c = color.lightnessF();
    const qreal u = under.lightnessF();
    const bool lighter = c >= u;
    const qreal room = lighter ? 1.0 - u : u;
    const qreal amount = room > 0 ? qBound(0.04, qAbs(c - u) / room, 1.0) : 0.0;
    QColor veil = lighter ? QColor(Qt::white) : QColor(Qt::black);
    veil.setAlphaF(amount);
    return veil;
}

/// Makes the QML views (KisQQuickWidget) in \p widget see-through as well.
void setQmlGlass(QWidget *widget, bool on)
{
    Q_FOREACH (QWidget *child, widget->findChildren<QWidget *>()) {
        if (child->inherits("KisQQuickWidget")) child->setProperty("studioGlassQml", on);
    }
}

/// A list on a see-through panel does not fill its viewport.
void setListOnGlass(QAbstractItemView *view, bool on)
{
    QWidget *viewport = view->viewport();
    if (!viewport) return;
    if (on && viewport->autoFillBackground()) {
        viewport->setAutoFillBackground(false);
        view->setProperty(unfilledViewProperty, true);
        viewport->update();
    } else if (!on && view->property(unfilledViewProperty).toBool()) {
        viewport->setAutoFillBackground(true);
        view->setProperty(unfilledViewProperty, false);
        viewport->update();
    }
}

/// Whether \p widget is inside a see-through panel within its window.
bool inSeeThrough(const QWidget *widget)
{
    for (const QWidget *w = widget; w; w = w->parentWidget()) {
        if (w->property(seeThroughProperty).toBool()) return true;
        if (w->isWindow()) break;
    }
    return false;
}

/// A combo box's dropdown window, if it is a separate window.
QWidget *dropdownOf(QComboBox *box)
{
    QWidget *popup = box->view() ? box->view()->window() : nullptr;
    return popup != box->window() ? popup : nullptr;
}

/**
 * Rebuilds the see-through widgets' veils from the new colors when the theme changes. Qt only notifies
 * windows of application palette changes, so panels with their own palette are notified here.
 */
class ThemeFollower : public QObject
{
public:
    static ThemeFollower *instance()
    {
        static ThemeFollower *follower = new ThemeFollower();
        return follower;
    }

    void follow(QWidget *widget)
    {
        if (!m_widgets.contains(widget)) m_widgets.append(widget);
    }

    void forget(QWidget *widget)
    {
        m_widgets.removeAll(widget);
    }

private:
    ThemeFollower()
    {
        // Deferred until every widget has the new palette.
        connect(qApp, &QGuiApplication::paletteChanged, this, [this]() {
            if (!m_pending) {
                m_pending = true;
                QTimer::singleShot(0, this, [this]() { reapply(); });
            }
        });
    }

    void reapply()
    {
        m_pending = false;
        const QList<QPointer<QWidget>> widgets = m_widgets;
        for (const QPointer<QWidget> &widget : widgets) {
            if (!widget || !widget->property(seeThroughProperty).toBool()) continue;
            // Rebuild from the new theme; a saved palette belongs to the old one.
            widget->setProperty(savedPaletteProperty, QVariant());
            KisStudioGlass::setSeeThrough(widget, false);
            KisStudioGlass::setSeeThrough(widget, true);
            QEvent changed(QEvent::ApplicationPaletteChange);
            QCoreApplication::sendEvent(widget, &changed);
        }
        m_widgets.removeAll(QPointer<QWidget>());
    }

    QList<QPointer<QWidget>> m_widgets;
    bool m_pending {false};
};

} // namespace

void KisStudioGlass::setSeeThrough(QWidget *widget, bool on)
{
    if (on == widget->property(seeThroughProperty).toBool()) {
        return;
    }
    widget->setProperty(seeThroughProperty, on);
    if (on) {
        widget->setProperty(savedPaletteProperty, widget->testAttribute(Qt::WA_SetPalette) ? QVariant::fromValue(widget->palette()) : QVariant());
        // Fields and buttons become white or black veils, as light or dark as before without muting the glass.
        QPalette palette = widget->palette();
        for (QPalette::ColorGroup group : {QPalette::Active, QPalette::Inactive, QPalette::Disabled}) {
            QColor window = palette.color(group, QPalette::Window);
            for (QPalette::ColorRole role : {QPalette::Button, QPalette::Base, QPalette::AlternateBase}) {
                palette.setColor(group, role, veilFor(palette.color(group, role), window));
            }
            window.setAlpha(0);
            palette.setColor(group, QPalette::Window, window);
        }
        widget->setPalette(palette);
        setQmlGlass(widget, true);
        // Lists created later are handled when polished (see MenuGlass).
        Q_FOREACH (QAbstractItemView *view, widget->findChildren<QAbstractItemView *>()) {
            if (view->window() == widget->window()) setListOnGlass(view, true);
        }
        // A dropdown is a separate window, tinted like a menu with the box's own palette.
        Q_FOREACH (QComboBox *box, widget->findChildren<QComboBox *>()) {
            if (QWidget *popup = dropdownOf(box)) popup->setPalette(QApplication::palette(box));
        }
        ThemeFollower::instance()->follow(widget);
    } else {
        ThemeFollower::instance()->forget(widget);
        const QVariant saved = widget->property(savedPaletteProperty);
        if (saved.isValid()) {
            widget->setPalette(saved.value<QPalette>());
        } else {
            widget->setPalette(QPalette());
            widget->setAttribute(Qt::WA_SetPalette, false);
        }
        widget->setProperty(savedPaletteProperty, QVariant());
        setQmlGlass(widget, false);
        Q_FOREACH (QAbstractItemView *view, widget->findChildren<QAbstractItemView *>()) {
            if (!inSeeThrough(view)) setListOnGlass(view, false);
        }
    }
}

namespace {

// Settings are cached because glass is painted often and drawn every frame.
bool &enabled()
{
    static bool on = KisConfig(true).readEntry<bool>(settingKey, true);
    return on;
}

qreal &blurSetting()
{
    static qreal blur = qBound<qreal>(0, KisConfig(true).readEntry<qreal>(blurKey, 1.0), 2.0);
    return blur;
}

qreal &rimLightSetting()
{
    static qreal strength = qBound<qreal>(0, KisConfig(true).readEntry<qreal>(rimLightKey, 1.0), 2.0);
    return strength;
}

qreal &opacitySetting()
{
    static qreal opacity = qBound<qreal>(0, KisConfig(true).readEntry<qreal>(opacityKey, 0.7), 1.0);
    return opacity;
}

bool &ambientSetting()
{
    static bool ambient = []() {
        auto black = []() { return KisConfig(true).canvasBorderColor() == QColor(0, 0, 0); };
        QObject::connect(KisConfigNotifier::instance(), &KisConfigNotifier::configChanged, [black]() { ambientSetting() = black(); });
        return black();
    }();
    return ambient;
}

/// Paints \p widget's background as a glass panel with a border.
void paintMenuGlass(QWidget *widget, qreal radius)
{
    QPainter painter(widget);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QPainterPath shape = KisStudioGlass::paint(painter, widget, QRectF(widget->rect()).adjusted(0.5, 0.5, -0.5, -0.5), radius);
    QColor border = widget->palette().color(QPalette::WindowText);
    border.setAlphaF(0.12);
    KisStudioGlass::paintBorder(painter, widget, shape, border);
}

/// Must be called before the native window exists. Frameless, since Windows only makes those see-through.
void makeSeeThroughWindow(QWidget *widget)
{
    widget->overrideWindowFlags(widget->windowFlags() | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
    widget->setAttribute(Qt::WA_TranslucentBackground, true);
    widget->setProperty(glassMenuProperty, true);
}

/// Makes a dropdown window (a combo box's frame or a completer's list) glass, if not created yet.
void makeGlassDropdown(QWidget *popup)
{
    if (!popup || popup->testAttribute(Qt::WA_WState_Created) || popup->property(glassDropdownProperty).toBool()
        || !KisStudioGlass::isEnabled() || !KisStudioGlass::seeThroughWindows()) {
        return;
    }
    makeSeeThroughWindow(popup);
    popup->setProperty(glassDropdownProperty, true);
    // Rows sit directly on the glass, inset from its rounded corners.
    if (auto *frame = qobject_cast<QFrame *>(popup)) frame->setFrameStyle(QFrame::NoFrame);
    if (auto *view = qobject_cast<QAbstractItemView *>(popup)) {
        setListOnGlass(view, true);
    } else {
        popup->setContentsMargins(KisModernStyle::space1, KisModernStyle::space1, KisModernStyle::space1, KisModernStyle::space1);
    }
    Q_FOREACH (QAbstractItemView *view, popup->findChildren<QAbstractItemView *>()) setListOnGlass(view, true);
}

/// Whether \p widget is a dialog over the main window that should become a glass panel. Standard dialogs
/// (files, colors, fonts, printing) stay as they are.
bool becomesPanel(QWidget *widget)
{
    auto *dialog = qobject_cast<QDialog *>(widget);
    if (!dialog || dialog->testAttribute(Qt::WA_WState_Created) || !KisStudioGlass::isEnabled() || !KisStudioGlass::seeThroughWindows()) {
        return false;
    }
    if (dialog->inherits("QFileDialog") || dialog->inherits("QColorDialog") || dialog->inherits("QFontDialog") || dialog->inherits("QPrintDialog")) {
        return false;
    }
    // Only dialogs over the main window, not over another dialog.
    QWidget *parent = dialog->parentWidget();
    return parent && parent->window()->inherits("KisMainWindow");
}

/// Lets a frameless glass dialog be dragged by any part that is not a control.
class DialogMover : public QObject
{
protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        // on every child of a glass popup, so the event's type first
        const QEvent::Type type = event->type();
        if (type != QEvent::MouseButtonPress && type != QEvent::MouseMove && type != QEvent::MouseButtonRelease) return false;
        if ((type == QEvent::MouseMove || type == QEvent::MouseButtonRelease) && !m_moving) return false;
        auto *widget = qobject_cast<QWidget *>(watched);
        if (!widget) return false;
        QWidget *dialog = widget->window();
        if (!dialog->property(glassDialogProperty).toBool() && !dialog->property(movableProperty).toBool()) return false;
        switch (type) {
        case QEvent::MouseButtonPress: {
            auto *mouse = static_cast<QMouseEvent *>(event);
            // Presses on the background, labels, group boxes or plain frames start a drag.
            // (a plain widget that follows the pointer draws something to click: a grid of items, a handle)
            const bool background = widget == dialog || qobject_cast<QLabel *>(widget) || qobject_cast<QGroupBox *>(widget)
                || (widget->metaObject() == &QWidget::staticMetaObject && !widget->hasMouseTracking())
                || (qobject_cast<QFrame *>(widget) && !qobject_cast<QAbstractScrollArea *>(widget));
            if (mouse->button() == Qt::LeftButton && background) {
                m_moving = true;
                m_grab = mouse->globalPos() - dialog->frameGeometry().topLeft();
                return true;
            }
            break;
        }
        case QEvent::MouseMove:
            if (m_moving) {
                dialog->move(static_cast<QMouseEvent *>(event)->globalPos() - m_grab);
                return true;
            }
            break;
        case QEvent::MouseButtonRelease:
            if (m_moving) {
                m_moving = false;
                return true;
            }
            break;
        default:
            break;
        }
        return false;
    }

private:
    QPoint m_grab;
    bool m_moving {false};
};

DialogMover *dialogMover()
{
    static DialogMover *mover = new DialogMover();
    return mover;
}

bool isTooltip(const QWidget *widget)
{
    // Qt's own, and the item tooltips of resource lists (brushes, patterns…)
    return widget->windowType() == Qt::ToolTip && (widget->inherits("QTipLabel") || widget->inherits("KoItemToolTip"));
}

/// Tints a glass tooltip like a menu rather than with the near-black tooltip color
void tintGlassTooltip(QWidget *tooltip)
{
    QPalette palette = tooltip->palette();
    palette.setColor(QPalette::Window, QApplication::palette().color(QPalette::Window));
    tooltip->setPalette(palette);
}

void makeGlassTooltip(QWidget *tooltip)
{
    makeSeeThroughWindow(tooltip);
    tintGlassTooltip(tooltip);
}

/**
 * Windows fades menus and tooltips in through the window opacity without notifying Qt, so the canvas would
 * draw their glass only once, at the faint initial opacity; a combo box's list rolls out as a grabbed picture.
 */
void stopFades()
{
    QApplication::setEffectEnabled(Qt::UI_AnimateMenu, false);
    QApplication::setEffectEnabled(Qt::UI_FadeTooltip, false);
    QApplication::setEffectEnabled(Qt::UI_AnimateTooltip, false);
    QApplication::setEffectEnabled(Qt::UI_AnimateCombo, false);
}

/// Makes menus, tooltips, dropdowns and dialogs glass as they are polished, and paints their glass.
class MenuGlass : public QObject
{
protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        const QEvent::Type type = event->type();
        // A dialog's native window can be created before it is polished, so decide when its first child is added.
        if (type == QEvent::ChildAdded) {
            auto *dialog = qobject_cast<QDialog *>(watched);
            if (dialog && !dialog->property(glassDialogProperty).isValid()) {
                const bool panel = becomesPanel(dialog);
                dialog->setProperty(glassDialogProperty, panel);
                if (panel) makeSeeThroughWindow(dialog);
            }
            return false;
        }
        if (type == QEvent::ApplicationPaletteChange) {
            // a tooltip a list keeps (resource lists do) takes the new theme's tint, not the one it was made in
            auto *tooltip = qobject_cast<QWidget *>(watched);
            if (tooltip && isTooltip(tooltip) && tooltip->testAttribute(Qt::WA_TranslucentBackground)) tintGlassTooltip(tooltip);
            return false;
        }
        if (type != QEvent::Polish && type != QEvent::Paint && type != QEvent::MouseButtonPress && type != QEvent::KeyPress
            && type != QEvent::Show) {
            return false;
        }
        QWidget *widget = qobject_cast<QWidget *>(watched);
        if (!widget) {
            return false;
        }
        // everything painted or shown here is a window: every other widget's paint passes straight through
        if ((type == QEvent::Paint || type == QEvent::Show) && (!widget->isWindow() || !widget->property(glassMenuProperty).toBool())) {
            return false;
        }
        if (type == QEvent::Show) {
            if (qobject_cast<QMenu *>(widget)) Panels::instance().hideTooltipsUnder(widget);
            return false;
        }
        if (type == QEvent::MouseButtonPress || type == QEvent::KeyPress) {
            // A combo box polished before this filter was installed, about to open its list.
            if (auto *box = qobject_cast<QComboBox *>(widget)) makeGlassDropdown(dropdownOf(box));
            return false;
        }
        if (type == QEvent::Polish) {
            if (auto *view = qobject_cast<QAbstractItemView *>(widget)) {
                if (inSeeThrough(view)) setListOnGlass(view, true);
                // A completer's list.
                if (view->windowType() == Qt::Popup) makeGlassDropdown(view);
            } else if (auto *box = qobject_cast<QComboBox *>(widget)) {
                makeGlassDropdown(dropdownOf(box));
            } else if (widget->property(glassDialogProperty).toBool() && !widget->property(glassDialogDoneProperty).toBool()) {
                // A glass panel over the canvas, dragged by its background.
                DialogMover *mover = dialogMover();
                widget->setProperty(glassDialogDoneProperty, true);
                widget->setContentsMargins(KisModernStyle::space2, KisModernStyle::space2, KisModernStyle::space2, KisModernStyle::space2);
                widget->installEventFilter(mover);
                Q_FOREACH (QWidget *child, widget->findChildren<QWidget *>()) child->installEventFilter(mover);
                KisStudioGlass::setSeeThrough(widget, true);
            } else if ((qobject_cast<QMenu *>(widget) || isTooltip(widget)) && !widget->testAttribute(Qt::WA_WState_Created)
                       && KisStudioGlass::isEnabled() && KisStudioGlass::seeThroughWindows()) {
                if (isTooltip(widget)) {
                    makeGlassTooltip(widget);
                } else {
                    makeSeeThroughWindow(widget);
                }
            }
        } else if (widget->property(glassDialogProperty).toBool()) {
            paintMenuGlass(widget, KisModernStyle::panelRadius);
        } else if (qobject_cast<QMenu *>(widget) || isTooltip(widget) || widget->property(glassDropdownProperty).toBool()) {
            paintMenuGlass(widget, KisModernStyle::cornerRadius + 2);
        }
        return false;
    }
};

class PopupFrameGlass : public QObject
{
protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::Paint && watched->property(glassMenuProperty).toBool()) {
            paintMenuGlass(static_cast<QWidget *>(watched), KisModernStyle::cornerRadius + 6);
        }
        return false;
    }
};

/// Makes KisPopupButton's popup glass whenever its window is (re)created, including when pinned.
void decoratePopupFrame(QFrame *frame, bool detached, bool holdsMenu)
{
    static PopupFrameGlass *painter = new PopupFrameGlass();
    const bool glass = KisStudioGlass::isEnabled() && KisStudioGlass::seeThroughWindows() && !holdsMenu;
    if (glass) {
        makeSeeThroughWindow(frame);
        frame->installEventFilter(painter);
        KisStudioGlass::setMovableByBackground(frame);
        frame->setFrameStyle(QFrame::NoFrame);
        // When pinned, leave a strip at the top for dragging and the pin button.
        frame->layout()->setContentsMargins(8, detached ? 30 : 8, 8, 8);
    } else {
        // KisPopupButton restores the window flags and frame itself.
        frame->setAttribute(Qt::WA_TranslucentBackground, false);
        frame->setProperty(glassMenuProperty, false);
        frame->removeEventFilter(painter);
        Panels::instance().forget(frame);
    }
    // The frame keeps its color, which tints the glass.
    Q_FOREACH (QWidget *content, frame->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly)) {
        KisStudioGlass::setSeeThrough(content, glass);
    }
}

/// A squircle's shadow as black with alpha: gaussian-blurred, at most \p strength, dithered and cached.
QImage shadowImage(const QSizeF &panel, qreal radius, int margin, qreal sigma, qreal strength, qreal ratio)
{
    static QHash<QString, QImage> cache;
    const QString key = QStringLiteral("%1 %2 %3 %4 %5 %6 %7").arg(panel.width()).arg(panel.height()).arg(radius).arg(margin).arg(sigma).arg(strength).arg(ratio);
    auto it = cache.constFind(key);
    if (it != cache.constEnd()) {
        return *it;
    }

    const QSize size = (QSizeF(panel.width() + 2 * margin, panel.height() + 2 * margin) * ratio).toSize();
    QImage mask(size, QImage::Format_Alpha8);
    mask.fill(0);
    {
        QPainter painter(&mask);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.scale(ratio, ratio);
        painter.fillPath(KisModernStyle::squircle(QRectF(QPointF(margin, margin), panel), radius), Qt::black);
    }

    const int w = size.width(), h = size.height();
    std::vector<float> alpha(size_t(w) * h);
    for (int y = 0; y < h; y++) {
        const uchar *line = mask.constScanLine(y);
        for (int x = 0; x < w; x++) alpha[size_t(y) * w + x] = line[x] / 255.0f;
    }
    const int box = qMax(1, int(std::lround(sigma * ratio / std::sqrt(3.0))));
    std::vector<float> scratch;
    for (int pass = 0; pass < 3; pass++) {
        for (int y = 0; y < h; y++) boxLine(alpha.data() + size_t(y) * w, w, 1, box, scratch);
        for (int x = 0; x < w; x++) boxLine(alpha.data() + x, h, w, box, scratch);
    }

    // A 4x4 ordered dither keeps the long faint tail from banding.
    static const float bayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < h; y++) {
        QRgb *line = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < w; x++) {
            const float value = alpha[size_t(y) * w + x] * float(strength) * 255.0f + (bayer[y & 3][x & 3] + 0.5f) / 16.0f - 0.5f;
            line[x] = qRgba(0, 0, 0, qBound(0, int(value + 0.5f), 255));
        }
    }
    image.setDevicePixelRatio(ratio);
    if (cache.size() > 32) cache.clear();
    cache.insert(key, image);
    return image;
}

} // namespace

void KisStudioGlass::setMovableByBackground(QWidget *window)
{
    window->setProperty(movableProperty, true);
    window->installEventFilter(dialogMover());
    Q_FOREACH (QWidget *child, window->findChildren<QWidget *>()) child->installEventFilter(dialogMover());
}

bool KisStudioGlass::isEnabled()
{
    return enabled();
}

void KisStudioGlass::setEnabled(bool on)
{
    if (on == enabled()) {
        return;
    }
    enabled() = on;
    if (on) stopFades();
    KisConfig(false).writeEntry(settingKey, on);
    Panels::instance().repaintAll();
}

qreal KisStudioGlass::blurScale(const QSizeF &size)
{
    return qBound<qreal>(1, std::sqrt(qMax<qreal>(0, size.width() * size.height())) / 300, maxBlurScale);
}

qreal KisStudioGlass::blurSigma()
{
    return 18 * blur();
}

qreal KisStudioGlass::tintOverDark()
{
    // Clear at 0 and solid at 1, along one smooth curve each: less over a dark canvas, more over a bright one
    // (0.63 and 0.77 at the default 0.7).
    const qreal o = opacity();
    return o * (0.8 + 0.2 * o * o);
}

qreal KisStudioGlass::tintOverBright()
{
    const qreal o = opacity();
    return o * (1.2 - 0.2 * o * o);
}

qreal KisStudioGlass::blur()
{
    return blurSetting();
}

void KisStudioGlass::setBlur(qreal blur)
{
    blur = qBound<qreal>(0, blur, 2.0);
    if (qFuzzyCompare(blur, blurSetting())) return;
    blurSetting() = blur;
    KisSettingsRegistry::writeLater(blurKey, blur);
    Engine::instance().clear();
    Panels::instance().repaintAll();
}

qreal KisStudioGlass::rimLight()
{
    return rimLightSetting();
}

void KisStudioGlass::setRimLight(qreal strength)
{
    strength = qBound<qreal>(0, strength, 2.0);
    if (qFuzzyCompare(strength + 1, rimLightSetting() + 1)) return;
    rimLightSetting() = strength;
    KisSettingsRegistry::writeLater(rimLightKey, strength);
    Panels::instance().repaintAll();
}

qreal KisStudioGlass::opacity()
{
    return opacitySetting();
}

void KisStudioGlass::setOpacity(qreal opacity)
{
    opacity = qBound<qreal>(0, opacity, 1.0);
    if (qFuzzyCompare(opacity + 1, opacitySetting() + 1)) return;
    opacitySetting() = opacity;
    KisSettingsRegistry::writeLater(opacityKey, opacity);
    Engine::instance().clear();
    Panels::instance().repaintAll();
}

bool KisStudioGlass::ambientSurround()
{
    return ambientSetting();
}

void KisStudioGlass::setOverOverlays(QWidget *panel)
{
    panel->setProperty(overOverlaysProperty, true);
    changed(panel);
}

void KisStudioGlass::installMenuGlass()
{
    static MenuGlass *filter = nullptr;
    if (filter) return;
    filter = new MenuGlass();
    qApp->installEventFilter(filter);
    KisPopupButton::setFrameDecorator(decoratePopupFrame);
    // menus and tooltips made before, such as the item tooltips of resource lists, were polished without the filter
    if (isEnabled() && seeThroughWindows()) {
        Q_FOREACH (QWidget *widget, QApplication::allWidgets()) {
            if (widget->testAttribute(Qt::WA_WState_Created)) continue;
            if (isTooltip(widget)) {
                makeGlassTooltip(widget);
            } else if (qobject_cast<QMenu *>(widget)) {
                makeSeeThroughWindow(widget);
            }
        }
    }
    if (isEnabled()) {
        stopFades();
    }
}

void KisStudioGlass::paintBorder(QPainter &painter, QWidget *widget, const QPainterPath &shape, const QColor &color, qreal width)
{
    QWidget *canvas = isEnabled() ? canvasWidgetOf(widget) : nullptr;
    if (!drawnByCanvas(canvas) || !canvas->isVisible() || !Panels::instance().isDrawn(widget)) {
        painter.strokePath(shape, QPen(color, width));
        return;
    }
    // The canvas draws a bevel instead, so only stroke the part outside the canvas.
    const QRectF overRect(widget->mapFromGlobal(canvas->mapToGlobal(QPoint())), canvas->size());
    const QRectF beyondRect = QRectF(widget->rect()).adjusted(-width, -width, width, width);
    if (overRect.contains(beyondRect)) {
        return;
    }
    QPainterPath beyond;
    beyond.addRect(beyondRect);
    QPainterPath over;
    over.addRect(overRect);
    painter.save();
    painter.setClipPath(beyond.subtracted(over), Qt::IntersectClip);
    painter.strokePath(shape, QPen(color, width));
    painter.restore();
}

bool KisStudioGlass::seeThroughWindows()
{
    static const bool solid = qEnvironmentVariableIntValue("KRITA_STUDIO_SOLID_WINDOWS") != 0;
    if (solid) {
        return false;
    }
#ifdef HAVE_X11
    if (QGuiApplication::platformName() == QLatin1String("xcb")) {
        // Checked every time, since a compositor can start or stop.
        QPlatformNativeInterface *native = QGuiApplication::platformNativeInterface();
        return native && native->nativeResourceForScreen(QByteArrayLiteral("compositingEnabled"), QGuiApplication::primaryScreen());
    }
#endif
    return true;
}

Qt::WindowType KisStudioGlass::popupWindowType()
{
#ifdef Q_OS_WIN
    static const bool popups = qEnvironmentVariableIntValue("KRITA_STUDIO_POPUP_WINDOWS") != 0;
    return popups ? Qt::Popup : Qt::Tool;
#else
    return Qt::Popup;
#endif
}

QPainterPath KisStudioGlass::paint(QPainter &painter, QWidget *widget, const QRectF &rect, qreal radius)
{
    // See-through widgets keep their color with zero alpha (see setSeeThrough), so restore it for solid fills.
    QColor window = widget->palette().color(QPalette::Window);
    window.setAlpha(255);
    if (widget->isWindow() && !widget->testAttribute(Qt::WA_TranslucentBackground)) {
        // A solid window (see seeThroughWindows): square, filled to the edges.
        painter.fillRect(rect, window);
        QPainterPath edge;
        edge.addRect(rect);
        return edge;
    }
    const QPainterPath shape = KisModernStyle::squircle(rect, radius);
    if (QWidget *behind = windowBehind(widget)) {
        const QImage glass = isEnabled() ? glassOverWindow(widget, rect.toAlignedRect(), behind) : QImage();
        if (glass.isNull()) {
            painter.fillPath(shape, window);
        } else {
            painter.save();
            painter.setClipPath(shape, Qt::IntersectClip);
            painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
            painter.drawImage(rect.toAlignedRect(), glass);
            painter.restore();
        }
        return shape;
    }
    const bool drawn = Panels::instance().paint(widget, rect, radius);
    const QRegion covered = Panels::instance().covered(widget);
    if (!covered.isEmpty()) {
        painter.setClipRegion(QRegion(widget->rect()) - covered, Qt::IntersectClip);
    }

    // A faint highlight along the top edge.
    auto sheen = [&]() {
        const QRectF bounds = shape.boundingRect();
        QLinearGradient gradient(bounds.topLeft(), QPointF(bounds.left(), bounds.top() + qMin<qreal>(28, bounds.height())));
        gradient.setColorAt(0, QColor(255, 255, 255, 18));
        gradient.setColorAt(1, QColor(255, 255, 255, 0));
        painter.fillPath(shape, gradient);
    };

    QWidget *canvas = isEnabled() ? canvasWidgetOf(widget) : nullptr;
    if (drawnByCanvas(canvas) && canvas->isVisible()) {
        if (!drawn) {
            // Solid until the canvas has drawn the glass under it.
            painter.fillPath(shape, window);
            sheen();
            return shape;
        }
        // The canvas draws the glass; outside the canvas use the panel color.
        const QRectF overRect(widget->mapFromGlobal(canvas->mapToGlobal(QPoint())), canvas->size());
        // Never fully clear: Windows passes the pointer through fully transparent pixels to the canvas below.
        QColor catcher = window;
        catcher.setAlpha(1);
        if (overRect.contains(shape.boundingRect())) {
            // all over the canvas, as panels nearly always are: no path to cut
            if (widget->isWindow()) painter.fillPath(shape, catcher);
        } else {
            QPainterPath over;
            over.addRect(overRect);
            painter.fillPath(shape.subtracted(over), window);
            if (widget->isWindow()) painter.fillPath(shape.intersected(over), catcher);
        }
        // the canvas's lit rim is its highlight
        return shape;
    }

    const QRect bounds = shape.boundingRect().toAlignedRect();
    const Engine::Entry behind = isEnabled() ? Engine::instance().backdrop(widget, bounds) : Engine::Entry();
    if (behind.image.isNull()) {
        painter.fillPath(shape, window);
        return shape;
    }
    painter.save();
    painter.setClipPath(shape, Qt::IntersectClip);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.drawImage(QRectF(bounds), behind.image);
    painter.restore();
    sheen();
    return shape;
}

void KisStudioGlass::paintShadow(QPainter &painter, const QRectF &panel, qreal radius, int margin, int darkness, int drop)
{
    // A wide faint shadow all around and a tight one at the edge, slightly lower.
    const qreal ratio = painter.device() ? painter.device()->devicePixelRatioF() : 1.0;
    const qreal strength = darkness / 36.0;
    painter.save();
    QPainterPath outside;
    outside.addRect(panel.adjusted(-margin, -margin, margin, margin));
    painter.setClipPath(outside.subtracted(KisModernStyle::squircle(panel, radius)), Qt::IntersectClip);
    const QPointF origin = panel.topLeft() - QPointF(margin, margin);
    painter.drawImage(origin + QPointF(0, drop * 0.5), shadowImage(panel.size(), radius, margin, margin / 3.2, 0.20 * strength, ratio));
    painter.drawImage(origin + QPointF(0, 1), shadowImage(panel.size(), radius, margin, 1.6, 0.16 * strength, ratio));
    painter.restore();
}

void KisStudioGlass::drewGlassOver(QWidget *canvasWidget, const QRect &painted)
{
    Panels::instance().drewOver(canvasWidget, painted);
}

QVector<KisStudioGlass::Surface> KisStudioGlass::surfacesOver(const QWidget *canvasWidget)
{
    return Panels::instance().over(canvasWidget);
}

void KisStudioGlass::setDrawnByCanvas(QWidget *canvasWidget, bool on)
{
    if (drawnByCanvas(canvasWidget) == on) {
        return;
    }
    canvasWidget->setProperty(drawnByCanvasProperty, on);
    if (on) Panels::instance().addCanvas(canvasWidget);
    Panels::instance().repaintOver(canvasWidget);
    canvasWidget->update();
}

void KisStudioGlass::changed(QWidget *widget)
{
    Panels::instance().changed(widget);
}
