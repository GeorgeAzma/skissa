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
#include <optional>

#include <KisBrushModel.h>
#include <kis_precision_option.h>
#include <kritapaintop_export.h>

class KisAutoBrushModel;
class KisPredefinedBrushModel;

/**
 * The brush tip, shaped where it is seen: the dab drawn large with handles for its width and
 * height (kept in proportion unless the chain is open), its soft edge and its angle, and its
 * values written on it so a screenshot shows them. Below, the tip's shape: round, square, a star,
 * or a picture tip; then the rest of the tip's page: its switches (anti-aliasing, auto spacing,
 * auto precision) and a picture tip's own colors.
 */
class PAINTOP_EXPORT KisBrushTipQuickPage : public QWidget
{
    Q_OBJECT
public:
    /// \p precision: for the engines that have it; without \p withRest, only the tip and its shapes (the classic page has the rest)
    KisBrushTipQuickPage(KisAutoBrushModel *autoModel,
                         KisPredefinedBrushModel *predefinedModel,
                         lager::cursor<KisBrushModel::BrushType> type,
                         std::optional<lager::cursor<KisBrushModel::PrecisionData>> precision,
                         QWidget *parent,
                         bool withRest = true);
    ~KisBrushTipQuickPage() override;

    /// The tip's own grain (noise and coverage), for the Texture tab; hidden for picture tips
    static QWidget *createGrainPage(KisAutoBrushModel *autoModel,
                                    lager::cursor<KisBrushModel::BrushType> type,
                                    QWidget *parent);

private:
    void updateShapes();
    /// The switches and the picture tip's settings, as the tip is now
    void updateRest();

    struct Private;
    const std::unique_ptr<Private> d;
};

#endif // KISBRUSHTIPEDITOR_H
