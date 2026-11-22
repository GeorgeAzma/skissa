/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioLayerStyles.h"

#include <QAction>
#include <QCursor>
#include <QGridLayout>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QTimer>
#include <QToolButton>
#include <QUuid>
#include <QVBoxLayout>

#include <KisGlobalResourcesInterface.h>
#include <KisModernStyle.h>
#include <KisViewManager.h>
#include <KoCanvasResourceProvider.h>
#include <KoColorSpaceRegistry.h>
#include <canvas/kis_canvas2.h>
#include <commands_new/kis_set_layer_style_command.h>
#include <kis_action.h>
#include <kis_action_manager.h>
#include <kis_post_execution_undo_adapter.h>
#include <kis_canvas_resource_provider.h>
#include <kis_image.h>
#include <kis_layer.h>
#include <kis_paint_device.h>
#include <kis_paint_layer.h>
#include <kis_psd_layer_style.h>
#include <klocalizedstring.h>
#include <psd.h>

#include "KisStudioLayout.h"
#include "widgets/KisStudioPopup.h"
#include "widgets/KisStudioValueRow.h"

namespace
{
/// Set while More… hands the layer to Krita's own dialog
bool classic = false;

struct Kind {
    QString name;
    std::function<psd_layer_effects_shadow_base *(KisPSDLayerStyle *)> of;
    /// Shadows are cast some way off; the rest sit on the layer's edge
    bool distance;
};

QVector<Kind> kinds()
{
    return {
        {i18nc("layer style", "Shadow"), [](KisPSDLayerStyle *s) -> psd_layer_effects_shadow_base * { return s->dropShadow(); }, true},
        {i18nc("layer style", "Glow"), [](KisPSDLayerStyle *s) -> psd_layer_effects_shadow_base * { return s->outerGlow(); }, false},
        {i18nc("layer style", "Outline"), [](KisPSDLayerStyle *s) -> psd_layer_effects_shadow_base * { return s->stroke(); }, false},
        {i18nc("layer style", "Inner shadow"), [](KisPSDLayerStyle *s) -> psd_layer_effects_shadow_base * { return s->innerShadow(); }, true},
        {i18nc("layer style", "Inner glow"), [](KisPSDLayerStyle *s) -> psd_layer_effects_shadow_base * { return s->innerGlow(); }, false},
        {i18nc("layer style", "Bevel"), [](KisPSDLayerStyle *s) -> psd_layer_effects_shadow_base * { return s->bevelAndEmboss(); }, false},
    };
}

constexpr QSize tileSize(72, 56);

/// The layer, small, with only \p kind of \p style on it, sized to show at that size, on light paper
QImage picture(const QImage &content, KisPSDLayerStyleSP style, int kind)
{
    const KoColorSpace *space = KoColorSpaceRegistry::instance()->rgb8();
    KisImageSP image = new KisImage(nullptr, tileSize.width(), tileSize.height(), space, QStringLiteral("style picture"));
    KisPaintDeviceSP device = new KisPaintDevice(space);
    device->convertFromQImage(content, nullptr);
    KisPaintLayerSP layer = new KisPaintLayer(image, QStringLiteral("layer"), OPACITY_OPAQUE_U8, device);
    image->addNode(layer, image->root());
    KisPSDLayerStyleSP shown = style->clone().dynamicCast<KisPSDLayerStyle>();
    const QVector<Kind> all = kinds();
    for (int i = 0; i < all.size(); i++) all[i].of(shown.data())->setEffectEnabled(i == kind);
    psd_layer_effects_shadow_base *effect = all[kind].of(shown.data());
    effect->setSize(4);
    if (all[kind].distance) effect->setDistance(3);
    shown->setEnabled(true);
    layer->setLayerStyle(shown);
    image->initialRefreshGraph();
    image->waitForDone();
    QImage made(tileSize, QImage::Format_ARGB32_Premultiplied);
    made.fill(QColor(232, 232, 232));
    QPainter painter(&made);
    painter.drawImage(0, 0, image->projection()->convertToQImage(nullptr, 0, 0, tileSize.width(), tileSize.height()));
    return made;
}

/// What the tiles show the styles on: the layer's own paint, or a rounded shape where it has none
QImage contentOf(KisLayerSP layer)
{
    QImage content(tileSize, QImage::Format_ARGB32);
    content.fill(Qt::transparent);
    QPainter painter(&content);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QRect room = QRect(QPoint(0, 0), tileSize).adjusted(14, 12, -14, -12);
    KisPaintDeviceSP paint = layer->paintDevice();
    QImage small;
    if (paint && !paint->exactBounds().isEmpty()) {
        small = paint->createThumbnail(room.width(), room.height(), Qt::KeepAspectRatio, KisThumbnailBoundsMode::Coarse);
    }
    // thin lines this small show no style: then a shape stands in for the layer
    int covered = 0;
    for (int y = 0; y < small.height(); y++) {
        for (int x = 0; x < small.width(); x++) covered += qAlpha(small.pixel(x, y)) > 128;
    }
    if (covered > small.width() * small.height() / 6) {
        painter.drawImage(room.center() - QPoint(small.width() / 2, small.height() / 2), small);
    } else {
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(120, 160, 220));
        painter.drawPath(KisModernStyle::squircle(room, KisModernStyle::tileRadius));
    }
    return content;
}

