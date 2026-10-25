/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KOCOMPOSITEOPINLINEAR_H
#define KOCOMPOSITEOPINLINEAR_H

#include <QMutex>
#include <QMutexLocker>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

#include "KoChannelInfo.h"
#include "KoColorConversionTransformation.h"
#include "KoColorModelStandardIds.h"
#include "KoColorSpace.h"
#include "KoColorSpaceRegistry.h"
#include "KoCompositeOp.h"

namespace KoMixing
{
inline void linearToOklab(const float rgb[3], float lab[3])
{
    const float l = std::cbrt(0.4122214708f * rgb[0] + 0.5363325363f * rgb[1] + 0.0514459929f * rgb[2]);
    const float m = std::cbrt(0.2119034982f * rgb[0] + 0.6806995451f * rgb[1] + 0.1073969566f * rgb[2]);
    const float s = std::cbrt(0.0883024619f * rgb[0] + 0.2817188376f * rgb[1] + 0.6299787005f * rgb[2]);
    lab[0] = 0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s;
    lab[1] = 1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s;
    lab[2] = 0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s;
}

inline void oklabToLinear(const float lab[3], float rgb[3])
{
    const float l1 = lab[0] + 0.3963377774f * lab[1] + 0.2158037573f * lab[2];
    const float m1 = lab[0] - 0.1055613458f * lab[1] - 0.0638541728f * lab[2];
    const float s1 = lab[0] - 0.0894841775f * lab[1] - 1.2914855480f * lab[2];
    const float l = l1 * l1 * l1, m = m1 * m1 * m1, s = s1 * s1 * s1;
    rgb[0] = 4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s;
    rgb[1] = -1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s;
    rgb[2] = -0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s;
}

/// Linear sRGB with alpha, in floats: where the mixing happens
inline const KoColorSpace *linearSpace()
{
    static const KoColorSpace *space = KoColorSpaceRegistry::instance()->colorSpace(
        RGBAColorModelID.id(), Float32BitsColorDepthID.id(), KoColorSpaceRegistry::instance()->p709G10Profile());
    return space;
}
} // namespace KoMixing

/**
 * A blend that works in linear sRGB: each row is brought there through the image's own profile, so an sRGB image
 * and a linear one blend alike, and the pixels the blend changed are brought back.
 */
