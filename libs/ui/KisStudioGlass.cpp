/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioGlass.h"

#include <array>
#include <cmath>
#include <vector>

#include <QEvent>
#include <QFile>
#include <QHash>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QtMath>
#include <QTimer>
#include <QWidget>

#include "KisMainWindow.h"
#include "KisPart.h"
#include "KisViewManager.h"
#include "canvas/kis_canvas2.h"
#include "kis_config.h"
#include "kis_coordinates_converter.h"
#include "kis_image.h"
#include "kis_paint_device.h"

namespace {

const QString settingKey = QStringLiteral("StudioGlass");

/// How the glass looks: soft enough that nothing in it reads as a shape, tinted enough that text on it reads
constexpr qreal blurRadius = 26;   ///< in widget pixels
constexpr qreal sampleScale = 0.25; ///< what is behind is drawn at a quarter of the resolution: the blur hides it
constexpr qreal vibrance = 1.18;   ///< a little more saturated, as light through glass looks
/// The tint, over a dark and over a bright canvas: text on it reads as well over either
constexpr qreal tintOverDark = 0.56;
constexpr qreal tintOverBright = 0.84;
/// How often at most the glass follows the canvas
constexpr int refreshInterval = 90;

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

/// Blurred in linear light (three box passes each way: close to a gaussian), a little more vibrant
QImage blurred(const QImage &source, int radius)
{
    const QImage image = source.convertToFormat(QImage::Format_RGB32);
    const int w = image.width(), h = image.height();
    std::vector<float> channels[3];
    for (auto &channel : channels) channel.resize(size_t(w) * h);
    const auto &linear = toLinear();
    for (int y = 0; y < h; y++) {
        const QRgb *line = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < w; x++) {
            const size_t i = size_t(y) * w + x;
            const float r = linear[qRed(line[x])], g = linear[qGreen(line[x])], b = linear[qBlue(line[x])];
            // vibrance: away from the gray of the same luminance
            const float luminance = 0.2126f * r + 0.7152f * g + 0.0722f * b;
            channels[0][i] = qMax(0.0f, luminance + (r - luminance) * float(vibrance));
            channels[1][i] = qMax(0.0f, luminance + (g - luminance) * float(vibrance));
            channels[2][i] = qMax(0.0f, luminance + (b - luminance) * float(vibrance));
        }
    }
    // a box of r three times is about a gaussian of sigma r: the passes share the radius
    const int box = qMax(1, int(std::lround(radius / std::sqrt(3.0))));
    std::vector<float> scratch;
    for (auto &channel : channels) {
        for (int pass = 0; pass < 3; pass++) {
            for (int y = 0; y < h; y++) boxLine(channel.data() + size_t(y) * w, w, 1, box, scratch);
            for (int x = 0; x < w; x++) boxLine(channel.data() + x, h, w, box, scratch);
        }
    }
    QImage result(w, h, QImage::Format_RGB32);
    for (int y = 0; y < h; y++) {
        QRgb *line = reinterpret_cast<QRgb *>(result.scanLine(y));
        for (int x = 0; x < w; x++) {
            const size_t i = size_t(y) * w + x;
            line[x] = qRgb(toSrgb(channels[0][i]), toSrgb(channels[1][i]), toSrgb(channels[2][i]));
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

/**
 * Keeps what is behind each glass widget, and repaints them when the
 * canvas changes under them: the image painted on, or the view moved.
 */
class Engine : public QObject
{
public:
    static Engine &instance()
    {
        static Engine *engine = new Engine();
        return *engine;
    }

    /// The canvas behind \p rect of \p widget, blurred; null if there is none
    QImage backdrop(QWidget *widget, const QRect &rect);

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
        m_refresh.setInterval(refreshInterval);
        connect(&m_refresh, &QTimer::timeout, this, [this]() { refresh(); });
    }

    struct Entry {
        QRect globalRect;
        QImage image;
    };
public:
    /// How bright what is behind is, 0 to 1 (perceived lightness): how strong the tint over it is
    static qreal lightness(const QImage &image)
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
private:

    void follow(KisCanvas2 *canvas);
    void refresh();
    QImage render(KisCanvas2 *canvas, const QRect &canvasRect, const QColor &outside);

    QPointer<KisCanvas2> m_canvas;
    QPointer<QWidget> m_canvasWidget;
    KisImageWSP m_image;
    QMetaObject::Connection m_imageConnection;
    QMetaObject::Connection m_viewConnection;
    bool m_imageChanged {false};
    QTransform m_transform;
    QSize m_canvasSize;

    QTimer m_refresh;
    QHash<QWidget *, Entry> m_cache;
    QList<QPointer<QWidget>> m_watched;
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
        m_imageConnection = connect(m_image.data(), &KisImage::sigImageUpdated, this, [this](const QRect &) {
            m_imageChanged = true;
            if (!m_refresh.isActive()) m_refresh.start();
        });
    }
    m_cache.clear();
}

void Engine::refresh()
{
    // anything to follow: the image painted on, or the view moved
    bool changed = m_imageChanged;
    m_imageChanged = false;
    if (m_canvas) {
        const QTransform transform = m_canvas->coordinatesConverter()->imageToWidgetTransform();
        const QSize size = m_canvasWidget ? m_canvasWidget->size() : QSize();
        if (transform != m_transform || size != m_canvasSize) {
            m_transform = transform;
            m_canvasSize = size;
            changed = true;
        }
    }
    if (!changed) {
        return;
    }
    m_cache.clear();
    for (int i = m_watched.size() - 1; i >= 0; i--) {
        QWidget *widget = m_watched[i];
        if (!widget) {
            m_watched.removeAt(i);
        } else if (widget->isVisible()) {
            widget->update();
        }
    }
}

QImage Engine::render(KisCanvas2 *canvas, const QRect &canvasRect, const QColor &outside)
{
    // what is behind, a blur's reach around it included, so that the edges blur as the middle does
    const int pad = qCeil(blurRadius);
    const QRect area = canvasRect.adjusted(-pad, -pad, pad, pad);
    const QSize size(qMax(1, qCeil(area.width() * sampleScale)), qMax(1, qCeil(area.height() * sampleScale)));
    QImage image(size, QImage::Format_RGB32);
    image.fill(outside);

    QPainter painter(&image);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.scale(sampleScale, sampleScale);
    painter.translate(-area.topLeft());

    // the canvas around the image, as the canvas shows it
    const KisConfig cfg(true);
    QWidget *canvasWidget = canvas->canvasWidget();
    painter.fillRect(QRect(QPoint(0, 0), canvasWidget->size()), cfg.canvasBorderColor());

    KisImageSP image2 = canvas->image();
    const KisCoordinatesConverter *converter = canvas->coordinatesConverter();
    if (image2 && image2->projection()) {
        const QTransform toWidget = converter->imageToWidgetTransform();
        // the part of the image behind, at about the resolution it is drawn at here
        const QRect imageArea = toWidget.inverted().mapRect(QRectF(area)).toAlignedRect() & image2->bounds();
        if (!imageArea.isEmpty()) {
            const qreal zoom = qMax(1e-3, converter->effectiveZoom());
            const QSize thumbSize(qBound(1, qCeil(imageArea.width() * zoom * sampleScale), imageArea.width()),
                                  qBound(1, qCeil(imageArea.height() * zoom * sampleScale), imageArea.height()));
            const QImage thumbnail = image2->projection()->createThumbnailUncached(thumbSize.width(), thumbSize.height(), imageArea, 2);
            painter.save();
            painter.setTransform(toWidget, true);
            // where the image is see-through: the middle of the checkers
            const QColor c1 = cfg.checkersColor1(), c2 = cfg.checkersColor2();
            painter.fillRect(imageArea, QColor((c1.red() + c2.red()) / 2, (c1.green() + c2.green()) / 2, (c1.blue() + c2.blue()) / 2));
            painter.drawImage(QRectF(imageArea), thumbnail);
            painter.restore();
        }
    }
    painter.end();

    // blurred, and the reach around cut away again
    const QImage soft = blurred(image, qMax(1, qRound(blurRadius * sampleScale)));
    const int cut = qRound(pad * sampleScale);
    return soft.copy(cut, cut, qMax(1, soft.width() - 2 * cut), qMax(1, soft.height() - 2 * cut));
}

QImage Engine::backdrop(QWidget *widget, const QRect &rect)
{
    KisMainWindow *mainWindow = mainWindowOf(widget);
    KisViewManager *view = mainWindow ? mainWindow->viewManager() : nullptr;
    KisCanvas2 *canvas = view ? view->canvasBase() : nullptr;
    follow(canvas);
    if (!canvas || !m_canvasWidget || !m_canvasWidget->isVisible()) {
        return QImage();
    }
    if (!m_watched.contains(widget)) {
        m_watched << widget;
    }

    const QRect globalRect(widget->mapToGlobal(rect.topLeft()), rect.size());
    auto it = m_cache.constFind(widget);
    if (it != m_cache.constEnd() && it->globalRect == globalRect) {
        return it->image;
    }
    // nothing of the canvas behind: no glass
    const QRect canvasRect(m_canvasWidget->mapFromGlobal(globalRect.topLeft()), globalRect.size());
    if (!canvasRect.intersects(m_canvasWidget->rect())) {
        m_cache.insert(widget, {globalRect, QImage()});
        return QImage();
    }
    const QImage image = render(canvas, canvasRect, widget->palette().color(QPalette::Window));
    m_cache.insert(widget, {globalRect, image});
    return image;
}

const char *const savedPaletteProperty = "studioGlassSavedPalette";
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
        // its surfaces show the glass behind; fields are a darker glass, so that they still read as fields
        QPalette palette = widget->palette();
        for (QPalette::ColorGroup group : {QPalette::Active, QPalette::Inactive, QPalette::Disabled}) {
            QColor window = palette.color(group, QPalette::Window);
            window.setAlpha(0);
            palette.setColor(group, QPalette::Window, window);
            QColor base = palette.color(group, QPalette::Base);
            base.setAlphaF(0.4);
            palette.setColor(group, QPalette::Base, base);
            QColor alternate = palette.color(group, QPalette::AlternateBase);
            alternate.setAlphaF(0.3);
            palette.setColor(group, QPalette::AlternateBase, alternate);
        }
        widget->setPalette(palette);
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
    }
}

