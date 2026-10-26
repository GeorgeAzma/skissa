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

/// A marker ring visible on any color: dark outside, light inside
void drawMarker(QPainter &painter, const QPointF &center, qreal radius)
{
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QColor(0, 0, 0, 160), 3));
    painter.drawEllipse(center, radius, radius);
    painter.setPen(QPen(Qt::white, 1.5));
    painter.drawEllipse(center, radius, radius);
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

/// Base of the OKLCH and HSV pickers: a hue ring around a picker for that hue's colors
class HueRing : public QWidget
{
public:
    std::function<void()> started;

    explicit HueRing(QWidget *parent) : QWidget(parent)
    {
        setMinimumSize(140, 140);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setCursor(Qt::CrossCursor);
        // a long press here picks a color; it must not open a context menu
        setProperty("ownLongPress", true);
    }

    QSize sizeHint() const override { return QSize(240, 240); }

protected:
    enum Part { None, Ring, Inside };

    virtual qreal innerRadius() const = 0;
    virtual QRgb ringColor(const QPointF &offset) const = 0;
    virtual Part partAt(const QPoint &pos) const = 0;
    virtual void pick(Part part, const QPoint &pos) = 0;

    QPointF center() const { return QRectF(rect()).center(); }
    qreal outerRadius() const { return qMin(width(), height()) / 2.0 - 1; }
    qreal distanceTo(const QPoint &pos) const
    {
        const QPointF offset = QPointF(pos) - center();
        return std::hypot(offset.x(), offset.y());
    }

    void paintRing(QPainter &painter)
    {
        const qreal dpr = devicePixelRatioF();
        const QSize pixels = size() * dpr;
        if (m_ring.size() != pixels) {
            m_ring = QImage(pixels, QImage::Format_ARGB32_Premultiplied);
            m_ring.fill(Qt::transparent);
            const QPointF c(pixels.width() / 2.0, pixels.height() / 2.0);
            const qreal outer = outerRadius() * dpr;
            const qreal inner = innerRadius() * dpr;
            for (int y = 0; y < pixels.height(); y++) {
                QRgb *line = reinterpret_cast<QRgb *>(m_ring.scanLine(y));
                for (int x = 0; x < pixels.width(); x++) {
                    const QPointF offset(x + 0.5 - c.x(), y + 0.5 - c.y());
                    const qreal distance = std::hypot(offset.x(), offset.y());
                    if (distance > inner - 2 && distance < outer + 2) line[x] = ringColor(offset);
                }
            }
            QPainterPath ring;
            ring.addEllipse(c, outer, outer);
            ring.addEllipse(c, inner, inner);
            clipTo(m_ring, ring);
            m_ring.setDevicePixelRatio(dpr);
        }
        painter.drawImage(0, 0, m_ring);
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
    Part m_part {None};
    QImage m_ring;
};

/**
 * The OKLCH picker: hues around a ring (clockwise from the top) and, inside it,
 * a square of the hue's lightness (vertical) and chroma (horizontal). Out of
 * gamut, the square shows the nearest sRGB color under a veil, and the gamut
 * edge is drawn.
 */
class OklchRing : public HueRing
{
public:
    std::function<void(qreal hue)> huePicked;
    /// \p x: chroma as a fraction of maxPlaneChroma; \p y: 0 is white, 1 is black
    std::function<void(qreal x, qreal y)> planePicked;

    using HueRing::HueRing;

    void setContents(const Oklch &color, const QColor &rgb)
    {
        if (color.h != m_color.h) {
            m_plane = QImage();
        }
        m_color = color;
        m_rgb = rgb;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        const qreal dpr = devicePixelRatioF();
        const QRectF plane = square();
        const QSize planePixels = (plane.size() * dpr).toSize();
        if (m_plane.size() != planePixels) {
            m_plane = renderPlane(planePixels, dpr);
        }

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        paintRing(painter);
        painter.save();
        painter.setClipPath(KisModernStyle::squircle(plane, KisModernStyle::tileRadius));
        painter.drawImage(plane.topLeft(), m_plane);
        painter.restore();

        const qreal angle = hsvHueOfOklch(m_color.h) * M_PI / 180.0;
        const QPointF direction(std::sin(angle), -std::cos(angle));
        drawKnob(painter, center() + (outerRadius() - ringWidth() / 2) * direction, ringWidth() / 2 + 2, hueColor(m_color.h));
        drawKnob(painter, QPointF(plane.left() + qMin(1.0, m_color.c / maxPlaneChroma) * plane.width(), plane.top() + (1.0 - m_color.l) * plane.height()),
                 8, m_rgb);
    }

