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
#include <QApplication>
#include <QComboBox>
#include <QEvent>
#include <QFile>
#include <QFrame>
#include <QGraphicsOpacityEffect>
#include <QHash>
#include <QLayout>
#include <QLinearGradient>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QRegion>
#include <QSet>
#include <QThreadPool>
#include <QtMath>
#include <QTimer>
#include <QWidget>

#include "KisMainWindow.h"
#include "KisUpdatePacer.h"
#include "KisPart.h"
#include "KisViewManager.h"
#include "KisModernStyle.h"
#include "KisPopupButton.h"
#include "canvas/kis_canvas2.h"
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
const QString edgeKey = QStringLiteral("StudioGlassEdge");

/// How the glass looks: soft enough that nothing in it reads as a shape, tinted enough that text on it reads
constexpr qreal blurRadius = 26;   ///< in widget pixels
constexpr qreal sampleScale = 0.25; ///< what is behind is drawn at a quarter of the resolution: the blur hides it

// ------------------------------------------------------------ blurring in linear light

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

// ------------------------------------------------------------ Oklab, as the canvas draws the glass

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

/// \p c with its chroma lowered as much as it takes to be a color of the screen
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

/// \p image (sRGB) through glass tinted \p tint, \p strength strong (see KisStudioGlass::veil)
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
            // as colorful as what is behind (its chroma kept), veiled by the light the glass scatters
            const float lighter = qBound(0.0f, (l - b.l + 0.02f) / 0.08f, 1.0f);
            const float scattered = s * (veil + (1.0f - veil) * lighter * lighter * (3 - 2 * lighter));
            const std::array<float, 3> c = inGamut({l, b.a * (1 - scattered) + t.a * scattered, b.b * (1 - scattered) + t.b * scattered});
            out[x] = qRgb(toSrgb(c[0]), toSrgb(c[1]), toSrgb(c[2]));
        }
    }
    return result;
}

/// One box blur pass along a line of \p count values \p stride apart
void boxLine(float *data, int count, int stride, int radius, std::vector<float> &scratch)
{
    if (radius <= 0 || count <= 1) return;
    scratch.resize(count);
    const float scale = 1.0f / (2 * radius + 1);
    float sum = 0;
    // the edge repeated beyond it
    for (int i = -radius; i <= radius; i++) sum += data[qBound(0, i, count - 1) * stride];
    for (int i = 0; i < count; i++) {
        scratch[i] = sum * scale;
        sum += data[qMin(i + radius + 1, count - 1) * stride] - data[qMax(i - radius, 0) * stride];
    }
    for (int i = 0; i < count; i++) data[i * stride] = scratch[i];
}

/**
 * The frost, as the canvas makes it (see frostedLab() in KisOpenGLGlass):
 * blurred \p radius (three box passes each way: close to a gaussian), a
 * saturated color counting more, the mean in Oklab: its lightness a power
 * mean; what disagreeing hues cancel partly restored
 */
QImage blurred(const QImage &source, int radius)
{
    const QImage image = source.convertToFormat(QImage::Format_RGB32);
    const int w = image.width(), h = image.height();
    // weighted: lightness to the power, Oklab's a and b, and chroma; and the weight
    enum { L, Weight, A, Bb, Chroma, Count };
    const float power = float(KisStudioGlass::lightnessPower);
    std::vector<float> channels[Count];
    for (auto &channel : channels) channel.resize(size_t(w) * h);
    const auto &linear = toLinear();
    for (int y = 0; y < h; y++) {
        const QRgb *line = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < w; x++) {
            const size_t i = size_t(y) * w + x;
            const float r = linear[qRed(line[x])], g = linear[qGreen(line[x])], b = linear[qBlue(line[x])];
            const Lab lab = toOklab(r, g, b);
            const float chroma = std::hypot(lab.a, lab.b);
            const float weight = 1.0f + float(KisStudioGlass::salience) * chroma;
            channels[L][i] = std::pow(qMax(0.0f, lab.l), power) * weight;
            channels[Weight][i] = weight;
            channels[A][i] = lab.a * weight;
            channels[Bb][i] = lab.b * weight;
            channels[Chroma][i] = chroma * weight;
        }
    }
    // a box of r three times is about a gaussian of sigma r: the passes share the radius
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

