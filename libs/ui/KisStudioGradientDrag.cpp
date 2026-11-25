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
#include <kis_pixel_selection.h>
#include <QPointer>

#include "KisColorDrop.h"
#include "KisRedoLast.h"
#include "KisStudioCanvasHandles.h"
#include "canvas/kis_canvas2.h"
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

namespace
{
/// A gradient dropped on the picture, drawn again while its handles move, until something else is done
class DroppedGradient : public QObject
{
public:
    QPointer<KisViewManager> viewManager;
    KisNodeSP active;
    KisSelectionSP area;
    QRect bounds;
    QPoint seed;
    KoAbstractGradientSP gradient;
    QPointF from;
    QPointF to;
    KisRedoLast redo;
    QPointer<KisStudioCanvasHandles> handles;

    void paint()
    {
        KisImageSP image = viewManager ? viewManager->image() : nullptr;
        if (!image) return;
        const KUndo2MagicString name = kundo2_i18n("Gradient");
        redo.made(viewManager->canvasBase(), name, [this]() { paint(); });
        KisProcessingApplicator applicator(image, nullptr, KisProcessingApplicator::NONE, KisImageSignalVector(), name);
        // the layer a fill there would go on: a Colors layer under line art, else the current one, or a layer of its own
        KisColorDrop::FillTarget target = KisColorDrop::fillTarget(image, active, seed, area);
        if (!target.filled() || !target.filled()->paintDevice() || !target.filled()->isEditable()) {
            target = KisColorDrop::FillTarget();
            target.newLayer = new KisPaintLayer(image, i18nc("a layer's name", "Gradient"), OPACITY_OPAQUE_U8);
            target.parent = active && active->parent() ? active->parent() : image->root();
            target.above = active && active->parent() ? active : target.parent->lastChild();
        }
        if (target.newLayer) {
            applicator.applyCommand(new KisImageLayerAddCommand(image, target.newLayer, target.parent, target.above), KisStrokeJobData::SEQUENTIAL,
                                    KisStrokeJobData::EXCLUSIVE);
        }
        KisNodeSP node = target.filled();
        KoAbstractGradientSP copy = gradient->clone().dynamicCast<KoAbstractGradient>();
        applicator.applyCommand(new KisCommandUtils::LambdaCommand([node, selection = area, copy, rect = bounds, a = from, b = to]() -> KUndo2Command * {
                                    KisPaintDeviceSP device = node->paintDevice();
                                    KisTransaction transaction(device);
                                    KisGradientPainter painter(device, selection);
                                    painter.setGradient(copy);
                                    painter.setGradientShape(KisGradientPainter::GradientShapeLinear);
                                    painter.paintGradient(a, b, KisGradientPainter::GradientRepeatNone, 0.0, false, rect.x(), rect.y(), rect.width(),
                                                          rect.height());
                                    device->setDirty(rect);
                                    return transaction.endAndTake();
                                }),
                                KisStrokeJobData::SEQUENTIAL, KisStrokeJobData::EXCLUSIVE);
        applicator.end();
    }

    void showHandles()
    {
        handles = KisStudioCanvasHandles::start(viewManager->canvasBase());
        if (!handles) return;
        handles->moved = [this](int index, const QPointF &at, Qt::KeyboardModifiers modifiers) {
            auto straight = [](const QPointF &point, const QPointF &other) {
                const QPointF d = point - other;
                return qAbs(d.x()) > qAbs(d.y()) ? QPointF(point.x(), other.y()) : QPointF(other.x(), point.y());
            };
            const bool shift = modifiers & Qt::ShiftModifier;
            if (index == 0) from = shift ? straight(at, to) : at;
            else if (index == 1) to = shift ? straight(at, from) : at;
            else {
                const QPointF move = at - (from + to) / 2;
                from += move;
                to += move;
            }
            updateHandles();
            redo.settingChanged();
        };
        handles->drawUnder = [this](QPainter &painter, const QTransform &toWidget) {
            const QLineF line(toWidget.map(from), toWidget.map(to));
            painter.setPen(QPen(QColor(0, 0, 0, 110), 3));
            painter.drawLine(line);
            painter.setPen(QPen(QColor(255, 255, 255, 220), 1.25));
            painter.drawLine(line);
        };
        handles->alive = [this]() { return redo.isLive(); };
        handles->ended = [this]() { deleteLater(); };
        updateHandles();
    }

    void updateHandles()
    {
        if (!handles) return;
        KoColor color;
        gradient->colorAt(color, 0.0);
        QColor first = color.toQColor();
        gradient->colorAt(color, 1.0);
        QColor last = color.toQColor();
        first.setAlpha(255);
        last.setAlpha(255);
        handles->setHandles({{from, first}, {to, last}, {(from + to) / 2, QColor()}});
    }
};
}

void KisStudioGradientDrag::dropOnCanvas(KisViewManager *viewManager, KoAbstractGradientSP gradient, const QPoint &seed, KisPixelSelectionSP shown)
{
    KisImageSP image = viewManager ? viewManager->image() : nullptr;
    if (!image || !gradient) return;
    // the area a color would fill there; else the selection, else the whole picture
    KisSelectionSP area;
    QRect bounds;
    if (shown && !shown->selectedExactRect().isEmpty()) {
        area = new KisSelection();
        area->pixelSelection()->applySelection(shown, SELECTION_REPLACE);
        bounds = shown->selectedExactRect();
    } else if (KisSelectionSP selection = viewManager->selection()) {
        area = selection;
        bounds = selection->selectedExactRect();
    } else {
        bounds = image->bounds();
    }
    if (bounds.isEmpty()) return;

    auto *dropped = new DroppedGradient;
    dropped->viewManager = viewManager;
    dropped->active = viewManager->activeNode();
    dropped->area = area;
    dropped->bounds = bounds;
    dropped->seed = seed;
    dropped->gradient = gradient->clone().dynamicCast<KoAbstractGradient>();
    // light falls from above: the first color at the top
    dropped->from = QPointF(bounds.center().x(), bounds.top());
    dropped->to = QPointF(bounds.center().x(), bounds.bottom() + 1);
    dropped->paint();
    dropped->showHandles();
    if (!dropped->handles) dropped->deleteLater();
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
