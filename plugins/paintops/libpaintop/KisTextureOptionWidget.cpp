/*
 *  SPDX-FileCopyrightText: 2022 Dmitry Kazakov <dimula73@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisTextureOptionWidget.h"

#include <QHash>

#include <kis_paintop_lod_limitations.h>
#include <KisTextureOptionModel.h>
#include <KisWidgetConnectionUtils.h>

#include "kis_texture_chooser.h"

#include <QPainter>
#include <QVBoxLayout>

#include <KisGlobalResourcesInterface.h>
#include <KisModernStyle.h>
#include <KisResourceModel.h>
#include <KoCompositeOpRegistry.h>
#include <KoPattern.h>
#include <kis_config.h>
#include <widgets/KisStudioChips.h>
#include <widgets/KisStudioPicker.h>
#include <widgets/KisStudioValueRow.h>

#include <memory>

namespace {
const QString recentPatternsKey = QStringLiteral("StudioRecentPatterns");

/// Fills \p box with \p pattern repeated, small enough to read as a texture
void paintPattern(QPainter &painter, const QRectF &box, const QImage &pattern)
{
    if (pattern.isNull()) return;
    painter.fillRect(box, QBrush(pattern.scaled(qMin(pattern.width(), 64), qMin(pattern.height(), 64), Qt::KeepAspectRatio, Qt::SmoothTransformation)));
}

void rememberPattern(const QString &md5)
{
    QStringList recent = KisConfig(true).readEntry<QStringList>(recentPatternsKey, QStringList());
    recent.removeAll(md5);
    recent.prepend(md5);
    KisConfig(false).writeEntry(recentPatternsKey, recent.mid(0, 6));
}

void paintMore(QPainter &painter, const QRectF &box, const QColor &ink)
{
    painter.setPen(Qt::NoPen);
    painter.setBrush(KisModernStyle::secondaryInk(ink));
    for (int i = -1; i <= 1; i++) {
        painter.drawEllipse(box.center() + QPointF(i * 6, 0), 1.6, 1.6);
    }
}
} // namespace


struct KisTextureOptionWidget::Private
{
    Private(lager::cursor<KisTextureOptionData> optionData, KisResourcesInterfaceSP resourcesInterface)
        : model(optionData, resourcesInterface)
    {
    }

    KisTextureOptionModel model;
    KisBrushTextureFlags flags;
};


KisTextureOptionWidget::KisTextureOptionWidget(lager::cursor<KisTextureOptionData> optionData, KisResourcesInterfaceSP resourcesInterface, KisBrushTextureFlags flags)
    : KisPaintOpOption(i18n("Pattern"), KisPaintOpOption::TEXTURE, optionData[&KisTextureOptionData::isEnabled])
    , m_d(new Private(optionData, resourcesInterface))
{
    m_d->flags = flags;
    using namespace KisWidgetConnectionUtils;
    setObjectName("KisTextureOption");

    KisTextureChooser *chooserWidget = new KisTextureChooser(flags);

    setConfigurationPage(chooserWidget);

    connect(&m_d->model, &KisTextureOptionModel::textureResourceChanged,
            chooserWidget->textureSelectorWidget, &KisPatternChooser::setCurrentPattern);
    connect(chooserWidget->textureSelectorWidget, &KisPatternChooser::resourceSelected,
            &m_d->model, &KisTextureOptionModel::settextureResource);
    chooserWidget->textureSelectorWidget->setCurrentPattern(m_d->model.textureResource());

    connect(&m_d->model, &KisTextureOptionModel::cutOffLeftNormalizedChanged,
            chooserWidget->cutoffSlider, &KisInputLevelsSlider::setBlackPoint);
    connect(chooserWidget->cutoffSlider, &KisInputLevelsSlider::blackPointChanged,
            &m_d->model, &KisTextureOptionModel::setcutOffLeftNormalized);
    chooserWidget->cutoffSlider->setBlackPoint(m_d->model.cutOffLeftNormalized());

    connect(&m_d->model, &KisTextureOptionModel::cutOffRightNormalizedChanged,
            chooserWidget->cutoffSlider, &KisInputLevelsSlider::setWhitePoint);
    connect(chooserWidget->cutoffSlider, &KisInputLevelsSlider::whitePointChanged,
            &m_d->model, &KisTextureOptionModel::setcutOffRightNormalized);
    chooserWidget->cutoffSlider->setWhitePoint(m_d->model.cutOffRightNormalized());

    connectControl(chooserWidget->scaleSlider, &m_d->model, "scale");
    connectControl(chooserWidget->brightnessSlider, &m_d->model, "brightness");
    connectControl(chooserWidget->contrastSlider, &m_d->model, "contrast");
    connectControl(chooserWidget->neutralPointSlider, &m_d->model, "neutralPoint");
    connectControl(chooserWidget->offsetSliderX, &m_d->model, "offsetX");
    connectControl(chooserWidget->randomOffsetX, &m_d->model, "isRandomOffsetX");
    connectControl(chooserWidget->randomOffsetY, &m_d->model, "isRandomOffsetY");
    connectControl(chooserWidget->offsetSliderY, &m_d->model, "offsetY");
    connectControl(chooserWidget->cmbTexturingMode, &m_d->model, "texturingMode");
    connectControl(chooserWidget->checkBoxSoftTexturing, &m_d->model, "useSoftTexturing");
    connectControl(chooserWidget->cmbCutoffPolicy, &m_d->model, "cutOffPolicy");
    connectControl(chooserWidget->chkInvert, &m_d->model, "invert");
    connectControl(chooserWidget->chkAutoInvertOnErase, &m_d->model, "autoInvertOnErase");

    m_d->model.LAGER_QT(maximumOffsetX).bind(std::bind(&KisSliderSpinBox::setMaximum, chooserWidget->offsetSliderX, std::placeholders::_1, true));
    m_d->model.LAGER_QT(maximumOffsetY).bind(std::bind(&KisSliderSpinBox::setMaximum, chooserWidget->offsetSliderY, std::placeholders::_1, true));

    m_d->model.optionData.bind(std::bind(&KisTextureOptionWidget::emitSettingChanged, this));
}

KisTextureOptionWidget::~KisTextureOptionWidget()
{
}

void KisTextureOptionWidget::writeOptionSetting(KisPropertiesConfigurationSP setting) const
{
    m_d->model.bakedOptionData().write(setting.data());
}

void KisTextureOptionWidget::readOptionSetting(const KisPropertiesConfigurationSP setting)
{
    KisTextureOptionData data = *m_d->model.optionData;
    data.read(setting.data());
    m_d->model.optionData.set(data);
}

bool KisTextureOptionWidget::hasQuickPage(PaintopCategory category) const
{
    return category == TEXTURE;
}

QWidget *KisTextureOptionWidget::createQuickPage(PaintopCategory category, QWidget *parent)
{
    if (category != TEXTURE) return nullptr;
    KisTextureOptionModel *model = &m_d->model;
    auto *page = new QWidget(parent);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(KisModernStyle::space2);

    // the patterns: none, this one, the ones used last, all
    auto *patterns = new KisStudioChips(page);
    patterns->setTileSize(44);
    layout->addWidget(patterns);
    auto ids = std::make_shared<QStringList>();
    auto refreshPatterns = [model, patterns, ids]() {
        const QColor ink = patterns->palette().color(QPalette::WindowText);
        const bool on = model->isEnabled();
        const KoPatternSP current = model->textureResource().dynamicCast<KoPattern>();
        QVector<KisStudioChips::Chip> chips;
        KisStudioChips::Chip none;
        none.toolTip = i18nc("no texture on the brush", "None");
        none.on = !on;
        none.picture = [ink](QPainter &painter, const QRectF &box) {
            painter.setPen(QPen(KisModernStyle::secondaryInk(ink), 1.5));
            painter.setBrush(Qt::NoBrush);
            const QRectF circle(box.center() - QPointF(8, 8), QSizeF(16, 16));
            painter.drawEllipse(circle);
            painter.drawLine(circle.bottomLeft() + QPointF(2.5, -2.5), circle.topRight() + QPointF(-2.5, 2.5));
        };
        chips << none;
        QStringList wanted;
        if (current) wanted << current->md5Sum();
        Q_FOREACH (const QString &md5, KisConfig(true).readEntry<QStringList>(recentPatternsKey, QStringList())) {
            if (!wanted.contains(md5)) wanted << md5;
        }
        ids->clear();
        auto source = KisGlobalResourcesInterface::instance()->source<KoPattern>(ResourceType::Patterns);
        Q_FOREACH (const QString &md5, wanted) {
            if (ids->size() >= 5) break;
            const KoPatternSP match = source.bestMatch(md5, QString(), QString());
            if (!match) continue;
            *ids << md5;
            KisStudioChips::Chip chip;
            chip.toolTip = match->name();
            chip.on = on && current && current->md5Sum() == md5;
            const QImage image = match->pattern();
            chip.picture = [image](QPainter &painter, const QRectF &box) { paintPattern(painter, box.adjusted(-4, -4, 4, 4), image); };
            chips << chip;
        }
        KisStudioChips::Chip more;
        more.toolTip = i18nc("@info:tooltip", "All patterns");
        more.picture = [ink](QPainter &painter, const QRectF &box) { paintMore(painter, box, ink); };
        chips << more;
        patterns->setChips(chips);
    };
    // by value: picking it refreshes the patterns, which clears the list a picked id could refer into
    auto use = [model, refreshPatterns](QString md5) {
        auto source = KisGlobalResourcesInterface::instance()->source<KoPattern>(ResourceType::Patterns);
        const KoPatternSP match = source.bestMatch(md5, QString(), QString());
        if (!match) return;
        model->settextureResource(match);
        model->setisEnabled(true);
        rememberPattern(md5);
        refreshPatterns();
    };
    connect(patterns, &KisStudioChips::clicked, page, [model, patterns, ids, refreshPatterns, use](int index) {
        if (index == 0) {
            model->setisEnabled(false);
            refreshPatterns();
            return;
        }
        if (index - 1 < ids->size()) {
            use(ids->at(index - 1));
            return;
        }
        auto *resources = new KisResourceModel(ResourceType::Patterns);
        QStringList all;
        QHash<QString, QString> names;
        QHash<QString, QImage> thumbnails;
        for (int row = 0; row < resources->rowCount(); row++) {
            const QModelIndex at = resources->index(row, 0);
            const QString md5 = resources->data(at, Qt::UserRole + KisAbstractResourceModel::MD5).toString();
            if (md5.isEmpty() || all.contains(md5)) continue;
            all << md5;
            names.insert(md5, resources->data(at, Qt::UserRole + KisAbstractResourceModel::Name).toString());
            thumbnails.insert(md5, resources->data(at, Qt::UserRole + KisAbstractResourceModel::Thumbnail).value<QImage>());
        }
        delete resources;
        KisStudioPicker::Source source;
        source.pinnedKey = QStringLiteral("StudioPinnedPatterns");
        source.initialPinned = []() { return QStringList(); };
        source.allIds = [all]() { return all; };
        source.otherGroups = [all](const QStringList &pinned) {
            KisStudioPicker::Group group{i18nc("pattern group", "Patterns"), {}};
            Q_FOREACH (const QString &id, all) {
                if (!pinned.contains(id)) group.ids << id;
            }
            return QVector<KisStudioPicker::Group>{group};
        };
        source.name = [names](const QString &id) { return names.value(id); };
        source.paintPreview = [thumbnails](QPainter &painter, const QRect &rect, const QString &id) {
            paintPattern(painter, rect, thumbnails.value(id));
        };
        source.recent = []() { return KisConfig(true).readEntry<QStringList>(recentPatternsKey, QStringList()); };
        source.tiles = true;
        source.tileSize = QSize(84, 96);
        source.yoursTitle = i18nc("pattern group", "Your patterns");
        source.searchPlaceholder = i18nc("@info:placeholder", "Search %1 patterns", all.size());
        const KoPatternSP current = model->textureResource().dynamicCast<KoPattern>();
        auto *picker = new KisStudioPicker(source, current ? current->md5Sum() : QString(), {}, patterns->window());
        connect(picker, &KisStudioPicker::sigPicked, patterns, use);
        picker->popupBelow(patterns);
    });
    connect(model, &KisTextureOptionModel::isEnabledChanged, patterns, refreshPatterns);
    connect(model, &KisTextureOptionModel::textureResourceChanged, patterns, refreshPatterns);
    refreshPatterns();

    // how it marks the paint: the commonest first
    auto *modes = new KisStudioChips(page);
    layout->addWidget(modes);
    const KoCompositeOpRegistry &registry = KoCompositeOpRegistry::instance();
    QVector<std::pair<QString, int>> all;
    all << std::make_pair(registry.getKoID(COMPOSITE_MULT).name(), int(KisTextureOptionData::MULTIPLY))
        << std::make_pair(registry.getKoID(COMPOSITE_SUBTRACT).name(), int(KisTextureOptionData::SUBTRACT));
    if (m_d->flags & SupportsLightnessMode) {
        all << std::make_pair(i18nc("Lightness Map blend mode for brush texture", "Lightness Map"), int(KisTextureOptionData::LIGHTNESS));
    }
    if (m_d->flags & SupportsGradientMode) {
        all << std::make_pair(i18nc("Gradient Map blend mode for brush texture", "Gradient Map"), int(KisTextureOptionData::GRADIENT));
    }
    all << std::make_pair(registry.getKoID(COMPOSITE_DARKEN).name(), int(KisTextureOptionData::DARKEN))
        << std::make_pair(registry.getKoID(COMPOSITE_OVERLAY).name(), int(KisTextureOptionData::OVERLAY))
        << std::make_pair(registry.getKoID(COMPOSITE_DODGE).name(), int(KisTextureOptionData::COLOR_DODGE))
        << std::make_pair(registry.getKoID(COMPOSITE_BURN).name(), int(KisTextureOptionData::COLOR_BURN))
        << std::make_pair(registry.getKoID(COMPOSITE_LINEAR_DODGE).name(), int(KisTextureOptionData::LINEAR_DODGE))
        << std::make_pair(registry.getKoID(COMPOSITE_LINEAR_BURN).name(), int(KisTextureOptionData::LINEAR_BURN))
        << std::make_pair(registry.getKoID(COMPOSITE_HARD_MIX_PHOTOSHOP).name(), int(KisTextureOptionData::HARD_MIX_PHOTOSHOP))
        << std::make_pair(registry.getKoID(COMPOSITE_HARD_MIX_SOFTER_PHOTOSHOP).name(), int(KisTextureOptionData::HARD_MIX_SOFTER_PHOTOSHOP))
        << std::make_pair(i18nc("Height blend mode for brush texture", "Height"), int(KisTextureOptionData::HEIGHT))
        << std::make_pair(i18nc("Linear Height blend mode for brush texture", "Linear Height"), int(KisTextureOptionData::LINEAR_HEIGHT))
        << std::make_pair(i18nc("Height (Photoshop) blend mode for brush texture", "Height (Photoshop)"), int(KisTextureOptionData::HEIGHT_PHOTOSHOP))
        << std::make_pair(i18nc("Linear Height (Photoshop) blend mode for brush texture", "Linear Height (Photoshop)"), int(KisTextureOptionData::LINEAR_HEIGHT_PHOTOSHOP));
    const QVector<int> common = {KisTextureOptionData::MULTIPLY, KisTextureOptionData::SUBTRACT, KisTextureOptionData::DARKEN,
                                 KisTextureOptionData::OVERLAY, KisTextureOptionData::HEIGHT};
    // what each does with the pattern, in one line
    const QHash<int, QString> tips = {
        {KisTextureOptionData::MULTIPLY, i18nc("@info:tooltip texture mode", "The pattern's darks darken the paint")},
        {KisTextureOptionData::SUBTRACT, i18nc("@info:tooltip texture mode", "The pattern cuts holes in the paint")},
        {KisTextureOptionData::LIGHTNESS, i18nc("@info:tooltip texture mode", "The pattern's lights and darks shade the paint's color")},
        {KisTextureOptionData::GRADIENT, i18nc("@info:tooltip texture mode", "The pattern's tones → your gradient's colors")},
        {KisTextureOptionData::DARKEN, i18nc("@info:tooltip texture mode", "Only the pattern's darker parts show")},
        {KisTextureOptionData::OVERLAY, i18nc("@info:tooltip texture mode", "The pattern deepens lights and darks")},
        {KisTextureOptionData::COLOR_DODGE, i18nc("@info:tooltip texture mode", "The pattern lightens the paint")},
        {KisTextureOptionData::LINEAR_DODGE, i18nc("@info:tooltip texture mode", "The pattern lightens the paint, evenly")},
        {KisTextureOptionData::COLOR_BURN, i18nc("@info:tooltip texture mode", "The pattern darkens the paint, strongly")},
        {KisTextureOptionData::LINEAR_BURN, i18nc("@info:tooltip texture mode", "The pattern darkens the paint, evenly")},
        {KisTextureOptionData::HARD_MIX_PHOTOSHOP, i18nc("@info:tooltip texture mode", "Paint all or nothing where the pattern is")},
        {KisTextureOptionData::HARD_MIX_SOFTER_PHOTOSHOP, i18nc("@info:tooltip texture mode", "Paint all or nothing, softened")},
        {KisTextureOptionData::HEIGHT, i18nc("@info:tooltip texture mode", "Light pressure catches only the grain's peaks, like charcoal on paper")},
        {KisTextureOptionData::LINEAR_HEIGHT, i18nc("@info:tooltip texture mode", "Like Height, with softer edges")},
        {KisTextureOptionData::HEIGHT_PHOTOSHOP, i18nc("@info:tooltip texture mode", "Like Height, as Photoshop does it")},
        {KisTextureOptionData::LINEAR_HEIGHT_PHOTOSHOP, i18nc("@info:tooltip texture mode", "Like Linear Height, as Photoshop does it")},
    };
    auto showAll = std::make_shared<bool>(false);
    auto shown = std::make_shared<QVector<int>>();
    auto refreshModes = [model, modes, all, common, showAll, shown, tips]() {
        const int current = model->texturingMode();
        shown->clear();
        QVector<KisStudioChips::Chip> chips;
        Q_FOREACH (const auto &mode, all) {
            if (!*showAll && !common.contains(mode.second) && mode.second != current) continue;
            KisStudioChips::Chip chip;
            chip.text = mode.first;
            chip.on = mode.second == current;
            chip.toolTip = tips.value(mode.second);
            chips << chip;
            *shown << mode.second;
        }
        KisStudioChips::Chip more;
        more.text = *showAll ? i18nc("show fewer choices", "Less") : QStringLiteral("···");
        more.toolTip = *showAll ? QString() : i18nc("@info:tooltip", "More modes");
        chips << more;
        modes->setChips(chips);
        modes->setVisible(model->isEnabled());
    };
    connect(modes, &KisStudioChips::clicked, page, [model, refreshModes, showAll, shown](int index) {
        if (index < shown->size()) {
            model->settexturingMode(shown->at(index));
        } else {
            *showAll = !*showAll;
        }
        refreshModes();
    });
    connect(model, &KisTextureOptionModel::texturingModeChanged, modes, refreshModes);
    connect(model, &KisTextureOptionModel::isEnabledChanged, modes, refreshModes);
    refreshModes();

    // its size and tone, in percent
    struct Setting {
        QString name;
        qreal minimum;
        qreal maximum;
        qreal neutral;
        /// Dragged by ratios, as sizes are
        bool size;
        std::function<qreal()> get;
        std::function<void(qreal)> set;
    };
    const QVector<Setting> settings = {
        {i18nc("texture setting", "Scale"), 1, 200, 100, true,
         [model]() { return model->scale() * 100.0; }, [model](qreal value) { model->setscale(value / 100.0); }},
        {i18nc("texture setting", "Brightness"), -100, 100, 0, false,
         [model]() { return model->brightness() * 100.0; }, [model](qreal value) { model->setbrightness(value / 100.0); }},
        {i18nc("texture setting", "Contrast"), 0, 200, 100, false,
         [model]() { return model->contrast() * 100.0; }, [model](qreal value) { model->setcontrast(value / 100.0); }},
    };
    Q_FOREACH (const Setting &setting, settings) {
        auto *row = new KisStudioValueRow(setting.name, page);
        KisStudioValueRow::Range range;
        range.minimum = setting.minimum;
        range.maximum = setting.maximum;
        range.suffix = i18nc("percent, after the number", "%");
        range.size = setting.size;
        row->setRange(range);
        row->setDefaultValue(setting.neutral);
        const auto get = setting.get;
        auto show = [row, get, model]() {
            row->setValue(get());
            row->setVisible(model->isEnabled());
        };
        show();
        connect(model, &KisTextureOptionModel::scaleChanged, row, show);
        connect(model, &KisTextureOptionModel::brightnessChanged, row, show);
        connect(model, &KisTextureOptionModel::contrastChanged, row, show);
        connect(model, &KisTextureOptionModel::isEnabledChanged, row, show);
        connect(row, &KisStudioValueRow::valueChanged, page, setting.set);
        layout->addWidget(row);
    }
    return page;
}

KisPaintOpOption::OptionalLodLimitationsReader KisTextureOptionWidget::lodLimitationsReader() const
{
    return m_d->model.optionData.map(std::mem_fn(&KisTextureOptionData::lodLimitations));
}
