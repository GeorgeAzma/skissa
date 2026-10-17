/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioMakeBrush.h"

#include <QFileInfo>
#include <QImage>
#include <QPointer>
#include <QTimer>
#include <QVector>

#include <klocalizedstring.h>

#include <KisBrushModel.h>
#include <KisDocument.h>
#include <KisGlobalResourcesInterface.h>
#include <KisMainWindow.h>
#include <KisResourceItemChooser.h>
#include <KisResourceModel.h>
#include <KisResourceTypes.h>
#include <KisTagFilterResourceProxyModel.h>
#include <KisTagResourceModel.h>
#include <KisViewManager.h>
#include <KoColorSpaceRegistry.h>
#include <kis_canvas_resource_provider.h>
#include <kis_config.h>
#include <kis_gbr_brush.h>
#include <kis_image.h>
#include <kis_paint_device.h>
#include <kis_paintop_box.h>
#include <kis_paintop_option.h>
#include <kis_paintop_preset.h>
#include <kis_paintop_settings.h>
#include <kis_predefined_brush_factory.h>
#include <kis_selection.h>
#include <widgets/kis_preset_chooser.h>

#include "KisStudioBrushEditor.h"
#include <KoToolManager.h>

namespace {

/// A tip larger than this is brought down to it: a huge picture tip paints slowly
constexpr int largestTip = 1024;

int channelDistance(QRgb a, QRgb b)
{
    return qMax(qAbs(qRed(a) - qRed(b)), qMax(qAbs(qGreen(a) - qGreen(b)), qAbs(qBlue(a) - qBlue(b))));
}

/**
 * The even color the selected pixels sit on, if they do: the color most of the selection's edge has (the paper
 * under a drawing), or none (0) when the edge is busy
 */
QRgb paperOf(const QImage &picture, const QVector<quint8> &mask)
{
    const int w = picture.width();
    const int h = picture.height();
    auto selected = [&](int x, int y) { return x >= 0 && y >= 0 && x < w && y < h && mask[y * w + x] > 127; };
    QVector<QRgb> edge;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            if (!selected(x, y)) continue;
            if (selected(x - 1, y) && selected(x + 1, y) && selected(x, y - 1) && selected(x, y + 1)) continue;
            const QRgb c = picture.pixel(x, y);
            if (qAlpha(c) > 200) edge << c;
        }
    }
    if (edge.size() < 16) return 0;
    long r = 0, g = 0, b = 0;
    for (QRgb c : qAsConst(edge)) {
        r += qRed(c);
        g += qGreen(c);
        b += qBlue(c);
    }
    const QRgb mean = qRgb(r / edge.size(), g / edge.size(), b / edge.size());
    int alike = 0;
    for (QRgb c : qAsConst(edge)) alike += channelDistance(c, mean) <= 24;
    return alike * 10 >= edge.size() * 7 ? (mean | 0xff000000) : 0;
}

/// \p picture without \p paper: each pixel as much of its own color as it takes over the paper (GIMP's color to alpha)
void removePaper(QImage &picture, QRgb paper)
{
    const int channels[3] = {qRed(paper), qGreen(paper), qBlue(paper)};
    for (int y = 0; y < picture.height(); y++) {
        QRgb *line = reinterpret_cast<QRgb *>(picture.scanLine(y));
        for (int x = 0; x < picture.width(); x++) {
            const QRgb c = line[x];
            const int own[3] = {qRed(c), qGreen(c), qBlue(c)};
            qreal alpha = 0;
            for (int i = 0; i < 3; i++) {
                const qreal room = own[i] > channels[i] ? 255 - channels[i] : channels[i];
                if (room > 0) alpha = qMax(alpha, qAbs(own[i] - channels[i]) / room);
            }
            // what is barely off the paper is the paper's grain, not paint
            alpha = alpha < 0.03 ? 0 : alpha;
            int out[3];
            for (int i = 0; i < 3; i++) out[i] = alpha > 0 ? qBound(0, qRound((own[i] - channels[i]) / alpha + channels[i]), 255) : own[i];
            line[x] = qRgba(out[0], out[1], out[2], qRound(qAlpha(c) * alpha));
        }
    }
}

