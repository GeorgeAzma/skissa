/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioColorPanel.h"

#include <QHBoxLayout>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QRegularExpression>
#include <QVBoxLayout>

#include <KoCanvasBase.h>
#include <KoCanvasResourceProvider.h>
#include <KoCanvasResourcesIds.h>
#include <KoColor.h>
#include <KoColorSpaceRegistry.h>
#include <KoDockRegistry.h>
#include <klocalizedstring.h>

#include <cmath>

#include "KisViewManager.h"
#include "canvas/kis_canvas2.h"
#include "kis_favorite_resource_manager.h"
#include "kis_paintop_box.h"

namespace {

/// The widest chroma the plane shows; sRGB reaches about 0.32 (magenta)
constexpr qreal maxPlaneChroma = 0.32;

struct Oklch {
    qreal l {0.0};
    qreal c {0.0};
    qreal h {0.0}; ///< degrees
};

qreal toLinear(qreal c)
{
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

qreal toGamma(qreal c)
{
    return c <= 0.0031308 ? 12.92 * c : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
}

Oklch oklchFromRgb(const QColor &color)
{
    const qreal r = toLinear(color.redF());
    const qreal g = toLinear(color.greenF());
    const qreal b = toLinear(color.blueF());
    const qreal l = std::cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b);
    const qreal m = std::cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b);
    const qreal s = std::cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b);
    const qreal L = 0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s;
    const qreal A = 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s;
    const qreal B = 0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s;
    qreal h = std::atan2(B, A) * 180.0 / M_PI;
    if (h < 0) h += 360.0;
    return {L, std::sqrt(A * A + B * B), h};
}

/// Linear sRGB, unclamped, so that the caller can tell whether it is in gamut
void linearRgbFromOklch(const Oklch &c, qreal &r, qreal &g, qreal &b)
{
    const qreal hr = c.h * M_PI / 180.0;
    const qreal A = c.c * std::cos(hr);
    const qreal B = c.c * std::sin(hr);
    const qreal l = std::pow(c.l + 0.3963377774 * A + 0.2158037573 * B, 3);
    const qreal m = std::pow(c.l - 0.1055613458 * A - 0.0638541728 * B, 3);
    const qreal s = std::pow(c.l - 0.0894841775 * A - 1.2914855480 * B, 3);
    r = 4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s;
    g = -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s;
    b = -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s;
}

bool inGamut(const Oklch &c)
{
    qreal r, g, b;
    linearRgbFromOklch(c, r, g, b);
    const qreal e = 1e-4;
    return r >= -e && r <= 1 + e && g >= -e && g <= 1 + e && b >= -e && b <= 1 + e;
}

QColor rgbFromOklch(const Oklch &c)
{
    qreal r, g, b;
    linearRgbFromOklch(c, r, g, b);
    return QColor::fromRgbF(qBound(0.0, toGamma(r), 1.0), qBound(0.0, toGamma(g), 1.0), qBound(0.0, toGamma(b), 1.0));
}

/// The most saturated in-gamut chroma at this lightness and hue
qreal maxChroma(qreal l, qreal h)
{
    qreal low = 0.0;
    qreal high = 0.4;
    for (int i = 0; i < 18; i++) {
        const qreal mid = (low + high) / 2;
        (inGamut({l, mid, h}) ? low : high) = mid;
    }
    return low;
}

/// A ring that reads on any color: dark outside, light inside
void drawMarker(QPainter &painter, const QPointF &center, qreal radius)
{
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QColor(0, 0, 0, 160), 3));
    painter.drawEllipse(center, radius, radius);
    painter.setPen(QPen(Qt::white, 1.5));
    painter.drawEllipse(center, radius, radius);
}

/// Lightness down, chroma across, at one hue
class Plane : public QWidget
{
public:
    std::function<void(qreal l, qreal c)> picked;

    explicit Plane(QWidget *parent) : QWidget(parent)
    {
        setMinimumSize(160, 120);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setCursor(Qt::CrossCursor);
    }

    void setColor(const Oklch &color)
    {
        if (!qFuzzyCompare(color.h + 1.0, m_color.h + 1.0)) {
            m_image = QImage();
        }
        m_color = color;
        update();
    }

    QSize sizeHint() const override { return QSize(240, 170); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        const qreal dpr = devicePixelRatioF();
        const QSize pixels = size() * dpr;
        if (m_image.size() != pixels) {
            m_image = QImage(pixels, QImage::Format_ARGB32_Premultiplied);
            m_image.fill(Qt::transparent);
            for (int y = 0; y < pixels.height(); y++) {
                const qreal l = 1.0 - qreal(y) / qMax(1, pixels.height() - 1);
                QRgb *line = reinterpret_cast<QRgb *>(m_image.scanLine(y));
                for (int x = 0; x < pixels.width(); x++) {
                    const Oklch c{l, maxPlaneChroma * x / qMax(1, pixels.width() - 1), m_color.h};
                    if (inGamut(c)) {
                        line[x] = rgbFromOklch(c).rgba();
                    }
                }
            }
            m_image.setDevicePixelRatio(dpr);
        }

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        QPainterPath clip;
        clip.addRoundedRect(QRectF(rect()), 8, 8);
        painter.setClipPath(clip);
        painter.drawImage(0, 0, m_image);
        painter.setClipping(false);

        const QPointF marker(m_color.c / maxPlaneChroma * (width() - 1), (1.0 - m_color.l) * (height() - 1));
        drawMarker(painter, marker, 6);
    }

