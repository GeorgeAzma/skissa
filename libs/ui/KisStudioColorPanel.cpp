/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioColorPanel.h"
#include "KisColorDrop.h"
#include "KisColorMath.h"

#include <algorithm>
#include <array>

#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QKeyEvent>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QMap>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QRegularExpression>
#include <QScreen>
#include <QToolButton>
#include <QToolTip>
#include <QHelpEvent>
#include <QLocale>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <cmath>
#include <functional>
#include <memory>
#include <optional>

#include <KoColorSpaceTraits.h>
#include <KoCanvasBase.h>
#include <KoCanvasResourceProvider.h>
#include <KoCanvasResourcesIds.h>
#include <KoColor.h>
#include <KoColorSpaceRegistry.h>
#include <KoColorModelStandardIds.h>
#include <KoDockRegistry.h>
#include <klocalizedstring.h>

#include "KisStudioGlass.h"
#include "KisDocument.h"
#include "KisViewManager.h"
#include "kis_canvas_resource_provider.h"
#include <KisUniqueColorSet.h>
#include <KoCompositeOpRegistry.h>
#include "canvas/kis_canvas2.h"
#include "kis_image.h"
#include "KisSettingsRegistry.h"
#include "KisStudioTips.h"
#include "widgets/KisStudioInlineEdit.h"
#include "kis_config.h"
#include <KisModernStyle.h>

