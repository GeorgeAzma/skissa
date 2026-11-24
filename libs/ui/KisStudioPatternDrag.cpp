/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioPatternDrag.h"

#include <QAbstractButton>
#include <QApplication>
#include <QMouseEvent>
#include <QDrag>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>

#include <KisModernStyle.h>
#include <KisPart.h>
#include <KisMainWindow.h>
#include <KisViewManager.h>
#include <KoColorSpace.h>
#include <KoResourceServerProvider.h>
#include <commands/kis_image_layer_add_command.h>
#include <kis_canvas_resource_provider.h>
#include <kis_command_utils.h>
#include <kis_fill_painter.h>
#include <kis_image.h>
#include <kis_paint_device.h>
#include <kis_paint_layer.h>
#include <kis_painter.h>
#include <kis_processing_applicator.h>
#include <kis_selection.h>
#include <kis_transaction.h>
#include <klocalizedstring.h>

#include "KisPatternTint.h"

namespace
{

class DragOut : public QObject
{
public:
    DragOut(QWidget *widget, std::function<KoPatternSP()> pattern)
        : QObject(widget), m_widget(widget), m_pattern(std::move(pattern))
    {
        widget->installEventFilter(this);
    }

    bool eventFilter(QObject *, QEvent *event) override
    {
        switch (event->type()) {
        case QEvent::MouseButtonPress: {
            auto *mouse = static_cast<QMouseEvent *>(event);
            m_pressed = mouse->button() == Qt::LeftButton;
            m_from = mouse->pos();
            break;
        }
        case QEvent::MouseMove: {
            auto *mouse = static_cast<QMouseEvent *>(event);
            if (!m_pressed || !(mouse->buttons() & Qt::LeftButton)
                || (mouse->pos() - m_from).manhattanLength() < QApplication::startDragDistance()) {
                break;
            }
            m_pressed = false;
            if (auto *button = qobject_cast<QAbstractButton *>(m_widget)) button->setDown(false);
            KisStudioPatternDrag::start(m_widget, m_pattern());
            return true;
        }
        case QEvent::MouseButtonRelease:
            m_pressed = false;
            break;
        default:
            break;
        }
        return false;
    }

private:
    QWidget *m_widget;
    std::function<KoPatternSP()> m_pattern;
    QPoint m_from;
    bool m_pressed {false};
};

/// The pattern as a fill takes it: in the current color while Tint is on
KoPatternSP asPainted(KoPatternSP pattern)
{
    KisMainWindow *window = KisPart::instance()->currentMainwindow();
    KisViewManager *view = window ? window->viewManager() : nullptr;
    if (!view || !view->canvasResourceProvider()) return pattern;
    return KisPatternTint::applied(pattern, view->canvasResourceProvider()->fgColor());
}

/// Paints \p pattern over \p area of \p device, inside \p selection, leaving its transparency alone when \p keepAlpha
KUndo2Command *paintPattern(KisPaintDeviceSP device, KisSelectionSP selection, KoPatternSP pattern, const QRect &area, bool keepAlpha)
{
    KisPaintDeviceSP fill = new KisPaintDevice(device->colorSpace());
    {
        KisFillPainter filler(fill);
        filler.fillRect(area, pattern);
    }
    KisTransaction transaction(device);
    KisPainter painter(device, selection);
    if (keepAlpha) painter.setChannelFlags(device->colorSpace()->channelFlags(true, false));
    painter.bitBlt(area.topLeft(), fill, area);
    device->setDirty(area);
    return transaction.endAndTake();
}
}

QString KisStudioPatternDrag::mimeType()
{
    return QStringLiteral("application/x-skissa-pattern");
}

bool KisStudioPatternDrag::carries(const QMimeData *data)
{
    return data && data->hasFormat(mimeType());
}

KoPatternSP KisStudioPatternDrag::patternOf(const QMimeData *data)
{
    if (!carries(data)) return nullptr;
    const QStringList parts = QString::fromUtf8(data->data(mimeType())).split(QLatin1Char('\n'));
    if (parts.size() < 3) return nullptr;
    return KoResourceServerProvider::instance()->patternServer()->resource(parts[0], parts[1], parts[2]);
}

