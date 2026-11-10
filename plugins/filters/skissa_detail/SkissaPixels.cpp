/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "SkissaPixels.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

#include <QCoreApplication>
#include <QMutex>
#include <QThread>
#include <QTimer>

#include <KoChannelInfo.h>
#include <KoColorConversionTransformation.h>
#include <KoColorModelStandardIds.h>
#include <KoColorProfile.h>
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

namespace
{

using Clock = std::chrono::steady_clock;

/// What is kept at most: so many results, within so many bytes, none unasked for longer than a while
constexpr int keptMost = 4;
constexpr size_t keptBytes = size_t(512) << 20;
constexpr std::chrono::seconds keptUnused(30);

struct Kept {
    quint64 key;
    size_t type;
    size_t bytes;
    Clock::time_point used;
    std::shared_ptr<void> value;
};

QMutex keptLock;
std::vector<Kept> keptList;
bool sweepWaiting = false;

/// What nobody asked for lately let go; with keptLock held
void dropUnused()
{
    const Clock::time_point now = Clock::now();
    keptList.erase(std::remove_if(keptList.begin(), keptList.end(), [now](const Kept &kept) { return now - kept.used > keptUnused; }),
                   keptList.end());
}

void sweepLater();

void sweep()
{
    QMutexLocker locker(&keptLock);
    sweepWaiting = false;
    dropUnused();
    if (!keptList.empty()) sweepLater();
}

/// A sweep once what is kept now has gone unused for long enough, on the window's thread, whose timers run; with
/// keptLock held
void sweepLater()
{
    QCoreApplication *app = QCoreApplication::instance();
    if (sweepWaiting || !app) return;
    sweepWaiting = true;
    const int wait = int(std::chrono::duration_cast<std::chrono::milliseconds>(keptUnused).count()) + 1000;
    QMetaObject::invokeMethod(
        app,
        [app, wait]() {
            QTimer::singleShot(wait, app, []() { sweep(); });
        },
        Qt::QueuedConnection);
}

/// A whole result, made by the first patch that asks while the others wait for it
struct Made {
    QMutex lock;
    bool done {false};
    std::shared_ptr<std::vector<float>> pixels;
};

} // namespace

std::shared_ptr<void> keptFor(quint64 key, size_t type, size_t bytes, const std::function<std::shared_ptr<void>()> &make)
{
    QMutexLocker locker(&keptLock);
    const Clock::time_point now = Clock::now();
    for (Kept &kept : keptList) {
        if (kept.key == key && kept.type == type) {
            kept.used = now;
            return kept.value;
        }
    }
    dropUnused();
    const std::shared_ptr<void> value = make();
    keptList.push_back(Kept{key, type, bytes, now, value});
    // the least lately used go first, never the one just made
    auto total = [] {
        size_t sum = 0;
        for (const Kept &kept : keptList) sum += kept.bytes;
        return sum;
    };
    while (keptList.size() > 1 && (int(keptList.size()) > keptMost || total() > keptBytes)) {
        keptList.erase(std::min_element(keptList.begin(), keptList.end() - 1, [](const Kept &a, const Kept &b) { return a.used < b.used; }));
    }
    sweepLater();
    return value;
}

std::shared_ptr<const std::vector<float>> wholeResult(KisPaintDeviceSP device, const QRect &whole, quint64 settingsKey,
                                                      const std::function<void(const float *in, const QRect &part, float *out)> &make)
{
    // the image read and its result, both as floats: past this a patch makes its own part
    constexpr qint64 mostPixels = 16'000'000;
    if (whole.isEmpty() || qint64(whole.width()) * whole.height() > mostPixels) return nullptr;
    const quint64 key = originalKeyOf(device, whole) ^ settingsKey;
    const std::shared_ptr<Made> made = kept<Made>(key, size_t(whole.width()) * whole.height() * 4 * sizeof(float));
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

bool primariesAreSrgb(const KoColorSpace *cs)
{
    if (*cs == *KoColorSpaceRegistry::instance()->rgb8()) return true;
    if (cs->colorModelId() != RGBAColorModelID) return false;
    const KoColorProfile *own = cs->profile();
    const KoColorProfile *linear = KoMixing::linearSpace()->profile();
    if (!own || !linear || !own->hasColorants() || !linear->hasColorants()) return false;
    const QVector<KoColorimetryUtils::xyY> mine = own->getColorantsxyY(), srgb = linear->getColorantsxyY();
    if (mine.size() < 3 || srgb.size() < 3) return false;
    // sRGB profiles differ in their last digits; Display P3's and Adobe RGB's primaries are hundredths away
    for (int i = 0; i < 3; i++) {
        if (std::abs(mine[i].x - srgb[i].x) > 1e-3 || std::abs(mine[i].y - srgb[i].y) > 1e-3) return false;
    }
    return true;
}

} // namespace SkissaPixels