namespace {

/// The widest chroma the picker square shows; sRGB reaches about 0.3225 (magenta)
constexpr qreal maxPlaneChroma = 0.33;

struct Oklch {
    qreal l {0.0};
    qreal c {0.0};
    qreal h {0.0}; ///< degrees
};

using KisColorMath::toLinear;

Oklch oklchFromRgb(const QColor &color)
{
    const KisColorMath::Oklab lab = KisColorMath::oklabOf(color);
    qreal h = std::atan2(lab.b, lab.a) * 180.0 / M_PI;
    if (h < 0) h += 360.0;
    return {lab.l, std::hypot(lab.a, lab.b), h};
}

/// Converts to linear sRGB without clamping, so that callers can check the gamut
void linearRgbFromOklch(const Oklch &c, qreal &r, qreal &g, qreal &b)
{
    const qreal hr = c.h * M_PI / 180.0;
    KisColorMath::linearFromOklab({c.l, c.c * std::cos(hr), c.c * std::sin(hr)}, r, g, b);
}

bool inGamut(qreal r, qreal g, qreal b)
{
    const qreal e = 1e-4;
    return r >= -e && r <= 1 + e && g >= -e && g <= 1 + e && b >= -e && b <= 1 + e;
}

QColor rgbFromLinear(qreal r, qreal g, qreal b)
{
    // the gamut test lets a hair past the edge through, and QColor makes a value outside 0..1 an invalid (black) color:
    // blues at their most vivid have red a little below zero
    auto channel = [](qreal linear) { return qBound(0.0, KisColorMath::toSrgb(linear), 1.0); };
    return QColor::fromRgbF(channel(r), channel(g), channel(b));
}

bool inGamut(const Oklch &c)
{
    qreal r, g, b;
    linearRgbFromOklch(c, r, g, b);
    return inGamut(r, g, b);
}

QColor rgbFromOklch(const Oklch &c)
{
    qreal r, g, b;
    linearRgbFromOklch(c, r, g, b);
    return rgbFromLinear(r, g, b);
}

/// The sRGB color, or an invalid QColor if it is out of gamut
QColor rgbIfInGamut(const Oklch &c)
{
    qreal r, g, b;
    linearRgbFromOklch(c, r, g, b);
    return inGamut(r, g, b) ? rgbFromLinear(r, g, b) : QColor();
}

/// The highest in-gamut chroma at this lightness and hue
qreal maxChroma(qreal l, qreal h)
{
    // near black the gamut test's tolerance lets any chroma pass, so taper the maximum linearly to zero instead
    const qreal darkest = 0.12;
    if (l < darkest) {
        return l <= 0.0 ? 0.0 : maxChroma(darkest, h) * l / darkest;
    }
    // out from gray in steps to the first color past the edge, then finer: for some dark blues the colors in
    // gamut aren't one stretch, and halving the whole range landed on either edge from one lightness to the next
    const qreal step = 0.005;
    qreal low = 0.0;
    while (low + step <= 0.4 && inGamut({l, low + step, h})) low += step;
    qreal high = low + step;
    for (int i = 0; i < 12; i++) {
        const qreal mid = (low + high) / 2;
        (inGamut({l, mid, h}) ? low : high) = mid;
    }
    return low;
}

/// Converts to 16-bit sRGB, since the pickers are finer than 8 bits
KoColor koColorFromRgb(const QColor &rgb)
{
    KoColorSpaceRegistry *registry = KoColorSpaceRegistry::instance();
    KoColor color(registry->rgb16(registry->rgb8()->profile()));
    auto *pixel = reinterpret_cast<KoBgrU16Traits::Pixel *>(color.data());
    const QRgba64 rgba = rgb.rgba64();
    pixel->red = rgba.red();
    pixel->green = rgba.green();
    pixel->blue = rgba.blue();
    pixel->alpha = rgba.alpha();
    return color;
}

/// A knob filled with \p fill, with a white ring and a dark edge so that it shows on any color
void drawKnob(QPainter &painter, const QPointF &center, qreal radius, const QColor &fill)
{
    painter.setPen(QPen(QColor(0, 0, 0, 150), 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawEllipse(center, radius + 0.5, radius + 0.5);
    painter.setPen(QPen(Qt::white, 2.5));
    painter.setBrush(fill);
    painter.drawEllipse(center, radius - 1.25, radius - 1.25);
}

/**
 * The color of a hue: its most vivid color, the cusp of the sRGB gamut. At one lightness and chroma for all,
 * yellows and cyans came out dull and alike.
 */
QColor hueColor(qreal h)
{
    // Björn Ottosson's cusp of the sRGB gamut (from Okhsv): the most saturated chroma/lightness ratio of the hue
    // (a polynomial fit and one Halley step), then the lightness at which it touches the gamut
    const qreal a = std::cos(h * M_PI / 180.0);
    const qreal b = std::sin(h * M_PI / 180.0);
    qreal k0, k1, k2, k3, k4, wl, wm, ws;
    if (-1.88170328 * a - 0.80936493 * b > 1) {
        // red is the channel that runs out
        k0 = 1.19086277; k1 = 1.76576728; k2 = 0.59662641; k3 = 0.75515197; k4 = 0.56771245;
        wl = 4.0767416621; wm = -3.3077115913; ws = 0.2309699292;
    } else if (1.81444104 * a - 1.19445276 * b > 1) {
        // green
        k0 = 0.73956515; k1 = -0.45954404; k2 = 0.08285427; k3 = 0.12541070; k4 = 0.14503204;
        wl = -1.2684380046; wm = 2.6097574011; ws = -0.3413193965;
    } else {
        // blue
        k0 = 1.35733652; k1 = -0.00915799; k2 = -1.15130210; k3 = -0.50559606; k4 = 0.00692167;
        wl = -0.0041960863; wm = -0.7034186147; ws = 1.7076147010;
    }
    qreal saturation = k0 + k1 * a + k2 * b + k3 * a * a + k4 * a * b;
    const qreal kl = 0.3963377774 * a + 0.2158037573 * b;
    const qreal km = -0.1055613458 * a - 0.0638541728 * b;
    const qreal ks = -0.0894841775 * a - 1.2914855480 * b;
    {
        const qreal l = 1 + saturation * kl, m = 1 + saturation * km, s = 1 + saturation * ks;
        const qreal f = wl * l * l * l + wm * m * m * m + ws * s * s * s;
        const qreal f1 = wl * 3 * kl * l * l + wm * 3 * km * m * m + ws * 3 * ks * s * s;
        const qreal f2 = wl * 6 * kl * kl * l + wm * 6 * km * km * m + ws * 6 * ks * ks * s;
        saturation -= f * f1 / (f1 * f1 - 0.5 * f * f2);
    }
    qreal r, g, bl;
    KisColorMath::linearFromOklab({1, saturation * a, saturation * b}, r, g, bl);
    const qreal lightness = std::cbrt(1 / qMax(qMax(r, g), qMax(bl, 1e-6)));
    return rgbFromOklch({lightness, lightness * saturation, h});
}

/// The OKLCH ring's angle at \p offset from its center, in degrees clockwise from the top
qreal ringAngleAt(const QPointF &offset)
{
    return std::fmod(std::atan2(offset.x(), -offset.y()) * 180.0 / M_PI + 360.0, 360.0);
}

const QVector<qreal> &ringHues()
{
    // the OKLCH hues of the pure colors every quarter degree, unwrapped. Around blue they stall and even fall back a
    // little (HSV 229° to 240° are all near 264°), which made the ring stick and its knob jump there: where they rise
    // slower than 0.15° a degree, they rise evenly instead (no hue moves more than 2°)
    static const QVector<qreal> hues = []() {
        const int count = 1441;
        const qreal least = 0.15 * 0.25;
        QVector<qreal> hues(count);
        qreal turns = 0;
        for (int i = 0; i < count; i++) {
            qreal h = oklchFromRgb(QColor::fromHsvF(std::fmod(i * 0.25 / 360.0, 1.0), 1.0, 1.0)).h + turns;
            if (i > 0 && h < hues[i - 1] - 180.0) {
                turns += 360.0;
                h += 360.0;
            }
            hues[i] = h;
        }
        for (int i = 1; i < count;) {
            if (hues[i] - hues[i - 1] >= least - 1e-9) {
                i++;
                continue;
            }
            int a = i - 1;
            int b = i;
            while (hues[b] - hues[a] < least * (b - a) && (a > 0 || b < count - 1)) {
                a = qMax(0, a - 1);
                b = qMin(count - 1, b + 1);
            }
            const qreal from = hues[a];
            const qreal to = hues[b];
            for (int k = a; k <= b; k++) hues[k] = from + (to - from) * (k - a) / (b - a);
            i = a + 1;
        }
        return hues;
    }();
    return hues;
}

/**
 * The OKLCH ring and hue track are spaced as an HSV wheel is: at \p hsvHue (degrees) is the pure color of that
 * HSV hue, and picking there picks its OKLCH hue. Spaced by OKLCH hue, greens and blues took most of the ring.
 */
qreal oklchHueOfHsv(qreal hsvHue)
{
    const QVector<qreal> &hues = ringHues();
    qreal at = std::fmod(hsvHue, 360.0);
    if (at < 0) at += 360.0;
    const int i = qMin(int(at * 4), hues.size() - 2);
    const qreal along = at * 4 - i;
    return std::fmod(hues[i] + (hues[i + 1] - hues[i]) * along, 360.0);
}

/// Where OKLCH hue \p oklchHue is on the HSV-spaced ring and track, in HSV degrees (see oklchHueOfHsv())
qreal hsvHueOfOklch(qreal oklchHue)
{
    const QVector<qreal> &hues = ringHues();
    qreal h = std::fmod(oklchHue - hues.first(), 360.0);
    if (h < 0) h += 360.0;
    h += hues.first();
    const auto above = std::upper_bound(hues.cbegin(), hues.cend(), h);
    const int i = qBound(1, int(above - hues.cbegin()), hues.size() - 1);
    const qreal span = hues[i] - hues[i - 1];
    const qreal along = span > 0 ? (h - hues[i - 1]) / span : 0.0;
    return std::fmod((i - 1 + along) * 0.25, 360.0);
}

/// Darkens \p color to mark the area past the edge of sRGB
QRgb veiled(const QColor &color)
{
    const qreal veil = 0.66;
    return qRgb(qRound(color.red() * (1 - veil) + 13 * veil), qRound(color.green() * (1 - veil) + 18 * veil),
                qRound(color.blue() * (1 - veil) + 24 * veil));
}

/// Clears the pixels of \p image outside \p shape, with antialiased edges
void clipTo(QImage &image, const QPainterPath &shape)
{
    QImage mask(image.size(), QImage::Format_ARGB32_Premultiplied);
    mask.fill(Qt::transparent);
    {
        QPainter painter(&mask);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.fillPath(shape, Qt::black);
    }
    QPainter painter(&image);
    painter.setCompositionMode(QPainter::CompositionMode_DestinationIn);
    painter.drawImage(0, 0, mask);
}

/// Where the hues are: around a ring, or down a strip beside the shape
enum class HueLayout { Ring, Strip };
/// The shape the hue's colors are picked in: HSV's triangle or square, or OKLCH's lightness by chroma
enum class PickerShape { Triangle, Square, Oklch };

/**
 * The picker: the hues around a ring (clockwise from red at the top) or down a strip beside the shape, and the shape
 * the hue's colors are picked in. HSV's triangle has black at the top and white and the pure hue at the bottom
 * corners, as Krita's Advanced Color Selector draws it; HSV's square has saturation across and value up; OKLCH's
 * has chroma across and lightness up, with the colors past sRGB veiled and its edge drawn.
 */
class ColorPicker : public QWidget
{
public:
    std::function<void()> started;
    /// The hue as HSV has it, 0..1
    std::function<void(qreal hue)> huePicked;
    std::function<void(qreal s, qreal v)> svPicked;
    /// \p x: chroma as a fraction of maxPlaneChroma; \p y: 0 is white, 1 is black
    std::function<void(qreal x, qreal y)> planePicked;

    explicit ColorPicker(QWidget *parent) : QWidget(parent)
    {
        setMinimumSize(140, 140);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setCursor(Qt::CrossCursor);
        // a long press here picks a color; it must not open a context menu
        setProperty("ownLongPress", true);
    }

    QSize sizeHint() const override { return QSize(240, 240); }

    void setStyle(HueLayout layout, PickerShape shape)
    {
        if (layout == m_layout && shape == m_shape) return;
        m_layout = layout;
        m_shape = shape;
        m_hues = QImage();
        m_plane = QImage();
        update();
    }

    /// The color as HSV's hue, saturation and value, 0..1
    void setHsv(qreal hue, qreal s, qreal v, const QColor &rgb)
    {
        if (hue != m_hue) m_plane = QImage();
        m_hue = hue;
        m_s = s;
        m_v = v;
        m_rgb = rgb;
        update();
    }

    /// The color as OKLCH, its hue where the pure color of that hue is
    void setOklch(const Oklch &color, const QColor &rgb)
    {
        if (color.h != m_color.h) m_plane = QImage();
        m_color = color;
        m_hue = hsvHueOfOklch(color.h) / 360.0;
        m_rgb = rgb;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        const qreal dpr = devicePixelRatioF();
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        paintHues(painter, dpr);
        paintShape(painter, dpr);
        const QColor pure = QColor::fromHsvF(std::fmod(m_hue, 1.0), 1.0, 1.0);
        drawKnob(painter, huePoint(), (m_layout == HueLayout::Ring ? ringWidth() : stripWidth) / 2 + 2, pure);
        drawKnob(painter, shapePoint(), 8, m_rgb);
    }

    void resizeEvent(QResizeEvent *event) override
    {
        QWidget::resizeEvent(event);
        m_hues = QImage();
        m_plane = QImage();
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton) return;
        m_part = partAt(event->pos());
        if (m_part == None) return;
        if (started) started();
        pick(m_part, event->pos());
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (m_part != None && (event->buttons() & Qt::LeftButton)) pick(m_part, event->pos());
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) m_part = None;
    }

private:
    enum Part { None, Hues, Shape };
    static constexpr qreal stripWidth = 18;
    /// How far the hue's knob reaches past the strip
    static constexpr qreal knobRoom = stripWidth / 2 + 3;

    QPointF center() const { return QRectF(rect()).center(); }
    qreal outerRadius() const { return qMin(width(), height()) / 2.0 - 1; }
    qreal ringWidth() const { return qBound(10.0, outerRadius() * 0.11, 18.0); }
    qreal innerRadius() const { return outerRadius() - ringWidth(); }

    /// Where the shape is: a square inside the ring with a small gap, or beside the strip; on whole pixels
    QRectF planeRect() const
    {
        if (m_layout == HueLayout::Strip) {
            // room around for the knobs, which reach past the edges
            const QRectF area = QRectF(rect()).adjusted(knobRoom, knobRoom, -knobRoom, -knobRoom);
            const qreal side = std::floor(qMax(0.0, qMin(area.width() - stripWidth - KisModernStyle::space2, area.height())));
            const qreal left = std::round(area.left() + (area.width() - side - KisModernStyle::space2 - stripWidth) / 2);
            return QRectF(left, std::round(area.top() + (area.height() - side) / 2), side, side);
        }
        const qreal side = std::floor((innerRadius() - 6) * std::sqrt(2.0));
        const QPointF c = center();
        return QRectF(std::round(c.x() - side / 2), std::round(c.y() - side / 2), side, side);
    }

    QRectF stripRect() const
    {
        const QRectF plane = planeRect();
        return QRectF(plane.right() + KisModernStyle::space2, plane.top(), stripWidth, plane.height());
    }

    /// The triangle's corners, scaled by \p scale: black at the top, white at the bottom left, the pure hue at the
    /// bottom right
    void corners(QPointF &black, QPointF &white, QPointF &pure, qreal scale = 1.0) const
    {
        if (m_layout == HueLayout::Ring) {
            // in the circle just inside the ring
            const QPointF c = center() * scale;
            const qreal r = (innerRadius() - 4) * scale;
            black = c + QPointF(0, -r);
            white = c + QPointF(-r * std::sqrt(3.0) / 2, r / 2);
            pure = c + QPointF(r * std::sqrt(3.0) / 2, r / 2);
        } else {
            const QRectF plane = planeRect();
            black = QPointF(plane.center().x(), plane.top()) * scale;
            white = plane.bottomLeft() * scale;
            pure = plane.bottomRight() * scale;
        }
    }

    QPointF huePoint() const
    {
        if (m_layout == HueLayout::Strip) {
            const QRectF strip = stripRect();
            return QPointF(strip.center().x(), strip.top() + m_hue * strip.height());
        }
        const qreal angle = m_hue * 2 * M_PI;
        return center() + (outerRadius() - ringWidth() / 2) * QPointF(std::sin(angle), -std::cos(angle));
    }

    QPointF shapePoint() const
    {
        const QRectF plane = planeRect();
        switch (m_shape) {
        case PickerShape::Triangle: {
            QPointF black, white, pure;
            corners(black, white, pure);
            const QPointF left = black + (white - black) * m_v;
            return left + (black + (pure - black) * m_v - left) * m_s;
        }
        case PickerShape::Square:
            return QPointF(plane.left() + m_s * plane.width(), plane.top() + (1.0 - m_v) * plane.height());
        case PickerShape::Oklch:
            return QPointF(plane.left() + qMin(1.0, m_color.c / maxPlaneChroma) * plane.width(), plane.top() + (1.0 - m_color.l) * plane.height());
        }
        return plane.center();
    }

    Part partAt(const QPoint &pos) const
    {
        if (m_layout == HueLayout::Strip) {
            const QRectF strip = stripRect();
            if (pos.y() < strip.top() - 8 || pos.y() > strip.bottom() + 8) return None;
            return pos.x() >= strip.left() - KisModernStyle::space1 ? Hues : Shape;
        }
        const QPointF offset = QPointF(pos) - center();
        const qreal distance = std::hypot(offset.x(), offset.y());
        if (distance > outerRadius() + 2) return None;
        // the whole inside of the ring belongs to the shape, so that its corners are easy to reach
        return distance >= innerRadius() - 2 ? Hues : Shape;
    }

    void pick(Part part, const QPoint &pos)
    {
        if (part == Hues) {
            const QRectF strip = stripRect();
            const qreal hue = m_layout == HueLayout::Ring ? ringAngleAt(QPointF(pos) - center()) / 360.0
                                                          : qBound(0.0, (pos.y() - strip.top()) / strip.height(), 0.9999);
            if (huePicked) huePicked(std::fmod(hue, 1.0));
            return;
        }
        const QRectF plane = planeRect();
        const qreal x = qBound(0.0, (pos.x() - plane.left()) / plane.width(), 1.0);
        const qreal y = qBound(0.0, (pos.y() - plane.top()) / plane.height(), 1.0);
        switch (m_shape) {
        case PickerShape::Triangle: {
            // outside the triangle, the nearest row, then the nearest point on it
            QPointF black, white, pure;
            corners(black, white, pure);
            const qreal v = qBound(0.0, (pos.y() - black.y()) / (white.y() - black.y()), 1.0);
            const QPointF left = black + (white - black) * v;
            const qreal rowLength = (black.x() + (pure.x() - black.x()) * v) - left.x();
            // at the apex every saturation gives black, so keep the current one
            const qreal s = rowLength < 1.0 ? m_s : qBound(0.0, (pos.x() - left.x()) / rowLength, 1.0);
            if (svPicked) svPicked(s, v);
            break;
        }
        case PickerShape::Square:
            if (svPicked) svPicked(x, 1.0 - y);
            break;
        case PickerShape::Oklch:
            if (planePicked) planePicked(x, y);
            break;
        }
    }

    void paintHues(QPainter &painter, qreal dpr)
    {
        if (m_layout == HueLayout::Strip) {
            // stops every ten degrees: between them HSV's pure colors run straight in RGB
            const QRectF strip = stripRect();
            QLinearGradient hues(strip.topLeft(), strip.bottomLeft());
            for (int i = 0; i <= 36; i++) hues.setColorAt(i / 36.0, QColor::fromHsvF(std::fmod(i / 36.0, 1.0), 1.0, 1.0));
            painter.fillPath(KisModernStyle::squircle(strip, stripWidth / 2), hues);
            return;
        }
        const QSize pixels = size() * dpr;
        if (m_hues.size() != pixels) {
            m_hues = QImage(pixels, QImage::Format_ARGB32_Premultiplied);
            m_hues.fill(Qt::transparent);
            const QPointF c(pixels.width() / 2.0, pixels.height() / 2.0);
            const qreal outer = outerRadius() * dpr;
            const qreal inner = innerRadius() * dpr;
            for (int y = 0; y < pixels.height(); y++) {
                QRgb *line = reinterpret_cast<QRgb *>(m_hues.scanLine(y));
                for (int x = 0; x < pixels.width(); x++) {
                    const QPointF offset(x + 0.5 - c.x(), y + 0.5 - c.y());
                    const qreal distance = std::hypot(offset.x(), offset.y());
                    if (distance > inner - 2 && distance < outer + 2) line[x] = QColor::fromHsvF(ringAngleAt(offset) / 360.0, 1.0, 1.0).rgb();
                }
            }
            QPainterPath ring;
            ring.addEllipse(c, outer, outer);
            ring.addEllipse(c, inner, inner);
            clipTo(m_hues, ring);
            m_hues.setDevicePixelRatio(dpr);
        }
        painter.drawImage(0, 0, m_hues);
    }

    void paintShape(QPainter &painter, qreal dpr)
    {
        const QRectF plane = planeRect();
        if (m_shape == PickerShape::Triangle) {
            if (m_plane.size() != size() * dpr) m_plane = renderTriangle(size() * dpr, dpr);
            painter.drawImage(0, 0, m_plane);
            return;
        }
        const QSize pixels = (plane.size() * dpr).toSize();
        if (m_plane.size() != pixels) m_plane = m_shape == PickerShape::Square ? renderSquare(pixels, dpr) : renderOklch(pixels, dpr);
        painter.save();
        painter.setClipPath(KisModernStyle::squircle(plane, KisModernStyle::tileRadius));
        painter.drawImage(plane.topLeft(), m_plane);
        painter.restore();
    }

    QImage renderTriangle(const QSize &pixels, qreal dpr) const
    {
        QImage image(pixels, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPointF black, white, pure;
        corners(black, white, pure, dpr);
        // at a fixed hue, HSV is v * ((1 - s) * white + s * pure hue) in encoded RGB
        const QColor hue = QColor::fromHsvF(std::fmod(m_hue, 1.0), 1.0, 1.0);
        const qreal pr = hue.redF(), pg = hue.greenF(), pb = hue.blueF();
        const int top = qMax(0, int(black.y()) - 1);
        const int bottom = qMin(pixels.height() - 1, int(white.y()) + 1);
        const int left = qMax(0, int(white.x()) - 1);
        const int right = qMin(pixels.width() - 1, int(pure.x()) + 1);
        for (int y = top; y <= bottom; y++) {
            QRgb *line = reinterpret_cast<QRgb *>(image.scanLine(y));
            const qreal v = qBound(0.0, (y + 0.5 - black.y()) / (white.y() - black.y()), 1.0);
            const qreal rowStart = black.x() + (white.x() - black.x()) * v;
            const qreal rowLength = black.x() + (pure.x() - black.x()) * v - rowStart;
            for (int x = left; x <= right; x++) {
                // also fill past the edges, clamped, so that the antialiased border gets the edge color
                const qreal s = rowLength > 0 ? qBound(0.0, (x + 0.5 - rowStart) / rowLength, 1.0) : 0.0;
                line[x] = qRgb(qRound(255 * v * (1 - s + s * pr)), qRound(255 * v * (1 - s + s * pg)), qRound(255 * v * (1 - s + s * pb)));
            }
        }
        QPainterPath triangle;
        triangle.moveTo(black);
        triangle.lineTo(pure);
        triangle.lineTo(white);
        triangle.closeSubpath();
        clipTo(image, triangle);
        image.setDevicePixelRatio(dpr);
        return image;
    }

    QImage renderSquare(const QSize &pixels, qreal dpr) const
    {
        QImage image(pixels, QImage::Format_ARGB32_Premultiplied);
        const QColor hue = QColor::fromHsvF(std::fmod(m_hue, 1.0), 1.0, 1.0);
        const qreal pr = hue.redF(), pg = hue.greenF(), pb = hue.blueF();
        for (int y = 0; y < pixels.height(); y++) {
            QRgb *line = reinterpret_cast<QRgb *>(image.scanLine(y));
            const qreal v = 1.0 - qreal(y) / qMax(1, pixels.height() - 1);
            for (int x = 0; x < pixels.width(); x++) {
                const qreal s = qreal(x) / qMax(1, pixels.width() - 1);
                line[x] = qRgb(qRound(255 * v * (1 - s + s * pr)), qRound(255 * v * (1 - s + s * pg)), qRound(255 * v * (1 - s + s * pb)));
            }
        }
        image.setDevicePixelRatio(dpr);
        return image;
    }

    QImage renderOklch(const QSize &pixels, qreal dpr) const
    {
        QImage image(pixels, QImage::Format_ARGB32_Premultiplied);
        if (pixels.isEmpty()) return image;
        const qreal h = m_color.h;
        // made again at every hue while the ring is dragged: the hue's angle once, and 8-bit sRGB found among
        // the 255 steps' linear thresholds rather than through a power per channel and pixel
        const qreal cosH = std::cos(h * M_PI / 180.0);
        const qreal sinH = std::sin(h * M_PI / 180.0);
        static const std::array<qreal, 255> steps = []() {
            std::array<qreal, 255> thresholds;
            for (int i = 0; i < 255; i++) thresholds[i] = toLinear((i + 0.5) / 255.0);
            return thresholds;
        }();
        auto channel = [](qreal linear) { return int(std::upper_bound(steps.begin(), steps.end(), linear) - steps.begin()); };
        auto rgbAt = [&](qreal l, qreal c) {
            const qreal A = c * cosH;
            const qreal B = c * sinH;
            const qreal lc = l + 0.3963377774 * A + 0.2158037573 * B;
            const qreal mc = l - 0.1055613458 * A - 0.0638541728 * B;
            const qreal sc = l - 0.0894841775 * A - 1.2914855480 * B;
            const qreal L = lc * lc * lc, M = mc * mc * mc, S = sc * sc * sc;
            return qRgb(channel(4.0767416621 * L - 3.3077115913 * M + 0.2309699292 * S),
                        channel(-1.2684380046 * L + 2.6097574011 * M - 0.3413193965 * S),
                        channel(-0.0041960863 * L - 0.7034186147 * M + 1.7076147010 * S));
        };
        QPolygonF edge;
        for (int y = 0; y < pixels.height(); y++) {
            QRgb *line = reinterpret_cast<QRgb *>(image.scanLine(y));
            const qreal l = 1.0 - qreal(y) / qMax(1, pixels.height() - 1);
            const qreal widest = maxChroma(l, h);
            const QRgb past = veiled(rgbFromOklch({l, widest, h}));
            for (int x = 0; x < pixels.width(); x++) {
                const qreal c = maxPlaneChroma * x / qMax(1, pixels.width() - 1);
                line[x] = c <= widest ? rgbAt(l, c) : past;
            }
            edge << QPointF(qMin(1.0, widest / maxPlaneChroma) * (pixels.width() - 1) + 0.5, y + 0.5);
        }
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(QPen(QColor(255, 255, 255, 140), 1.2 * dpr));
        painter.drawPolyline(edge);
        painter.end();
        image.setDevicePixelRatio(dpr);
        return image;
    }

    HueLayout m_layout {HueLayout::Ring};
    PickerShape m_shape {PickerShape::Oklch};
    qreal m_hue {0.0};
    qreal m_s {0.0};
    qreal m_v {0.0};
    Oklch m_color {-1.0, -1.0, -1.0};
    QColor m_rgb;
    Part m_part {None};
    QImage m_hues;
    QImage m_plane;
};

/// A row of color chips; a click picks one, a drag takes it onto the canvas
class Swatches : public QWidget
{
public:
    std::function<void(int index)> picked;

    Swatches(int chipSize, QWidget *parent) : QWidget(parent), m_chip(chipSize)
    {
        setFixedHeight(chipSize);
    }

    void setColors(const QVector<QColor> &colors)
    {
        m_colors = colors;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        QColor border = palette().color(QPalette::WindowText);
        border.setAlphaF(0.18);
        painter.setPen(QPen(border, 1));
        for (int i = 0; i < shownCount(); i++) {
            painter.setBrush(m_colors[i]);
            painter.drawPath(KisModernStyle::squircle(chipRect(i).adjusted(0.5, 0.5, -0.5, -0.5), 4));
        }
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        m_pressed = event->button() == Qt::LeftButton ? chipAt(event->pos()) : -1;
        m_pressAt = event->pos();
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (m_pressed < 0 || (event->pos() - m_pressAt).manhattanLength() < QApplication::startDragDistance()) return;
        const int index = m_pressed;
        m_pressed = -1;
        KisColorDrop::startDrag(this, m_colors[index]);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        const int index = m_pressed;
        m_pressed = -1;
        if (index >= 0 && picked && chipAt(event->pos()) == index) picked(index);
    }

private:
    int chipAt(const QPoint &pos) const
    {
        for (int i = 0; i < shownCount(); i++) {
            if (chipRect(i).contains(pos)) return i;
        }
        return -1;
    }

    int m_pressed {-1};
    QPoint m_pressAt;

    QRectF chipRect(int i) const { return QRectF(i * (m_chip + 4), 0, m_chip, m_chip); }
    /// How many chips fit
    int shownCount() const { return qMin(int(m_colors.size()), (width() + 4) / (m_chip + 4)); }

    int m_chip;
    QVector<QColor> m_colors;
};

} // namespace

/// A small picture of a picker: the hues around a ring or down a strip, and the shape inside or beside them
static QIcon pickerIcon(HueLayout layout, PickerShape shape)
{
    const qreal dpr = 2.0;
    const qreal size = 20;
    QImage image(QSize(size * dpr, size * dpr), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    image.setDevicePixelRatio(dpr);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);

    // a faint dark edge, so light hues show on a light panel too
    const QPen edge(QColor(0, 0, 0, 70), 0.8);
    QRectF area;
    if (layout == HueLayout::Ring) {
        const QRectF bounds(1, 1, size - 2, size - 2);
        for (int i = 0; i < 72; i++) {
            painter.setBrush(QColor::fromHsvF(i / 72.0, 1.0, 1.0));
            painter.drawPie(bounds, int((90 - i * 5) * 16), -6 * 16);
        }
        painter.setCompositionMode(QPainter::CompositionMode_Clear);
        painter.setBrush(Qt::black);
        painter.drawEllipse(bounds.center(), 6.4, 6.4);
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(edge);
        painter.drawEllipse(bounds.adjusted(0.4, 0.4, -0.4, -0.4));
        painter.setPen(Qt::NoPen);
        area = QRectF(bounds.center() - QPointF(4.2, 4.2), QSizeF(8.4, 8.4));
    } else {
        QLinearGradient hues(QPointF(0, 1.5), QPointF(0, size - 1.5));
        for (int i = 0; i <= 6; i++) hues.setColorAt(i / 6.0, QColor::fromHsvF(std::fmod(i / 6.0, 1.0), 1.0, 1.0));
        painter.setBrush(hues);
        painter.setPen(edge);
        painter.drawRoundedRect(QRectF(size - 5.5, 1.5, 4, size - 3), 2, 2);
        area = QRectF(1.5, (size - 12.5) / 2, 12.5, 12.5);
    }

    const QColor hue = hueColor(35);
    // the hue's colors: white to the hue across, darkening toward black
    auto shaded = [&](const QPainterPath &shape, bool blackAtTop) {
        QLinearGradient across(area.topLeft(), area.topRight());
        across.setColorAt(0, Qt::white);
        across.setColorAt(1, hue);
        painter.fillPath(shape, across);
        QLinearGradient down(area.topLeft(), area.bottomLeft());
        down.setColorAt(blackAtTop ? 0 : 1, Qt::black);
        down.setColorAt(blackAtTop ? 1 : 0, QColor(0, 0, 0, 0));
        painter.fillPath(shape, down);
        painter.strokePath(shape, edge);
    };
    switch (shape) {
    case PickerShape::Triangle: {
        QPainterPath triangle;
        triangle.moveTo(area.center().x(), area.top());
        triangle.lineTo(area.bottomRight());
        triangle.lineTo(area.bottomLeft());
        triangle.closeSubpath();
        shaded(triangle, true);
        break;
    }
    case PickerShape::Square: {
        QPainterPath square;
        square.addRoundedRect(area, 1.5, 1.5);
        shaded(square, false);
        break;
    }
    case PickerShape::Oklch: {
        // lightness up, and the edge of sRGB bulging toward the hue
        QPainterPath square;
        square.addRoundedRect(area, 1.5, 1.5);
        QLinearGradient light(area.topLeft(), area.bottomLeft());
        light.setColorAt(0, Qt::white);
        light.setColorAt(0.45, hue);
        light.setColorAt(1, Qt::black);
        painter.fillPath(square, light);
        QPainterPath past;
        past.moveTo(area.topLeft());
        past.quadTo(QPointF(area.right() + area.width() * 0.2, area.center().y() - area.height() * 0.1), area.bottomLeft());
        past.lineTo(area.bottomRight());
        past.lineTo(area.topRight());
        past.closeSubpath();
        painter.fillPath(past.intersected(square), QColor(20, 22, 28, 170));
        painter.strokePath(square, edge);
        break;
    }
    }
    painter.end();
    return QIcon(QPixmap::fromImage(image));
}

namespace {

/// How a typed color is written: as a code, or as the numbers of a model
enum class ColorFormat { Hex, Rgb, RgbFloat, Oklch, Hsv };
/// A color as typed: a code, or a model's numbers
struct TypedColor {
    ColorFormat format {ColorFormat::Hex};
    QVector<qreal> parts;
    QColor code;
};

/**
 * Reads a color in any of the ways it is written: 00F2FC, #0ef, rgb(0, 242, 252), rgb(0 0.95 0.99), 0 242 252,
 * oklch(0.86 0.15 196), hsv(184, 100%, 99%), hsl(…). Bare numbers are red, green and blue.
 */
std::optional<TypedColor> readColor(const QString &text)
{
    const QString typed = text.trimmed().toLower();
    static const QRegularExpression code(QStringLiteral("^#?([0-9a-f]{3}|[0-9a-f]{6})$"));
    QRegularExpressionMatch match = code.match(typed);
    if (match.hasMatch()) {
        QString digits = match.captured(1);
        if (digits.size() == 3) digits = QString(digits[0]) + digits[0] + digits[1] + digits[1] + digits[2] + digits[2];
        return TypedColor{ColorFormat::Hex, {}, QColor(QLatin1Char('#') + digits)};
    }
    ColorFormat format = ColorFormat::Rgb;
    QString body = typed;
    bool hsl = false;
    static const QRegularExpression call(QStringLiteral("^([a-z]+)\\s*\\((.*)\\)$"));
    match = call.match(typed);
    if (match.hasMatch()) {
        const QString name = match.captured(1);
        body = match.captured(2);
        if (name == QLatin1String("rgb") || name == QLatin1String("rgba")) format = ColorFormat::Rgb;
        else if (name == QLatin1String("oklch")) format = ColorFormat::Oklch;
        else if (name == QLatin1String("hsv") || name == QLatin1String("hsb")) format = ColorFormat::Hsv;
        else if (name == QLatin1String("hsl") || name == QLatin1String("hsla")) hsl = true;
        else return std::nullopt;
    }
    const QStringList pieces = body.split(QRegularExpression(QStringLiteral("[\\s,;/]+")), Qt::SkipEmptyParts);
    if (pieces.size() < 3) return std::nullopt;
    QVector<qreal> values;
    QVector<bool> percent;
    bool fractions = true;
    for (int i = 0; i < 3; i++) {
        QString piece = pieces[i];
        percent << piece.endsWith(QLatin1Char('%'));
        piece.remove(QLatin1Char('%')).remove(QStringLiteral("°")).remove(QStringLiteral("deg"));
        bool ok = false;
        const qreal value = piece.toDouble(&ok);
        if (!ok) return std::nullopt;
        values << value;
        if (value > 1.0 || percent.last()) fractions = false;
    }
    if (hsl) {
        auto share = [&](int i) { return qBound(0.0, percent[i] || values[i] > 1.0 ? values[i] / 100.0 : values[i], 1.0); };
        const QColor rgb = QColor::fromHslF(std::fmod(std::fmod(values[0], 360.0) + 360.0, 360.0) / 360.0, share(1), share(2));
        return TypedColor{ColorFormat::Rgb, {qreal(rgb.red()), qreal(rgb.green()), qreal(rgb.blue())}, QColor()};
    }
    switch (format) {
    case ColorFormat::Rgb: {
        // 0 to 1 when every part is at most 1 and one has a point (rgb(1 1 1) counts to 255, rgb(1. 1. 1.) is white)
        const bool floats = fractions && body.contains(QLatin1Char('.'));
        for (int i = 0; i < 3; i++) {
            if (percent[i]) values[i] = values[i] / 100.0 * (floats ? 1.0 : 255.0);
            values[i] = qBound(0.0, values[i], floats ? 1.0 : 255.0);
        }
        return TypedColor{floats ? ColorFormat::RgbFloat : ColorFormat::Rgb, values, QColor()};
    }
    case ColorFormat::Oklch:
        // as CSS has it: lightness 100% is 1, chroma 100% is 0.4
        if (percent[0]) values[0] /= 100.0;
        if (percent[1]) values[1] *= 0.4 / 100.0;
        return TypedColor{ColorFormat::Oklch, {qBound(0.0, values[0], 1.0), qBound(0.0, values[1], 0.4), values[2]}, QColor()};
    case ColorFormat::Hsv:
        for (int i = 1; i < 3; i++) {
            if (!percent[i] && values[i] <= 1.0 && pieces[i].contains(QLatin1Char('.'))) values[i] *= 100.0;
            values[i] = qBound(0.0, values[i], 100.0);
        }
        return TypedColor{ColorFormat::Hsv, values, QColor()};
    default:
        return std::nullopt;
    }
}

} // namespace

/// The hex code field: dimmed unless it has focus
class HexField : public QLineEdit
{
public:
    /// The color written as each format copies it, in ColorFormat order
    std::function<QStringList()> written;

