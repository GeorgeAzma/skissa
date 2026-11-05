/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisResampling.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include <QSet>

#include <compositeops/KoCompositeOpInLinear.h>
#include <KoChannelInfo.h>
#include <KoColorModelStandardIds.h>
#include <KoColorProfile.h>
#include <KoColorSpace.h>
#include <KoColorSpaceRegistry.h>
#include <klocalizedstring.h>

#include "kis_paint_device.h"
#include "kis_painter.h"

namespace {

qreal sinc(qreal x)
{
    x *= M_PI;
    return x != 0 ? std::sin(x) / x : 1.0;
}

template<typename T>
void clampChannel(quint8 *pixel, const quint8 *a, const quint8 *b, int offset)
{
    T value;
    T first;
    T second;
    std::memcpy(&value, pixel + offset, sizeof(T));
    std::memcpy(&first, a + offset, sizeof(T));
    std::memcpy(&second, b + offset, sizeof(T));
    const T low = first < second ? first : second;
    const T high = first < second ? second : first;
    value = value < low ? low : value > high ? high : value;
    std::memcpy(pixel + offset, &value, sizeof(T));
}

/// 8-bit sRGB, the usual image, goes to linear light and back by tables as the brush mixing does: through LCMS,
/// on one thread, it took most of a shrink's time
bool isSrgb8(const KoColorSpace *cs)
{
    return *cs == *KoColorSpaceRegistry::instance()->rgb8();
}

/// \p rect of \p from written into \p to through \p convert(from, to, count), a band of rows at a time
template<typename Convert>
void convertRows(KisPaintDeviceSP from, KisPaintDeviceSP to, const QRect &rect, Convert convert)
{
    if (rect.isEmpty()) return;
    const int band = 64;
    std::vector<quint8> in(size_t(rect.width()) * band * from->pixelSize());
    std::vector<quint8> out(size_t(rect.width()) * band * to->pixelSize());
    for (int y = rect.top(); y <= rect.bottom(); y += band) {
        const QRect rows(rect.left(), y, rect.width(), qMin(band, rect.bottom() + 1 - y));
        from->readBytes(in.data(), rows);
        convert(in.data(), out.data(), rows.width() * rows.height());
        to->writeBytes(out.data(), rows);
    }
}

}

KisSmoothFilterStrategy::KisSmoothFilterStrategy()
    : KisFilterStrategy(KoID("Smooth", i18nc("resampling filter: sharp, without halos", "Smooth")))
{
    supportVal = 3.0;
    intSupportVal = 768;
}

QString KisSmoothFilterStrategy::description()
{
    return i18nc("resampling filter", "Sharp, without halos at edges. For paintings and photos.");
}

qreal KisSmoothFilterStrategy::valueAt(qreal t, qreal weightsPositionScale) const
{
    Q_UNUSED(weightsPositionScale);
    t = std::abs(t);
    return t < 3.0 ? sinc(t) * sinc(t / 3.0) : 0.0;
}

KisPixelFilterStrategy::KisPixelFilterStrategy()
    : KisFilterStrategy(KoID("Pixel", i18nc("resampling filter for pixel art", "Pixel")))
{
    supportVal = 1.0;
    intSupportVal = 256;
}

QString KisPixelFilterStrategy::description()
{
    return i18nc("resampling filter", "Keeps pixels crisp and even. For pixel art.");
}

qreal KisPixelFilterStrategy::valueAt(qreal t, qreal weightsPositionScale) const
{
    t = std::abs(t);
    // the same size: a pixel taken whole, never blurred into its neighbours
    if (qFuzzyCompare(weightsPositionScale, 1.0)) return t <= 0.5 ? 1.0 : 0.0;

    // how much a new pixel covers a source one, measured in the units t comes in: a source
    // pixel is 1 wide and a new one r when enlarging, the other way round when shrinking
    const qreal r = weightsPositionScale > 1.0 ? 1.0 / weightsPositionScale : weightsPositionScale;
    // weights are taken at 1/256 steps, so an edge that falls between steps (thirds) is met there
    const qreal slack = 1.0 / 256;
    const qreal full = 0.5 * (1.0 - r) + slack;
    const qreal none = qMax(full + slack, 0.5 * (1.0 + r) - slack);
    if (t <= full) return 1.0;
    if (t >= none) return 0.0;
    return (none - t) / r;
}

KisResampling::ClampBetween::ClampBetween(const KoColorSpace *cs)
{
    if (!cs) return;
    const QList<KoChannelInfo *> channels = cs->channels();
    bool bytes = true;
    for (const KoChannelInfo *channel : channels) {
        const KoChannelInfo::enumChannelValueType type = channel->channelValueType();
        // half floats and the rest keep Lanczos as it is, from that channel on
        if (type != KoChannelInfo::UINT8 && type != KoChannelInfo::UINT16 && type != KoChannelInfo::FLOAT32) break;
        m_channels.push_back({channel->pos(), int(type)});
        bytes = bytes && type == KoChannelInfo::UINT8;
    }
    // every channel a byte of its own: the pixel clamped byte by byte
    if (bytes && m_channels.size() == size_t(channels.size()) && channels.size() == int(cs->pixelSize())) {
        m_bytes = int(cs->pixelSize());
    }
}