/// Whether \p picture is gray: then it is a shape, painted in the brush's color
bool isGray(const QImage &picture)
{
    long chroma = 0;
    long count = 0;
    for (int y = 0; y < picture.height(); y++) {
        const QRgb *line = reinterpret_cast<const QRgb *>(picture.constScanLine(y));
        for (int x = 0; x < picture.width(); x++) {
            if (qAlpha(line[x]) < 128) continue;
            chroma += qMax(qRed(line[x]), qMax(qGreen(line[x]), qBlue(line[x]))) - qMin(qRed(line[x]), qMin(qGreen(line[x]), qBlue(line[x])));
            count++;
        }
    }
    return count == 0 || chroma / count < 20;
}

/// The brush panel's tag, when it is one the user made (not All or Untagged)
KisTagSP userTagShown(KisViewManager *view)
{
    if (!view->mainWindow()) return KisTagSP();
    Q_FOREACH (KisPresetChooser *chooser, view->mainWindow()->findChildren<KisPresetChooser *>()) {
        if (!chooser->isVisible() || !chooser->itemChooser() || !chooser->itemChooser()->tagFilterModel()) continue;
        KisTagSP tag = chooser->itemChooser()->tagFilterModel()->currentTagFilter();
        if (tag && tag->valid() && tag->id() >= 0) return tag;
    }
    return KisTagSP();
}

QString uniqueName(const QString &base)
{
    KisResourceModel presets(ResourceType::PaintOpPresets);
    QString name = base;
    for (int n = 2; !presets.resourcesForName(name).isEmpty(); n++) name = QStringLiteral("%1 %2").arg(base).arg(n);
    return name;
}

QString fileNameOf(const QString &name)
{
    QString file = name;
    file.replace(QLatin1Char(' '), QLatin1Char('_')).replace(QLatin1Char('.'), QLatin1Char('_')).replace(QLatin1Char('/'), QLatin1Char('_'));
    return file;
}

} // namespace