    qreal innerRadius() const override { return outerRadius() - ringWidth(); }

    QRgb ringColor(const QPointF &offset) const override
    {
        return QColor::fromHsvF(ringAngleAt(offset) / 360.0, 1.0, 1.0).rgb();
    }

    Part partAt(const QPoint &pos) const override
    {
        const qreal distance = distanceTo(pos);
        if (distance <= outerRadius() + 2 && distance >= innerRadius() - 2) return Ring;
        if (square().adjusted(-6, -6, 6, 6).contains(pos)) return Inside;
        return None;
    }

    void pick(Part part, const QPoint &pos) override
    {
        if (part == Ring) {
            if (huePicked) huePicked(oklchHueOfHsv(ringAngleAt(QPointF(pos) - center())));
        } else if (planePicked) {
            const QRectF plane = square();
            planePicked(qBound(0.0, (pos.x() - plane.left()) / plane.width(), 1.0), qBound(0.0, (pos.y() - plane.top()) / plane.height(), 1.0));
        }
    }

private:
    qreal ringWidth() const { return qBound(10.0, outerRadius() * 0.11, 18.0); }

    /// The square inside the ring, with a small gap, aligned to whole pixels
    QRectF square() const
    {
        const qreal side = std::floor((innerRadius() - 6) * std::sqrt(2.0));
        const QPointF c = center();
        return QRectF(std::round(c.x() - side / 2), std::round(c.y() - side / 2), side, side);
    }

    QImage renderPlane(const QSize &pixels, qreal dpr) const
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

    Oklch m_color {-1.0, -1.0, -1.0};
    QColor m_rgb;
    QImage m_plane;
};

/**
 * Krita's classic selector, drawn like the Advanced Color Selector's default:
 * a hue ring (red on the left) around an upright triangle with black at the
 * top, white at the bottom left and the full hue at the bottom right. Value
 * increases down the triangle and saturation across each row.
 */
class HsvWheel : public HueRing
{
public:
    std::function<void(qreal hue)> huePicked;
    std::function<void(qreal s, qreal v)> svPicked;

    using HueRing::HueRing;

    /// Hue, saturation and value in 0..1
    void setContents(qreal hue, qreal s, qreal v)
    {
        if (hue != m_hue) {
            m_triangle = QImage();
        }
        m_hue = hue;
        m_s = s;
        m_v = v;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        const qreal dpr = devicePixelRatioF();
        const QSize pixels = size() * dpr;
        if (m_triangle.size() != pixels) {
            m_triangle = renderTriangle(pixels, dpr);
        }

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        paintRing(painter);
        painter.drawImage(0, 0, m_triangle);

        // the hue marker across the ring, as in the Advanced Color Selector: a dark line beside a light one
        const qreal angle = m_hue * 2 * M_PI + M_PI;
        const QPointF direction(std::cos(angle), std::sin(angle));
        const QLineF across(center() + innerRadius() * direction, center() + outerRadius() * direction);
        painter.setPen(QPen(Qt::black, 1.5));
        painter.drawLine(across);
        painter.setPen(QPen(Qt::white, 1.5));
        painter.drawLine(across.translated(1.5 * QPointF(-direction.y(), direction.x())));
        drawMarker(painter, trianglePoint(center(), triangleRadius(), m_s, m_v), 6);
    }

    /// The ring is 15% of the radius, as in the Advanced Color Selector
    qreal innerRadius() const override { return outerRadius() * 0.85; }

    QRgb ringColor(const QPointF &offset) const override
    {
        return QColor::fromHsvF(hueAt(offset), 1.0, 1.0).rgb();
    }

    Part partAt(const QPoint &pos) const override
    {
        const qreal distance = distanceTo(pos);
        // the whole inside of the ring belongs to the triangle, so that its corners are easy to reach
        return distance > outerRadius() + 2 ? None : distance < innerRadius() ? Inside : Ring;
    }

