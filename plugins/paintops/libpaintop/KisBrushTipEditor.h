/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISBRUSHTIPEDITOR_H
#define KISBRUSHTIPEDITOR_H

#include <QWidget>

#include <lager/cursor.hpp>

#include <memory>

#include <KisBrushModel.h>
#include <kritapaintop_export.h>

class KisAutoBrushModel;
class KisPredefinedBrushModel;

/**
 * The brush tip, shaped where it is seen: the dab drawn large with handles for its width and
 * height (kept in proportion unless the chain is open), its soft edge and its angle, and its
 * values written on it so a screenshot shows them. Below, the tip's shape: round, square, a star,
 * or a picture tip.
 */
class PAINTOP_EXPORT KisBrushTipQuickPage : public QWidget
{
    Q_OBJECT
public:
    KisBrushTipQuickPage(KisAutoBrushModel *autoModel,
                         KisPredefinedBrushModel *predefinedModel,
                         lager::cursor<KisBrushModel::BrushType> type,
                         QWidget *parent);
    ~KisBrushTipQuickPage() override;

    /// The tip's own grain (noise and coverage), for the Texture tab; hidden for picture tips
    static QWidget *createGrainPage(KisAutoBrushModel *autoModel,
                                    lager::cursor<KisBrushModel::BrushType> type,
                                    QWidget *parent);

private:
    void updateShapes();

    struct Private;
    const std::unique_ptr<Private> d;
};

#endif // KISBRUSHTIPEDITOR_H
