/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisResampling.h"

#include <cmath>
#include <cstring>
#include <vector>

#include <QSet>

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

void KisResampling::clampBetween(const KoColorSpace *cs, quint8 *pixel, const quint8 *a, const quint8 *b)
{
    for (const KoChannelInfo *channel : cs->channels()) {
        const int offset = channel->pos();
        switch (channel->channelValueType()) {
        case KoChannelInfo::UINT8:
            clampChannel<quint8>(pixel, a, b, offset);
            break;
        case KoChannelInfo::UINT16:
            clampChannel<quint16>(pixel, a, b, offset);
            break;
        case KoChannelInfo::FLOAT32:
            clampChannel<float>(pixel, a, b, offset);
            break;
        default:
            // half floats and the rest keep Lanczos as it is
            return;
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
    QSet<QByteArray> colors;
    for (size_t i = 0; i < alpha.size(); i++) {
        if (!alpha[i]) continue;
        colors.insert(QByteArray(reinterpret_cast<const char *>(bytes.data() + i * pixelSize), pixelSize));
        if (colors.size() > mostColors) return false;
    }
    return !colors.isEmpty();
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
    copy->convertTo(linear);
    return copy;
}

void KisResampling::writeBack(KisPaintDeviceSP linear, KisPaintDeviceSP device, const QRect &cleared, const QRect &written)
{
    linear->convertTo(device->colorSpace());
    const QRect changed = cleared | written;
    device->clear(changed);
    KisPainter::copyAreaOptimized(changed.topLeft(), linear, device, changed);
}

QString KisResampling::kritaFilterId(const QString &id)
{
    if (id == QLatin1String("Smooth")) return QStringLiteral("Lanczos3");
    if (id == QLatin1String("Pixel")) return QStringLiteral("NearestNeighbor");
    return id;
}
