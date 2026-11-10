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
    // tiles are shared until written: each tile's block is read, and only one holding hidden colors is written,
    // so only those are copied
    constexpr int tile = 64;
    KisPaintDeviceSP copy;
    for (int top = rect.top() - ((rect.top() % tile) + tile) % tile; top <= rect.bottom(); top += tile) {
        for (int left = rect.left() - ((rect.left() % tile) + tile) % tile; left <= rect.right(); left += tile) {
            const QRect block = QRect(left, top, tile, tile) & rect;
            if (block.isEmpty()) continue;
            bool found = false;
            KisSequentialConstIterator seek(device, block);
            while (!found && seek.nextPixel()) found = hides(seek.rawDataConst());
            if (!found) continue;
            if (!copy) copy = new KisPaintDevice(*device);
            KisSequentialIterator it(copy, block);
            while (it.nextPixel()) {
                if (hides(it.rawDataConst())) std::memcpy(it.rawData(), clear.data(), pixelSize);
            }
        }
    }
    return copy ? copy : device;
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