    void pick(Part part, const QPoint &pos) override
    {
        const QPointF c = center();
        if (part == Ring) {
            if (huePicked) huePicked(hueAt(QPointF(pos) - c));
        } else if (svPicked) {
            // outside the triangle, clamp to the nearest row, then to the nearest point on it
            const qreal r = triangleRadius();
            const qreal v = qBound(0.0, (pos.y() - (c.y() - r)) / (1.5 * r), 1.0);
            const qreal rowLength = v * r * std::sqrt(3.0);
            // at the apex every saturation gives black, so keep the current one
            const qreal s = rowLength < 1.0 ? m_s : qBound(0.0, (pos.x() - (c.x() - rowLength / 2)) / rowLength, 1.0);
            svPicked(s, v);
        }
    }

private:
    /// Radius of the circle through the triangle's corners, slightly inside the ring
    qreal triangleRadius() const { return innerRadius() - 4; }

    /// The hue at \p offset: red on the left, then yellow above it, as in the Advanced Color Selector
    static qreal hueAt(const QPointF &offset)
    {
        return std::fmod((std::atan2(offset.y(), offset.x()) + M_PI) / (2 * M_PI), 1.0);
    }

    /// A point in the triangle inscribed in a circle of radius \p r around \p c; value 0 is the apex
    static QPointF trianglePoint(const QPointF &c, qreal r, qreal s, qreal v)
    {
        const qreal rowLength = v * r * std::sqrt(3.0);
        return QPointF(c.x() - rowLength / 2 + s * rowLength, c.y() - r + v * 1.5 * r);
    }

    QImage renderTriangle(const QSize &pixels, qreal dpr) const
    {
        QImage image(pixels, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        const QPointF c(pixels.width() / 2.0, pixels.height() / 2.0);
        const qreal r = triangleRadius() * dpr;
        // at a fixed hue, HSV is v * ((1 - s) * white + s * pure hue) in encoded RGB
        const QColor pure = QColor::fromHsvF(m_hue, 1.0, 1.0);
        const qreal pr = pure.redF(), pg = pure.greenF(), pb = pure.blueF();
        const int top = qMax(0, int(c.y() - r) - 1);
        const int bottom = qMin(pixels.height() - 1, int(c.y() + r / 2) + 1);
        const int left = qMax(0, int(c.x() - r) - 1);
        const int right = qMin(pixels.width() - 1, int(c.x() + r) + 1);
        for (int y = top; y <= bottom; y++) {
            QRgb *line = reinterpret_cast<QRgb *>(image.scanLine(y));
            const qreal v = qBound(0.0, (y + 0.5 - (c.y() - r)) / (1.5 * r), 1.0);
            const qreal rowLength = v * r * std::sqrt(3.0);
            const qreal rowStart = c.x() - rowLength / 2;
            for (int x = left; x <= right; x++) {
                // also fill past the edges, clamped, so that the antialiased border gets the edge color
                const qreal s = rowLength > 0 ? qBound(0.0, (x + 0.5 - rowStart) / rowLength, 1.0) : 0.0;
                line[x] = qRgb(qRound(255 * v * (1 - s + s * pr)), qRound(255 * v * (1 - s + s * pg)), qRound(255 * v * (1 - s + s * pb)));
            }
        }
        QPainterPath triangle;
        triangle.moveTo(trianglePoint(c, r, 0, 0));
        triangle.lineTo(trianglePoint(c, r, 1, 1));
        triangle.lineTo(trianglePoint(c, r, 0, 1));
        triangle.closeSubpath();
        clipTo(image, triangle);
        image.setDevicePixelRatio(dpr);
        return image;
    }

    qreal m_hue {0.0};
    qreal m_s {0.0};
    qreal m_v {0.0};
    QImage m_triangle;
};

/**
 * A slider whose track shows colorAt(t) for t in 0..1, hatched where colorAt
 * returns an invalid color (outside sRGB). The knob shows the current color.
 */
class Track : public QWidget
{
public:
    std::function<QColor(qreal t)> colorAt;
    std::function<void(qreal t)> picked;
    std::function<void()> started;

    Track(int height, QWidget *parent) : QWidget(parent)
    {
        setFixedHeight(height);
        setMinimumWidth(60);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setCursor(Qt::PointingHandCursor);
        setProperty("ownLongPress", true);
    }

