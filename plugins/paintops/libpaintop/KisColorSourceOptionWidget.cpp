/*
 *  SPDX-FileCopyrightText: 2022 Dmitry Kazakov <dimula73@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisColorSourceOptionWidget.h"

#include <QGroupBox>
#include <QHash>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QButtonGroup>
#include <QRadioButton>
#include <QVBoxLayout>

#include <KoCanvasResourcesIds.h>
#include <KoCanvasResourcesInterface.h>
#include <KoColor.h>
#include <KoID.h>

#include <KisColorSourceOptionModel.h>
#include <KisWidgetConnectionUtils.h>
#include <widgets/KisStudioChips.h>
#include <widgets/KisStudioDabPictures.h>

namespace {

/// A short stroke painted with color source \p type: the paint's color, the gradient, colors changing per dab or per pixel, a pattern
void drawSource(QPainter &painter, const QRectF &rect, KisColorSourceOptionData::Type type, const QColor &fg, const QColor &bg)
{
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    const qreal r = rect.height() * 0.3;
    // close enough to read as one stroke; a pattern's dabs further apart, to show each starting it again
    const int count = qMax(4, int(rect.width() / (r * (type == KisColorSourceOptionData::PATTERN ? 1.2 : 0.5))));
    const QVector<QPointF> dabs = KisStudioDabPictures::strokeDabs(rect, count, r);
    QPainterPath body;
    body.setFillRule(Qt::WindingFill);
    Q_FOREACH (const QPointF &c, dabs) {
        body.addEllipse(c, r, r);
    }
    body = body.simplified();
    const qreal hue = qMax(0.0, fg.toHsv().hsvHueF());
    // diagonal stripes starting at origin.x
    auto stripes = [&](qreal origin, const QRectF &area) {
        QColor faint = fg;
        faint.setAlphaF(0.35);
        painter.fillRect(area, faint);
        painter.setPen(QPen(fg, 1.6));
        const qreal step = 4.5;
        for (qreal x = origin - std::ceil((origin - area.left() + area.height()) / step) * step; x < area.right(); x += step) {
            painter.drawLine(QPointF(x, area.top()), QPointF(x + area.height(), area.bottom()));
        }
        painter.setPen(Qt::NoPen);
    };

    switch (type) {
    case KisColorSourceOptionData::GRADIENT: {
        QLinearGradient gradient(rect.topLeft(), rect.topRight());
        gradient.setColorAt(0, fg);
        gradient.setColorAt(1, bg);
        painter.fillPath(body, gradient);
        break;
    }
    case KisColorSourceOptionData::UNIFORM_RANDOM: {
        static const qreal shifts[] = {0.0, 0.45, 0.15, 0.7, 0.3, 0.85, 0.55, 0.05, 0.4};
        for (int i = 0; i < dabs.size(); i++) {
            painter.setBrush(QColor::fromHsvF(fmod(hue + shifts[i % 9], 1.0), 0.7, 0.95));
            painter.drawEllipse(dabs[i], r, r);
        }
        break;
    }
    case KisColorSourceOptionData::TOTAL_RANDOM: {
        painter.save();
        painter.setClipPath(body);
        const qreal cell = 2.5;
        int k = 0;
        for (qreal y = rect.top(); y < rect.bottom(); y += cell) {
            for (qreal x = rect.left(); x < rect.right(); x += cell, k++) {
                const quint32 hash = quint32(k) * 2654435761u;
                painter.fillRect(QRectF(x, y, cell, cell), QColor::fromHsvF((hash % 997) / 997.0, 0.75, 0.6 + ((hash >> 12) % 40) / 100.0));
            }
        }
        painter.restore();
        break;
    }
    case KisColorSourceOptionData::PATTERN:
        // the pattern starts again with each dab
        for (int i = 0; i < dabs.size(); i++) {
            painter.save();
            QPainterPath one;
            one.addEllipse(dabs[i], r, r);
            painter.setClipPath(one);
            stripes(dabs[i].x() + (i % 3) * 1.5, QRectF(dabs[i].x() - r, dabs[i].y() - r, 2 * r, 2 * r));
            painter.restore();
        }
        break;
    case KisColorSourceOptionData::PATTERN_LOCKED:
        // the pattern stays where it is, under the whole stroke
        painter.save();
        painter.setClipPath(body);
        stripes(rect.left(), rect);
        painter.restore();
        break;
    default:
        painter.fillPath(body, fg);
        break;
    }
}

} // namespace

struct KisColorSourceOptionWidget::Private
{
    Private(lager::cursor<KisColorSourceOptionData> optionData)
        : model(optionData)
    {
    }

    KisColorSourceOptionModel model;
};

KisColorSourceOptionWidget::KisColorSourceOptionWidget(lager::cursor<KisColorSourceOptionData> optionData)
    : KisPaintOpOption(i18nc("Color source", "Source"), KisPaintOpOption::COLOR, true)
    , m_d(new Private(optionData))
{
    using namespace KisWidgetConnectionUtils;

    setObjectName("KisColorSourceOptionWidget");
    m_checkable = false;

    QWidget* widget = new QWidget;

    QGroupBox* groupBox = new QGroupBox(widget);
    QVBoxLayout* verticalLayout = new QVBoxLayout(groupBox);

    QButtonGroup *buttonGroup = new QButtonGroup(widget);
    buttonGroup->setExclusive(true);

    Q_FOREACH (const KoID &id, KisColorSourceOptionData::colorSourceTypeIds()) {
        QRadioButton* radioButton = new QRadioButton(groupBox);
        radioButton->setText(id.name());
        verticalLayout->addWidget(radioButton);
        buttonGroup->addButton(radioButton, KisColorSourceOptionData::id2Type(id));
    }
    QVBoxLayout* verticalLayout_2 = new QVBoxLayout(widget);
    verticalLayout_2->setContentsMargins(0, 0, 0, 0);
    verticalLayout_2->addWidget(groupBox);
    verticalLayout_2->addStretch();

    setConfigurationPage(widget);

    connectControl(buttonGroup, &m_d->model, "type");

    m_d->model.optionData.bind(std::bind(&KisColorSourceOptionWidget::emitSettingChanged, this));
}

KisColorSourceOptionWidget::~KisColorSourceOptionWidget()
{
}

bool KisColorSourceOptionWidget::hasQuickPage(PaintopCategory category) const
{
    return category == COLOR;
}

QWidget *KisColorSourceOptionWidget::createQuickPage(PaintopCategory category, QWidget *parent)
{
    if (category != COLOR) return nullptr;
    // where the paint's color comes from, each drawn as a stroke
    auto *sources = new KisStudioChips(parent);
    sources->setTileColumns(3, 54);
    KisColorSourceOptionModel *model = &m_d->model;
    const QPointer<KisColorSourceOptionWidget> self(this);
    auto refresh = [sources, model, self]() {
        QVector<KisStudioChips::Chip> chips;
        Q_FOREACH (const KoID &id, KisColorSourceOptionData::colorSourceTypeIds()) {
            const KisColorSourceOptionData::Type type = KisColorSourceOptionData::id2Type(id);
            KisStudioChips::Chip chip;
            chip.text = id.name();
            chip.on = type == model->type();
            static const QHash<int, QString> tips = {
                {KisColorSourceOptionData::PLAIN, i18nc("@info:tooltip color source", "Your current color")},
                {KisColorSourceOptionData::GRADIENT, i18nc("@info:tooltip color source", "Colors along your current gradient")},
                {KisColorSourceOptionData::UNIFORM_RANDOM, i18nc("@info:tooltip color source", "A random color each dab")},
                {KisColorSourceOptionData::TOTAL_RANDOM, i18nc("@info:tooltip color source", "Random colors pixel by pixel, like noise")},
                {KisColorSourceOptionData::PATTERN, i18nc("@info:tooltip color source", "Colors from a pattern that moves with each dab")},
                {KisColorSourceOptionData::PATTERN_LOCKED, i18nc("@info:tooltip color source", "Colors from a pattern fixed to the canvas")},
            };
            chip.toolTip = tips.value(type);
            chip.picture = [sources, self, type](QPainter &painter, const QRectF &rect) {
                QColor fg(70, 140, 230);
                QColor bg(240, 200, 90);
                KoCanvasResourcesInterfaceSP resources = self ? self->canvasResourcesInterface() : KoCanvasResourcesInterfaceSP();
                if (resources) {
                    fg = resources->resource(KoCanvasResource::ForegroundColor).value<KoColor>().toQColor();
                    bg = resources->resource(KoCanvasResource::BackgroundColor).value<KoColor>().toQColor();
                }
                fg = KisStudioDabPictures::paintColor(fg, sources->palette().color(QPalette::Highlight));
                drawSource(painter, rect, type, fg, bg);
            };
            chips << chip;
        }
        sources->setChips(chips);
    };
    connect(sources, &KisStudioChips::clicked, sources, [model, refresh](int index) {
        const QVector<KoID> ids = KisColorSourceOptionData::colorSourceTypeIds();
        if (index >= 0 && index < ids.size()) model->settype(KisColorSourceOptionData::id2Type(ids[index]));
        refresh();
    });
    connect(model, &KisColorSourceOptionModel::typeChanged, sources, refresh);
    refresh();
    return sources;
}

void KisColorSourceOptionWidget::writeOptionSetting(KisPropertiesConfigurationSP setting) const
{
    m_d->model.optionData->write(setting.data());
}

void KisColorSourceOptionWidget::readOptionSetting(const KisPropertiesConfigurationSP setting)
{
    KisColorSourceOptionData data = *m_d->model.optionData;
    data.read(setting.data());
    m_d->model.optionData.set(data);
}