    void mousePressEvent(QMouseEvent *event) override { pick(event->pos()); }
    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (event->buttons() & Qt::LeftButton) pick(event->pos());
    }

private:
    void pick(const QPoint &pos)
    {
        const qreal l = qBound(0.0, 1.0 - qreal(pos.y()) / qMax(1, height() - 1), 1.0);
        // past the edge of the gamut means "as saturated as it gets here"
        const qreal c = qMin(qBound(0.0, maxPlaneChroma * pos.x() / qMax(1, width() - 1), maxPlaneChroma), maxChroma(l, m_color.h));
        if (picked) picked(l, c);
    }

    Oklch m_color;
    QImage m_image;
};

/// Hues at even perceived steps, all at one lightness and chroma
class HueStrip : public QWidget
{
public:
    std::function<void(qreal h)> picked;

    explicit HueStrip(QWidget *parent) : QWidget(parent)
    {
        setFixedHeight(18);
        setCursor(Qt::PointingHandCursor);
    }

    void setHue(qreal hue)
    {
        m_hue = hue;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        const qreal dpr = devicePixelRatioF();
        const int pixels = qRound(width() * dpr);
        if (m_image.width() != pixels) {
            m_image = QImage(pixels, 1, QImage::Format_ARGB32);
            for (int x = 0; x < pixels; x++) {
                const qreal h = 360.0 * x / qMax(1, pixels - 1);
                m_image.setPixel(x, 0, rgbFromOklch({0.72, qMin(0.14, maxChroma(0.72, h)), h}).rgba());
            }
        }
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        QPainterPath clip;
        const QRectF strip = QRectF(rect()).adjusted(0, 3, 0, -3);
        clip.addRoundedRect(strip, strip.height() / 2, strip.height() / 2);
        painter.setClipPath(clip);
        painter.drawImage(strip, m_image);
        painter.setClipping(false);
        drawMarker(painter, QPointF(m_hue / 360.0 * (width() - 1), height() / 2.0), 6);
    }

    void mousePressEvent(QMouseEvent *event) override { pick(event->pos().x()); }
    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (event->buttons() & Qt::LeftButton) pick(event->pos().x());
    }

private:
    void pick(int x)
    {
        if (picked) picked(qBound(0.0, 360.0 * x / qMax(1, width() - 1), 360.0));
    }

    qreal m_hue {0.0};
    QImage m_image;
};

/// A row of color chips; a click picks one
class Swatches : public QWidget
{
public:
    std::function<void(int index)> picked;

    explicit Swatches(int chipSize, QWidget *parent) : QWidget(parent), m_chip(chipSize)
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
        for (int i = 0; i < m_colors.size(); i++) {
            const QRectF chip = chipRect(i);
            if (chip.right() > width()) break;
            painter.setPen(QPen(border, 1));
            painter.setBrush(m_colors[i]);
            painter.drawRoundedRect(chip.adjusted(0.5, 0.5, -0.5, -0.5), 4, 4);
        }
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        for (int i = 0; i < m_colors.size(); i++) {
            if (chipRect(i).contains(event->pos()) && picked) {
                picked(i);
                return;
            }
        }
    }

private:
    QRectF chipRect(int i) const { return QRectF(i * (m_chip + 4), 0, m_chip, m_chip); }

    int m_chip;
    QVector<QColor> m_colors;
};

} // namespace

struct KisStudioColorPanel::Private
{
    QPointer<KoCanvasBase> canvas;
    QPointer<KisFavoriteResourceManager> favorites;
    QList<QMetaObject::Connection> canvasConnections;

    Plane *plane {nullptr};
    HueStrip *hueStrip {nullptr};
    Swatches *currentRow {nullptr};
    Swatches *recentRow {nullptr};
    QLineEdit *hex {nullptr};

    Oklch color;
    QColor current;
    QColor previous;

    void applyColor(const QColor &rgb)
    {
        if (!canvas || !canvas->resourceManager()) return;
        canvas->resourceManager()->setForegroundColor(KoColor(rgb, KoColorSpaceRegistry::instance()->rgb8()));
    }