    /// The track's colors are regenerated when \p key changes; \p edge (0..1) marks where sRGB ends
    void setContents(const QString &key, qreal marker, const QColor &knob, qreal edge = -1.0)
    {
        if (key != m_key) {
            m_key = key;
            m_image = QImage();
        }
        m_marker = marker;
        m_knob = knob;
        m_edge = edge;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        const qreal dpr = devicePixelRatioF();
        const int pixels = qRound(width() * dpr);
        if (m_image.width() != pixels && colorAt) {
            m_image = QImage(pixels, 1, QImage::Format_ARGB32_Premultiplied);
            m_image.fill(Qt::transparent);
            for (int x = 0; x < pixels; x++) {
                const QColor color = colorAt(positionAt((x + 0.5) / dpr));
                if (color.isValid()) m_image.setPixel(x, 0, color.rgb());
            }
        }
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QRectF track(rect());
        painter.save();
        painter.setClipPath(KisModernStyle::squircle(track, track.height() / 2));
        if (m_edge >= 0.0 && m_edge < 1.0) {
            // hatch the out-of-gamut part
            painter.fillRect(track, QColor(13, 18, 24, 200));
            painter.fillRect(track, QBrush(QColor(255, 255, 255, 46), Qt::BDiagPattern));
        }
        painter.drawImage(track, m_image);
        if (m_edge > 0.0 && m_edge < 1.0) {
            painter.fillRect(QRectF(knobX(m_edge) - 0.75, 0, 1.5, height()), QColor(255, 255, 255, 200));
        }
        painter.restore();
        drawKnob(painter, QPointF(knobX(m_marker), height() / 2.0), height() / 2.0, m_knob);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton) return;
        if (started) started();
        // dragging the knob keeps the grab offset; pressing elsewhere jumps there
        const qreal offset = event->pos().x() - knobX(m_marker);
        m_grab = qAbs(offset) <= height() / 2.0 ? offset : 0.0;
        if (picked) picked(positionAt(event->pos().x() - m_grab));
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if ((event->buttons() & Qt::LeftButton) && picked) picked(positionAt(event->pos().x() - m_grab));
    }

    void wheelEvent(QWheelEvent *event) override
    {
        // each notch moves 1% of the track; a burst of notches counts as one change for the previous color
        if (const int steps = KisModernStyle::wheelSteps(event, this)) {
            if (started && (!m_lastWheel.isValid() || m_lastWheel.elapsed() > 600)) started();
            m_lastWheel.restart();
            if (picked) picked(qBound(0.0, m_marker + steps * 0.01, 1.0));
        }
        event->accept();
    }

private:
    /// Keeps the knob within the track's rounded ends
    qreal knobX(qreal t) const { return height() / 2.0 + t * (width() - height()); }
    qreal positionAt(qreal x) const { return qBound(0.0, (x - height() / 2.0) / qMax(1, width() - height()), 1.0); }

    QString m_key;
    qreal m_marker {0.0};
    qreal m_grab {0.0};
    QElapsedTimer m_lastWheel;
    qreal m_edge {-1.0};
    QColor m_knob;
    QImage m_image;
};

/// A large swatch of the current color, beside the sliders
class ColorChip : public QWidget
{
public:
    using QWidget::QWidget;

    void setColor(const QColor &color)
    {
        m_color = color;
        update();
    }

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        m_pressed = event->button() == Qt::LeftButton;
        m_pressAt = event->pos();
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (!m_pressed || (event->pos() - m_pressAt).manhattanLength() < QApplication::startDragDistance()) return;
        m_pressed = false;
        KisColorDrop::startDrag(this, m_color);
    }

    void mouseReleaseEvent(QMouseEvent *) override { m_pressed = false; }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        QColor border = palette().color(QPalette::WindowText);
        border.setAlphaF(0.22);
        painter.setPen(QPen(border, 1));
        painter.setBrush(m_color);
        painter.drawPath(KisModernStyle::squircle(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), KisModernStyle::panelRadius));
    }

private:
    QColor m_color;
    bool m_pressed {false};
    QPoint m_pressAt;
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

/// The available pickers, in menu order
enum class PickerStyle { Ring, Pill, Hsv };

/**
 * Shown beside the picker menu while an entry is hovered: every hue of HSV and
 * of OKLCH, each with its perceived lightness below it. HSV's swings between
 * near white and near black; OKLCH's stays level.
 */
