/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioColorSpace.h"

#include <QComboBox>
#include <QPointer>
#include <QPushButton>
#include <QVBoxLayout>

#include <KisModernStyle.h>
#include <KoColorModelStandardIds.h>
#include <KoColorProfile.h>
#include <KoColorSpace.h>
#include <KoColorSpaceRegistry.h>
#include <klocalizedstring.h>

#include "KisMainWindow.h"
#include "KisViewManager.h"
#include "kis_config.h"
#include "kis_image.h"
#include "kis_layer.h"
#include "widgets/KisStudioChips.h"
#include "widgets/KisStudioPopup.h"

namespace
{
struct Choice {
    KoID id;
    QString name;
    QString toolTip;
};

QVector<Choice> models()
{
    return {{RGBAColorModelID, i18nc("color model", "RGB"), QString()},
            {GrayAColorModelID, i18nc("color model", "Gray"), i18nc("@info:tooltip color model", "Shades of gray only")},
            {CMYKAColorModelID, i18nc("color model", "CMYK"), i18nc("@info:tooltip color model", "Inks, for print")},
            {LABAColorModelID, i18nc("color model", "Lab"), i18nc("@info:tooltip color model", "Lightness apart from color")}};
}

QVector<Choice> depths()
{
    return {{Integer8BitsColorDepthID, i18nc("color depth", "8-bit"), i18nc("@info:tooltip color depth", "Smallest files")},
            {Integer16BitsColorDepthID, i18nc("color depth", "16-bit"), i18nc("@info:tooltip color depth", "Gradients without bands")},
            {Float16BitsColorDepthID, i18nc("color depth", "16-bit float"), i18nc("@info:tooltip color depth", "Light brighter than white")},
            {Float32BitsColorDepthID, i18nc("color depth", "32-bit float"), i18nc("@info:tooltip color depth", "Light brighter than white, most exact")}};
}

int indexOf(const QVector<Choice> &choices, const QString &id)
{
    for (int i = 0; i < choices.size(); i++) {
        if (choices[i].id.id() == id) return i;
    }
    return 0;
}
}

void KisStudioColorSpace::open(KisViewManager *viewManager, KisLayerSP layer)
{
    KisImageSP image = viewManager ? viewManager->image() : nullptr;
    KisMainWindow *window = viewManager ? viewManager->mainWindow() : nullptr;
    if (!image || !window || !viewManager->blockUntilOperationsFinished(image)) return;

    const KoColorSpace *current = layer ? layer->colorSpace() : image->colorSpace();
    KoColorSpaceRegistry *registry = KoColorSpaceRegistry::instance();
    QPointer<KisViewManager> view = viewManager;

    auto *popup = new KisStudioPopup(window);
    popup->setAttribute(Qt::WA_DeleteOnClose);
    auto *layout = new QVBoxLayout(popup);
    popup->fitLayout(layout, KisModernStyle::space3);
    layout->setSpacing(KisModernStyle::space2);

    auto *modelChips = new KisStudioChips(popup);
    modelChips->setTabs(true);
    auto *depthChips = new KisStudioChips(popup);
    depthChips->setTabs(true);
    auto *profiles = new QComboBox(popup);
    profiles->setMinimumWidth(popup->fontMetrics().averageCharWidth() * 36);
    layout->addWidget(modelChips);
    layout->addWidget(depthChips);
    layout->addWidget(profiles);

    auto *convert = new QPushButton(i18nc("@action:button convert the image's colors", "Convert"), popup);
    convert->setDefault(true);
    layout->addWidget(convert, 0, Qt::AlignRight);

    struct State {
        int model;
        int depth;
    };
    auto state = std::make_shared<State>(State{indexOf(models(), current->colorModelId().id()), indexOf(depths(), current->colorDepthId().id())});

    // the depths this kind of color has, and its profiles with the one in use (or the usual one) chosen
    std::function<void()> refresh = [=]() {
        const QVector<Choice> allModels = models();
        const QVector<Choice> allDepths = depths();
        const QList<KoID> offered = registry->colorDepthList(allModels[state->model].id, KoColorSpaceRegistry::OnlyUserVisible);
        if (!offered.contains(allDepths[state->depth].id)) state->depth = 0;

        QVector<KisStudioChips::Chip> chips;
        for (int i = 0; i < allModels.size(); i++) {
            KisStudioChips::Chip chip;
            chip.text = allModels[i].name;
            chip.toolTip = allModels[i].toolTip;
            chip.on = i == state->model;
            chips << chip;
        }
        modelChips->setChips(chips);
        chips.clear();
        for (int i = 0; i < allDepths.size(); i++) {
            if (!offered.contains(allDepths[i].id)) continue;
            KisStudioChips::Chip chip;
            chip.text = allDepths[i].name;
            chip.toolTip = allDepths[i].toolTip;
            chip.on = i == state->depth;
            chip.dot = allDepths[i].id == current->colorDepthId() && allModels[state->model].id == current->colorModelId();
            chips << chip;
        }
        depthChips->setChips(chips);
        depthChips->setProperty("depthIds", [&]() {
            QStringList ids;
            for (const Choice &depth : allDepths) {
                if (offered.contains(depth.id)) ids << depth.id.id();
            }
            return ids;
        }());

        const QString csId = registry->colorSpaceId(allModels[state->model].id, allDepths[state->depth].id);
        const QString wanted = current->profile() ? current->profile()->name() : QString();
        QStringList names;
        for (const KoColorProfile *profile : registry->profilesFor(csId)) names << profile->name();
        names.sort(Qt::CaseInsensitive);
        QSignalBlocker blocker(profiles);
        profiles->clear();
        profiles->addItems(names);
        const int keep = names.indexOf(wanted);
        profiles->setCurrentIndex(keep >= 0 ? keep : qMax(0, names.indexOf(registry->defaultProfileForColorSpace(csId))));
    };
    refresh();

    QObject::connect(modelChips, &KisStudioChips::clicked, popup, [=](int index) {
        state->model = index;
        refresh();
    });
    QObject::connect(depthChips, &KisStudioChips::clicked, popup, [=](int index) {
        const QStringList ids = depthChips->property("depthIds").toStringList();
        if (index < 0 || index >= ids.size()) return;
        state->depth = indexOf(depths(), ids[index]);
        refresh();
    });

    QObject::connect(convert, &QPushButton::clicked, popup, [=]() mutable {
        const KoColorSpace *target = registry->colorSpace(models()[state->model].id.id(), depths()[state->depth].id.id(), profiles->currentText());
        popup->close();
        if (!view || !target || *target == *current) return;
        KoColorConversionTransformation::ConversionFlags flags =
            KoColorConversionTransformation::HighQuality | KoColorConversionTransformation::BlackpointCompensation;
        if (!KisConfig(true).allowLCMSOptimization()) flags |= KoColorConversionTransformation::NoOptimization;
        if (layer) image->convertLayerColorSpace(layer, target, KoColorConversionTransformation::IntentPerceptual, flags);
        else image->convertImageColorSpace(target, KoColorConversionTransformation::IntentPerceptual, flags);
    });

    popup->adjustSize();
    const QRect frame = window->geometry();
    popup->popupAt(frame.center() - QPoint(popup->width() / 2, popup->height() / 2));
}