KisMainWindow *mainWindowOf(QWidget *widget)
{
    for (QWidget *w = widget; w; w = w->parentWidget()) {
        if (auto *mainWindow = qobject_cast<KisMainWindow *>(w)) return mainWindow;
    }
    return KisPart::instance()->currentMainwindow();
}

/// What a backdrop is made from: taken from the canvas on the GUI thread, made into one on any
struct Scene {
    KisPaintDeviceSP projection;
    QRect imageBounds;
    QTransform toWidget;
    qreal zoom {1};
    QSize canvasSize;
    /// The canvas around the image
    QColor border;
    /// Where the image is see-through: the middle of the checkers
    QColor transparent;
};

/// What is behind \p canvasRect (in canvas widget pixels), blurred
QImage render(const Scene &scene, const QRect &canvasRect, const QColor &outside)
{
    // as wide as the glass is large
    const qreal scale = KisStudioGlass::blurScale(canvasRect.size());
    const qreal radius = blurRadius * scale;
    // what is behind, a blur's reach around it included, so that the edges blur as the middle does
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
        // the part of the image behind, at about the resolution it is drawn at here
        const QRect imageArea = scene.toWidget.inverted().mapRect(QRectF(area)).toAlignedRect() & scene.imageBounds;
        if (!imageArea.isEmpty()) {
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

    // blurred, and the reach around cut away again
    const QImage soft = blurred(image, qMax(1, qRound(radius * sampleScale)));
    const int cut = qRound(pad * sampleScale);
    return soft.copy(cut, cut, qMax(1, soft.width() - 2 * cut), qMax(1, soft.height() - 2 * cut));
}

/// How bright \p image is, 0 to 1 (perceived lightness): how strong the tint over it is
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

/**
 * Keeps what is behind each glass widget, and makes it again when the
 * canvas changes under it: the image painted on, or the view moved. A
 * widget's first glass is made at once, for it never to be shown without;
 * after that in the background, the glass it has shown meanwhile: the
 * canvas is never kept waiting on the glass.
 */
class Engine : public QObject
{
public:
    /// What is behind a glass widget
    struct Entry {
        /// What of the widget is glass, and where that was on the screen when this was made
        QRect rect;
        QRect globalRect;
        QImage image;
        /// How bright it is, 0 to 1
        qreal lightness {0};
    };

    static Engine &instance()
    {
        static Engine *engine = new Engine();
        return *engine;
    }

    /// The canvas behind \p rect of \p widget, blurred; a null image if there is none
    Entry backdrop(QWidget *widget, const QRect &rect);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::Resize) {
            if (!m_refresh.isActive()) m_refresh.start();
        }
        return QObject::eventFilter(watched, event);
    }

private:
    Engine()
    {
        m_refresh.setSingleShot(true);
        // at most once a frame of the screen
        m_refresh.setInterval(KisFrameRate::frameMs());
        connect(&m_refresh, &QTimer::timeout, this, [this]() { refresh(); });
        // one at a time: the glass is never far behind, and takes no more than a thread
        m_pool.setMaxThreadCount(1);
    }

    void follow(KisCanvas2 *canvas);
    void watch(QWidget *widget);
    void refresh();
    Scene scene() const;
    /// Makes the glass of \p widget again in the background
    void renderLater(QWidget *widget);
    void rendered(QWidget *widget, const QPointer<QWidget> &guard, int epoch, const Entry &entry);

    QPointer<KisCanvas2> m_canvas;
    QPointer<QWidget> m_canvasWidget;
    KisImageWSP m_image;
    QMetaObject::Connection m_imageConnection;
    QMetaObject::Connection m_viewConnection;
    /// Where the image changed since the glass last followed it, in image pixels
    QRect m_imageChanged;
    QTransform m_transform;
    QSize m_canvasSize;

    QTimer m_refresh;
    QHash<QWidget *, Entry> m_cache;
    QSet<QWidget *> m_watched;
    QThreadPool m_pool;
    /// Being made in the background; and changed again meanwhile
    QSet<QWidget *> m_rendering;
    QSet<QWidget *> m_again;
    /// Counts the canvases followed: glass made for another one is dropped
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
    // panned, zoomed, rotated, mirrored
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
    m_cache.clear();
    m_again.clear();
    m_epoch++;
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
    // the view moved: all the glass follows it; the image painted on: only the glass over (or near) the change
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
        // as far as the blur carries it
        const int reach = qCeil(blurRadius * KisStudioGlass::maxBlurScale);
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
            // made when it is shown again
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

