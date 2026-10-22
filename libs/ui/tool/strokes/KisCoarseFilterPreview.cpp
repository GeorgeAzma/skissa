/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisCoarseFilterPreview.h"

#include <cmath>
#include <limits>

#include <QVector>

#include <KoChannelInfo.h>
#include <KoColorSpace.h>
#include <KoMixColorsOp.h>
#include <filter/kis_filter.h>
#include <filter/kis_filter_configuration.h>
#include <kis_default_bounds_base.h>
#include <kis_lod_transform_base.h>
#include <kis_paint_device.h>

namespace
{
/// The bounds of a device a level of detail smaller: filters scale their sizes by it (KisLodTransformScalar)
class LodBounds : public KisDefaultBoundsBase
{
public:
    LodBounds(KisDefaultBoundsBaseSP base, int lod)
        : m_base(base)
        , m_lod(lod)
    {
    }
    QRect bounds() const override { return KisLodTransformBase::scaledRect(m_base->bounds(), m_lod); }
    bool wrapAroundMode() const override { return m_base->wrapAroundMode(); }
    WrapAroundAxis wrapAroundModeAxis() const override { return m_base->wrapAroundModeAxis(); }
    int currentLevelOfDetail() const override { return m_lod; }
    int currentTime() const override { return m_base->currentTime(); }
    bool externalFrameActive() const override { return m_base->externalFrameActive(); }
    void *sourceCookie() const override { return m_base->sourceCookie(); }

private:
    KisDefaultBoundsBaseSP m_base;
    int m_lod;
};

/// Bilinear upscale of \p in (covering \p from) by \p scale into \p patch, for channels all of plain type T
template <typename T>
void bilinear(const quint8 *in, const QRect &from, quint8 *out, const QRect &patch, int channels, int scale)
{
    auto source = [scale](int x) { return (x + 0.5f) / scale - 0.5f; };
    QVector<int> x0(patch.width()), x1(patch.width());
    QVector<float> wx(patch.width());
    for (int x = 0; x < patch.width(); x++) {
        const float fx = source(patch.left() + x);
        const int column = qBound(0, qFloor(fx) - from.left(), from.width() - 1);
        x0[x] = column * channels;
        x1[x] = qMin(column + 1, from.width() - 1) * channels;
        wx[x] = fx - qFloor(fx);
    }
    const int rowLength = from.width() * channels;
    QVector<float> mixed(rowLength);
    const T *src = reinterpret_cast<const T *>(in);
    T *dst = reinterpret_cast<T *>(out);
    const bool integer = std::numeric_limits<T>::is_integer;
    for (int y = 0; y < patch.height(); y++) {
        const float fy = source(patch.top() + y);
        const int y0 = qBound(0, qFloor(fy) - from.top(), from.height() - 1);
        const int y1 = qMin(y0 + 1, from.height() - 1);
        const float wy = fy - qFloor(fy);
        const T *a = src + y0 * rowLength;
        const T *b = src + y1 * rowLength;
        for (int i = 0; i < rowLength; i++) {
            mixed[i] = float(a[i]) + (float(b[i]) - float(a[i])) * wy;
        }
        for (int x = 0; x < patch.width(); x++) {
            const float *p = mixed.constData() + x0[x];
            const float *q = mixed.constData() + x1[x];
            const float w = wx[x];
            for (int c = 0; c < channels; c++) {
                const float v = p[c] + (q[c] - p[c]) * w;
                *dst++ = integer ? T(v + 0.5f) : T(v);
            }
        }
    }
}
} // namespace

KisDefaultBoundsBaseSP KisCoarseFilterPreview::lodBounds(KisDefaultBoundsBaseSP base, int lod)
{
    return new LodBounds(base, lod);
}