void KisStudioMakeBrush::fromSelection(KisViewManager *view)
{
    KisImageSP image = view ? KisImageSP(view->image()) : KisImageSP();
    KisSelectionSP selection = view ? view->selection() : KisSelectionSP();
    const QRect rect = selection ? selection->selectedExactRect() & (image ? image->bounds() : QRect()) : QRect();
    if (!image || rect.isEmpty()) {
        if (view) view->showFloatingMessage(i18nc("@info making a brush needs a selection", "Select what the brush is made of first"), QIcon());
        return;
    }

    // what is seen there, as the selection takes it
    image->barrierLock();
    KisPaintDeviceSP seen = new KisPaintDevice(*image->projection());
    image->unlock();
    QImage picture = seen->convertToQImage(KoColorSpaceRegistry::instance()->rgb8()->profile(), rect.x(), rect.y(), rect.width(), rect.height())
                         .convertToFormat(QImage::Format_ARGB32);
    QVector<quint8> mask(rect.width() * rect.height());
    selection->projection()->readBytes(mask.data(), rect);
    for (int y = 0; y < picture.height(); y++) {
        QRgb *line = reinterpret_cast<QRgb *>(picture.scanLine(y));
        for (int x = 0; x < picture.width(); x++) {
            const QRgb c = line[x];
            line[x] = qRgba(qRed(c), qGreen(c), qBlue(c), qAlpha(c) * mask[y * rect.width() + x] / 255);
        }
    }
    const QRgb paper = paperOf(picture, mask);
    if (qAlpha(paper)) removePaper(picture, paper);

    // only the paint
    QRect paint;
    for (int y = 0; y < picture.height(); y++) {
        const QRgb *line = reinterpret_cast<const QRgb *>(picture.constScanLine(y));
        for (int x = 0; x < picture.width(); x++) {
            if (qAlpha(line[x]) > 8) paint |= QRect(x, y, 1, 1);
        }
    }
    if (paint.isEmpty()) {
        view->showFloatingMessage(i18nc("@info", "There is nothing in the selection to make a brush of"), QIcon());
        return;
    }
    picture = picture.copy(paint);
    if (qMax(picture.width(), picture.height()) > largestTip) {
        picture = picture.scaled(largestTip, largestTip, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }

    const QFileInfo file(view->document() ? view->document()->path() : QString());
    const QString name = uniqueName(file.completeBaseName().isEmpty() ? i18nc("a brush made from part of the image", "My brush")
                                                                       : i18nc("a brush made from part of the image, %1 the image's name", "%1 brush", file.completeBaseName()));

    // the tip: gray is a shape painted in the brush's color, colors stay as they are
    // made from a device, as Krita's own brush maker does: that marks it a picture, whose colors count
    KisPaintDeviceSP pixels = new KisPaintDevice(KoColorSpaceRegistry::instance()->rgb8());
    pixels->convertFromQImage(picture, nullptr);
    KisGbrBrushSP tip(new KisGbrBrush(pixels, 0, 0, picture.width(), picture.height()));
    tip->setName(name);
    if (isGray(picture)) {
        tip->makeMaskImage(false);
    } else {
        tip->setBrushApplication(IMAGESTAMP);
    }
    tip->setSpacing(0.15);
    tip->setFilename(fileNameOf(name) + QStringLiteral(".gbr"));
    tip->setValid(true);
    KisResourceModel(ResourceType::Brushes).addResourceDeduplicateFileName(tip, QString());

    // a pixel brush of it: the one in use if it is one, else Krita's plain one
    KisCanvasResourceProvider *resources = view->canvasResourceProvider();
    KisPaintOpPresetSP base = resources ? resources->currentPreset() : KisPaintOpPresetSP();
    if (!base || base->paintOp().id() != QLatin1String("paintbrush")) {
        base = KisPaintOpPresetSP(new KisPaintOpPreset(QStringLiteral(":/presets/paintbrush.kpp")));
        if (!base->load(KisGlobalResourcesInterface::instance())) return;
    }
    KisPaintOpPresetSP preset = base->clone().dynamicCast<KisPaintOpPreset>();
    if (!preset) return;
    preset->setResourceId(-1);
    KisPaintOpSettingsSP settings = preset->settings();
    KisBrushModel::BrushData data = KisBrushModel::BrushData::read(settings.data(), preset->resourcesInterface()).value_or(KisBrushModel::BrushData());
    KisPredefinedBrushFactory::loadFromBrushResource(data.common, data.predefinedBrush, tip);
    // no larger than a few hundred pixels to start with: the Size row makes it any size
    data.predefinedBrush.scale = qMin(1.0, 300.0 / qMax(picture.width(), picture.height()));
    data.predefinedBrush.application = tip->brushApplication();
    data.type = KisBrushModel::Predefined;
    data.write(settings.data());
    // the clone's caches are its base's tip's
    preset->setResourceCacheInterface(KoResourceCacheInterfaceSP());
    preset->setName(name);
    preset->setFilename(fileNameOf(name) + preset->defaultFileExtension());
    preset->setImage(picture.scaled(200, 200, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    preset->setValid(true);
    preset->setStorageLocation(QString());
    preset->setDirty(false);
    if (!KisResourceModel(ResourceType::PaintOpPresets).addResourceDeduplicateFileName(preset, QString())) return;
    if (KisTagSP tag = userTagShown(view)) {
        KisTagResourceModel(ResourceType::PaintOpPresets).tagResources(tag, {preset->resourceId()});
    }

    // in use as if picked in the brush panel (the stored one, not this copy), its tip open in the quick editor
    const KisPaintOpPresetSP stored = KisResourceModel(ResourceType::PaintOpPresets).resourceForId(preset->resourceId()).dynamicCast<KisPaintOpPreset>();
    // painting with it: the brush tool first, or switching to it later brings back the brush it had
    KoToolManager::instance()->switchToolRequested(QStringLiteral("KritaShape/KisToolBrush"));
    if (resources) resources->setPaintOpPreset(stored ? stored : preset);
    KisConfig(false).writeEntry(QStringLiteral("StudioBrushEditorPart"), int(KisPaintOpOption::GENERAL));
    QPointer<KisViewManager> self = view;
    QTimer::singleShot(0, view, [self]() {
        if (!self) return;
        if (KisMainWindow *window = self->mainWindow()) {
            if (KisStudioBrushEditor::isOpen(window)) KisStudioBrushEditor::toggle(window, nullptr, Qt::BottomEdge);
        }
        if (KisPaintopBox *box = self->paintOpBox()) Q_EMIT box->sigQuickEditorRequested();
    });
}