/// The glass of \p rect (\p canvasRect on the canvas) made from \p scene
Engine::Entry makeEntry(const Scene &scene, const QRect &rect, const QRect &globalRect, const QRect &canvasRect, const QColor &outside)
{
    Engine::Entry entry {rect, globalRect, QImage(), 0};
    // nothing of the canvas behind: no glass
    if (canvasRect.intersects(QRect(QPoint(), scene.canvasSize))) {
        entry.image = render(scene, canvasRect, outside);
        entry.lightness = lightness(entry.image);
        // tinted as much as what is behind needs, for what is on it to read: a bright canvas more, a dark one less
        const qreal contrast = qBound(0.0, qAbs(entry.lightness - outside.lightnessF()), 1.0);
        entry.image = tinted(entry.image, outside, KisStudioGlass::tintOverDark + (KisStudioGlass::tintOverBright - KisStudioGlass::tintOverDark) * contrast);
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
        // the image let go of on the thread it lives in
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
    // gone, another canvas, or made again at another size meanwhile
    if (!guard || epoch != m_epoch || it == m_cache.end() || it->rect.size() != entry.rect.size()) {
        return;
    }
    *it = entry;
    if (widget->isVisible()) widget->update();
    if (again) renderLater(widget);
}

Engine::Entry Engine::backdrop(QWidget *widget, const QRect &rect)
{
    KisMainWindow *mainWindow = mainWindowOf(widget);
    KisViewManager *view = mainWindow ? mainWindow->viewManager() : nullptr;
    KisCanvas2 *canvas = view ? view->canvasBase() : nullptr;
    follow(canvas);
    if (!canvas || !m_canvasWidget || !m_canvasWidget->isVisible()) {
        return Entry();
    }
    watch(widget);

    const QRect globalRect(widget->mapToGlobal(rect.topLeft()), rect.size());
    auto it = m_cache.find(widget);
    if (it != m_cache.end() && it->rect.size() == rect.size()) {
        // moved: made again in the background, the glass it has shown meanwhile
        if (it->globalRect != globalRect) {
            it->rect = rect;
            renderLater(widget);
        }
        return *it;
    }
    // new, or of another size: made now
    const QRect canvasRect(m_canvasWidget->mapFromGlobal(globalRect.topLeft()), globalRect.size());
    const Entry entry = makeEntry(scene(), rect, globalRect, canvasRect, widget->palette().color(QPalette::Window));
    m_cache.insert(widget, entry);
    return entry;
}

/// Glass drawn by the canvas under it (on the GPU), not here
const char *const drawnByCanvasProperty = "studioGlassDrawnByCanvas";

QWidget *canvasWidgetOf(QWidget *widget)
{
    KisMainWindow *mainWindow = mainWindowOf(widget);
    KisViewManager *view = mainWindow ? mainWindow->viewManager() : nullptr;
    KisCanvas2 *canvas = view ? view->canvasBase() : nullptr;
    return canvas ? canvas->canvasWidget() : nullptr;
}

bool drawnByCanvas(const QWidget *canvasWidget)
{
    return canvasWidget && canvasWidget->property(drawnByCanvasProperty).toBool();
}

/// Hidden where a glass window of its own is over it
const char *const maskedProperty = "studioGlassMasked";
/// A panel in the canvas' window over the rest there, as a glass window is (see KisStudioGlass::setOverOverlays)
const char *const overOverlaysProperty = "studioGlassOverOverlays";

/// Whether \p a is painted before \p b (under it), both in one window: a parent before its children, siblings in their order
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

/// What lies over \p canvas in its window: the tool rail, the zoom pill
QList<QWidget *> overlaysOf(QWidget *canvas)
{
    QList<QWidget *> overlays;
    const QRect canvasRect(canvas->mapToGlobal(QPoint()), canvas->size());
    auto add = [&](QObject *object) {
        QWidget *w = qobject_cast<QWidget *>(object);
        if (w && !w->isWindow() && canvasRect.intersects(QRect(w->mapToGlobal(QPoint()), w->size()))) overlays << w;
    };
    for (QObject *child : canvas->children()) add(child);
    // stacked over it: what comes after it, or after what holds it, among its siblings
    for (QWidget *w = canvas; !w->isWindow() && w->parentWidget(); w = w->parentWidget()) {
        const QObjectList &siblings = w->parentWidget()->children();
        for (int i = siblings.indexOf(w) + 1; i < siblings.size(); i++) add(siblings[i]);
    }
    return overlays;
}

/**
 * The glass panels there are, and where: for a canvas that draws their
 * glass itself, which it draws again as they are shown, hidden, moved or
 * faded.
 */
class Panels : public QObject
{
public:
    struct Panel {
        QRectF rect;
        qreal radius {0};
        /// The canvas has drawn its glass since it was shown: until then it is the plain panel color
        bool drawn {false};
        /// Where a glass window over it covers it (its coordinates): nothing of it is painted there
        QRegion covered;
    };

    static Panels &instance()
    {
        static Panels *panels = new Panels();
        return *panels;
    }

    /// \p widget is glass in \p rect, with corners of \p radius; whether the canvas has drawn its glass yet
    bool paint(QWidget *widget, const QRectF &rect, qreal radius)
    {
        auto it = m_panels.find(widget);
        if (it == m_panels.end()) {
            m_panels.insert(widget, {rect, radius, false, QRegion()});
            if (!m_shown.contains(widget->window())) m_shown.insert(widget->window(), ++m_shownCount);
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

    /// \p widget is not glass anymore (it is still there): the canvas under it shows plain again
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
     * changed(), once the window's painting is done: \p widget is being
     * painted, and Qt forgets a canvas asked to draw again meanwhile (it
     * clears what is to be drawn into textures as it finishes). The glass
     * of a panel shown with the window was drawn so only where the canvas
     * happened to draw next, the brush outline, until something drew all of
     * it again.
     */
    void changedAfterPainting(QWidget *widget)
    {
        QMetaObject::invokeMethod(this, [this, guard = QPointer<QWidget>(widget)]() {
            if (guard) changed(guard);
        }, Qt::QueuedConnection);
    }

    /// Where a panel was, the canvases show plain again
    void showPlainAgain()
    {
        for (QWidget *canvas : qAsConst(m_canvases)) {
            if (!canvas) continue;
            canvas->update();
            hideUnderWindows(canvas);
        }
    }

    /// Whether the canvas has drawn \p widget's glass since it was shown
    bool isDrawn(QWidget *widget) const
    {
        return m_panels.value(widget).drawn;
    }

    /// All the panels, and the canvases under them, painted again: e.g. their edge changed
    void repaintAll()
    {
        for (auto it = m_panels.constBegin(); it != m_panels.constEnd(); ++it) it.key()->update();
        for (QWidget *canvas : qAsConst(m_canvases)) {
            if (canvas) canvas->update();
        }
    }

    /// Where a glass window over \p widget covers it
    QRegion covered(QWidget *widget) const
    {
        return m_panels.value(widget).covered;
    }

    /// \p widget, or a panel in its window, is shown otherwise: the canvas under it draws again
    void changed(QWidget *widget)
    {
        KisUpdatePacer::log(QStringLiteral("glass changed ") + QString::fromLatin1(widget->metaObject()->className()) + QLatin1Char(' ') + widget->objectName());
        QWidget *window = widget->window();
        QSet<QWidget *> canvases;
        for (auto it = m_panels.constBegin(); it != m_panels.constEnd(); ++it) {
            if (it.key() == widget || it.key()->window() == window) {
                QWidget *canvas = canvasWidgetOf(it.key());
                if (drawnByCanvas(canvas)) canvases.insert(canvas);
            }
        }
        for (QWidget *canvas : qAsConst(canvases)) {
            canvas->update();
            hideUnderWindows(canvas);
        }
    }

    /**
     * The canvas draws the glass of all the panels over it under them all,
     * in their stacking order, each over the one under it; but what is on a
     * panel would show through the glass of a window over it. So what lies
     * over the canvas in its window (the tool rail), and what is in a glass
     * window, is hidden where a glass window over it covers it. A panel in
     * the canvas' window set over the rest there (setOverOverlays) hides what
     * is under it there the same way.
     */
    void hideUnderWindows(QWidget *canvas)
    {
        // the glass over the rest (windows, and panels set over the rest in the canvas' window), the lowest first,
        // and their shapes on the screen
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

        // what lies over the canvas in its window: under every glass window, and under a panel there set over it
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

        QRegion above;

        for (int i = 0; i < windows.size(); i++) {
            above = QRegion();
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
        // no longer under one
        for (auto it = m_panels.begin(); it != m_panels.end(); ++it) {
            if (!windows.contains(it.key()) && !it->covered.isEmpty()) {
                it->covered = QRegion();
                hide(it.key()->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly), QRegion());
                it.key()->update();
            }
        }
    }

    /// \p widgets hidden where \p region (on the screen) is, shown elsewhere
    static void hide(const QList<QWidget *> &widgets, const QRegion &region)
    {
        for (QWidget *w : widgets) {
            const bool masked = w->property(maskedProperty).toBool();
            if (!masked && !w->mask().isEmpty()) {
                // a shape of its own: left alone
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
                // all of it hidden: a mask of nothing would be no mask, so one pixel beside it
                shown = QRegion(-1, -1, 1, 1);
            }
            if (!masked || w->mask() != shown) {
                w->setMask(shown);
                w->setProperty(maskedProperty, true);
            }
        }
    }

    /// The panels over \p canvasWidget, for it to draw their glass: from then on they show it
    QVector<KisStudioGlass::Surface> over(const QWidget *canvasWidget)
    {
        QVector<KisStudioGlass::Surface> surfaces;
        if (!KisStudioGlass::isEnabled()) {
            return surfaces;
        }
        const QPoint canvasOrigin = canvasWidget->mapToGlobal(QPoint());
        // in their stacking order: those in the canvas' window first, under windows of their own
        QList<QWidget *> widgets = m_panels.keys();
        std::sort(widgets.begin(), widgets.end(), [this, canvasWidget](QWidget *a, QWidget *b) { return below(a, b, canvasWidget); });
        for (QWidget *widget : qAsConst(widgets)) {
            Panel &panel = m_panels[widget];
            if (!widget->isVisible() || canvasWidgetOf(widget) != canvasWidget) {
                continue;
            }
            if (!panel.drawn) {
                panel.drawn = true;
                widget->update();
            }
            const qreal opacity = opacityOf(widget);
            if (opacity <= 0) {
                continue;
            }
            const QPointF offset = widget->mapToGlobal(QPoint()) - canvasOrigin;
            surfaces.append({panel.rect.translated(offset), panel.radius, widget->palette().color(QPalette::Window), opacity});
        }
        return surfaces;
    }

    /// A canvas that draws glass
    void addCanvas(QWidget *canvas)
    {
        if (!m_canvases.contains(canvas)) m_canvases.append(canvas);
    }

    /// The panels over \p canvas, painted again (with their glass drawn here, or not)
    void repaintOver(QWidget *canvas)
    {
        for (auto it = m_panels.constBegin(); it != m_panels.constEnd(); ++it) {
            if (canvasWidgetOf(it.key()) == canvas) it.key()->update();
        }
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        switch (event->type()) {
        case QEvent::Hide:
            // shown again, it is the panel color until the canvas draws its glass
            for (auto it = m_panels.begin(); it != m_panels.end(); ++it) {
                if (it.key() == watched || it.key()->window() == watched) it->drawn = false;
            }
            changed(static_cast<QWidget *>(watched));
            break;
        case QEvent::Show:
            // a window shown later is over those shown before
            if (static_cast<QWidget *>(watched)->isWindow()) m_shown.insert(static_cast<QWidget *>(watched), ++m_shownCount);
            changed(static_cast<QWidget *>(watched));
            break;
        case QEvent::Move:
        case QEvent::Resize:
        case QEvent::PaletteChange:
        case QEvent::WindowStateChange:
            changed(static_cast<QWidget *>(watched));
            break;
        default:
            break;
        }
        return false;
    }

private:
    /// How opaque \p widget is shown: its window's opacity, and that of the opacity effects on it
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

    /// Where \p panel's window is in the stacking: windows shown later are over earlier ones
    quint64 stacking(QWidget *panel) const
    {
        return m_shown.value(panel->window());
    }

    /**
     * Whether panel \p a is under panel \p b over \p canvas: those in the
     * canvas' window under windows of their own, those in one window as it
     * paints them, windows as they were shown
     */
    bool below(QWidget *a, QWidget *b, const QWidget *canvas) const
    {
        const bool aOwn = a->window() != canvas->window(), bOwn = b->window() != canvas->window();
        if (aOwn != bOwn) return bOwn;
        if (a->window() != b->window()) return stacking(a) < stacking(b);
        return paintedBefore(a, b);
    }

    QHash<QWidget *, Panel> m_panels;
    QList<QPointer<QWidget>> m_canvases;
    QHash<QWidget *, quint64> m_shown;
    quint64 m_shownCount {0};
};

/**
 * White or black, as opaque as it takes over \p under to make \p color's
 * lightness (a little more, at least: a field still shows as one)
 */
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

const char *const savedPaletteProperty = "studioGlassSavedPalette";

/**
 * The QML in \p widget glass too, or not: its QML views (KisQQuickWidget,
 * whose glassProperty this is) drawn see-through, their fields and buttons
 * veils as the widgets' are
 */
void setQmlGlass(QWidget *widget, bool on)
{
    Q_FOREACH (QWidget *child, widget->findChildren<QWidget *>()) {
        if (child->inherits("KisQQuickWidget")) child->setProperty("studioGlassQml", on);
    }
}
const char *const seeThroughProperty = "studioGlassSeeThrough";

} // namespace

void KisStudioGlass::setSeeThrough(QWidget *widget, bool on)
{
    if (on == widget->property(seeThroughProperty).toBool()) {
        return;
    }
    widget->setProperty(seeThroughProperty, on);
    if (on) {
        widget->setProperty(savedPaletteProperty, widget->testAttribute(Qt::WA_SetPalette) ? QVariant::fromValue(widget->palette()) : QVariant());
        /**
         * Its surfaces show the glass behind. Fields, buttons and rows still
         * read as they did over the panel's color: each a veil of white (one
         * lighter than the panel) or black (one darker), as strong as it takes
         * to be that much lighter or darker. A veil of a gray would mute the
         * glass's colors: one of white or black changes the most of its
         * lightness with the least of it covered.
         */
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
        // a dropdown's list is a window of its own, but given its box's palette: it stays solid
        Q_FOREACH (QComboBox *box, widget->findChildren<QComboBox *>()) {
            QWidget *popup = box->view() ? box->view()->window() : nullptr;
            if (popup && popup != box->window()) popup->setPalette(QApplication::palette(box));
        }
    } else {
        const QVariant saved = widget->property(savedPaletteProperty);
        if (saved.isValid()) {
            widget->setPalette(saved.value<QPalette>());
        } else {
            // back to the palette it would have, from the window it is in
            widget->setPalette(QPalette());
            widget->setAttribute(Qt::WA_SetPalette, false);
        }
        widget->setProperty(savedPaletteProperty, QVariant());
        setQmlGlass(widget, false);
    }
}

namespace {
/// The setting, read once: glass is painted often
bool &enabled()
{
    static bool on = KisConfig(true).readEntry<bool>(settingKey, true);
    return on;
}
} // namespace

bool KisStudioGlass::isEnabled()
{
    return enabled();
}

namespace {
/// The setting, read once: the canvas draws the glass every frame
KisStudioGlass::Edge &edgeSetting()
{
    static KisStudioGlass::Edge edge = KisStudioGlass::Edge(qBound(0, KisConfig(true).readEntry<int>(edgeKey, int(KisStudioGlass::Edge::Subtle)), 2));
    return edge;
}
} // namespace

qreal KisStudioGlass::blurScale(const QSizeF &size)
{
    return qBound<qreal>(1, std::sqrt(qMax<qreal>(0, size.width() * size.height())) / 300, maxBlurScale);
}

KisStudioGlass::Edge KisStudioGlass::edge()
{
    return edgeSetting();
}

namespace {
const QString surroundKey = QStringLiteral("StudioCanvasSurround");

KisStudioGlass::Surround &surroundSetting()
{
    static KisStudioGlass::Surround surround =
        KisStudioGlass::Surround(qBound(0, KisConfig(true).readEntry<int>(surroundKey, int(KisStudioGlass::Surround::Ambient)), 2));
    return surround;
}
} // namespace

KisStudioGlass::Surround KisStudioGlass::surround()
{
    return surroundSetting();
}

namespace {
const char *const glassMenuProperty = "studioGlassMenu";

/// \p widget's glass painted as a panel's is, under what it paints, with corners of \p radius
void paintMenuGlass(QWidget *widget, qreal radius)
{
    QPainter painter(widget);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QPainterPath shape = KisStudioGlass::paint(painter, widget, QRectF(widget->rect()).adjusted(0.5, 0.5, -0.5, -0.5), radius);
    QColor border = widget->palette().color(QPalette::WindowText);
    border.setAlphaF(0.12);
    KisStudioGlass::paintBorder(painter, widget, shape, border);
}

/// See-through on Windows only as a frameless window (a popup is not one of itself), and its square shadow not
void makeSeeThroughWindow(QWidget *widget)
{
    widget->overrideWindowFlags(widget->windowFlags() | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
    widget->setAttribute(Qt::WA_TranslucentBackground, true);
    widget->setProperty(glassMenuProperty, true);
}

/**
 * Makes menus glass: see-through windows (set before their window is made,
 * which is what Qt needs for it), their background painted as a panel's
 * is, before they paint their items over it
 */
class MenuGlass : public QObject
{
protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() != QEvent::Polish && event->type() != QEvent::Paint) {
            return false;
        }
        QMenu *menu = qobject_cast<QMenu *>(watched);
        if (!menu) {
            return false;
        }
        if (event->type() == QEvent::Polish && !menu->testAttribute(Qt::WA_WState_Created) && KisStudioGlass::isEnabled()
            && KisStudioGlass::seeThroughWindows()) {
            makeSeeThroughWindow(menu);
        } else if (event->type() == QEvent::Paint && menu->property(glassMenuProperty).toBool()) {
            paintMenuGlass(menu, KisModernStyle::cornerRadius + 2);
        }
        return false;
    }
};

/// Paints the glass of a popup button's popup (see decoratePopupFrame), installed on it
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

/**
 * A popup of KisPopupButton (the brush editor, the brush size's slider...)
 * glass as a menu is, as its window is about to be made (again after it
 * was detached or attached). A detached one is a dialog, a window of its
 * own, and stays solid; a menu in one is glass itself.
 */
void decoratePopupFrame(QFrame *frame, bool detached, bool holdsMenu)
{
    static PopupFrameGlass *painter = new PopupFrameGlass();
    const bool glass = KisStudioGlass::isEnabled() && KisStudioGlass::seeThroughWindows() && !detached && !holdsMenu;
    if (glass) {
        makeSeeThroughWindow(frame);
        frame->installEventFilter(painter);
        // no box: what is in it lies on the glass, kept clear of its round corners
        frame->setFrameStyle(QFrame::NoFrame);
        frame->layout()->setContentsMargins(8, 8, 8, 8);
    } else {
        // the window flags and the frame are the frame's own again already (see KisPopupButton::setFrameDecorator)
        frame->setAttribute(Qt::WA_TranslucentBackground, false);
        frame->setProperty(glassMenuProperty, false);
        frame->removeEventFilter(painter);
        Panels::instance().forget(frame);
    }
    // what is in it see-through (the frame keeps its color: the glass is tinted with it)
    Q_FOREACH (QWidget *content, frame->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly)) {
        KisStudioGlass::setSeeThrough(content, glass);
    }
}
} // namespace

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
    if (isEnabled()) {
        // Windows fades menus in through their window's opacity, of which nothing is told: the canvas drew
        // the glass under one as faint as it was then, and did not again, and the menu showed no background
        QApplication::setEffectEnabled(Qt::UI_AnimateMenu, false);
    }
}