    explicit HexField(QWidget *parent) : QLineEdit(parent)
    {
        fade();
    }

protected:
    /// Copy, with nothing selected, copies the color in the format copied last (hex at first); Copy as, in any
    void contextMenuEvent(QContextMenuEvent *event) override
    {
        QMenu *menu = createStandardContextMenu();
        const QStringList formats = written ? written() : QStringList();
        QAction *copy = nullptr;
        // Qt's own Copy, by its name, which some Qt versions follow with a tab and the shortcut
        const QString copyName = QCoreApplication::translate("QLineEdit", "&Copy");
        Q_FOREACH (QAction *action, menu->actions()) {
            const QString text = action->text();
            if (action->objectName() == QLatin1String("edit-copy") || text == copyName || text.startsWith(copyName + QLatin1Char('\t'))) {
                copy = action;
            }
        }
        if (copy && !hasSelectedText() && !formats.isEmpty()) {
            const QString chosen = formats.value(copyFormat(formats.size()));
            auto *color = new QAction(i18nc("@action:inmenu %1 is a color as written, like #00F2FC", "Copy %1", chosen), menu);
            connect(color, &QAction::triggered, this, [chosen]() { QApplication::clipboard()->setText(chosen); });
            QMenu *as = new QMenu(i18nc("@title:menu copying the color in another format", "Copy as"), menu);
            for (int i = 0; i < formats.size(); i++) {
                const QString text = formats[i];
                as->addAction(text, this, [i, text]() {
                    QApplication::clipboard()->setText(text);
                    KisConfig(false).writeEntry("StudioColorCopyFormat", i);
                });
            }
            menu->insertAction(copy, color);
            menu->insertMenu(copy, as);
            menu->removeAction(copy);
        }
        menu->exec(event->globalPos());
        delete menu;
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        // Ctrl+C with nothing selected: the color, as Copy has it
        const QStringList formats = written && event->matches(QKeySequence::Copy) && !hasSelectedText() ? written() : QStringList();
        if (!formats.isEmpty()) {
            QApplication::clipboard()->setText(formats.value(copyFormat(formats.size())));
            event->accept();
            return;
        }
        QLineEdit::keyPressEvent(event);
    }

