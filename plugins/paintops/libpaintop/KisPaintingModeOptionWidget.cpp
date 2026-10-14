/*
 *  SPDX-FileCopyrightText: 2022 Dmitry Kazakov <dimula73@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisPaintingModeOptionWidget.h"

#include <widgets/KisStudioChips.h>
#include <widgets/KisStudioDabPictures.h>

#include <QPainter>
#include <QPainterPath>

#include <QButtonGroup>

#include <lager/constant.hpp>
#include "ui_wdgincremental.h"

#include "KisPaintingModeOptionModel.h"

namespace {

class KisPaintingModeWidget: public QWidget, public Ui::WdgIncremental
{
public:
    KisPaintingModeWidget(QWidget *parent = 0)
        : QWidget(parent) {
        setupUi(this);
    }
};

QString calcButtonGroupToolTip(bool maskingBrushEnabled) {
    return maskingBrushEnabled ?
        i18nc("@info:tooltip",
              "Only wash mode is possible when using a masked brush.") :
        "";
}

}


struct KisPaintingModeOptionWidget::Private
{
    Private(lager::cursor<KisPaintingModeOptionData> optionData, lager::reader<bool> maskingBrushEnabled)
        : model(optionData, maskingBrushEnabled)
        , buttonGroupToolTip {maskingBrushEnabled.map(&calcButtonGroupToolTip)}
    {
    }

    KisPaintingModeOptionModel model;
    lager::reader<QString> buttonGroupToolTip;
};


KisPaintingModeOptionWidget::KisPaintingModeOptionWidget(lager::cursor<KisPaintingModeOptionData> optionData)
    : KisPaintingModeOptionWidget(optionData, lager::make_constant(false))
{
}

KisPaintingModeOptionWidget::KisPaintingModeOptionWidget(lager::cursor<KisPaintingModeOptionData> optionData, lager::reader<bool> maskingBrushEnabled)
    : KisPaintOpOption(i18n("Painting Mode"), KisPaintOpOption::COLOR, true)
    , m_d(new Private(optionData, maskingBrushEnabled))
{
    using namespace KisWidgetConnectionUtils;

    setObjectName("KisPaintActionTypeOption");
    m_checkable = false;

    KisPaintingModeWidget *widget = new KisPaintingModeWidget();

    QButtonGroup *group = new QButtonGroup(widget);
    group->addButton(widget->radioBuildup, static_cast<int>(enumPaintingMode::BUILDUP));
    group->addButton(widget->radioWash, static_cast<int>(enumPaintingMode::WASH));
    group->setExclusive(true);

    setConfigurationPage(widget);

    connectControlState(group, &m_d->model, "paintingModeState", "paintingMode");
    m_d->buttonGroupToolTip.bind(std::bind(&QWidget::setToolTip, widget->radioBuildup, std::placeholders::_1));
    m_d->buttonGroupToolTip.bind(std::bind(&QWidget::setToolTip, widget->radioWash, std::placeholders::_1));
    m_d->buttonGroupToolTip.bind(std::bind(&QWidget::setToolTip, widget->label, std::placeholders::_1));

    m_d->model.optionData.bind(std::bind(&KisPaintingModeOptionWidget::emitSettingChanged, this));
}

KisPaintingModeOptionWidget::~KisPaintingModeOptionWidget()
{
}

bool KisPaintingModeOptionWidget::hasQuickPage(PaintopCategory category) const
{
    return category == COLOR;
}

QWidget *KisPaintingModeOptionWidget::createQuickPage(PaintopCategory category, QWidget *parent)
{
    if (category != COLOR) return nullptr;
    // paint piling up within a stroke, or laid evenly as a wash: a stroke of see-through dabs each way
    auto *modes = new KisStudioChips(parent);
    modes->setTileColumns(2, 54);
    KisPaintingModeOptionModel *model = &m_d->model;
    auto picture = [modes](bool wash) {
        return [modes, wash](QPainter &painter, const QRectF &rect) {
            painter.setRenderHint(QPainter::Antialiasing, true);
            QColor paint = modes->palette().color(QPalette::WindowText);
            paint.setAlphaF(0.35);
            const qreal r = rect.height() * 0.45;
            // overlapping by half, where build up shows
            const qreal span = qMin(rect.width(), 12 * r);
            const QRectF along(rect.center().x() - span / 2, rect.top(), span, rect.height());
            const QVector<QPointF> dabs = KisStudioDabPictures::strokeDabs(along, 11, r);
            QPainterPath body;
            Q_FOREACH (const QPointF &c, dabs) {
                QPainterPath one;
                one.addEllipse(c, r, r);
                if (wash) {
                    body = body.united(one);
                } else {
                    painter.fillPath(one, paint);
                }
            }
            if (wash) painter.fillPath(body, paint);
        };
    };
    auto refresh = [modes, model, picture]() {
        KisStudioChips::Chip buildup;
        buildup.text = i18nc("painting mode", "Build up");
        buildup.on = model->effectivePaintingMode() == int(enumPaintingMode::BUILDUP);
        buildup.toolTip = i18nc("@info:tooltip", "Dabs pile up within a stroke");
        buildup.picture = picture(false);
        KisStudioChips::Chip wash;
        wash.text = i18nc("painting mode", "Wash");
        wash.on = model->effectivePaintingMode() == int(enumPaintingMode::WASH);
        wash.toolTip = i18nc("@info:tooltip", "A stroke never gets darker than its opacity");
        wash.picture = picture(true);
        modes->setChips({buildup, wash});
        modes->setCurrent(-1);
    };
    connect(modes, &KisStudioChips::clicked, modes, [model, refresh](int index) {
        model->setpaintingMode(index == 0 ? int(enumPaintingMode::BUILDUP) : int(enumPaintingMode::WASH));
        refresh();
    });
    connect(model, &KisPaintingModeOptionModel::effectivePaintingModeChanged, modes, refresh);
    refresh();
    return modes;
}

void KisPaintingModeOptionWidget::writeOptionSetting(KisPropertiesConfigurationSP setting) const
{
    m_d->model.bakedOptionData().write(setting.data());
}

void KisPaintingModeOptionWidget::readOptionSetting(const KisPropertiesConfigurationSP setting)
{
    KisPaintingModeOptionData data = *m_d->model.optionData;
    data.read(setting.data());
    m_d->model.optionData.set(data);
}
