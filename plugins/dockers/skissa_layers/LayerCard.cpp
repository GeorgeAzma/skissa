/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "LayerCard.h"

#include <QAbstractButton>
#include <QHBoxLayout>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QTimer>
#include <QVBoxLayout>

#include <klocalizedstring.h>

#include <KisModernStyle.h>
#include <kis_clipboard.h>
#include <kis_icon.h>
#include <kis_layer.h>
#include <kis_node_view_color_scheme.h>

#include "KisViewManager.h"
#include "canvas/kis_canvas2.h"
#include "kis_action_manager.h"
#include "kis_node_manager.h"
#include "kis_node_model.h"

#include "LayerBox.h"

namespace {

/// The everyday commands' tiles, square
constexpr int tileSide = 38;
constexpr int tileCount = 6;
constexpr int contentWidth = tileCount * tileSide + (tileCount - 1) * KisModernStyle::hairline;

QColor inkAt(const QWidget *widget, qreal alpha)
{
    QColor ink = widget->palette().color(QPalette::WindowText);
    ink.setAlphaF(alpha);
    return ink;
}

/// A button drawn by its own paint, with a soft fill under the pointer
class Flat : public QAbstractButton
{
public:
    explicit Flat(QWidget *parent)
        : QAbstractButton(parent)
    {
        setAttribute(Qt::WA_Hover);
        setFocusPolicy(Qt::NoFocus);
    }

protected:
    void paintBackground(QPainter &painter)
    {
        painter.setRenderHint(QPainter::Antialiasing, true);
        if (!isEnabled() || !(isDown() || underMouse())) return;
        painter.setPen(Qt::NoPen);
        painter.setBrush(inkAt(this, isDown() ? 0.14 : 0.08));
        painter.drawPath(KisModernStyle::squircle(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), KisModernStyle::tileRadius));
    }
};

/// An everyday command: its icon alone, its name in the tooltip
class Tile : public Flat
{
public:
    explicit Tile(QWidget *parent)
        : Flat(parent)
    {
        setFixedSize(tileSide, tileSide);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        if (!isEnabled()) painter.setOpacity(0.3);
        paintBackground(painter);
        const int icon = 22;
        this->icon().paint(&painter, QRect((width() - icon) / 2, (height() - icon) / 2, icon, icon));
    }
};

/// A rarer command: its icon and its words, and a chevron when it opens a menu
class Row : public Flat
{
public:
    Row(const QString &icon, const QString &text, bool opensMenu, QWidget *parent)
        : Flat(parent)
        , m_opensMenu(opensMenu)
    {
        setIcon(KisIconUtils::loadIcon(icon));
        setText(text);
        setFixedHeight(30);
    }

    QSize sizeHint() const override { return QSize(contentWidth, 30); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        if (!isEnabled()) painter.setOpacity(0.3);
        paintBackground(painter);
        const int icon = 18;
        const int left = KisModernStyle::space2;
        this->icon().paint(&painter, QRect(left, (height() - icon) / 2, icon, icon));
        const QColor ink = palette().color(QPalette::WindowText);
        painter.setPen(ink);
        painter.drawText(rect().adjusted(left + icon + KisModernStyle::space2, 0, 0, 0), Qt::AlignLeft | Qt::AlignVCenter, text());
        if (m_opensMenu) {
            const QPointF tip(width() - KisModernStyle::space2 - 3, height() / 2.0);
            QPen pen(KisModernStyle::secondaryInk(ink), 1.5);
            pen.setCapStyle(Qt::RoundCap);
            pen.setJoinStyle(Qt::RoundJoin);
            painter.setPen(pen);
            painter.drawPolyline(QPolygonF({tip + QPointF(-3.5, -4), tip, tip + QPointF(-3.5, 4)}));
        }
    }

private:
    bool m_opensMenu {false};
};

/// A color label: the color, or for none an empty outline; the layer's own ringed
class Label : public Flat
{
public:
    Label(const QColor &color, QWidget *parent)
        : Flat(parent)
        , m_color(color)
    {
        setFixedHeight(24);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setMinimumWidth(20);
    }