    void focusInEvent(QFocusEvent *event) override
    {
        QLineEdit::focusInEvent(event);
        fade();
    }

    void focusOutEvent(QFocusEvent *event) override
    {
        QLineEdit::focusOutEvent(event);
        fade();
    }

    void changeEvent(QEvent *event) override
    {
        QLineEdit::changeEvent(event);
        if (event->type() == QEvent::PaletteChange && !m_fading) fade();
    }

private:
    void fade()
    {
        m_fading = true;
        QPalette palette = this->palette();
        QColor ink = QApplication::palette(this).color(QPalette::Text);
        ink.setAlphaF(hasFocus() ? 1.0 : 0.5);
        palette.setColor(QPalette::Text, ink);
        setPalette(palette);
        m_fading = false;
    }

    static int copyFormat(int count)
    {
        return qBound(0, KisConfig(true).readEntry("StudioColorCopyFormat", 0), count - 1);
    }

    bool m_fading {false};
};

/**
 * How many stops brighter than white the color is painted, for images that can hold it (floating point):
 * a sun and the number. +1 is twice as bright as white, +2 four times. Dragged, typed, reset by a right click.
 */
class IntensityChip : public QWidget
{
public:
    /// A change begins (the color before becomes the previous one)
    std::function<void()> started;
    std::function<void(qreal stops)> changed;