void KisResampling::ClampBetween::clampChannels(quint8 *pixel, const quint8 *a, const quint8 *b) const
{
    for (const Channel &channel : m_channels) {
        switch (channel.type) {
        case KoChannelInfo::UINT8:
            clampChannel<quint8>(pixel, a, b, channel.offset);
            break;
        case KoChannelInfo::UINT16:
            clampChannel<quint16>(pixel, a, b, channel.offset);
            break;
        default:
            clampChannel<float>(pixel, a, b, channel.offset);
            break;
        }
    }
}

bool KisResampling::looksLikePixelArt(KisPaintDeviceSP device, const QRect &rect)
{
    if (!device || rect.isEmpty()) return false;

    // the middle of the picture is enough to tell: painting and photos have thousands of colours there
    const int side = 512;
    const QRect window = QRect(rect.center() - QPoint(side / 2, side / 2), QSize(side, side)) & rect;
    const int pixelSize = device->pixelSize();
    std::vector<quint8> bytes(size_t(window.width()) * window.height() * pixelSize);
    device->readBytes(bytes.data(), window);

    const KoColorSpace *cs = device->colorSpace();
    std::vector<quint8> alpha(size_t(window.width()) * window.height());
    cs->copyOpacityU8(bytes.data(), alpha.data(), int(alpha.size()));

    const int mostColors = 64;
    // a colour as one number when it fits in one, rather than a byte array allocated for every pixel
    QSet<quint64> colors;
    QSet<QByteArray> wideColors;
    for (size_t i = 0; i < alpha.size(); i++) {
        if (!alpha[i]) continue;
        const quint8 *pixel = bytes.data() + i * pixelSize;
        if (pixelSize <= int(sizeof(quint64))) {
            quint64 key = 0;
            std::memcpy(&key, pixel, pixelSize);
            colors.insert(key);
        } else {
            wideColors.insert(QByteArray(reinterpret_cast<const char *>(pixel), pixelSize));
        }
        if (colors.size() + wideColors.size() > mostColors) return false;
    }
    return !colors.isEmpty() || !wideColors.isEmpty();
}

KisPaintDeviceSP KisResampling::linearCopy(KisPaintDeviceSP device, KisFilterStrategy *filter, qreal xscale, qreal yscale)
{
    if (!device || !filter || !filter->shrinksInLinearLight()) return nullptr;
    if (std::abs(xscale) >= 1.0 && std::abs(yscale) >= 1.0) return nullptr;

    const KoColorSpace *cs = device->colorSpace();
    if (cs->colorModelId() != RGBAColorModelID || !cs->profile() || cs->profile()->isLinear()) return nullptr;

    const KoColorSpace *linear = KoColorSpaceRegistry::instance()->colorSpace(
        RGBAColorModelID.id(), Float32BitsColorDepthID.id(), KoColorSpaceRegistry::instance()->p709G10Profile());
    if (!linear) return nullptr;

    KisPaintDeviceSP copy = new KisPaintDevice(*device);
    if (!isSrgb8(cs)) {
        copy->convertTo(linear);
        return copy;
    }
    // the copy keeps the device's place, bounds and default pixel; its pixels come from the tables
    const QRect rect = device->extent();
    copy->clear();
    copy->convertTo(linear);
    const float *decode = KoMixing::srgbToLinearTable();
    convertRows(device, copy, rect, [decode](const quint8 *from, quint8 *bytes, int count) {
        float *to = reinterpret_cast<float *>(bytes);
        // blue, green, red, alpha in, red, green, blue, alpha out
        for (int i = 0; i < count; i++, from += 4, to += 4) {
            to[0] = decode[from[2]];
            to[1] = decode[from[1]];
            to[2] = decode[from[0]];
            to[3] = from[3] * (1.0f / 255);
        }
    });
    return copy;
}

void KisResampling::writeBack(KisPaintDeviceSP linear, KisPaintDeviceSP device, const QRect &cleared, const QRect &written)
{
    const QRect changed = cleared | written;
    if (!isSrgb8(device->colorSpace())) {
        linear->convertTo(device->colorSpace());
        device->clear(changed);
        KisPainter::copyAreaOptimized(changed.topLeft(), linear, device, changed);
        return;
    }
    device->clear(changed);
    const quint8 *encode = KoMixing::linearToSrgbTable();
    auto encoded = [encode](float c) { return encode[int(std::min(std::max(c, 0.0f), 1.0f) * 65535 + 0.5f)]; };
    convertRows(linear, device, changed & linear->extent(), [encoded](const quint8 *bytes, quint8 *to, int count) {
        const float *from = reinterpret_cast<const float *>(bytes);
        for (int i = 0; i < count; i++, from += 4, to += 4) {
            to[0] = encoded(from[2]);
            to[1] = encoded(from[1]);
            to[2] = encoded(from[0]);
            to[3] = quint8(std::lround(std::min(std::max(from[3], 0.0f), 1.0f) * 255));
        }
    });
}

QString KisResampling::kritaFilterId(const QString &id)
{
    if (id == QLatin1String("Smooth")) return QStringLiteral("Lanczos3");
    if (id == QLatin1String("Pixel")) return QStringLiteral("NearestNeighbor");
    return id;
}
