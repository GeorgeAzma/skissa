/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioGradientDrag.h"

#include <QApplication>
#include <QDomDocument>
#include <QDrag>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>

#include <KisGlobalResourcesInterface.h>
#include <KisModernStyle.h>
#include <KisViewManager.h>
#include <KoSegmentGradient.h>
#include <KoStopGradient.h>
#include <commands/kis_image_layer_add_command.h>
#include <filter/kis_filter.h>
#include <filter/kis_filter_configuration.h>
#include <filter/kis_filter_registry.h>
#include <kis_command_utils.h>
#include <kis_filter_mask.h>
#include <kis_gradient_painter.h>
#include <kis_image.h>
#include <kis_layer.h>
#include <kis_paint_layer.h>
#include <kis_processing_applicator.h>
#include <kis_selection.h>
#include <kis_transaction.h>
#include <klocalizedstring.h>

namespace
{
const QString mimeType = QStringLiteral("application/x-skissa-gradient");
}

bool KisStudioGradientDrag::carries(const QMimeData *data)
{
    return data && data->hasFormat(mimeType);
}

QString KisStudioGradientDrag::xmlOf(KoAbstractGradientSP gradient)
{
    if (!gradient) return QString();
    QDomDocument document;
    QDomElement element = document.createElement(QStringLiteral("gradient"));
    element.setAttribute(QStringLiteral("name"), gradient->name());
    element.setAttribute(QStringLiteral("md5sum"), gradient->md5Sum());
    if (KoStopGradientSP stops = gradient.dynamicCast<KoStopGradient>()) {
        stops->toXML(document, element);
    } else if (KoSegmentGradientSP segments = gradient.dynamicCast<KoSegmentGradient>()) {
        segments->toXML(document, element);
    } else {
        return QString();
    }
    document.appendChild(element);
    return document.toString();
}

KoAbstractGradientSP KisStudioGradientDrag::gradientOf(const QMimeData *data)
{
    if (!carries(data)) return nullptr;
    QDomDocument document;
    if (!document.setContent(QString::fromUtf8(data->data(mimeType)))) return nullptr;
    const QDomElement element = document.firstChildElement();
    const QString type = element.attribute(QStringLiteral("type"));
    KoAbstractGradientSP gradient;
    if (type == QLatin1String("stop")) {
        gradient = KoStopGradient::fromXML(element).clone().dynamicCast<KoAbstractGradient>();
    } else if (type == QLatin1String("segment")) {
        gradient = KoSegmentGradient::fromXML(element).clone().dynamicCast<KoAbstractGradient>();
    }
    if (!gradient) return nullptr;
    gradient->setName(element.attribute(QStringLiteral("name")));
    gradient->setValid(true);
    return gradient;
}

void KisStudioGradientDrag::start(QWidget *source, KoAbstractGradientSP gradient)
{
    const QString xml = xmlOf(gradient);
    if (!source || xml.isEmpty()) return;
    auto *data = new QMimeData;
    data->setData(mimeType, xml.toUtf8());
    // a swatch may be rebuilt while the drag runs: the drag isn't its child
    QDrag *drag = new QDrag(qApp);
    drag->setMimeData(data);
    const qreal ratio = source->devicePixelRatioF();
    const QSize size(48, 20);
    QPixmap pixmap(size * ratio);
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(Qt::transparent);
    {
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QRectF rect = QRectF(QPointF(0, 0), size).adjusted(2, 2, -2, -2);
        const QPainterPath shape = KisModernStyle::squircle(rect, KisModernStyle::cornerRadius);
        QScopedPointer<QGradient> fill(gradient->toQGradient());
        if (fill) {
            QLinearGradient across(rect.topLeft(), rect.topRight());
            across.setStops(fill->stops());
            painter.fillPath(shape, across);
        }
        // a dark and a light edge, so it shows over any picture
        painter.setPen(QPen(QColor(0, 0, 0, 110), 3));
        painter.drawPath(shape);
        painter.setPen(QPen(QColor(255, 255, 255, 220), 1));
        painter.drawPath(shape);
    }
    drag->setPixmap(pixmap);
    drag->setHotSpot(QPoint(size.width() / 2, size.height() / 2));
    drag->exec(Qt::CopyAction);
}

void KisStudioGradientDrag::paintOnCanvas(KisViewManager *viewManager, KoAbstractGradientSP gradient)
{
    KisImageSP image = viewManager ? viewManager->image() : nullptr;
    if (!image || !gradient) return;
    KisSelectionSP selection = viewManager->selection();
    KisNodeSP active = viewManager->activeNode();
    const QRect area = selection ? selection->selectedExactRect() : image->bounds();
    if (area.isEmpty()) return;

    KisProcessingApplicator applicator(image, nullptr, KisProcessingApplicator::NONE, KisImageSignalVector(), kundo2_i18n("Gradient"));
    // in a selection on the layer it is on; else on a layer of its own above the current one, for the whole image
    KisNodeSP target = selection && active && qobject_cast<KisPaintLayer *>(active.data()) && active->isEditable() ? active : nullptr;
    if (!target) {
        KisPaintLayerSP layer = new KisPaintLayer(image, i18nc("a layer's name", "Gradient"), OPACITY_OPAQUE_U8);
        KisNodeSP parent = active && active->parent() ? active->parent() : image->root();
        KisNodeSP above = active && active->parent() ? active : parent->lastChild();
        applicator.applyCommand(new KisImageLayerAddCommand(image, layer, parent, above), KisStrokeJobData::SEQUENTIAL,
                                KisStrokeJobData::EXCLUSIVE);
        target = layer;
    }
    KoAbstractGradientSP copy = gradient->clone().dynamicCast<KoAbstractGradient>();
    applicator.applyCommand(new KisCommandUtils::LambdaCommand([target, selection, copy, area]() -> KUndo2Command * {
                                KisPaintDeviceSP device = target->paintDevice();
                                KisTransaction transaction(device);
                                KisGradientPainter painter(device, selection);
                                painter.setGradient(copy);
                                painter.setGradientShape(KisGradientPainter::GradientShapeLinear);
                                painter.paintGradient(QPointF(area.left(), area.center().y()), QPointF(area.right() + 1, area.center().y()),
                                                      KisGradientPainter::GradientRepeatNone, 0.0, false, area.x(), area.y(), area.width(),
                                                      area.height());
                                device->setDirty(area);
                                return transaction.endAndTake();
                            }),
                            KisStrokeJobData::SEQUENTIAL, KisStrokeJobData::EXCLUSIVE);
    applicator.end();
}

void KisStudioGradientDrag::mapLayer(KisImageSP image, KisNodeSP node, KoAbstractGradientSP gradient)
{
    KisLayerSP layer = qobject_cast<KisLayer *>(node.data());
    KisFilterSP filter = KisFilterRegistry::instance()->value(QStringLiteral("gradientmap"));
    if (!image || !layer || !filter || !gradient) return;
    KisFilterConfigurationSP config = filter->defaultConfiguration(KisGlobalResourcesInterface::instance());
    config->setProperty(QStringLiteral("gradientXML"), xmlOf(gradient));
    KisFilterMaskSP mask = new KisFilterMask(image, i18nc("a layer's name", "Gradient map"));
    mask->setFilter(config->cloneWithResourcesSnapshot());
    mask->initSelection(layer);
    image->undoAdapter()->addCommand(new KisImageLayerAddCommand(image, mask, layer, layer->lastChild()));
}