    explicit IntensityChip(QWidget *parent)
        : QWidget(parent)
    {
        setMouseTracking(true);
        setAttribute(Qt::WA_Hover);
        setFixedSize(48, 26);
        setCursor(Qt::SizeHorCursor);
    }

    void setStops(qreal stops)
    {
        m_stops = stops;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QColor ink = palette().color(QPalette::WindowText);
        const QColor shown = m_stops > 0.0 || underMouse() ? ink : KisModernStyle::secondaryInk(ink);
        // a sun: brighter than white
        const QPointF sun(8, height() / 2.0);
        painter.setPen(Qt::NoPen);
        painter.setBrush(shown);
        painter.drawEllipse(sun, 2.8, 2.8);
        painter.setPen(QPen(shown, 1.3, Qt::SolidLine, Qt::RoundCap));
        for (int i = 0; i < 8; i++) {
            const qreal a = i * M_PI / 4.0;
            const QPointF ray(std::cos(a), std::sin(a));
            painter.drawLine(sun + ray * 4.6, sun + ray * 6.4);
        }
        painter.setPen(shown);
        if (!m_editing) painter.drawText(textRect(), Qt::AlignRight | Qt::AlignVCenter, text());
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton) return;
        m_pressed = true;
        m_moved = false;
        m_pressX = event->pos().x();
        m_pressStops = m_stops;
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (!m_pressed) return;
        const int moved = event->pos().x() - m_pressX;
        if (!m_moved && qAbs(moved) <= 3) return;
        if (!m_moved && started) started();
        m_moved = true;
        // a stop for each 40 px; Shift: finer
        const qreal fine = event->modifiers() & Qt::ShiftModifier ? 0.25 : 1.0;
        set(m_pressStops + moved / 40.0 * fine);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) m_pressed = false;
    }

    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton) return;
        m_editing = true;
        QLineEdit *editor = KisStudioInlineEdit::open(this, textRect().toRect(), QString::number(m_stops, 'f', 1), Qt::AlignRight, font(),
                                                      [this](const QString &typed) {
            bool ok = false;
            const qreal stops = QLocale().toDouble(QString(typed).remove(QLatin1Char('+')).trimmed(), &ok);
            if (!ok) return;
            if (started) started();
            set(stops);
        });
        connect(editor, &QObject::destroyed, this, [this]() {
            m_editing = false;
            update();
        });
        update();
    }

    void contextMenuEvent(QContextMenuEvent *event) override
    {
        event->accept();
        if (qFuzzyIsNull(m_stops)) return;
        if (started) started();
        set(0.0);
    }

    void wheelEvent(QWheelEvent *event) override
    {
        event->accept();
        const int steps = event->angleDelta().y() / 120;
        if (steps) set(m_stops + 0.5 * steps);
    }

    bool event(QEvent *event) override
    {
        if (event->type() == QEvent::Enter || event->type() == QEvent::Leave) update();
        if (event->type() == QEvent::ToolTip) {
            QStringList gestures = KisStudioTips::valueGestures();
            gestures << KisStudioTips::scrollStep();
            const QString tip = KisStudioTips::tip(i18nc("@info:tooltip", "Brighter than white: +1 is twice as bright, +2 four times"), gestures);
            QToolTip::showText(static_cast<QHelpEvent *>(event)->globalPos(), KisStudioTips::html(tip), this);
            return true;
        }
        return QWidget::event(event);
    }

