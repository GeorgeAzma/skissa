/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "SkissaFewerFilter.h"

#include <QMutex>

#include <memory>
#include <vector>

#include <KoColorSpace.h>
#include <filter/kis_filter_category_ids.h>
#include <filter/kis_filter_configuration.h>
#include <kis_datamanager.h>
#include <kis_default_bounds_base.h>
#include <kis_paint_device.h>
#include <klocalizedstring.h>

#include "SkissaDetailWidget.h"
#include "SkissaFewer.h"
#include "SkissaPixels.h"

using namespace SkissaFewer;

namespace
{

/// Error diffusion runs through the whole image, so any pixel may change any after it
constexpr int anywhere = 16384;

/// The colors error diffusion chose for every pixel of an image, made once for all its patches
struct Diffused {
    QMutex lock;
    bool done {false};
    QRect rect;
    std::vector<quint16> chosen;
};

/**
 * The key of \p rect of \p device's pixels as they were, hashed once for all patches of a run: while a transaction
 * holds the device, its old pixels can't change. A mask's patches each have a copy of their own, and are hashed.
 */
quint64 originalKeyOf(KisPaintDeviceSP device, const QRect &rect)
{
    struct Seen {
        KisPaintDeviceWSP device;
        int sequence {0};
        QRect rect;
        quint64 key {0};
    };
    static QMutex lock;
    static Seen seen;
    const bool held = device->dataManager()->hasCurrentMemento();
    const int sequence = device->sequenceNumber();
    if (held) {
        QMutexLocker locker(&lock);
        if (seen.device.isValid() && seen.device == device.data() && seen.sequence == sequence && seen.rect == rect) return seen.key;
    }
    const quint64 key = SkissaPixels::originalKey(device, rect);
    if (held) {
        QMutexLocker locker(&lock);
        seen.device = device.data();
        seen.sequence = sequence;
        seen.rect = rect;
        seen.key = key;
    }
    return key;
}

/**
 * The diffusion of \p rect of \p device's pixels as they were when the effect began. Patches run side by side, and
 * a filter mask's each on a copy of their own, so it is found again by what it was made of: the pixels and the
 * settings. The last one is kept.
 */
std::shared_ptr<Diffused> diffusedFor(KisPaintDeviceSP device, const QRect &rect, const Settings &settings, const Quantizer &quantizer, const QString &config)
{
    const quint64 key = originalKeyOf(device, rect) ^ (quint64(qHash(config)) << 32 | qHash(device->colorSpace()->id()));
    static QMutex kept;
    static quint64 keptKey = 0;
    static std::shared_ptr<Diffused> last;
    std::shared_ptr<Diffused> diffused;
    {
        QMutexLocker locker(&kept);
        if (!last || keptKey != key) {
            last = std::make_shared<Diffused>();
            keptKey = key;
        }
        diffused = last;
    }
    QMutexLocker locker(&diffused->lock);
    if (diffused->done) return diffused;
    diffused->rect = rect;
    diffused->chosen.resize(size_t(rect.width()) * rect.height());
    Diffuser diffuser(settings, quantizer, rect.width());
    constexpr int strip = 64;
    for (int top = rect.top(); top <= rect.bottom(); top += strip) {
        const QRect part(rect.left(), top, rect.width(), std::min(strip, rect.bottom() + 1 - top));
        const std::vector<float> pixels = SkissaPixels::readLinear(device, part);
        for (int y = 0; y < part.height(); y++) {
            diffuser.row(pixels.data() + size_t(y) * part.width() * 4, diffused->chosen.data() + size_t(top - rect.top() + y) * rect.width());
        }
    }
    diffused->done = true;
    return diffused;
}

} // namespace

SkissaFewerFilter::SkissaFewerFilter()
    : KisFilter(fewerId(), FiltersCategoryArtisticId, i18nc("@action effect", "&Fewer Colors..."))
{
    setSupportsPainting(true);
    setSupportsAdjustmentLayers(true);
    setSupportsThreading(true);
    setSupportsLevelOfDetail(true);
    setColorSpaceIndependence(FULLY_INDEPENDENT);
}

KoID SkissaFewerFilter::fewerId()
{
    return KoID(QStringLiteral("skissa_fewer_colors"), i18nc("effect", "Fewer colors"));
}

QRect SkissaFewerFilter::neededRect(const QRect &rect, const KisFilterConfigurationSP config, int) const
{
    const Settings settings = Settings::from(*config);
    if (settings.dither != Dither::Diffuse || settings.amount <= 0) return rect;
    return rect.adjusted(-anywhere, -anywhere, anywhere, anywhere);
}

QRect SkissaFewerFilter::changedRect(const QRect &rect, const KisFilterConfigurationSP config, int lod) const
{
    return neededRect(rect, config, lod);
}

void SkissaFewerFilter::processImpl(KisPaintDeviceSP device, const QRect &applyRect, const KisFilterConfigurationSP config, KoUpdater *) const
{
    KIS_SAFE_ASSERT_RECOVER_RETURN(config);
    if (applyRect.isEmpty()) return;
    const Settings settings = Settings::from(*config);
    const Quantizer quantizer(settings);
    std::vector<float> pixels = SkissaPixels::readLinear(device, applyRect);
    if (settings.dither != Dither::Diffuse || settings.amount <= 0) {
        apply(settings, quantizer, pixels.data(), applyRect);
    } else {
        // beyond the image there is nothing to diffuse through: the nearest color
        const QRect bounds = device->defaultBounds()->bounds();
        const std::shared_ptr<Diffused> diffused = diffusedFor(device, bounds, settings, quantizer, config->toXML());
        Settings plain = settings;
        plain.dither = Dither::None;
        for (int y = 0; y < applyRect.height(); y++) {
            for (int x = 0; x < applyRect.width(); x++) {
                float *pixel = pixels.data() + (size_t(y) * applyRect.width() + x) * 4;
                const QPoint at(applyRect.left() + x, applyRect.top() + y);
                if (!diffused->rect.contains(at)) {
                    apply(plain, quantizer, pixel, QRect(at, QSize(1, 1)));
                    continue;
                }
                const quint16 chosen = diffused->chosen[size_t(at.y() - diffused->rect.top()) * diffused->rect.width() + (at.x() - diffused->rect.left())];
                if (chosen != Diffuser::none) std::copy_n(quantizer.colorOf(chosen), 3, pixel);
            }
        }
    }
    SkissaPixels::writeLinear(device, applyRect, pixels.data());
}

KisConfigWidget *SkissaFewerFilter::createConfigurationWidget(QWidget *parent, const KisPaintDeviceSP dev, bool) const
{
    return new SkissaFewerWidget(parent, dev);
}

KisFilterConfigurationSP SkissaFewerFilter::defaultConfiguration(KisResourcesInterfaceSP resourcesInterface) const
{
    KisFilterConfigurationSP config = factoryConfiguration(resourcesInterface);
    // the picture's own colors; settings without a source read as levels, as they were saved
    Settings settings;
    settings.source = Source::Image;
    settings.count = 8;
    settings.writeTo(*config);
    return config;
}
