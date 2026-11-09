/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisHiddenColors.h"

#include <QImage>

#include <cstring>

#include <KoColor.h>
#include <KoColorSpace.h>
#include <kis_paint_device.h>
#include <kis_sequential_iterator.h>

namespace KisHiddenColors
{

KisPaintDeviceSP cleared(KisPaintDeviceSP device, const QRect &rect)
{
    const KoColorSpace *cs = device->colorSpace();
    const int pixelSize = cs->pixelSize();
    const KoColor clear = KoColor::createTransparent(cs);
    auto hides = [&](const quint8 *pixel) {
        return cs->opacityF(pixel) <= 0 && std::memcmp(pixel, clear.data(), pixelSize) != 0;
    };
    bool found = false;
    KisSequentialConstIterator seek(device, rect);
    while (!found && seek.nextPixel()) found = hides(seek.rawDataConst());
    if (!found) return device;

    // tiles are shared until written: only those with hidden colors are copied
    KisPaintDeviceSP copy = new KisPaintDevice(*device);
    KisSequentialIterator it(copy, rect);
    while (it.nextPixel()) {
        if (hides(it.rawDataConst())) std::memcpy(it.rawData(), clear.data(), pixelSize);
    }
    return copy;
}

void clear(QImage &image)
{
    for (int y = 0; y < image.height(); y++) {
        QRgb *line = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < image.width(); x++) {
            if (!qAlpha(line[x])) line[x] = 0;
        }
    }
}

}
