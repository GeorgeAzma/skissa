/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KISBENDTRANSFORMSTRATEGY_H
#define KISBENDTRANSFORMSTRATEGY_H

#include <QScopedPointer>

#include <kis_types.h>

#include "kis_transform_strategy_base.h"

class KisCoordinatesConverter;
class ToolTransformArgs;
class TransformTransactionProperties;

/**
 * Bend: pins on the picture, dragged to pose it as rigidly as possible (KisPuppet).
 * The pose is written into the liquify grid of the arguments, so the stroke,
 * undo and files are liquify's, and Krita opens the result as a liquify transform.
 */
class KisBendTransformStrategy : public KisTransformStrategyBase
{
    Q_OBJECT
public:
    KisBendTransformStrategy(const KisCoordinatesConverter *converter,
                             ToolTransformArgs &currentArgs,
                             TransformTransactionProperties &transaction);
    ~KisBendTransformStrategy() override;

    /// The pixels being transformed, as they were: what is stiff and what is air
    void setSource(KisPaintDeviceSP source);

    /// Removes the pin under the pointer, if there is one
    bool removeHoveredPin();

    void paint(QPainter &gc, const KoColorDisplayRendererInterface *displayRendererInterface) override;
    QCursor getCurrentCursor() const override;
    bool acceptsClicks() const override;
    void externalConfigChanged() override;

    bool beginPrimaryAction(KoPointerEvent *event) override;
    void continuePrimaryAction(KoPointerEvent *event) override;
    bool endPrimaryAction(KoPointerEvent *event) override;
    void hoverActionCommon(KoPointerEvent *event) override;

Q_SIGNALS:
    void requestCanvasUpdate();
    void requestImageRecalculation();

private:
    struct Private;
    const QScopedPointer<Private> m_d;
};

#endif