private:
    QRectF textRect() const
    {
        return QRectF(16, 0, width() - 16, height());
    }

    QString text() const
    {
        return m_stops > 0.0 ? QLatin1Char('+') + QLocale().toString(m_stops, 'f', 1) : QStringLiteral("0");
    }

    void set(qreal stops)
    {
        stops = qBound(0.0, std::round(stops * 10.0) / 10.0, maxStops);
        if (qFuzzyCompare(stops + 1.0, m_stops + 1.0)) return;
        m_stops = stops;
        update();
        if (changed) changed(stops);
    }

    static constexpr qreal maxStops = 10.0;
    qreal m_stops {0.0};
    qreal m_pressStops {0.0};
    int m_pressX {0};
    bool m_pressed {false};
    bool m_moved {false};
    bool m_editing {false};
};

/// The linear space with sRGB's primaries that intensity is applied in: what is past white stays past it there
const KoColorSpace *linearSpace()
{
    KoColorSpaceRegistry *registry = KoColorSpaceRegistry::instance();
    return registry->colorSpace(RGBAColorModelID.id(), Float32BitsColorDepthID.id(), registry->p709G10Profile());
}

struct KisStudioColorPanel::Private
{
    QPointer<KoCanvasBase> canvas;
    /// The image whose colors the recent row shows (saved in its .kra)
    QPointer<KisDocument> document;
    QList<QMetaObject::Connection> canvasConnections;

    ColorPicker *picker {nullptr};
    Swatches *currentRow {nullptr};
    Swatches *recentRow {nullptr};
    HexField *hex {nullptr};
    IntensityChip *intensityChip {nullptr};
    /// Stops brighter than white the color is painted, in images that can hold it
    qreal intensity {0.0};
    QToolButton *styleButton {nullptr};
    QMap<HueLayout, QAction *> layoutActions;
    QMap<PickerShape, QAction *> shapeActions;
    QAction *keepAction {nullptr};

    HueLayout layout {HueLayout::Ring};
    PickerShape shape {PickerShape::Oklch};
    /// Turning the hue in an HSV shape keeps how light the color looks
    bool keepLightness {true};
    Oklch color;
    /// The chroma last chosen; changing lightness or hue keeps as much of it as fits in sRGB
    qreal heldChroma {0.0};
    qreal hsvHue {0.0}, hsvS {0.0}, hsvV {0.0}; ///< 0..1
    /// The lightness and saturation last chosen in an HSV shape, which turning the hue keeps as it can
    qreal heldLightness {0.0}, heldSaturation {0.0};
    /// The color shown, in 8 bits
    QColor current;
    /// The color at full precision, as the canvas has it
    KoColor currentColor;
    std::optional<KoColor> previous;

    /// Set while the panel sets the color itself, so that its echo isn't taken for an outside change
    bool applying {false};

    void applyColor(const KoColor &color)
    {
        currentColor = color;
        if (!canvas || !canvas->resourceManager()) return;
        applying = true;
        canvas->resourceManager()->setForegroundColor(color);
        applying = false;
    }

    /// Updates both the OKLCH and the HSV coordinates from a color set elsewhere
    void readCoordinates(const QColor &rgb)
    {
        const Oklch picked = oklchFromRgb(rgb);
        color.l = picked.l;
        color.c = picked.c;
        heldChroma = picked.c;
        // grays have no hue: keep the current one
        if (picked.c > 0.012) color.h = picked.h;
        qreal h, s, v;
        rgb.getHsvF(&h, &s, &v);
        if (h >= 0.0 && s > 0.01) hsvHue = h;
        hsvS = s;
        hsvV = v;
        heldLightness = picked.l;
        heldSaturation = s;
    }

    /**
     * At a new hue, the value that keeps the color as light as the one last chosen, at the saturation last chosen.
     * Where even full value is too dark for that (a light yellow turned to blue), less saturation instead, which
     * comes back when the hue turns back.
     */
    void holdLightness()
    {
        auto lightnessAt = [this](qreal s, qreal v) { return oklchFromRgb(QColor::fromHsvF(hsvHue, s, v)).l; };
        auto solve = [this](const std::function<qreal(qreal)> &lightness) {
            qreal low = 0.0, high = 1.0;
            for (int i = 0; i < 20; i++) {
                const qreal middle = (low + high) / 2;
                if (lightness(middle) < heldLightness) low = middle;
                else high = middle;
            }
            return (low + high) / 2;
        };
        if (lightnessAt(heldSaturation, 1.0) >= heldLightness) {
            hsvS = heldSaturation;
            hsvV = solve([&](qreal v) { return lightnessAt(heldSaturation, v); });
        } else {
            hsvV = 1.0;
            hsvS = 1.0 - solve([&](qreal w) { return lightnessAt(1.0 - w, 1.0); });
        }
    }

    void refresh()
    {
        picker->setStyle(layout, shape);
        if (shape == PickerShape::Oklch) picker->setOklch(color, current);
        else picker->setHsv(hsvHue, hsvS, hsvV, current);
        currentRow->setColors({current, previous ? previous->toQColor() : current});
        hex->setText(current.name().mid(1).toUpper());
    }

    /// The color as copied in each format (ColorFormat order), as CSS writes them where it can, each picker's coordinates kept
    QStringList written() const
    {
        const auto number = [](qreal value, int decimals) { return QString::number(value, 'f', decimals); };
        Oklch lch = color;
        if (shape != PickerShape::Oklch) {
            lch = oklchFromRgb(current);
            if (lch.c <= 0.012) lch.h = color.h;
        }
        qreal h, sat, val;
        current.getHsvF(&h, &sat, &val);
        if (shape != PickerShape::Oklch) {
            h = hsvHue;
            sat = hsvS;
            val = hsvV;
        } else if (h < 0.0) {
            h = hsvHue;
        }
        return {
            QLatin1Char('#') + current.name().mid(1).toUpper(),
            QStringLiteral("rgb(%1, %2, %3)").arg(current.red()).arg(current.green()).arg(current.blue()),
            QStringLiteral("rgb(%1, %2, %3)").arg(number(current.redF(), 3), number(current.greenF(), 3), number(current.blueF(), 3)),
            QStringLiteral("oklch(%1 %2 %3)").arg(number(lch.l, 3), number(lch.c, 3), number(lch.h, 0)),
            QStringLiteral("hsv(%1, %2%, %3%)").arg(number(h * 360.0, 0), number(sat * 100.0, 0), number(val * 100.0, 0)),
        };
    }

