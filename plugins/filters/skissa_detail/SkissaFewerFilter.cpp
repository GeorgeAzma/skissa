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
 * The key of \p rect of \p device's pixels as they were, hashed once for all patches of a run: while one transaction
 * holds the device, its old pixels can't change. A mask's patches each have a copy of their own, and are hashed.
 */
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
    if (transaction) {
        QMutexLocker locker(&lock);
        if (seen.device.isValid() && seen.device == device.data() && seen.transaction.isValid() && seen.transaction == transaction.data()
            && seen.rect == rect) {
            return seen.key;
        }
    }
    const quint64 key = SkissaPixels::originalKey(device, rect);
    if (transaction) {
        QMutexLocker locker(&lock);
        seen.device = device.data();
        seen.transaction = transaction;
        seen.rect = rect;
        seen.key = key;
    }
    return key;
}

/**
 * The diffusion of \p rect of \p device's pixels as they were when the effect began, or of the cells \p cell wide
 * over it (then its rect is in cells). Patches run side by side, and a filter mask's each on a copy of their own, so
 * it is found again by what it was made of: the pixels and the settings. The last one is kept.
 */
std::shared_ptr<Diffused> diffusedFor(KisPaintDeviceSP device, const QRect &rect, const Settings &settings, const Quantizer &quantizer, const QString &config,
                                      int cell)
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
    if (cell > 1) {
        // a row of cells at a time, read with the rows of cells either side, which their colors look at
        const QRect cells = cellsOver(rect, cell);
        diffused->rect = cells;
        diffused->chosen.resize(size_t(cells.width()) * cells.height());
        Diffuser diffuser(settings, quantizer, cells.width());
        for (int row = cells.top(); row <= cells.bottom(); row++) {
            const QRect line(cells.left(), row, cells.width(), 1);
            const QRect read = pixelsOf(line.adjusted(-1, -1, 1, 1), cell);
            const std::vector<float> pixels = SkissaPixels::readLinear(device, read);
            const std::vector<float> colors = cellColors(pixels.data(), read, rect, cell, line);
            diffuser.row(colors.data(), diffused->chosen.data() + size_t(row - cells.top()) * cells.width());
        }
        diffused->done = true;
        return diffused;
    }
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

QRect SkissaFewerFilter::neededRect(const QRect &rect, const KisFilterConfigurationSP config, int lod) const
{
    const Settings settings = Settings::from(*config);
    if (settings.dither == Dither::Diffuse && settings.amount > 0) return rect.adjusted(-anywhere, -anywhere, anywhere, anywhere);
    // pixel art reads whole cells, and the cells around them
    const int cell = cellAt(settings.pixel, lod);
    if (cell > 1) return pixelsOf(cellsOver(rect, cell).adjusted(-1, -1, 1, 1), cell);
    return rect;
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
    const bool diffuses = settings.dither == Dither::Diffuse && settings.amount > 0;
    const int cell = cellAt(settings.pixel, device->defaultBounds()->currentLevelOfDetail());
    if (cell > 1) {
        // pixel art: each cell's color, with fewer colors as one pixel, spread over the cell
        const QRect bounds = device->defaultBounds()->bounds();
        const QRect cells = cellsOver(applyRect, cell);
        const QRect read = pixelsOf(cells.adjusted(-1, -1, 1, 1), cell);
        const std::vector<float> source = SkissaPixels::readLinear(device, read);
        std::vector<float> colors = cellColors(source.data(), read, bounds, cell, cells);
        if (diffuses) {
            const std::shared_ptr<Diffused> diffused = diffusedFor(device, bounds, settings, quantizer, config->toXML(), cell);
            for (int cy = cells.top(); cy <= cells.bottom(); cy++) {
                for (int cx = cells.left(); cx <= cells.right(); cx++) {
                    if (!diffused->rect.contains(cx, cy)) continue;
                    const quint16 chosen = diffused->chosen[size_t(cy - diffused->rect.top()) * diffused->rect.width() + (cx - diffused->rect.left())];
                    float *to = colors.data() + (size_t(cy - cells.top()) * cells.width() + (cx - cells.left())) * 4;
                    if (chosen != Diffuser::none) std::copy_n(quantizer.colorOf(chosen), 3, to);
                }
            }
        } else {
            apply(settings, quantizer, colors.data(), cells);
        }
        std::vector<float> pixels(size_t(applyRect.width()) * applyRect.height() * 4);
        for (int y = 0; y < applyRect.height(); y++) {
            for (int x = 0; x < applyRect.width(); x++) {
                const QPoint at = cellsOver(QRect(applyRect.topLeft() + QPoint(x, y), QSize(1, 1)), cell).topLeft() - cells.topLeft();
                std::copy_n(colors.data() + (size_t(at.y()) * cells.width() + at.x()) * 4, 4, pixels.data() + (size_t(y) * applyRect.width() + x) * 4);
            }
        }
        SkissaPixels::writeLinear(device, applyRect, pixels.data());
        return;
    }
    std::vector<float> pixels = SkissaPixels::readLinear(device, applyRect);
    if (!diffuses) {
        apply(settings, quantizer, pixels.data(), applyRect);
    } else {
        // beyond the image there is nothing to diffuse through: the nearest color
        const QRect bounds = device->defaultBounds()->bounds();
        const std::shared_ptr<Diffused> diffused = diffusedFor(device, bounds, settings, quantizer, config->toXML(), 1);
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