void KisStudioPatternDrag::start(QWidget *source, KoPatternSP pattern)
{
    if (!source || !pattern || pattern->pattern().isNull()) return;
    auto *data = new QMimeData;
    data->setData(mimeType(), QStringList{pattern->md5Sum(), pattern->filename(), pattern->name()}.join(QLatin1Char('\n')).toUtf8());
    QDrag *drag = new QDrag(qApp);
    drag->setMimeData(data);
    const qreal ratio = source->devicePixelRatioF();
    const QSize size(36, 36);
    QPixmap pixmap(size * ratio);
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(Qt::transparent);
    {
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QRectF rect = QRectF(QPointF(0, 0), size).adjusted(2, 2, -2, -2);
        const QPainterPath shape = KisModernStyle::squircle(rect, KisModernStyle::cornerRadius);
        painter.fillPath(shape, QBrush(QPixmap::fromImage(asPainted(pattern)->pattern())));
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

void KisStudioPatternDrag::makeDraggable(QWidget *widget, std::function<KoPatternSP()> pattern)
{
    if (widget) new DragOut(widget, std::move(pattern));
}

void KisStudioPatternDrag::paintOnCanvas(KisViewManager *viewManager, KoPatternSP pattern)
{
    KisImageSP image = viewManager ? viewManager->image() : nullptr;
    if (!image || !pattern) return;
    KisSelectionSP selection = viewManager->selection();
    KisNodeSP active = viewManager->activeNode();
    const QRect area = selection ? selection->selectedExactRect() : image->bounds();
    if (area.isEmpty()) return;
    const KoPatternSP painted = asPainted(pattern);

    KisProcessingApplicator applicator(image, nullptr, KisProcessingApplicator::NONE, KisImageSignalVector(), kundo2_i18n("Pattern"));
    // in a selection on the layer it is on; else on a layer of its own above the current one, for the whole image
    KisNodeSP target = selection && active && qobject_cast<KisPaintLayer *>(active.data()) && active->isEditable() ? active : nullptr;
    if (!target) {
        KisPaintLayerSP layer = new KisPaintLayer(image, i18nc("a layer's name", "Pattern"), OPACITY_OPAQUE_U8);
        KisNodeSP parent = active && active->parent() ? active->parent() : image->root();
        KisNodeSP above = active && active->parent() ? active : parent->lastChild();
        applicator.applyCommand(new KisImageLayerAddCommand(image, layer, parent, above), KisStrokeJobData::SEQUENTIAL,
                                KisStrokeJobData::EXCLUSIVE);
        target = layer;
    }
    applicator.applyCommand(new KisCommandUtils::LambdaCommand([target, selection, painted, area]() {
                                return paintPattern(target->paintDevice(), selection, painted, area, false);
                            }),
                            KisStrokeJobData::SEQUENTIAL, KisStrokeJobData::EXCLUSIVE);
    applicator.end();
}

bool KisStudioPatternDrag::canFillLayer(KisNodeSP node)
{
    return node && qobject_cast<KisPaintLayer *>(node.data()) && node->hasEditablePaintDevice();
}

void KisStudioPatternDrag::fillLayer(KisImageSP image, KisNodeSP node, KoPatternSP pattern)
{
    if (!image || !pattern || !canFillLayer(node)) return;
    const QRect area = node->paintDevice()->exactBounds();
    if (area.isEmpty()) return;
    const KoPatternSP painted = asPainted(pattern);
    KisProcessingApplicator applicator(image, node, KisProcessingApplicator::NONE, KisImageSignalVector(), kundo2_i18n("Pattern"));
    applicator.applyCommand(new KisCommandUtils::LambdaCommand([node, painted, area]() {
                                return paintPattern(node->paintDevice(), nullptr, painted, area, true);
                            }),
                            KisStrokeJobData::SEQUENTIAL, KisStrokeJobData::EXCLUSIVE);
    applicator.end();
}
