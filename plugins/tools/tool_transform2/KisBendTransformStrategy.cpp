/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisBendTransformStrategy.h"

#include <QPainter>

#include <KoPointerEvent.h>
#include <KisGridRenderer.h>
#include <KisPuppet.h>

#include "kis_coordinates_converter.h"
#include "kis_cursor.h"
#include "kis_liquify_transform_worker.h"
#include "kis_paint_device.h"
#include "kis_transform_utils.h"
#include "tool_transform_args.h"
#include "transform_transaction_properties.h"

struct KisBendTransformStrategy::Private {
    const KisCoordinatesConverter *converter;
    ToolTransformArgs &currentArgs;
    TransformTransactionProperties &transaction;

    KisPaintDeviceSP source;
    QScopedPointer<KisPuppet> puppet;
    QRect puppetRect;
    KisPaintDevice *puppetSource {nullptr};

    int hovered {-1};
    int dragged {-1};
    QPointF grabOffset;

    Private(const KisCoordinatesConverter *_converter, ToolTransformArgs &_currentArgs, TransformTransactionProperties &_transaction)
        : converter(_converter)
        , currentArgs(_currentArgs)
        , transaction(_transaction)
    {
    }

    QPointF imagePoint(KoPointerEvent *event) const
    {
        return converter->documentToImage(event->point);
    }

    /// The puppet of what this transform moves, with the arguments' pins
    bool ensurePuppet()
    {
        KisLiquifyTransformWorker *worker = currentArgs.liquifyWorker();
        if (!worker) return false;

        const QRect rect = transaction.originalRect().toAlignedRect();
        if (rect.isEmpty()) return false;
        if (!puppet || puppetRect != rect || puppetSource != source.data()) {
            puppet.reset(source ? new KisPuppet(KisPuppet::ofDevice(source, rect))
                                : new KisPuppet(rect, KisPuppet::stepFor(rect), [](const QRect &) { return true; }));
            puppetRect = rect;
            puppetSource = source.data();
        }
        syncPins();
        return true;
    }

    /// The pins as the arguments have them: undo and redo bring theirs back
    void syncPins()
    {
        KisLiquifyTransformWorker *worker = currentArgs.liquifyWorker();
        if (!puppet || !worker) return;
        QVector<QPointF> rest;
        QVector<QPointF> at;
        for (int i = 0; i < puppet->pinCount(); i++) {
            rest << puppet->pinRest(i);
            at << puppet->pinAt(i);
        }
        if (rest != worker->pinsRest() || at != worker->pinsAt()) {
            puppet->setPins(worker->pinsRest(), worker->pinsAt());
        }
    }

    /// The pose into the arguments' liquify grid
    void writeArgs()
    {
        KisLiquifyTransformWorker *worker = currentArgs.liquifyWorker();
        if (!puppet || !worker) return;

        const QVector<QPointF> &grid = worker->originalPoints();
        QVector<QPointF> moved(grid.size());
        const QPointF *from = grid.constData();
        QPointF *to = moved.data();
        const KisPuppet &posed = *puppet;
        const int chunk = 4096;
        KisGridRenderer::forEachOnAllThreads((grid.size() + chunk - 1) / chunk, [&](int part) {
            const int end = qMin(grid.size(), (part + 1) * chunk);
            for (int i = part * chunk; i < end; i++) to[i] = posed.map(from[i]);
        });
        worker->setTransformedPoints(moved);

        QVector<QPointF> rest;
        QVector<QPointF> at;
        for (int i = 0; i < puppet->pinCount(); i++) {
            rest << puppet->pinRest(i);
            at << puppet->pinAt(i);
        }
        worker->setPins(rest, at);
    }
};

KisBendTransformStrategy::KisBendTransformStrategy(const KisCoordinatesConverter *converter,
                                                   ToolTransformArgs &currentArgs,
                                                   TransformTransactionProperties &transaction)
    : m_d(new Private(converter, currentArgs, transaction))
{
}

KisBendTransformStrategy::~KisBendTransformStrategy()
{
}