    /// A color typed in the hex field, in any format; the one before becomes the previous
    void pickWritten(const TypedColor &written)
    {
        const QVector<qreal> &parts = written.parts;
        QColor rgb;
        Oklch lch;
        switch (written.format) {
        case ColorFormat::Hex: rgb = written.code; break;
        case ColorFormat::Rgb: rgb = QColor(qRound(parts[0]), qRound(parts[1]), qRound(parts[2])); break;
        case ColorFormat::RgbFloat: rgb = QColor::fromRgbF(parts[0], parts[1], parts[2]); break;
        case ColorFormat::Oklch:
            lch = {parts[0], parts[1], std::fmod(std::fmod(parts[2], 360.0) + 360.0, 360.0)};
            // past what the screen shows: as vivid as it goes at this lightness and hue
            lch.c = qMin(lch.c, maxChroma(lch.l, lch.h));
            rgb = rgbFromOklch(lch);
            break;
        case ColorFormat::Hsv: rgb = QColor::fromHsvF(parts[0] / 360.0, parts[1] / 100.0, parts[2] / 100.0); break;
        }
        if (!rgb.isValid()) return;
        setBase(rgb);
        // the model it was written in keeps its numbers, a gray's hue too
        if (written.format == ColorFormat::Oklch) {
            color = lch;
            // the chroma asked for is held, as the ring holds it, where the screen can't show it all
            heldChroma = parts[1];
        } else if (written.format == ColorFormat::Hsv) {
            hsvHue = parts[0] / 360.0;
            hsvS = parts[1] / 100.0;
            hsvV = parts[2] / 100.0;
        }
        refresh();
    }

    /// Shows a color set elsewhere; the one before becomes the previous color. Taken by value, as it may be \c previous itself.
    void showColor(KoColor color)
    {
        // a color past white is shown as the color it is at white, and how many stops past
        QColor rgb;
        const qreal stops = intensityOf(color, rgb);
        if (rgb != current || !qFuzzyCompare(stops + 1.0, intensity + 1.0)) {
            if (current.isValid()) previous = currentColor;
            current = rgb;
            intensity = stops;
            intensityChip->setStops(stops);
            readCoordinates(rgb);
            refresh();
        }
        currentColor = color;
    }

    /// The image's colors are floating point: they can be past white
    bool holdsBrighterThanWhite() const
    {
        auto *kisCanvas = dynamic_cast<KisCanvas2 *>(canvas.data());
        KisImageSP image = kisCanvas ? KisImageSP(kisCanvas->image()) : KisImageSP();
        if (!image) return false;
        const QString depth = image->colorSpace()->colorDepthId().id();
        return depth == Float16BitsColorDepthID.id() || depth == Float32BitsColorDepthID.id() || depth == Float64BitsColorDepthID.id();
    }

    bool showsIntensity() const
    {
        return holdsBrighterThanWhite() && KisSettingsRegistry::instance()->value(QStringLiteral("StudioColorIntensity")).toBool();
    }

    void updateIntensityShown()
    {
        intensityChip->setVisible(showsIntensity());
    }

    /// \p base made the intensity's stops brighter, where the image can hold it
    KoColor brightened(const KoColor &base) const
    {
        if (intensity <= 0.0 || !showsIntensity()) return base;
        KoColor light = base.convertedTo(linearSpace());
        float *pixel = reinterpret_cast<float *>(light.data());
        const float factor = float(std::exp2(intensity));
        for (int i = 0; i < 3; i++) pixel[i] *= factor;
        return light;
    }

    /// How many stops past white \p color is (0 if it isn't), and in \p base the color it is at white
    static qreal intensityOf(const KoColor &color, QColor &base)
    {
        base = color.toQColor();
        if (!color.colorSpace() || !color.colorSpace()->colorDepthId().id().startsWith(QLatin1Char('F'))) return 0.0;
        KoColor light = color.convertedTo(linearSpace());
        float *pixel = reinterpret_cast<float *>(light.data());
        const float brightest = qMax(pixel[0], qMax(pixel[1], pixel[2]));
        if (brightest <= 1.0001f) return 0.0;
        for (int i = 0; i < 3; i++) pixel[i] /= brightest;
        base = light.toQColor();
        return std::round(std::log2(brightest) * 10.0) / 10.0;
    }

    /// A new color at white, keeping the intensity; the one before becomes the previous
    void setBase(const QColor &rgb)
    {
        if (current.isValid()) previous = currentColor;
        current = QColor::fromRgb(rgb.rgb());
        readCoordinates(rgb);
        refresh();
        applyColor(brightened(koColorFromRgb(rgb)));
    }

    /// Shows the color and sends it to the canvas
    void setColor(const KoColor &color)
    {
        showColor(color);
        applyColor(currentColor);
    }

    /// Applies a color from a picker; the previous color was saved when the drag started
    void pickColor(const QColor &rgb)
    {
        // keep the picked coordinates instead of deriving them from the color, so that the marker
        // follows the pointer even where the color doesn't change (e.g. along black)
        current = QColor::fromRgb(rgb.rgb());
        refresh();
        applyColor(brightened(koColorFromRgb(rgb)));
    }

    void remember()
    {
        if (current.isValid()) previous = currentColor;
    }

    void setStyle(HueLayout pickedLayout, PickerShape pickedShape)
    {
        layout = pickedLayout;
        shape = pickedShape;
        // each choice pictured as it would look with the other
        styleButton->setIcon(pickerIcon(layout, shape));
        for (auto it = layoutActions.cbegin(); it != layoutActions.cend(); ++it) {
            it.value()->setIcon(pickerIcon(it.key(), shape));
            it.value()->setChecked(it.key() == layout);
        }
        for (auto it = shapeActions.cbegin(); it != shapeActions.cend(); ++it) {
            it.value()->setIcon(pickerIcon(layout, it.key()));
            it.value()->setChecked(it.key() == shape);
        }
        // OKLCH keeps lightness by itself
        keepAction->setVisible(shape != PickerShape::Oklch);
        // derive the new model's coordinates from the current color
        if (current.isValid()) readCoordinates(current);
        refresh();
    }

    void updateRecent()
    {
        QVector<QColor> colors;
        if (KisUniqueColorSet *history = document ? document->colorHistoryModel() : nullptr) {
            for (int i = 0; i < history->size() && colors.size() < 12; i++) {
                colors << history->color(i).toQColor();
            }
        }
        recentRow->setColors(colors);
    }
};

namespace {
/// The config values of each HueLayout and PickerShape, in enum order
const QStringList layoutNames = {QStringLiteral("ring"), QStringLiteral("strip")};
const QStringList shapeNames = {QStringLiteral("triangle"), QStringLiteral("square"), QStringLiteral("oklch")};
}