class PickerTip : public QWidget
{
public:
    /// Created in \p window, above everything in it, other glass overlays included
    explicit PickerTip(QWidget *window)
        : QWidget(window)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        KisStudioGlass::setOverOverlays(this);
        setFixedSize(tipWidth + 2 * margin, contentHeight());
        hide();
    }

    /// Shows the tip for \p style beside the menu at \p beside (screen coordinates), level with \p row
    void showFor(PickerStyle style, const QRect &beside, int row)
    {
        m_oklch = style != PickerStyle::Hsv;
        QPoint at(beside.left() - width() - 6, row - height() / 2);
        if (QScreen *screen = QGuiApplication::screenAt(beside.center())) {
            const QRect area = screen->availableGeometry();
            if (at.x() < area.left()) at.setX(beside.right() + 6);
            at.setY(qBound(area.top(), at.y(), area.bottom() - height()));
        }
        move(parentWidget()->mapFromGlobal(at));
        update();
        show();
        raise();
        KisStudioGlass::changed(this);
    }

protected:
    void hideEvent(QHideEvent *event) override
    {
        QWidget::hideEvent(event);
        KisStudioGlass::changed(this);
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QPainterPath shape = KisStudioGlass::paint(painter, this, QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 9);
        QColor line = palette().color(QPalette::WindowText);
        line.setAlphaF(0.12);
        KisStudioGlass::paintBorder(painter, this, shape, line);

        int y = margin;
        painter.setOpacity(m_oklch ? 0.38 : 1.0);
        y = paintStrip(painter, y, QStringLiteral("HSV"), [](qreal h) { return QColor::fromHsvF(h / 360.0, 1.0, 1.0); });
        y += gap;
        painter.setOpacity(m_oklch ? 1.0 : 0.38);
        paintStrip(painter, y, QStringLiteral("OKLCH"), [](qreal h) { return rgbFromOklch({0.75, qMin(0.13, maxChroma(0.75, h)), h}); });
    }

private:
    QFont labelFont() const { return KisModernStyle::textFont(font(), 0.8, QFont::DemiBold); }
    int labelHeight() const { return QFontMetrics(labelFont()).height(); }

    int contentHeight() const
    {
        const int strip = labelHeight() + 3 + stripHeight + 3 + lightnessHeight;
        return margin + strip + gap + strip + margin;
    }

    /// Paints a label, the hue strip below it, and below that each hue's OKLab lightness as a gray
    int paintStrip(QPainter &painter, int y, const QString &name, const std::function<QColor(qreal)> &colorAt)
    {
        QFont label = labelFont();
        label.setLetterSpacing(QFont::PercentageSpacing, 106);
        painter.setFont(label);
        painter.setPen(KisModernStyle::secondaryInk(palette().color(QPalette::WindowText)));
        painter.drawText(QRect(margin, y, tipWidth, labelHeight()), Qt::AlignLeft | Qt::AlignVCenter, name);
        y += labelHeight() + 3;

        QImage hues(tipWidth, stripHeight, QImage::Format_ARGB32_Premultiplied);
        QImage light(tipWidth, lightnessHeight, QImage::Format_ARGB32_Premultiplied);
        for (int x = 0; x < tipWidth; x++) {
            const QColor color = colorAt(360.0 * x / (tipWidth - 1));
            const QColor gray = rgbFromOklch({oklchFromRgb(color).l, 0.0, 0.0});
            for (int row = 0; row < stripHeight; row++) hues.setPixel(x, row, color.rgb());
            for (int row = 0; row < lightnessHeight; row++) light.setPixel(x, row, gray.rgb());
        }
        auto put = [&](const QImage &image, int top) {
            const QRectF area(margin, top, tipWidth, image.height());
            painter.save();
            painter.setClipPath(KisModernStyle::squircle(area, 3));
            painter.drawImage(area.topLeft(), image);
            painter.restore();
        };
        put(hues, y);
        y += stripHeight + 3;
        put(light, y);
        return y + lightnessHeight;
    }

    static constexpr int tipWidth = 220;
    static constexpr int margin = 10;
    static constexpr int gap = 10;
    static constexpr int stripHeight = 14;
    static constexpr int lightnessHeight = 8;
    bool m_oklch {true};
};