void KisBendTransformStrategy::setSource(KisPaintDeviceSP source)
{
    m_d->source = source;
}

bool KisBendTransformStrategy::removeHoveredPin()
{
    if (!m_d->puppet || m_d->hovered < 0 || m_d->hovered >= m_d->puppet->pinCount()) return false;
    m_d->puppet->removePin(m_d->hovered);
    m_d->hovered = -1;
    m_d->writeArgs();
    Q_EMIT requestImageRecalculation();
    Q_EMIT requestCanvasUpdate();
    return true;
}

void KisBendTransformStrategy::paint(QPainter &gc, const KoColorDisplayRendererInterface *displayRendererInterface)
{
    Q_UNUSED(displayRendererInterface);
    if (!m_d->puppet) return;

    const QTransform toFlake = KisTransformUtils::imageToFlakeTransform(m_d->converter);
    gc.save();
    gc.setRenderHint(QPainter::Antialiasing, true);
    for (int i = 0; i < m_d->puppet->pinCount(); i++) {
        const QPointF center = toFlake.map(m_d->puppet->pinAt(i));
        // white with a dark ring, so a pin reads on any picture; the one under the pointer larger
        const qreal radius = (i == m_d->hovered || i == m_d->dragged) ? 7.0 : 5.0;
        gc.setPen(QPen(QColor(0, 0, 0, 170), 1.5));
        gc.setBrush(Qt::white);
        gc.drawEllipse(center, radius, radius);
    }
    gc.restore();
}

QCursor KisBendTransformStrategy::getCurrentCursor() const
{
    return m_d->hovered >= 0 || m_d->dragged >= 0 ? KisCursor::pointingHandCursor() : KisCursor::crossCursor();
}

bool KisBendTransformStrategy::acceptsClicks() const
{
    return true;
}

void KisBendTransformStrategy::externalConfigChanged()
{
    // the arguments changed under the pins (undo, redo, a new transform): their pins and grid are the truth
    if (m_d->puppet) {
        m_d->syncPins();
        if (m_d->hovered >= m_d->puppet->pinCount()) m_d->hovered = -1;
    }
    Q_EMIT requestImageRecalculation();
    Q_EMIT requestCanvasUpdate();
}

bool KisBendTransformStrategy::beginPrimaryAction(KoPointerEvent *event)
{
    if (!m_d->ensurePuppet()) return false;

    const QPointF point = m_d->imagePoint(event);
    int pin = m_d->puppet->pinNear(point, KisTransformUtils::effectiveHandleGrabRadius(m_d->converter));
    if (pin < 0) {
        // a press elsewhere puts a pin there, held and ready to drag
        pin = m_d->puppet->addPin(point);
        m_d->writeArgs();
        Q_EMIT requestImageRecalculation();
    }
    m_d->dragged = pin;
    m_d->hovered = pin;
    m_d->grabOffset = m_d->puppet->pinAt(pin) - point;
    Q_EMIT requestCanvasUpdate();
    return true;
}

void KisBendTransformStrategy::continuePrimaryAction(KoPointerEvent *event)
{
    if (!m_d->puppet || m_d->dragged < 0) return;
    m_d->puppet->movePin(m_d->dragged, m_d->imagePoint(event) + m_d->grabOffset);
    m_d->writeArgs();
    Q_EMIT requestImageRecalculation();
    Q_EMIT requestCanvasUpdate();
}

bool KisBendTransformStrategy::endPrimaryAction(KoPointerEvent *event)
{
    Q_UNUSED(event);
    m_d->dragged = -1;
    Q_EMIT requestCanvasUpdate();
    return true;
}

void KisBendTransformStrategy::hoverActionCommon(KoPointerEvent *event)
{
    int hovered = -1;
    if (m_d->puppet) {
        m_d->syncPins();
        hovered = m_d->puppet->pinNear(m_d->imagePoint(event), KisTransformUtils::effectiveHandleGrabRadius(m_d->converter));
    }
    if (hovered != m_d->hovered) {
        m_d->hovered = hovered;
        Q_EMIT requestCanvasUpdate();
    }
}