    void showColor(const QColor &rgb, bool fromOutside)
    {
        if (rgb == current) return;
        if (current.isValid()) previous = current;
        current = rgb;
        if (fromOutside) {
            const Oklch picked = oklchFromRgb(rgb);
            color.l = picked.l;
            color.c = picked.c;
            // grays have no hue, keep the one in use
            if (picked.c > 0.012) color.h = picked.h;
        }
        plane->setColor(color);
        hueStrip->setHue(color.h);
        currentRow->setColors({current, previous.isValid() ? previous : current});
        hex->setText(current.name().mid(1).toUpper());
    }

    void updateRecent()
    {
        QVector<QColor> colors;
        if (favorites) {
            for (int i = 0; i < favorites->recentColorsTotal() && colors.size() < 12; i++) {
                colors << favorites->recentColorAt(i).toQColor();
            }
        }
        recentRow->setColors(colors);
    }
};

KisStudioColorPanel::KisStudioColorPanel()
    : d(new Private)
{
    setWindowTitle(i18nc("@title:window", "Color"));

    auto *contents = new QWidget(this);
    auto *layout = new QVBoxLayout(contents);
    layout->setContentsMargins(8, 6, 8, 8);
    layout->setSpacing(8);

    d->plane = new Plane(contents);
    d->plane->setToolTip(i18nc("@info:tooltip", "Lightness (up) and saturation (right), in even perceived steps"));
    layout->addWidget(d->plane, 1);

    d->hueStrip = new HueStrip(contents);
    d->hueStrip->setToolTip(i18nc("@info:tooltip", "Hue"));
    layout->addWidget(d->hueStrip);

    auto *row = new QHBoxLayout();
    row->setSpacing(8);
    d->currentRow = new Swatches(28, contents);
    d->currentRow->setFixedWidth(28 * 2 + 4);
    d->currentRow->setToolTip(i18nc("@info:tooltip", "Current color, and the one before it"));
    row->addWidget(d->currentRow);
    row->addStretch(1);
    d->hex = new QLineEdit(contents);
    d->hex->setFrame(false);
    d->hex->setMaximumWidth(84);
    d->hex->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    d->hex->setToolTip(i18nc("@info:tooltip", "Hex code"));
    row->addWidget(d->hex);
    layout->addLayout(row);

    d->recentRow = new Swatches(18, contents);
    d->recentRow->setToolTip(i18nc("@info:tooltip", "Painted with recently"));
    layout->addWidget(d->recentRow);

    setWidget(contents);

    d->plane->picked = [this](qreal l, qreal c) {
        d->color.l = l;
        d->color.c = c;
        const QColor rgb = rgbFromOklch(d->color);
        d->showColor(rgb, false);
        d->applyColor(rgb);
    };
    d->hueStrip->picked = [this](qreal h) {
        d->color.h = h;
        // keep the lightness, and the chroma where the new hue allows it
        d->color.c = qMin(d->color.c, maxChroma(d->color.l, h));
        const QColor rgb = rgbFromOklch(d->color);
        d->showColor(rgb, false);
        d->applyColor(rgb);
    };
    d->currentRow->picked = [this](int index) {
        if (index == 1 && d->previous.isValid()) {
            const QColor previous = d->previous;
            d->showColor(previous, true);
            d->applyColor(previous);
        }
    };
    d->recentRow->picked = [this](int index) {
        if (d->favorites && index < d->favorites->recentColorsTotal()) {
            const QColor rgb = d->favorites->recentColorAt(index).toQColor();
            d->showColor(rgb, true);
            d->applyColor(rgb);
        }
    };
    connect(d->hex, &QLineEdit::editingFinished, this, [this]() {
        static const QRegularExpression hexCode(QStringLiteral("^#?([0-9a-fA-F]{6}|[0-9a-fA-F]{3})$"));
        const QString text = d->hex->text().trimmed();
        if (!hexCode.match(text).hasMatch()) {
            d->hex->setText(d->current.name().mid(1).toUpper());
            return;
        }
        const QColor rgb(text.startsWith(QLatin1Char('#')) ? text : QLatin1Char('#') + text);
        d->showColor(rgb, true);
        d->applyColor(rgb);
    });

    d->showColor(Qt::black, true);
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
                                        if (key == KoCanvasResource::ForegroundColor) {
                                            d->showColor(value.value<KoColor>().toQColor(), true);
                                        }
                                    });
    d->showColor(canvas->resourceManager()->foregroundColor().toQColor(), true);

    auto *kisCanvas = dynamic_cast<KisCanvas2 *>(canvas);
    if (kisCanvas && kisCanvas->viewManager() && kisCanvas->viewManager()->paintOpBox()) {
        d->favorites = kisCanvas->viewManager()->paintOpBox()->favoriteResourcesManager();
        if (d->favorites) {
            d->canvasConnections << connect(d->favorites, &KisFavoriteResourceManager::setSelectedColor, this, [this]() { d->updateRecent(); });
            d->canvasConnections << connect(d->favorites, &KisFavoriteResourceManager::updatePalettes, this, [this]() { d->updateRecent(); });
        }
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
    d->favorites = nullptr;
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