void KisStudioGlass::setSurround(Surround surround)
{
    if (surround == surroundSetting()) {
        return;
    }
    surroundSetting() = surround;
    KisConfig(false).writeEntry(surroundKey, int(surround));
    // the canvases take the color around the image again (see KisConfig::canvasBorderColor) and draw again
    KisConfigNotifier::instance()->notifyConfigChanged();
    Panels::instance().repaintAll();
}

void KisStudioGlass::setEdge(Edge edge)
{
    if (edge == edgeSetting()) {
        return;
    }
    edgeSetting() = edge;
    KisConfig(false).writeEntry(edgeKey, int(edge));
    Panels::instance().repaintAll();
}

KisStudioGlass::Bevel KisStudioGlass::bevel()
{
    switch (edge()) {
    case Edge::Subtle:
        return {14, 14, 26};
    case Edge::Strong:
        return {18, 20, 36};
    case Edge::Line:
        break;
    }
    return {};
}

void KisStudioGlass::paintBorder(QPainter &painter, QWidget *widget, const QPainterPath &shape, const QColor &color, qreal width)
{
    QWidget *canvas = isEnabled() && edge() != Edge::Line ? canvasWidgetOf(widget) : nullptr;
    if (!drawnByCanvas(canvas) || !canvas->isVisible() || !Panels::instance().isDrawn(widget)) {
        painter.strokePath(shape, QPen(color, width));
        return;
    }
    // the canvas draws a bevel where the glass is over it: the line only beyond it
    QPainterPath beyond;
    beyond.addRect(QRectF(widget->rect()).adjusted(-width, -width, width, width));
    QPainterPath over;
    over.addRect(QRectF(widget->mapFromGlobal(canvas->mapToGlobal(QPoint())), canvas->size()));
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
        // asked each time (a compositing manager can start or stop): Qt's xcb plugin watches _NET_WM_CM_Sn for it
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

void KisStudioGlass::setEnabled(bool on)
{
    enabled() = on;
    KisConfig(false).writeEntry(settingKey, on);
}

QColor KisStudioGlass::tint(const QWidget *widget)
{
    QColor color = widget->palette().color(QPalette::Window);
    color.setAlphaF(tintOverBright);
    return color;
}

QPainterPath KisStudioGlass::paint(QPainter &painter, QWidget *widget, const QRectF &rect, qreal radius)
{
    const QColor window = widget->palette().color(QPalette::Window);
    if (widget->isWindow() && !widget->testAttribute(Qt::WA_TranslucentBackground)) {
        // a solid window of its own (see seeThroughWindows): its color to its square edges, where its border goes
        painter.fillRect(rect, window);
        QPainterPath edge;
        edge.addRect(rect);
        return edge;
    }
    const QPainterPath shape = KisModernStyle::squircle(rect, radius);
    const bool drawn = Panels::instance().paint(widget, rect, radius);
    // under a glass window, nothing of it shows through that window's glass (what is painted on it after neither)
    const QRegion covered = Panels::instance().covered(widget);
    if (!covered.isEmpty()) {
        painter.setClipRegion(QRegion(widget->rect()) - covered, Qt::IntersectClip);
    }

    // light catching the top edge, faintly
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
            // until the canvas has drawn the glass under it, not to show the canvas plain through it
            painter.fillPath(shape, window);
            sheen();
            return shape;
        }
        // the canvas draws the glass under it; beyond the canvas it is the panel color
        QPainterPath over;
        over.addRect(QRectF(widget->mapFromGlobal(canvas->mapToGlobal(QPoint())), canvas->size()));
        painter.fillPath(shape.subtracted(over), window);
        // a bevel catches the light itself
        if (edge() == Edge::Line) sheen();
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
    // tinted already
    painter.drawImage(QRectF(bounds), behind.image);
    painter.restore();
    sheen();
    return shape;
}

