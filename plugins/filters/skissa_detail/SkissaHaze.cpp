/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "SkissaHaze.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include <QtMath>

#include <kis_properties_configuration.h>

#include "KisColorMath.h"
#include "KoCompositeOpInLinear.h"

namespace SkissaHaze
{

namespace
{

/**
 * How much more each of red, green and blue is scattered than green: Rayleigh's 1/λ⁴ at 612, 549 and 465 nm, half of
 * the scattering, the rest the droplets' gray
 */
constexpr float scattered[3] = {0.5f + 0.5f * 0.647f, 1.0f, 0.5f + 0.5f * 1.943f};

QColor colorFrom(const KisPropertiesConfiguration &config, const char *key, const QColor &fallback)
{
    const QColor color(config.getString(key, fallback.name()));
    return color.isValid() ? color : fallback;
}

} // namespace

Settings Settings::from(const KisPropertiesConfiguration &config)
{
    Settings s;
    s.amount = float(std::clamp(config.getDouble("amount", s.amount), 0.0, 1.0));
    s.air = colorFrom(config, "air", s.air);
    s.gradient = config.getBool("gradient", s.gradient);
    s.angle = float(std::clamp(config.getDouble("angle", s.angle), -180.0, 180.0));
    return s;
}

void Settings::writeTo(KisPropertiesConfiguration &config) const
{
    config.setProperty("amount", double(amount));
    config.setProperty("air", air.name());
    config.setProperty("gradient", gradient);
    config.setProperty("angle", double(angle));
}

bool Settings::operator==(const Settings &other) const
{
    return amount == other.amount && air == other.air && gradient == other.gradient && angle == other.angle;
}

bool Settings::changesNothing() const
{
    return amount <= 0;
}

void apply(const Settings &settings, const QRect &bounds, const QRect &rect, float *pixels)
{
    if (settings.changesNothing()) return;
    const float air[3] = {float(KisColorMath::toLinear(settings.air.redF())), float(KisColorMath::toLinear(settings.air.greenF())),
                          float(KisColorMath::toLinear(settings.air.blueF()))};
    // the air's thickness, in green, where it is thickest
    const float depth = -std::log(std::max(1e-4f, 1 - settings.amount));
    // a gradient: how far along it each pixel is, from the frame's nearest corner to its farthest
    const QRectF frame = bounds.isEmpty() ? QRectF(rect) : QRectF(bounds);
    const double a = qDegreesToRadians(double(settings.angle));
    const float dx = float(std::cos(a)), dy = float(std::sin(a));
    float nearest = 1e30f, farthest = -1e30f;
    for (const QPointF &corner : {frame.topLeft(), frame.topRight(), frame.bottomLeft(), frame.bottomRight()}) {
        const float along = float(corner.x()) * dx + float(corner.y()) * dy;
        nearest = std::min(nearest, along);
        farthest = std::max(farthest, along);
    }
    const float span = std::max(1.0f, farthest - nearest);
    float through[3];
    for (int c = 0; c < 3; c++) through[c] = std::exp(-depth * scattered[c]);
    for (int y = 0; y < rect.height(); y++) {
        for (int x = 0; x < rect.width(); x++) {
            float *pixel = pixels + (size_t(y) * rect.width() + x) * 4;
            if (settings.gradient) {
                const float share = std::clamp(((rect.left() + x + 0.5f) * dx + (rect.top() + y + 0.5f) * dy - nearest) / span, 0.0f, 1.0f);
                for (int c = 0; c < 3; c++) {
                    const float kept = std::exp(-depth * share * scattered[c]);
                    pixel[c] = pixel[c] * kept + air[c] * (1 - kept);
                }
            } else {
                for (int c = 0; c < 3; c++) pixel[c] = pixel[c] * through[c] + air[c] * (1 - through[c]);
            }
        }
    }
}

QImage applied(const QImage &image, const Settings &settings)
{
    QImage result = image.convertToFormat(QImage::Format_ARGB32);
    const QRect rect = result.rect();
    const float *decode = KoMixing::srgbToLinearTable();
    const quint8 *encode = KoMixing::linearToSrgbTable();
    std::vector<float> pixels(size_t(rect.width()) * rect.height() * 4);
    for (int y = 0; y < rect.height(); y++) {
        const QRgb *line = reinterpret_cast<const QRgb *>(result.constScanLine(y));
        for (int x = 0; x < rect.width(); x++) {
            float *pixel = pixels.data() + (size_t(y) * rect.width() + x) * 4;
            pixel[0] = decode[qRed(line[x])];
            pixel[1] = decode[qGreen(line[x])];
            pixel[2] = decode[qBlue(line[x])];
            pixel[3] = qAlpha(line[x]) / 255.0f;
        }
    }
    apply(settings, rect, rect, pixels.data());
    auto encoded = [encode](float c) { return int(encode[int(std::clamp(c, 0.0f, 1.0f) * 65535 + 0.5f)]); };
    for (int y = 0; y < rect.height(); y++) {
        QRgb *line = reinterpret_cast<QRgb *>(result.scanLine(y));
        for (int x = 0; x < rect.width(); x++) {
            const float *pixel = pixels.data() + (size_t(y) * rect.width() + x) * 4;
            line[x] = qRgba(encoded(pixel[0]), encoded(pixel[1]), encoded(pixel[2]), qAlpha(line[x]));
        }
    }
    return result;
}

} // namespace SkissaHaze