    void setCurrent(bool current)
    {
        if (m_current == current) return;
        m_current = current;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const qreal side = underMouse() ? 14 : 12;
        QRectF swatch(0, 0, side, side);
        swatch.moveCenter(QRectF(rect()).center());
        const qreal radius = KisModernStyle::cornerRadius - 1;
        if (m_color.alpha() == 0) {
            painter.setPen(QPen(inkAt(this, underMouse() ? 0.6 : 0.4), 1.2));
            painter.setBrush(Qt::NoBrush);
            painter.drawPath(KisModernStyle::squircle(swatch.adjusted(0.6, 0.6, -0.6, -0.6), radius));
        } else {
            painter.setPen(Qt::NoPen);
            painter.setBrush(m_color);
            painter.drawPath(KisModernStyle::squircle(swatch, radius));
        }
        if (m_current) {
            QRectF ring(0, 0, 20, 20);
            ring.moveCenter(swatch.center());
            painter.setPen(QPen(palette().color(QPalette::WindowText), 1.5));
            painter.setBrush(Qt::NoBrush);
            painter.drawPath(KisModernStyle::squircle(ring, radius + 3));
        }
    }

private:
    QColor m_color;
    bool m_current {false};
};

}

struct LayerCard::Private
{
    QPointer<LayerBox> box;
    KisNodeSP node;
    QVector<QPair<Tile *, QPointer<QAction>>> tiles;
    QVector<Label *> labels;
    QTimer later;

    QAction *action(const QString &id) const
    {
        return box && box->m_canvas ? box->m_canvas->viewManager()->actionManager()->actionByName(id) : nullptr;
    }
};