namespace {

/**
 * A squircle's shadow, as the alpha of black: the shape blurred by a
 * gaussian (a penumbra fades so; stacked outlines fade in steps), \p
 * strength at most, dithered so that its long faint tail does not band.
 * Made once for each size, and kept.
 */
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

    // three box blurs each way: a gaussian, near enough
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

    // black, as see-through as the blur says; a 4 x 4 ordered dither under the step of eight bits
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

void KisStudioGlass::paintShadow(QPainter &painter, const QRectF &panel, qreal radius, int margin, int darkness, int drop)
{
    /**
     * Two shadows, as of a pane held a little over the canvas in soft light
     * from above: a wide faint one all around (the room's light it keeps
     * off), and a tight one at its edge, a little lower (where it nearly
     * touches). Only outside the panel: the glass shows what is under it.
     */
    const qreal ratio = painter.device() ? painter.device()->devicePixelRatioF() : 1.0;
    const qreal strength = darkness / 36.0;
    painter.save();
    QPainterPath outside;
    outside.addRect(panel.adjusted(-margin, -margin, margin, margin));
    painter.setClipPath(outside.subtracted(KisModernStyle::squircle(panel, radius)), Qt::IntersectClip);
    const QPointF origin = panel.topLeft() - QPointF(margin, margin);
    // wide: fading out by the edge of the margin, lowered as far as the light is above
    painter.drawImage(origin + QPointF(0, drop * 0.5), shadowImage(panel.size(), radius, margin, margin / 3.2, 0.20 * strength, ratio));
    // tight, at the edge
    painter.drawImage(origin + QPointF(0, 1), shadowImage(panel.size(), radius, margin, 1.6, 0.16 * strength, ratio));
    painter.restore();
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