bool KisStudioGlass::isEnabled()
{
    return KisConfig(true).readEntry<bool>(settingKey, true);
}

void KisStudioGlass::setEnabled(bool on)
{
    KisConfig(false).writeEntry(settingKey, on);
}

QColor KisStudioGlass::tint(const QWidget *widget)
{
    QColor color = widget->palette().color(QPalette::Window);
    color.setAlphaF(tintOverBright);
    return color;
}

void KisStudioGlass::paint(QPainter &painter, QWidget *widget, const QPainterPath &shape)
{
    const QRect rect = shape.boundingRect().toAlignedRect();
    const QImage backdrop = isEnabled() ? Engine::instance().backdrop(widget, rect) : QImage();
    if (backdrop.isNull()) {
        painter.fillPath(shape, widget->palette().color(QPalette::Window));
        return;
    }
    painter.save();
    painter.setClipPath(shape, Qt::IntersectClip);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.drawImage(QRectF(rect), backdrop);

    // tinted as much as what is behind needs, for what is on it to read: a bright canvas more, a dark one less
    const QColor window = widget->palette().color(QPalette::Window);
    const qreal behind = Engine::lightness(backdrop);
    const qreal contrast = qAbs(behind - window.lightnessF());
    QColor tinted = window;
    tinted.setAlphaF(tintOverDark + (tintOverBright - tintOverDark) * qBound(0.0, contrast, 1.0));
    painter.fillRect(rect, tinted);

    // light catching the top edge, faintly
    QLinearGradient sheen(rect.topLeft(), QPointF(rect.left(), rect.top() + qMin(28, rect.height())));
    sheen.setColorAt(0, QColor(255, 255, 255, 18));
    sheen.setColorAt(1, QColor(255, 255, 255, 0));
    painter.fillRect(rect, sheen);
    painter.restore();
}