int KisCoarseFilterPreview::level(KisFilterSP filter, const KisFilterConfigurationSP &config, const QRect &rect)
{
    const qreal pixels = qreal(rect.width()) * rect.height();
    if (pixels < 2.0e6) return 0;
    // per pixel only (a curve, levels): as fast at full resolution as making it smaller and back
    const QRect needed = filter->neededRect(rect, config, 0);
    if (needed.width() <= rect.width() + 4 && needed.height() <= rect.height() + 4) return 0;
    const int margin = qMax(needed.width() - rect.width(), needed.height() - rect.height()) / 2;
    int lod = qBound(1, qCeil(std::log(pixels / 0.5e6) / std::log(4.0)), 6);
    while (lod < 6 && (margin >> lod) > 24) lod++;
    while (lod > 0 && !filter->supportsLevelOfDetail(config, lod)) lod--;
    return lod;
}

void KisCoarseFilterPreview::upscale(KisPaintDeviceSP small, KisPaintDeviceSP target, const QRect &patch, int lod)
{
    const int scale = 1 << lod;
    const KoColorSpace *cs = small->colorSpace();
    const int pixelSize = int(cs->pixelSize());
    auto source = [scale](int x) { return (x + 0.5) / scale - 0.5; };
    const QRect from(QPoint(qFloor(source(patch.left())), qFloor(source(patch.top()))),
                     QPoint(qFloor(source(patch.right())) + 1, qFloor(source(patch.bottom())) + 1));
    QVector<quint8> in(from.width() * from.height() * pixelSize);
    small->readBytes(in.data(), from);
    QVector<quint8> out(patch.width() * patch.height() * pixelSize);
    // channels all of one plain type: mixed as numbers, not through the color space
    const QList<KoChannelInfo *> channels = cs->channels();
    const KoChannelInfo::enumChannelValueType type = channels.first()->channelValueType();
    bool plain = type == KoChannelInfo::UINT8 || type == KoChannelInfo::UINT16 || type == KoChannelInfo::FLOAT32;
    Q_FOREACH (const KoChannelInfo *channel, channels) plain &= channel->channelValueType() == type;
    const int count = channels.size();
    if (plain && count * int(channels.first()->size()) == pixelSize) {
        if (type == KoChannelInfo::UINT8) {
            bilinear<quint8>(in.constData(), from, out.data(), patch, count, scale);
        } else if (type == KoChannelInfo::UINT16) {
            bilinear<quint16>(in.constData(), from, out.data(), patch, count, scale);
        } else {
            bilinear<float>(in.constData(), from, out.data(), patch, count, scale);
        }
        target->writeBytes(out.constData(), patch);
        return;
    }
    KoMixColorsOp *mix = cs->mixColorsOp();
    const quint8 *colors[4];
    qint16 weights[4];
    for (int y = 0; y < patch.height(); y++) {
        const qreal fy = source(patch.top() + y);
        const int y0 = qBound(0, qFloor(fy) - from.top(), from.height() - 1);
        const int y1 = qMin(y0 + 1, from.height() - 1);
        const qreal wy = fy - qFloor(fy);
        for (int x = 0; x < patch.width(); x++) {
            const qreal fx = source(patch.left() + x);
            const int x0 = qBound(0, qFloor(fx) - from.left(), from.width() - 1);
            const int x1 = qMin(x0 + 1, from.width() - 1);
            const qreal wx = fx - qFloor(fx);
            colors[0] = in.constData() + (y0 * from.width() + x0) * pixelSize;
            colors[1] = in.constData() + (y0 * from.width() + x1) * pixelSize;
            colors[2] = in.constData() + (y1 * from.width() + x0) * pixelSize;
            colors[3] = in.constData() + (y1 * from.width() + x1) * pixelSize;
            quint8 *dst = out.data() + (y * patch.width() + x) * pixelSize;
            weights[0] = qint16(qRound((1 - wx) * (1 - wy) * 255));
            weights[1] = qint16(qRound(wx * (1 - wy) * 255));
            weights[2] = qint16(qRound((1 - wx) * wy * 255));
            weights[3] = qint16(255 - weights[0] - weights[1] - weights[2]);
            mix->mixColors(colors, weights, 4, dst, 255);
        }
    }
    target->writeBytes(out.constData(), patch);
    target->writeBytes(out.constData(), patch);
}
