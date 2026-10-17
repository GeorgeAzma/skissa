/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioMixing.h"

#include <QPainter>
#include <QPainterPath>
#include <QPointer>

#include <klocalizedstring.h>

#include <KisModernStyle.h>
#include <KoCanvasResourceProvider.h>
#include <brushengine/kis_paintop_preset.h>
#include <brushengine/kis_paintop_settings.h>
#include <compositeops/KoCompositeOpMix.h>

#include "KisMainWindow.h"
#include "KisViewManager.h"
#include "kis_canvas_resource_provider.h"
#include "widgets/KisStudioChips.h"

namespace
{

float toLinear(float v)
{
    return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
}

int toDisplay(float v)
{
    v = qBound(0.0f, v, 1.0f);
    v = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1 / 2.4f) - 0.055f;
    return qRound(v * 255);
}

/// Blue running into yellow as \p mode mixes them
std::function<void(QPainter &, const QRectF &)> picture(int mode)
{
    return [mode](QPainter &painter, const QRectF &rect) {
        const QColor blueColor(0x00, 0x21, 0x85);
        const QColor yellowColor(0xfc, 0xd2, 0x00);
        const float blue[3] = {toLinear(blueColor.redF()), toLinear(blueColor.greenF()), toLinear(blueColor.blueF())};
        const float yellow[3] = {toLinear(yellowColor.redF()), toLinear(yellowColor.greenF()), toLinear(yellowColor.blueF())};
        const QRectF band = rect.adjusted(KisModernStyle::space1, rect.height() * 0.2, -KisModernStyle::space1, -rect.height() * 0.2);
        QLinearGradient gradient(band.topLeft(), band.topRight());
        const int steps = 24;
        for (int i = 0; i <= steps; i++) {
            const float t = float(i) / steps;
            if (mode == KoMixing::Normal) {
                // as Krita mixes in an sRGB image, where most painting happens
                gradient.setColorAt(t, QColor::fromRgbF(blueColor.redF() + (yellowColor.redF() - blueColor.redF()) * t,
                                                        blueColor.greenF() + (yellowColor.greenF() - blueColor.greenF()) * t,
                                                        blueColor.blueF() + (yellowColor.blueF() - blueColor.blueF()) * t));
                continue;
            }
            float out[3];
            KoMixing::mix(mode, blue, yellow, t, out);
            gradient.setColorAt(t, QColor(toDisplay(out[0]), toDisplay(out[1]), toDisplay(out[2])));
        }
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.fillPath(KisModernStyle::squircle(band, KisModernStyle::cornerRadius), gradient);
    };
}

} // namespace

QWidget *KisStudioMixing::chooser(KisMainWindow *mainWindow, QWidget *parent)
{
    KisViewManager *view = mainWindow ? mainWindow->viewManager() : nullptr;
    KisCanvasResourceProvider *resources = view ? view->canvasResourceProvider() : nullptr;
    if (!resources) return nullptr;
    auto *modes = new KisStudioChips(parent);
    modes->setTileColumns(3, 48);
    auto refresh = [modes, resources]() {
        const KisPaintOpPresetSP preset = resources->currentPreset();
        const int mode = preset ? preset->settings()->getInt(KoMixing::settingName(), KoMixing::Normal) : KoMixing::Normal;
        KisStudioChips::Chip normal;
        normal.text = i18nc("how paint mixes: in the image's own red, green and blue, as Krita does", "RGB");
        normal.toolTip = i18nc("@info:tooltip", "As Krita mixes, in the image's own values");
        KisStudioChips::Chip even;
        even.text = i18nc("how paint mixes: in the OKLab color space", "OKLab");
        even.toolTip = i18nc("@info:tooltip", "Even steps from color to color");
        KisStudioChips::Chip pigment;
        pigment.text = i18nc("how paint mixes: as real pigments do", "Pigment");
        pigment.toolTip = i18nc("@info:tooltip", "Like paint: blue and yellow make green");
        QVector<KisStudioChips::Chip> chips {normal, even, pigment};
        for (int i = 0; i < chips.size(); i++) {
            chips[i].picture = picture(i);
            chips[i].on = i == mode;
        }
        modes->setChips(chips);
        modes->setCurrent(-1);
    };
    refresh();
    QObject::connect(modes, &KisStudioChips::clicked, modes, [resources, refresh](int index) {
        const KisPaintOpPresetSP preset = resources->currentPreset();
        if (!preset) return;
        preset->settings()->setProperty(KoMixing::settingName(), index);
        refresh();
    });
    QObject::connect(resources->resourceManager(), &KoCanvasResourceProvider::canvasResourceChanged, modes, [refresh](int key) {
        if (key == KoCanvasResource::CurrentPaintOpPreset) refresh();
    });
    return modes;
}