KisStudioColorPanel::KisStudioColorPanel()
    : d(new Private)
{
    setWindowTitle(i18nc("@title:window", "Color"));

    auto *contents = new QWidget(this);
    auto *layout = new QVBoxLayout(contents);
    layout->setContentsMargins(KisModernStyle::space2, KisModernStyle::space2, KisModernStyle::space2, KisModernStyle::space2);
    layout->setSpacing(KisModernStyle::space2);

    d->picker = new ColorPicker(contents);
    layout->addWidget(d->picker, 1);

    auto *row = new QHBoxLayout();
    row->setSpacing(KisModernStyle::space2);
    d->currentRow = new Swatches(22, contents);
    d->currentRow->setFixedWidth(22 * 2 + 4);
    d->currentRow->setToolTip(i18nc("@info:tooltip", "Current and previous color"));
    row->addWidget(d->currentRow);
    d->recentRow = new Swatches(16, contents);
    d->recentRow->setToolTip(i18nc("@info:tooltip", "Colors in this image"));
    row->addWidget(d->recentRow, 1, Qt::AlignVCenter);
    d->styleButton = new QToolButton(contents);
    d->styleButton->setAutoRaise(true);
    d->styleButton->setFocusPolicy(Qt::NoFocus);
    d->styleButton->setIconSize(QSize(20, 20));
    d->styleButton->setToolTip(i18nc("@info:tooltip", "Picker style"));
    auto *styles = new QMenu(d->styleButton);
    // Krita hides menu icons, but these are what tells the pickers apart
    auto addChoice = [styles](QActionGroup *group, const QString &text) {
        QAction *action = styles->addAction(text);
        action->setIconVisibleInMenu(true);
        action->setCheckable(true);
        group->addAction(action);
        return action;
    };
    auto *layouts = new QActionGroup(styles);
    const QList<QPair<HueLayout, QString>> layoutChoices = {
        {HueLayout::Ring, i18nc("@item:inmenu color picker: hues around a ring", "Ring")},
        {HueLayout::Strip, i18nc("@item:inmenu color picker: hues down a strip beside the square", "Strip")},
    };
    for (const auto &choice : layoutChoices) {
        const HueLayout picked = choice.first;
        d->layoutActions.insert(picked, addChoice(layouts, choice.second));
        connect(d->layoutActions.value(picked), &QAction::triggered, this, [this, picked]() {
            KisConfig(false).writeEntry("StudioColorHues", layoutNames[int(picked)]);
            d->setStyle(picked, d->shape);
        });
    }
    styles->addSeparator();
    auto *shapes = new QActionGroup(styles);
    const QList<QPair<PickerShape, QString>> shapeChoices = {
        {PickerShape::Triangle, i18nc("@item:inmenu color picker: HSV triangle", "Triangle")},
        {PickerShape::Square, i18nc("@item:inmenu color picker: HSV square", "Square")},
        {PickerShape::Oklch, i18nc("@item:inmenu color picker: OKLCH lightness by chroma", "OKLCH")},
    };
    for (const auto &choice : shapeChoices) {
        const PickerShape picked = choice.first;
        d->shapeActions.insert(picked, addChoice(shapes, choice.second));
        connect(d->shapeActions.value(picked), &QAction::triggered, this, [this, picked]() {
            KisConfig(false).writeEntry("StudioColorShape", shapeNames[int(picked)]);
            d->setStyle(d->layout, picked);
        });
    }
    styles->addSeparator();
    d->keepAction = styles->addAction(i18nc("@item:inmenu color picker", "Keep lightness when the hue turns"));
    d->keepAction->setCheckable(true);
    connect(d->keepAction, &QAction::toggled, this, [this](bool on) {
        d->keepLightness = on;
        KisConfig(false).writeEntry("StudioColorKeepLightness", on);
    });
    // opened by a click, without a menu button's arrow (a click while it is open closes it: KisPopupCloser::installToggle)
    connect(d->styleButton, &QToolButton::clicked, this, [this, styles]() {
        styles->popup(d->styleButton->mapToGlobal(QPoint(0, d->styleButton->height())));
    });
    row->addWidget(d->styleButton);
    d->intensityChip = new IntensityChip(contents);
    d->intensityChip->hide();
    d->intensityChip->started = [this]() { d->remember(); };
    d->intensityChip->changed = [this](qreal stops) {
        d->intensity = stops;
        d->applyColor(d->brightened(koColorFromRgb(d->current)));
    };
    row->addWidget(d->intensityChip);
    connect(KisSettingsRegistry::instance(), &KisSettingsRegistry::valueChanged, this, [this](const QString &id) {
        if (id == QLatin1String("StudioColorIntensity")) d->updateIntensityShown();
    });
    d->hex = new HexField(contents);
    d->hex->setFrame(false);
    d->hex->setFixedWidth(64);
    d->hex->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    d->hex->setToolTip(i18nc("@info:tooltip", "Type a hex code, or rgb(…), oklch(…), hsv(…)"));
    d->hex->written = [this]() { return d->written(); };
    row->addWidget(d->hex);
    layout->addLayout(row);

    setWidget(contents);

    d->picker->started = [this]() { d->remember(); };

    // a picked chroma is held while lightness or hue change (see heldChroma)
    auto pickChroma = [this](qreal chroma) {
        d->color.c = qMin(chroma, maxChroma(d->color.l, d->color.h));
        d->heldChroma = d->color.c;
        d->pickColor(rgbFromOklch(d->color));
    };
    auto pickHue = [this](qreal hue) {
        d->color.h = hue;
        d->color.c = qMin(d->heldChroma, maxChroma(d->color.l, d->color.h));
        d->pickColor(rgbFromOklch(d->color));
    };
    d->picker->huePicked = [this, pickHue](qreal hue) {
        if (d->shape == PickerShape::Oklch) {
            pickHue(oklchHueOfHsv(360.0 * hue));
            return;
        }
        d->hsvHue = hue;
        if (d->keepLightness) d->holdLightness();
        d->pickColor(QColor::fromHsvF(d->hsvHue, d->hsvS, d->hsvV));
    };
    d->picker->planePicked = [this, pickChroma](qreal x, qreal y) {
        d->color.l = 1.0 - y;
        // beyond the gamut edge means "as saturated as possible here"
        pickChroma(maxPlaneChroma * x);
    };
    d->picker->svPicked = [this](qreal s, qreal v) {
        d->hsvS = s;
        d->hsvV = v;
        const QColor rgb = QColor::fromHsvF(d->hsvHue, d->hsvS, d->hsvV);
        d->heldSaturation = s;
        d->heldLightness = oklchFromRgb(rgb).l;
        d->pickColor(rgb);
    };
    d->currentRow->picked = [this](int index) {
        // picking the previous color swaps the two, so a second click goes back
        if (index == 1 && d->previous) d->setColor(*d->previous);
    };
    d->recentRow->picked = [this](int index) {
        KisUniqueColorSet *history = d->document ? d->document->colorHistoryModel() : nullptr;
        if (history && index < history->size()) {
            d->setColor(history->color(index));
        }
    };
    connect(d->hex, &QLineEdit::editingFinished, this, [this]() {
        // focus leaving the field is not an edit
        if (!d->hex->isModified()) return;
        d->hex->setModified(false);
        const std::optional<TypedColor> typed = readColor(d->hex->text());
        if (typed) d->pickWritten(*typed);
        else d->hex->setText(d->current.name().mid(1).toUpper());
    });

    // the last chosen picker; before its two choices, one of three pickers (OKLCH, sliders, HSV), or an OKLCH/HSV switch
    KisConfig config(true);
    const QString old = config.readEntry<QString>("StudioColorStyle", config.readEntry<bool>("StudioColorHsv", false) ? QStringLiteral("hsv") : QString());
    const int savedLayout = layoutNames.indexOf(config.readEntry<QString>("StudioColorHues", old == QLatin1String("pill") ? QStringLiteral("strip") : QString()));
    const int savedShape = shapeNames.indexOf(config.readEntry<QString>("StudioColorShape", old == QLatin1String("hsv") ? QStringLiteral("triangle") : QString()));
    d->keepLightness = config.readEntry<bool>("StudioColorKeepLightness", true);
    {
        QSignalBlocker blocker(d->keepAction);
        d->keepAction->setChecked(d->keepLightness);
    }
    d->showColor(KoColor(Qt::black, KoColorSpaceRegistry::instance()->rgb8()));
    d->setStyle(savedLayout >= 0 ? HueLayout(savedLayout) : HueLayout::Ring, savedShape >= 0 ? PickerShape(savedShape) : PickerShape::Oklch);
}

KisStudioColorPanel::~KisStudioColorPanel() = default;

QString KisStudioColorPanel::id()
{
    return QStringLiteral("StudioColor");
}

void KisStudioColorPanel::registerFactory()
{
    if (!KoDockRegistry::instance()->contains(id())) {
        KoDockRegistry::instance()->add(new KisStudioColorPanelFactory);
    }
}

void KisStudioColorPanel::setCanvas(KoCanvasBase *canvas)
{
    unsetCanvas();
    d->canvas = canvas;
    setEnabled(canvas);
    if (!canvas || !canvas->resourceManager()) {
        return;
    }

    d->canvasConnections << connect(canvas->resourceManager(), &KoCanvasResourceProvider::canvasResourceChanged, this,
                                    [this](int key, const QVariant &value) {
                                        if (key == KoCanvasResource::ForegroundColor && !d->applying) {
                                            d->showColor(value.value<KoColor>());
                                        }
                                    });
    d->showColor(canvas->resourceManager()->foregroundColor());

    auto *kisCanvas = dynamic_cast<KisCanvas2 *>(canvas);
    d->updateIntensityShown();
    if (kisCanvas && kisCanvas->image()) {
        d->canvasConnections << connect(kisCanvas->image().data(), &KisImage::sigColorSpaceChanged, this, [this]() { d->updateIntensityShown(); });
    }
    // the colors painted in this image, newest first: Krita keeps them with the image
    d->document = kisCanvas && kisCanvas->imageView() ? kisCanvas->imageView()->document() : nullptr;
    KisUniqueColorSet *history = d->document ? d->document->colorHistoryModel() : nullptr;
    if (history && kisCanvas->viewManager()) {
        auto changed = [this]() { d->updateRecent(); };
        d->canvasConnections << connect(history, &KisUniqueColorSet::sigReset, this, changed);
        d->canvasConnections << connect(history, &KisUniqueColorSet::sigColorAdded, this, changed);
        d->canvasConnections << connect(history, &KisUniqueColorSet::sigColorMoved, this, changed);
        d->canvasConnections << connect(history, &KisUniqueColorSet::sigColorRemoved, this, changed);
        KisCanvasResourceProvider *provider = kisCanvas->viewManager()->canvasResourceProvider();
        d->canvasConnections << connect(provider, &KisCanvasResourceProvider::sigFGColorPainted, this, [this, provider](const KoColor &color) {
            // erasing doesn't use the color
            if (!d->document || provider->eraserMode() || provider->currentCompositeOp() == COMPOSITE_ERASE) return;
            d->document->colorHistoryModel()->addColor(color);
        });
    }
    d->updateRecent();
}

void KisStudioColorPanel::unsetCanvas()
{
    Q_FOREACH (const QMetaObject::Connection &connection, d->canvasConnections) {
        disconnect(connection);
    }
    d->canvasConnections.clear();
    d->canvas = nullptr;
    d->document = nullptr;
    d->updateIntensityShown();
    setEnabled(false);
}

QString KisStudioColorPanelFactory::id() const
{
    return KisStudioColorPanel::id();
}

QDockWidget *KisStudioColorPanelFactory::createDockWidget()
{
    auto *panel = new KisStudioColorPanel();
    panel->setObjectName(id());
    return panel;
}