/// A small picture of a picker style, for its menu entry and the menu's button
static QIcon styleIcon(PickerStyle style)
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
    const QRectF bounds(1, 1, size - 2, size - 2);
    const qreal hole = 4.2;
    // hues around a thick ring
    auto ring = [&](const std::function<QColor(qreal hue)> &hueAt) {
        for (int i = 0; i < 72; i++) {
            painter.setBrush(hueAt(i * 5.0));
            painter.drawPie(bounds, int((90 - i * 5) * 16), -6 * 16);
        }
        painter.setCompositionMode(QPainter::CompositionMode_Clear);
        painter.setBrush(Qt::black);
        painter.drawEllipse(bounds.center(), hole, hole);
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(edge);
        painter.drawEllipse(bounds.adjusted(0.4, 0.4, -0.4, -0.4));
        painter.setPen(Qt::NoPen);
    };
    const QColor hue = hueColor(35);

    switch (style) {
    case PickerStyle::Ring: {
        // a square in the hole, as the picker has one in its ring
        ring([](qreal h) { return QColor::fromHsvF(h / 360.0, 1.0, 1.0); });
        const QRectF plane(bounds.center() - QPointF(2.6, 2.6), QSizeF(5.2, 5.2));
        painter.setBrush(hue);
        painter.setPen(edge);
        painter.drawRoundedRect(plane, 1, 1);
        break;
    }
    case PickerStyle::Pill: {
        // three thick bars: lightness, chroma, hue
        auto bar = [&](qreal y, const QGradientStops &stops) {
            QLinearGradient gradient(QPointF(1, 0), QPointF(size - 1, 0));
            gradient.setStops(stops);
            painter.setBrush(gradient);
            painter.setPen(edge);
            painter.drawRoundedRect(QRectF(1.5, y, size - 3, 4.6), 2.3, 2.3);
        };
        bar(1.8, {{0, Qt::black}, {1, Qt::white}});
        bar(7.7, {{0, QColor(150, 150, 150)}, {1, hue}});
        QGradientStops hues;
        for (int i = 0; i <= 6; i++) hues << QGradientStop(i / 6.0, QColor::fromHsvF(std::fmod(i / 6.0, 1.0), 1.0, 1.0));
        bar(13.6, hues);
        break;
    }
    case PickerStyle::Hsv: {
        // a triangle in the hole, as the wheel has one
        ring([](qreal h) { return QColor::fromHsvF(std::fmod(h + 270.0, 360.0) / 360.0, 1.0, 1.0); });
        const QPointF c = bounds.center();
        QPolygonF triangle;
        triangle << c + QPointF(0, -3.4) << c + QPointF(3.0, 2.0) << c + QPointF(-3.0, 2.0);
        painter.setBrush(QColor(230, 40, 40));
        painter.setPen(edge);
        painter.drawPolygon(triangle);
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

    OklchRing *ring {nullptr};
    QWidget *pill {nullptr};
    /// Takes up the room left below the fixed-height sliders
    QWidget *filler {nullptr};
    ColorChip *chip {nullptr};
    Track *lightTrack {nullptr};
    Track *chromaTrack {nullptr};
    Track *hueTrack {nullptr};
    HsvWheel *wheel {nullptr};
    Swatches *currentRow {nullptr};
    Swatches *recentRow {nullptr};
    HexField *hex {nullptr};
    IntensityChip *intensityChip {nullptr};
    /// Stops brighter than white the color is painted, in images that can hold it
    qreal intensity {0.0};
    QToolButton *styleButton {nullptr};
    QMap<PickerStyle, QAction *> styleActions;

    PickerStyle style {PickerStyle::Ring};
    Oklch color;
    /// The chroma last chosen; changing lightness or hue keeps as much of it as fits in sRGB
    qreal heldChroma {0.0};
    qreal hsvHue {0.0}, hsvS {0.0}, hsvV {0.0}; ///< 0..1
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
    }

    void refresh()
    {
        ring->setVisible(style == PickerStyle::Ring);
        pill->setVisible(style == PickerStyle::Pill);
        filler->setVisible(style == PickerStyle::Pill);
        wheel->setVisible(style == PickerStyle::Hsv);
        switch (style) {
        case PickerStyle::Ring:
            ring->setContents(color, current);
            break;
        case PickerStyle::Pill:
            chip->setColor(current);
            lightTrack->setContents(QStringLiteral("%1 %2").arg(heldChroma).arg(color.h), color.l, current);
            chromaTrack->setContents(QStringLiteral("%1 %2").arg(color.l).arg(color.h), qMin(1.0, color.c / maxPlaneChroma), current,
                                     maxChroma(color.l, color.h) / maxPlaneChroma);
            hueTrack->setContents(QStringLiteral("hues"), hsvHueOfOklch(color.h) / 360.0, current);
            break;
        case PickerStyle::Hsv:
            wheel->setContents(hsvHue, hsvS, hsvV);
            break;
        }
        currentRow->setColors({current, previous ? previous->toQColor() : current});
        hex->setText(current.name().mid(1).toUpper());
    }

    /// The color as copied in each format (ColorFormat order), as CSS writes them where it can, each picker's coordinates kept
    QStringList written() const
    {
        const auto number = [](qreal value, int decimals) { return QString::number(value, 'f', decimals); };
        Oklch lch = color;
        if (style == PickerStyle::Hsv) {
            lch = oklchFromRgb(current);
            if (lch.c <= 0.012) lch.h = color.h;
        }
        qreal h, sat, val;
        current.getHsvF(&h, &sat, &val);
        if (style == PickerStyle::Hsv) {
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

    void setStyle(PickerStyle picked)
    {
        style = picked;
        styleButton->setIcon(styleIcon(style));
        if (QAction *action = styleActions.value(style)) action->setChecked(true);
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
/// The config value of each PickerStyle, in enum order
const QStringList styleNames = {QStringLiteral("ring"), QStringLiteral("pill"), QStringLiteral("hsv")};
}

KisStudioColorPanel::KisStudioColorPanel()
    : d(new Private)
{
    setWindowTitle(i18nc("@title:window", "Color"));

    auto *contents = new QWidget(this);
    auto *layout = new QVBoxLayout(contents);
    layout->setContentsMargins(KisModernStyle::space2, KisModernStyle::space2, KisModernStyle::space2, KisModernStyle::space2);
    layout->setSpacing(KisModernStyle::space2);

    d->ring = new OklchRing(contents);
    layout->addWidget(d->ring, 1);

    // the sliders: a swatch beside lightness, chroma and hue tracks
    d->pill = new QWidget(contents);
    d->pill->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    auto *pillLayout = new QHBoxLayout(d->pill);
    pillLayout->setContentsMargins(0, KisModernStyle::hairline, 0, KisModernStyle::hairline);
    pillLayout->setSpacing(KisModernStyle::space3);
    d->chip = new ColorChip(d->pill);
    d->chip->setFixedWidth(48);
    pillLayout->addWidget(d->chip);
    auto *tracks = new QVBoxLayout();
    tracks->setSpacing(KisModernStyle::space2);
    d->lightTrack = new Track(18, d->pill);
    d->lightTrack->setToolTip(i18nc("@info:tooltip", "Lightness"));
    d->chromaTrack = new Track(18, d->pill);
    d->chromaTrack->setToolTip(i18nc("@info:tooltip", "Chroma"));
    d->hueTrack = new Track(18, d->pill);
    d->hueTrack->setToolTip(i18nc("@info:tooltip", "Hue"));
    tracks->addWidget(d->lightTrack);
    tracks->addWidget(d->chromaTrack);
    tracks->addWidget(d->hueTrack);
    pillLayout->addLayout(tracks, 1);
    layout->addWidget(d->pill);

    d->wheel = new HsvWheel(contents);
    layout->addWidget(d->wheel, 1);

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
    auto *styleGroup = new QActionGroup(styles);
    // created on first use, in the panel's window at that time (it has none during construction)
    auto tip = std::make_shared<QPointer<PickerTip>>();
    connect(styles, &QMenu::hovered, this, [this, styles, tip](QAction *action) {
        if (!action || !action->data().isValid()) {
            if (*tip) (*tip)->hide();
            return;
        }
        if (!*tip || (*tip)->parentWidget() != d->styleButton->window()) {
            delete tip->data();
            *tip = new PickerTip(d->styleButton->window());
        }
        const QRect row = styles->actionGeometry(action);
        (*tip)->showFor(PickerStyle(action->data().toInt()), QRect(styles->mapToGlobal(QPoint()), styles->size()),
                        styles->mapToGlobal(row.center()).y());
    });
    connect(styles, &QMenu::aboutToHide, this, [tip]() {
        if (*tip) (*tip)->hide();
    });
    const QList<QPair<PickerStyle, QString>> pickers = {
        {PickerStyle::Ring, i18nc("@item:inmenu color picker: OKLCH hue ring and chroma/lightness square", "OKLCH")},
        {PickerStyle::Pill, i18nc("@item:inmenu color picker: compact OKLCH sliders", "Sliders")},
        {PickerStyle::Hsv, i18nc("@item:inmenu color picker", "HSV")},
    };
    for (const auto &picker : pickers) {
        const PickerStyle style = picker.first;
        QAction *action = styles->addAction(styleIcon(style), picker.second);
        action->setData(int(style));
        // Krita hides menu icons, but these are what tells the pickers apart
        action->setIconVisibleInMenu(true);
        action->setCheckable(true);
        styleGroup->addAction(action);
        d->styleActions.insert(style, action);
        connect(action, &QAction::triggered, this, [this, style]() {
            KisConfig(false).writeEntry("StudioColorStyle", styleNames[int(style)]);
            d->setStyle(style);
            fitHeight();
        });
    }
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
    d->filler = new QWidget(contents);
    d->filler->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    layout->addWidget(d->filler, 1);

    setWidget(contents);

    d->lightTrack->colorAt = [this](qreal t) {
        return rgbFromOklch({t, qMin(d->heldChroma, maxChroma(t, d->color.h)), d->color.h});
    };
    d->chromaTrack->colorAt = [this](qreal t) {
        return rgbIfInGamut({d->color.l, maxPlaneChroma * t, d->color.h});
    };
    d->hueTrack->colorAt = [](qreal t) {
        return QColor::fromHsvF(qBound(0.0, t, 1.0) * 0.9999, 1.0, 1.0);
    };

    auto remember = [this]() { d->remember(); };
    d->ring->started = remember;
    d->lightTrack->started = remember;
    d->chromaTrack->started = remember;
    d->hueTrack->started = remember;
    d->wheel->started = remember;

    // a picked chroma is held while lightness or hue change (see heldChroma)
    auto pickChroma = [this](qreal chroma) {
        d->color.c = qMin(chroma, maxChroma(d->color.l, d->color.h));
        d->heldChroma = d->color.c;
        d->pickColor(rgbFromOklch(d->color));
    };
    auto pickLightness = [this](qreal lightness) {
        d->color.l = lightness;
        d->color.c = qMin(d->heldChroma, maxChroma(d->color.l, d->color.h));
        d->pickColor(rgbFromOklch(d->color));
    };
    auto pickHue = [this](qreal hue) {
        d->color.h = hue;
        d->color.c = qMin(d->heldChroma, maxChroma(d->color.l, d->color.h));
        d->pickColor(rgbFromOklch(d->color));
    };
    d->ring->huePicked = pickHue;
    d->ring->planePicked = [this, pickChroma](qreal x, qreal y) {
        d->color.l = 1.0 - y;
        // beyond the gamut edge means "as saturated as possible here"
        pickChroma(maxPlaneChroma * x);
    };
    d->lightTrack->picked = pickLightness;
    d->chromaTrack->picked = [pickChroma](qreal t) { pickChroma(maxPlaneChroma * t); };
    d->hueTrack->picked = [pickHue](qreal t) { pickHue(oklchHueOfHsv(360.0 * t)); };
    d->wheel->huePicked = [this](qreal hue) {
        d->hsvHue = hue;
        d->pickColor(QColor::fromHsvF(d->hsvHue, d->hsvS, d->hsvV));
    };
    d->wheel->svPicked = [this](qreal s, qreal v) {
        d->hsvS = s;
        d->hsvV = v;
        d->pickColor(QColor::fromHsvF(d->hsvHue, d->hsvS, d->hsvV));
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

    // the last chosen picker, falling back to the legacy OKLCH/HSV switch
    KisConfig config(true);
    const int saved = styleNames.indexOf(config.readEntry<QString>("StudioColorStyle", QString()));
    const PickerStyle style = saved >= 0 ? PickerStyle(saved)
                              : config.readEntry<bool>("StudioColorHsv", false) ? PickerStyle::Hsv
                                                                                : PickerStyle::Ring;
    d->showColor(KoColor(Qt::black, KoColorSpaceRegistry::instance()->rgb8()));
    d->setStyle(style);
    fitHeight();
}

void KisStudioColorPanel::fitHeight()
{
    // the sliders have a fixed height; the panels below get the rest of the room
    setMaximumHeight(d->style == PickerStyle::Pill ? sizeHint().height() : QWIDGETSIZE_MAX);
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