class KoCompositeOpInLinear : public KoCompositeOp
{
public:
    using KoCompositeOp::KoCompositeOp;

protected:
    /**
     * Calls \p pixel(source, under, amount, alphaLocked) for each pixel: both are linear sRGB and alpha, the amount
     * is the opacity times the mask, and it changes \p under and says whether it did.
     */
    template<class Pixel>
    void compositeRows(const KoCompositeOp::ParameterInfo &params, Pixel &pixel) const
    {
        const KoColorSpace *own = colorSpace();
        const KoColorSpace *linear = KoMixing::linearSpace();
        if (!linear || params.rows <= 0 || params.cols <= 0) return;
        const int pixelSize = own->pixelSize();
        const int cols = params.cols;

        // channels switched off keep their values (the alpha channel off is the layer's alpha lock), but where
        // nothing was painted they are cleared, as Krita's own blends do
        const QList<KoChannelInfo *> channels = own->channels();
        bool alphaLocked = false;
        bool someLocked = false;
        if (!params.channelFlags.isEmpty()) {
            for (int i = 0; i < channels.size() && i < params.channelFlags.size(); i++) {
                if (params.channelFlags.testBit(i)) continue;
                someLocked = true;
                if (channels[i]->channelType() == KoChannelInfo::ALPHA) alphaLocked = true;
            }
        }

        const bool oneSource = params.srcRowStride == 0;
        std::unique_ptr<Workspace> workspace = take(linear);
        QVector<float> &source = workspace->source;
        QVector<float> &under = workspace->under;
        QVector<quint8> &back = workspace->back;
        QVector<bool> &changed = workspace->changed;
        QVector<bool> &wasClear = workspace->wasClear;
        source.resize(oneSource ? 4 : cols * 4);
        under.resize(cols * 4);
        back.resize(cols * pixelSize);
        changed.resize(cols);
        wasClear.resize(cols);
        // an image already in linear sRGB floats needs no conversion
        const bool same = !workspace->toLinear;
        auto toLinear = [&](const quint8 *from, float *to, int count) {
            if (same) std::memcpy(to, from, size_t(count) * pixelSize);
            else workspace->toLinear->transform(from, reinterpret_cast<quint8 *>(to), count);
        };
        if (oneSource) toLinear(params.srcRowStart, source.data(), 1);

        for (int row = 0; row < params.rows; row++) {
            quint8 *dst = params.dstRowStart + row * params.dstRowStride;
            const quint8 *src = params.srcRowStart + row * params.srcRowStride;
            const quint8 *mask = params.maskRowStart ? params.maskRowStart + row * params.maskRowStride : nullptr;
            toLinear(dst, under.data(), cols);
            if (!oneSource) toLinear(src, source.data(), cols);
            bool any = false;
            for (int x = 0; x < cols; x++) {
                const float *s = oneSource ? source.data() : source.data() + 4 * x;
                float *d = under.data() + 4 * x;
                wasClear[x] = d[3] <= 0;
                const float amount = params.opacity * (mask ? mask[x] / 255.0f : 1.0f);
                changed[x] = pixel(s, d, amount, alphaLocked);
                any = any || changed[x];
            }
            if (!any) continue;
            // only the pixels painted on are written back, so the rest keep their exact values
            if (same) std::memcpy(back.data(), under.data(), size_t(cols) * pixelSize);
            else workspace->toOwn->transform(reinterpret_cast<const quint8 *>(under.data()), back.data(), cols);
            for (int x = 0; x < cols; x++) {
                if (!changed[x]) continue;
                const quint8 *blended = back.data() + x * pixelSize;
                quint8 *out = dst + x * pixelSize;
                if (!someLocked) {
                    std::copy(blended, blended + pixelSize, out);
                    continue;
                }
                for (int i = 0; i < channels.size(); i++) {
                    const int at = channels[i]->pos();
                    const int size = channels[i]->size();
                    if (i < params.channelFlags.size() && !params.channelFlags.testBit(i)) {
                        if (wasClear[x]) std::fill(out + at, out + at + size, quint8(0));
                    } else {
                        std::copy(blended + at, blended + at + size, out + at);
                    }
                }
            }
        }
        giveBack(std::move(workspace));
    }

private:
    /**
     * What one composite() works with: the color conversions, which can't be shared between threads and are
     * costly to make (through the color spaces each row looked them up in a cache under a lock), and its rows
     */
    struct Workspace {
        std::unique_ptr<KoColorConversionTransformation> toLinear;
        std::unique_ptr<KoColorConversionTransformation> toOwn;
        QVector<float> source;
        QVector<float> under;
        QVector<quint8> back;
        QVector<bool> changed;
        QVector<bool> wasClear;
    };

    /// A workspace no other thread is using
    std::unique_ptr<Workspace> take(const KoColorSpace *linear) const
    {
        {
            QMutexLocker lock(&m_spareMutex);
            if (!m_spare.empty()) {
                std::unique_ptr<Workspace> spare = std::move(m_spare.back());
                m_spare.pop_back();
                return spare;
            }
        }
        auto made = std::make_unique<Workspace>();
        const KoColorSpace *own = colorSpace();
        if (!(*own == *linear)) {
            const auto intent = KoColorConversionTransformation::internalRenderingIntent();
            const auto flags = KoColorConversionTransformation::internalConversionFlags();
            made->toLinear.reset(own->createColorConverter(linear, intent, flags));
            made->toOwn.reset(linear->createColorConverter(own, intent, flags));
        }
        return made;
    }

    void giveBack(std::unique_ptr<Workspace> workspace) const
    {
        QMutexLocker lock(&m_spareMutex);
        m_spare.push_back(std::move(workspace));
    }

    mutable QMutex m_spareMutex;
    mutable std::vector<std::unique_ptr<Workspace>> m_spare;
};

#endif // KOCOMPOSITEOPINLINEAR_H
