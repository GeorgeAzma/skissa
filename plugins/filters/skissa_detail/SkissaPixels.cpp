/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "SkissaPixels.h"

#include <algorithm>
#include <cstring>
#include <thread>

#include <QMutex>
#include <QThread>

#include <KoChannelInfo.h>
#include <KoColorConversionTransformation.h>
#include <KoColorSpace.h>
#include <KoColorSpaceRegistry.h>
#include <kis_iterator_ng.h>
#include <kis_datamanager.h>
#include <kis_paint_device.h>

#include "KoCompositeOpInLinear.h"

namespace SkissaPixels
{

void readOriginal(KisPaintDeviceSP device, const QRect &rect, quint8 *out)
{
    const int pixelSize = device->pixelSize();
    const int w = rect.width();
    KisHLineConstIteratorSP it = device->createHLineConstIteratorNG(rect.x(), rect.y(), w);
    for (int y = 0; y < rect.height(); y++) {
        quint8 *row = out + size_t(y) * w * pixelSize;
        for (int x = 0; x < w;) {
            const int run = std::min(it->nConseqPixels(), w - x);
            std::memcpy(row + size_t(x) * pixelSize, it->oldRawData(), size_t(run) * pixelSize);
            x += run;
            if (!it->nextPixels(run)) break;
        }
        it->nextRow();
    }
}

quint64 originalKeyOf(KisPaintDeviceSP device, const QRect &rect)
{
    struct Seen {
        KisPaintDeviceWSP device;
        KisWeakSharedPtr<KisMemento> transaction;
        QRect rect;
        quint64 key {0};
    };
    static QMutex lock;
    static Seen seen;
    const KisMementoSP transaction = device->dataManager()->currentMemento();
    if (!transaction) return originalKey(device, rect);
    // hashed while the run's other patches wait for it: each would hash the whole image itself
    QMutexLocker locker(&lock);
    if (seen.device.isValid() && seen.device == device.data() && seen.transaction.isValid() && seen.transaction == transaction.data()
        && seen.rect == rect) {
        return seen.key;
    }
    seen.device = device.data();
    seen.transaction = transaction;
    seen.rect = rect;
    seen.key = originalKey(device, rect);
    return seen.key;
}

std::shared_ptr<const std::vector<float>> wholeResult(KisPaintDeviceSP device, const QRect &whole, quint64 settingsKey,
                                                      const std::function<void(const float *in, const QRect &part, float *out)> &make)
{
    struct Made {
        QMutex lock;
        bool done {false};
        std::shared_ptr<std::vector<float>> pixels;
    };
    // the image read and its result, both as floats: past this a patch makes its own part
    constexpr qint64 mostPixels = 16'000'000;
    if (whole.isEmpty() || qint64(whole.width()) * whole.height() > mostPixels) return nullptr;
    const quint64 key = originalKeyOf(device, whole) ^ settingsKey;
    static QMutex kept;
    static quint64 keptKey = 0;
    static std::shared_ptr<Made> last;
    std::shared_ptr<Made> made;
    {
        QMutexLocker locker(&kept);
        if (!last || keptKey != key) {
            last = std::make_shared<Made>();
            keptKey = key;
        }
        made = last;
    }
    QMutexLocker locker(&made->lock);
    if (made->done) return made->pixels;
    const std::vector<float> in = readLinear(device, whole);
    auto out = std::make_shared<std::vector<float>>(size_t(whole.width()) * whole.height() * 4);
    // a band a core, the patches waiting meanwhile
    const int bands = std::max(1, std::min(QThread::idealThreadCount(), whole.height()));
    std::vector<std::thread> threads;
    for (int b = 0; b < bands; b++) {
        const int top = whole.top() + whole.height() * b / bands, bottom = whole.top() + whole.height() * (b + 1) / bands;
        if (bottom <= top) continue;
        threads.emplace_back([&, top, bottom]() {
            const QRect part(whole.left(), top, whole.width(), bottom - top);
            make(in.data(), part, out->data() + size_t(top - whole.top()) * whole.width() * 4);
        });
    }
    for (std::thread &thread : threads) thread.join();
    made->pixels = out;
    made->done = true;
    return out;
}

std::vector<float> partOf(const std::vector<float> &pixels, const QRect &whole, const QRect &rect)
{
    std::vector<float> part(size_t(rect.width()) * rect.height() * 4);
    for (int y = 0; y < rect.height(); y++) {
        const float *from = pixels.data() + (size_t(rect.top() - whole.top() + y) * whole.width() + (rect.left() - whole.left())) * 4;
        std::copy_n(from, size_t(rect.width()) * 4, part.data() + size_t(y) * rect.width() * 4);
    }
    return part;
}

quint64 originalKey(KisPaintDeviceSP device, const QRect &rect)
{
    const int pixelSize = device->pixelSize();
    quint64 key = 0xcbf29ce484222325ull;
    auto add = [&key](quint64 word) {
        key = (key ^ word) * 0x100000001b3ull;
        key ^= key >> 29;
    };
    add(quint64(rect.x()) << 32 | quint32(rect.y()));
    add(quint64(rect.width()) << 32 | quint32(rect.height()));
    KisHLineConstIteratorSP it = device->createHLineConstIteratorNG(rect.x(), rect.y(), rect.width());
    for (int y = 0; y < rect.height(); y++) {
        for (int x = 0; x < rect.width();) {
            const int run = std::min(it->nConseqPixels(), rect.width() - x);
            const quint8 *data = it->oldRawData();
            const size_t bytes = size_t(run) * pixelSize;
            size_t i = 0;
            for (; i + 8 <= bytes; i += 8) {
                quint64 word;
                std::memcpy(&word, data + i, 8);
                add(word);
            }
            for (; i < bytes; i++) add(data[i]);
            x += run;
            if (!it->nextPixels(run)) break;
        }
        it->nextRow();
    }
    return key;
}

std::vector<float> readLinear(KisPaintDeviceSP device, const QRect &rect)
{
    const KoColorSpace *cs = device->colorSpace();
    const KoColorSpace *linear = KoMixing::linearSpace();
    const size_t count = size_t(rect.width()) * rect.height();
    std::vector<quint8> raw(count * cs->pixelSize());
    readOriginal(device, rect, raw.data());
    std::vector<float> in(count * 4);
    if (*cs == *KoColorSpaceRegistry::instance()->rgb8()) {
        // blue, green, red, alpha, as Krita keeps 8-bit RGB
        const float *decode = KoMixing::srgbToLinearTable();
        for (size_t i = 0; i < count; i++) {
            const quint8 *pixel = raw.data() + i * 4;
            float *to = in.data() + i * 4;
            to[0] = decode[pixel[2]];
            to[1] = decode[pixel[1]];
            to[2] = decode[pixel[0]];
            to[3] = pixel[3] / 255.0f;
        }
    } else if (*cs == *linear) {
        std::copy(raw.begin(), raw.end(), reinterpret_cast<quint8 *>(in.data()));
    } else {
        cs->convertPixelsTo(raw.data(), reinterpret_cast<quint8 *>(in.data()), linear, int(count),
                            KoColorConversionTransformation::internalRenderingIntent(), KoColorConversionTransformation::internalConversionFlags());
    }
    return in;
}

void writeLinear(KisPaintDeviceSP device, const QRect &rect, const float *pixels)
{
    const KoColorSpace *cs = device->colorSpace();
    const KoColorSpace *linear = KoMixing::linearSpace();
    const size_t count = size_t(rect.width()) * rect.height();
    std::vector<quint8> raw(count * cs->pixelSize());
    if (*cs == *KoColorSpaceRegistry::instance()->rgb8()) {
        const quint8 *encode = KoMixing::linearToSrgbTable();
        auto encoded = [encode](float c) { return encode[int(std::clamp(c, 0.0f, 1.0f) * 65535 + 0.5f)]; };
        for (size_t i = 0; i < count; i++) {
            const float *from = pixels + i * 4;
            quint8 *pixel = raw.data() + i * 4;
            pixel[2] = encoded(from[0]);
            pixel[1] = encoded(from[1]);
            pixel[0] = encoded(from[2]);
            pixel[3] = quint8(std::clamp(from[3], 0.0f, 1.0f) * 255 + 0.5f);
        }
    } else if (*cs == *linear) {
        std::copy_n(reinterpret_cast<const quint8 *>(pixels), raw.size(), raw.data());
    } else {
        linear->convertPixelsTo(reinterpret_cast<const quint8 *>(pixels), raw.data(), cs, int(count),
                                KoColorConversionTransformation::internalRenderingIntent(), KoColorConversionTransformation::internalConversionFlags());
    }
    device->writeBytes(raw.data(), rect);
}

bool holdsPastWhite(const KoColorSpace *cs)
{
    const KoChannelInfo::enumChannelValueType type = cs->channels().first()->channelValueType();
    return type == KoChannelInfo::FLOAT16 || type == KoChannelInfo::FLOAT32 || type == KoChannelInfo::FLOAT64;
}

} // namespace SkissaPixels