class StylesPopup : public KisStudioPopup
{
public:
    StylesPopup(KisViewManager *viewManager, KisLayerSP layer, QWidget *parent)
        : KisStudioPopup(parent)
        , m_view(viewManager)
        , m_layer(layer)
        , m_resources(viewManager->canvasBase()->resourceManager()->canvasResourcesInterface())
    {
        setAttribute(Qt::WA_DeleteOnClose);
        if (layer->layerStyle()) {
            m_before = layer->layerStyle()->clone().dynamicCast<KisPSDLayerStyle>();
        } else {
            m_before = toQShared(new KisPSDLayerStyle(QString(), KisGlobalResourcesInterface::instance()))
                           ->cloneWithResourcesSnapshot(KisGlobalResourcesInterface::instance(), m_resources);
        }
        m_style = m_before->clone().dynamicCast<KisPSDLayerStyle>();
        // a new style of its own: the same one would be taken for the old one when saved
        m_style->setUuid(QUuid::createUuid());
        m_content = contentOf(layer);

        auto *layout = new QVBoxLayout(this);
        fitLayout(layout, KisModernStyle::space2);
        layout->setSpacing(KisModernStyle::space2);
        auto *grid = new QGridLayout();
        grid->setSpacing(KisModernStyle::space1);
        const QVector<Kind> all = kinds();
        for (int i = 0; i < all.size(); i++) {
            auto *tile = new QToolButton(this);
            tile->setCheckable(true);
            tile->setAutoRaise(true);
            tile->setToolTip(all[i].name);
            tile->setIconSize(tileSize);
            tile->setChecked(all[i].of(m_style.data())->effectEnabled());
            connect(tile, &QToolButton::clicked, this, [this, i]() { pressTile(i); });
            grid->addWidget(tile, i / 3, i % 3);
            m_tiles << tile;
        }
        layout->addLayout(grid);

        m_size = new KisStudioValueRow(i18nc("layer style", "Size"), this);
        m_size->setRange({0, 250, 0, i18nc("pixels", " px"), true});
        m_distance = new KisStudioValueRow(i18nc("layer style, how far a shadow falls", "Distance"), this);
        m_distance->setRange({0, 250, 0, i18nc("pixels", " px"), true});
        m_opacity = new KisStudioValueRow(i18nc("layer style", "Opacity"), this);
        m_opacity->setRange({0, 100, 0, QStringLiteral("%")});
        for (KisStudioValueRow *row : {m_size, m_distance, m_opacity}) {
            connect(row, &KisStudioValueRow::valueChanged, this, [this](qreal) { fromRows(); });
            layout->addWidget(row);
        }
        auto *bottom = new QHBoxLayout();
        m_color = new QToolButton(this);
        m_color->setAutoRaise(true);
        m_color->setToolTip(i18nc("@info:tooltip a layer style's color", "Take the current color"));
        connect(m_color, &QToolButton::clicked, this, [this]() {
            psd_layer_effects_shadow_base *effect = current();
            if (!effect || !m_view) return;
            effect->setColor(m_view->canvasResourceProvider()->fgColor());
            showValues();
            changed();
        });
        bottom->addWidget(m_color);
        bottom->addStretch(1);
        auto *more = new QToolButton(this);
        more->setAutoRaise(true);
        more->setText(i18nc("@action:button every layer style setting, in Krita's dialog", "More…"));
        connect(more, &QToolButton::clicked, this, [this]() {
            QPointer<KisViewManager> view = m_view;
            close();
            QTimer::singleShot(0, [view]() {
                if (!view || !view->actionManager()->actionByName(QStringLiteral("layer_style"))) return;
                classic = true;
                view->actionManager()->actionByName(QStringLiteral("layer_style"))->trigger();
                classic = false;
            });
        });
        bottom->addWidget(more);
        layout->addLayout(bottom);

        m_render.setSingleShot(true);
        m_render.setInterval(60);
        connect(&m_render, &QTimer::timeout, this, [this]() { paintTiles(); });
        for (int i = 0; i < all.size(); i++) {
            if (all[i].of(m_style.data())->effectEnabled()) {
                m_kind = i;
                break;
            }
        }
        paintTiles();
        showValues();
    }