LayerCard::LayerCard(LayerBox *box, KisNodeSP node)
    : KisStudioPopup(box)
    , d(new Private)
{
    d->box = box;
    d->node = node;

    auto *layout = new QVBoxLayout(this);
    fitLayout(layout, KisModernStyle::space2);
    layout->setSpacing(KisModernStyle::space1);
    setFixedWidth(contentWidth + 2 * (shadowMargin() + KisModernStyle::space2));
    setFocusPolicy(Qt::StrongFocus);

    // a layer's own: the everyday commands, its color label
    if (node) {
        // the everyday commands, always in the same places
        auto *tiles = new QHBoxLayout();
        tiles->setSpacing(KisModernStyle::hairline);
        auto addTile = [&](const QString &icon, const QString &name, QAction *action) {
            auto *tile = new Tile(this);
            tile->setIcon(KisIconUtils::loadIcon(icon));
            tile->setToolTip(name);
            // its key shows in the tooltip
            if (action) tile->setProperty("runsAction", action->objectName());
            QPointer<QAction> target(action);
            connect(tile, &QAbstractButton::clicked, this, [this, target]() {
                // closed first: what it runs may open a dialog or take the layer away
                close();
                if (target) target->trigger();
            });
            d->tiles.append({tile, target});
            tiles->addWidget(tile);
        };
        addTile(QStringLiteral("studio_duplicate"), i18nc("@info:tooltip a layer's command", "Duplicate"), d->action(QStringLiteral("duplicatelayer")));
        addTile(QStringLiteral("studio_merge_down"), i18nc("@info:tooltip a layer's command", "Merge down"), d->action(QStringLiteral("merge_layer")));
        const bool group = node->inherits("KisGroupLayer") && box->m_nodeManager->selectedNodes().size() <= 1;
        addTile(group ? QStringLiteral("studio_ungroup") : QStringLiteral("studio_group"),
                group ? i18nc("@info:tooltip a layer's command", "Ungroup") : i18nc("@info:tooltip a layer's command", "Group"),
                d->action(group ? QStringLiteral("quick_ungroup") : QStringLiteral("create_quick_group")));
        addTile(QStringLiteral("studio_copy"), i18nc("@info:tooltip a layer's command", "Copy"), d->action(QStringLiteral("copy_layer_clipboard")));
        addTile(QStringLiteral("studio_paste"), i18nc("@info:tooltip a layer's command", "Paste"), d->action(QStringLiteral("paste_layer_from_clipboard")));
        addTile(QStringLiteral("studio_delete"), i18nc("@info:tooltip a layer's command", "Delete"), box->m_removeAction);
        layout->addLayout(tiles);

        // its color label
        auto *labels = new QHBoxLayout();
        labels->setSpacing(0);
        const QVector<QColor> colors = KisNodeViewColorScheme::instance()->allColorLabels();
        for (int i = 0; i < colors.size(); i++) {
            auto *label = new Label(colors[i], this);
            connect(label, &QAbstractButton::clicked, this, [this, i]() {
                if (d->box) d->box->slotColorLabelChanged(i);
                refresh();
            });
            d->labels << label;
            labels->addWidget(label);
        }
        layout->addLayout(labels);

        auto *line = new QWidget(this);
        line->setFixedHeight(1);
        line->setAutoFillBackground(true);
        QPalette linePalette = line->palette();
        linePalette.setColor(QPalette::Window, inkAt(this, 0.1));
        line->setPalette(linePalette);
        layout->addSpacing(KisModernStyle::hairline);
        layout->addWidget(line);
        layout->addSpacing(KisModernStyle::hairline);
    }

    // the rarer ones: a command, or a menu opening beside the row
    auto addRow = [&](const QString &icon, const QString &text, std::function<void(QMenu *)> fill, std::function<void(QWidget *)> run = nullptr) {
        auto *row = new Row(icon, text, bool(fill), this);
        connect(row, &QAbstractButton::clicked, this, [this, row, fill, run]() {
            if (!fill) {
                close();
                if (run) run(row);
                return;
            }
            if (!d->box) return;
            QMenu menu(d->box);
            fill(&menu);
            // run once the menu and the card are gone: a command may open a dialog or take the layer away
            QPointer<LayerCard> card(this);
            QList<QMenu *> menus{&menu};
            while (!menus.isEmpty()) {
                QMenu *each = menus.takeFirst();
                for (QAction *entry : each->actions()) {
                    if (entry->menu()) {
                        menus << entry->menu();
                        continue;
                    }
                    QPointer<QAction> action(qobject_cast<QAction *>(entry->property("kisShortcutTarget").value<QObject *>()));
                    if (!action) continue;
                    disconnect(entry, &QAction::triggered, nullptr, nullptr);
                    connect(entry, &QAction::triggered, d->box, [card, action]() {
                        if (card) card->close();
                        QTimer::singleShot(0, action, [action]() {
                            if (action) action->trigger();
                        });
                    });
                }
            }
            if (menu.exec(row->mapToGlobal(QPoint(row->width() + KisModernStyle::space1, 0))) && card) card->close();
        });
        layout->addWidget(row);
    };
    auto trigger = [this](const QString &id) {
        QPointer<QAction> action(d->action(id));
        return [action](QWidget *) {
            if (action) action->trigger();
        };
    };
    addRow(QStringLiteral("studio_add"), i18nc("@action:button new layers, in a layer's card", "New"), [this](QMenu *menu) {
        d->box->fillNewLayerMenu(menu, bool(d->node));
    });
    if (!node) {
        // empty rows: what concerns the whole list
        addRow(QStringLiteral("studio_paste"), i18nc("@action:button layers, in the layer list's card", "Paste"), nullptr,
               trigger(QStringLiteral("paste_layer_from_clipboard")));
        addRow(QStringLiteral("studio_find"), i18nc("@action:button in the layer list's card", "Find a layer"), nullptr, [this](QWidget *) {
            if (d->box) d->box->startFiltering(QString());
        });
        addRow(QStringLiteral("studio_layers"), i18nc("@action:button in the layer list's card", "Select layers"), [this](QMenu *menu) {
            LayerBox *box = d->box;
            box->addMenuEntry(menu, "select_all_layers", i18nc("@action:inmenu select layers", "All"));
            box->addMenuEntry(menu, "select_visible_layers", i18nc("@action:inmenu select layers", "Visible"));
            box->addMenuEntry(menu, "select_invisible_layers", i18nc("@action:inmenu select layers", "Hidden"));
            box->addMenuEntry(menu, "select_locked_layers", i18nc("@action:inmenu select layers", "Locked"));
            box->addMenuEntry(menu, "select_unlocked_layers", i18nc("@action:inmenu select layers", "Unlocked"));
        });
        // the list's own look, in the menu it has always been
        addRow(QStringLiteral("studio_settings"), i18nc("@action:button the layer list's look", "List"), nullptr, [this](QWidget *row) {
            if (d->box) d->box->m_configureMenu->popup(row->mapToGlobal(QPoint(row->width() + KisModernStyle::space1, 0)));
        });
    } else {
        addRow(QStringLiteral("studio_select"), i18nc("@action:button a layer's command", "Select its pixels"), nullptr, trigger(QStringLiteral("selectopaque")));
    }
    if (qobject_cast<KisLayer *>(node.data())) {
        addRow(QStringLiteral("studio_effects"), i18nc("@action:button a layer's menu", "Style"), [this](QMenu *menu) {
            LayerBox *box = d->box;
            KisLayer *layer = qobject_cast<KisLayer *>(d->node.data());
            box->addMenuEntry(menu, "layer_style", i18nc("@action:inmenu layer style", "Edit…"));
            if (layer && layer->layerStyle()) box->addMenuEntry(menu, "copy_layer_style", i18nc("@action:inmenu layer style", "Copy"));
            if (KisClipboard::instance()->hasLayerStyles()) box->addMenuEntry(menu, "paste_layer_style", i18nc("@action:inmenu layer style", "Paste"));
        });
    }
    if (node) addRow(QStringLiteral("studio_more"), i18nc("@action:button a layer's menu", "More"), [this](QMenu *menu) {
        LayerBox *box = d->box;
        box->addMenuEntry(menu, "create_quick_clipping_group", i18nc("@action:inmenu", "Clipping group"));
        box->addMenuEntry(menu, "flatten_layer", i18nc("@action:inmenu", "Flatten layer"));
        box->addMenuEntry(menu, "flatten_image", i18nc("@action:inmenu", "Flatten image"));
        if (d->node->inherits("KisCloneLayer")) box->addMenuEntry(menu, box->m_changeCloneSourceAction, i18nc("@action:inmenu", "Clone source…"));
        QMenu *convert = menu->addMenu(i18nc("@title:menu", "Convert"));
        box->addMenuEntry(convert, "convert_to_paint_layer", i18nc("@action:inmenu convert to", "To paint layer"), true);
        box->addMenuEntry(convert, "convert_to_transparency_mask", i18nc("@action:inmenu convert to", "To transparency mask"), true);
        box->addMenuEntry(convert, "convert_to_filter_mask", i18nc("@action:inmenu convert to", "To filter mask"), true);
        box->addMenuEntry(convert, "convert_to_selection_mask", i18nc("@action:inmenu convert to", "To local selection"), true);
        box->addMenuEntry(convert, "convert_to_file_layer", i18nc("@action:inmenu convert to", "To file layer"), true);
        box->addMenuEntry(convert, "convert_group_to_animated", i18nc("@action:inmenu convert to", "To animated layer"), true);
        box->addMenuEntry(convert, "layercolorspaceconversion", i18nc("@action:inmenu", "Color space…"), true);
        convert->addSeparator();
        box->addMenuEntry(convert, "split_alpha_into_mask", i18nc("@action:inmenu", "Alpha into mask"), true);
        box->addMenuEntry(convert, "split_alpha_write", i18nc("@action:inmenu", "Write as alpha"), true);
        box->addMenuEntry(convert, "split_alpha_save_merged", i18nc("@action:inmenu", "Save merged…"), true);
        QMenu *select = menu->addMenu(i18nc("@title:menu choosing layers", "Select layers"));
        box->addMenuEntry(select, "select_all_layers", i18nc("@action:inmenu select layers", "All"));
        box->addMenuEntry(select, "select_visible_layers", i18nc("@action:inmenu select layers", "Visible"));
        box->addMenuEntry(select, "select_invisible_layers", i18nc("@action:inmenu select layers", "Hidden"));
        box->addMenuEntry(select, "select_locked_layers", i18nc("@action:inmenu select layers", "Locked"));
        box->addMenuEntry(select, "select_unlocked_layers", i18nc("@action:inmenu select layers", "Unlocked"));
        menu->addSeparator();
        if (!d->node->inherits("KisTransformMask")) {
            box->addMenuEntry(menu, "isolate_active_layer", i18nc("@action:inmenu", "Isolate layer"));
            box->addMenuEntry(menu, "isolate_active_group", i18nc("@action:inmenu", "Isolate group"));
        }
        box->addMenuEntry(menu, "pin_to_timeline", i18nc("@action:inmenu", "Pin to timeline"));
    });

    // what changes the layer elsewhere (a key, its row) shows here too, once things settle
    d->later.setSingleShot(true);
    d->later.setInterval(40);
    connect(&d->later, &QTimer::timeout, this, [this]() { refresh(); });
    if (node) connect(box->m_nodeModel, &QAbstractItemModel::dataChanged, this, [this]() { d->later.start(); });
    refresh();
}

LayerCard::~LayerCard()
{
    delete d;
}

void LayerCard::refresh()
{
    if (!d->node || !d->box) return;
    for (const auto &tile : qAsConst(d->tiles)) {
        tile.first->setEnabled(tile.second && tile.second->isEnabled());
    }
    for (int i = 0; i < d->labels.size(); i++) {
        d->labels[i]->setCurrent(d->node->colorLabelIndex() == i);
    }
}

void LayerCard::popupBesideRow(const QRect &row, QWidget *panel)
{
    adjustSize();
    // beside the panel toward the window's middle, its top level with the row
    const QRect around(panel->mapToGlobal(QPoint(0, 0)), panel->size());
    const QWidget *window = panel->window();
    const int middle = window->mapToGlobal(window->rect().center()).x();
    const int width = this->width() - 2 * shadowMargin();
    const bool right = around.center().x() < middle;
    const QPoint at(right ? around.right() + 1 + KisModernStyle::space2 : around.left() - KisModernStyle::space2 - width, row.top());
    popupAt(at);
    setFocus();
}