    ~StylesPopup() override
    {
        // the changes made here, one step to undo
        if (!m_layer || !m_changed) return;
        KisImageSP image = m_layer->image();
        if (!image) return;
        KisPSDLayerStyleSP after = m_style->cloneWithResourcesSnapshot(KisGlobalResourcesInterface::instance(), m_resources);
        image->postExecutionUndoAdapter()->addCommand(toQShared(new KisSetLayerStyleCommand(m_layer, m_before, after)));
    }

private:
    psd_layer_effects_shadow_base *current() const
    {
        return m_kind >= 0 ? kinds()[m_kind].of(m_style.data()) : nullptr;
    }

    void pressTile(int kind)
    {
        psd_layer_effects_shadow_base *effect = kinds()[kind].of(m_style.data());
        const bool off = effect->effectEnabled() && kind == m_kind;
        effect->setEffectEnabled(!off);
        if (!off && kind == 2 && !m_outlineColored && m_view) {
            // an outline is drawn in the color at hand
            effect->setColor(m_view->canvasResourceProvider()->fgColor());
            m_outlineColored = true;
        }
        m_kind = kind;
        m_style->setEnabled(true);
        showValues();
        changed();
    }

    void showValues()
    {
        const QVector<Kind> all = kinds();
        for (int i = 0; i < m_tiles.size(); i++) {
            m_tiles[i]->setChecked(all[i].of(m_style.data())->effectEnabled());
        }
        psd_layer_effects_shadow_base *effect = current();
        for (KisStudioValueRow *row : {m_size, m_distance, m_opacity}) row->setVisible(effect);
        m_color->setVisible(effect);
        if (!effect) return;
        m_updating = true;
        m_size->setValue(effect->size());
        m_distance->setVisible(all[m_kind].distance);
        m_distance->setValue(effect->distance());
        m_opacity->setValue(effect->opacity());
        for (KisStudioValueRow *row : {m_size, m_distance, m_opacity}) row->setInactive(!effect->effectEnabled());
        m_updating = false;
        QPixmap swatch(QSize(20, 20) * devicePixelRatioF());
        swatch.setDevicePixelRatio(devicePixelRatioF());
        swatch.fill(Qt::transparent);
        QPainter painter(&swatch);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(QPen(palette().color(QPalette::WindowText), 1));
        painter.setBrush(effect->color().toQColor());
        painter.drawPath(KisModernStyle::squircle(QRectF(1.5, 1.5, 17, 17), KisModernStyle::cornerRadius));
        painter.end();
        m_color->setIcon(QIcon(swatch));
        adjustSize();
    }

    void fromRows()
    {
        psd_layer_effects_shadow_base *effect = current();
        if (m_updating || !effect) return;
        effect->setSize(qRound(m_size->value()));
        effect->setDistance(qRound(m_distance->value()));
        effect->setOpacity(qRound(m_opacity->value()));
        if (!effect->effectEnabled()) {
            effect->setEffectEnabled(true);
            showValues();
        }
        changed();
    }

    void changed()
    {
        m_changed = true;
        if (m_layer) {
            KisSetLayerStyleCommand::updateLayerStyle(m_layer, m_style->cloneWithResourcesSnapshot(KisGlobalResourcesInterface::instance(), m_resources));
        }
        m_render.start();
    }

    void paintTiles()
    {
        for (int i = 0; i < m_tiles.size(); i++) {
            m_tiles[i]->setIcon(QIcon(QPixmap::fromImage(picture(m_content, m_style, i))));
        }
    }

    QPointer<KisViewManager> m_view;
    KisLayerSP m_layer;
    KoCanvasResourcesInterfaceSP m_resources;
    KisPSDLayerStyleSP m_before;
    KisPSDLayerStyleSP m_style;
    QImage m_content;
    QVector<QToolButton *> m_tiles;
    KisStudioValueRow *m_size {nullptr};
    KisStudioValueRow *m_distance {nullptr};
    KisStudioValueRow *m_opacity {nullptr};
    QToolButton *m_color {nullptr};
    QTimer m_render;
    int m_kind {-1};
    bool m_updating {false};
    bool m_changed {false};
    bool m_outlineColored {false};
};
} // namespace

bool KisStudioLayerStyles::open(KisViewManager *viewManager, KisLayerSP layer)
{
    if (classic || !viewManager || !layer || !KisStudioLayout::isEnabledInConfig() || !viewManager->canvasBase()) return false;
    auto *popup = new StylesPopup(viewManager, layer, viewManager->mainWindowAsQWidget());
    popup->adjustSize();
    popup->popupAt(QCursor::pos());
    return true;
}
