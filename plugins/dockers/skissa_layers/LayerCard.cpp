/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "LayerCard.h"

#include <QAbstractButton>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QTimer>
#include <QVBoxLayout>

#include <klocalizedstring.h>

#include <KisModernStyle.h>
#include <KisSentenceCase.h>
#include <KoCanvasResourceProvider.h>
#include <KoCompositeOpRegistry.h>
#include <kis_clipboard.h>
#include <kis_config.h>
#include <kis_group_layer.h>
#include <kis_icon.h>
#include <kis_layer.h>
#include <kis_layer_properties_icons.h>
#include <kis_node_view_color_scheme.h>

#include "KisBlendModePicker.h"
#include "KisViewManager.h"
#include "canvas/kis_canvas2.h"
#include "kis_action_manager.h"
#include "kis_node_manager.h"
#include "kis_node_model.h"
#include "widgets/KisStudioValueRow.h"
#include "widgets/kis_cmb_composite.h"

#include "LayerBox.h"
#include "ui_WdgLayerBox.h"

namespace {

/// Content width of the card, inside its padding
constexpr int contentWidth = 340;

QColor inkAt(const QWidget *widget, qreal alpha)
{
    QColor ink = widget->palette().color(QPalette::WindowText);
    ink.setAlphaF(alpha);
    return ink;
}

QFont smaller(QFont font)
{
    font.setPointSizeF(font.pointSizeF() * 0.9);
    return font;
}

/// \p image filling \p rect's squircle, its see-through parts over the canvas's checkers; \p fit: all of it shown
void paintPicture(QPainter &painter, const QWidget *widget, const QRectF &rect, const QImage &image, bool fit)
{
    const QPainterPath shape = KisModernStyle::squircle(rect, KisModernStyle::tileRadius);
    painter.fillPath(shape, inkAt(widget, 0.06));
    if (image.isNull()) return;
    painter.save();
    painter.setClipPath(shape);
    QSizeF size = QSizeF(image.size()) / image.devicePixelRatio();
    size.scale(rect.size(), fit ? Qt::KeepAspectRatio : Qt::KeepAspectRatioByExpanding);
    QRectF at(QPointF(), size);
    at.moveCenter(rect.center());
    painter.setClipRect(at, Qt::IntersectClip);
    KisConfig cfg(true);
    const int cell = 5;
    for (int y = 0; at.top() + y * cell < at.bottom(); y++) {
        for (int x = 0; at.left() + x * cell < at.right(); x++) {
            painter.fillRect(QRectF(at.left() + x * cell, at.top() + y * cell, cell, cell), (x + y) % 2 ? cfg.checkersColor2() : cfg.checkersColor1());
        }
    }
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.drawImage(at, image);
    painter.restore();
}

/// The layer's picture, large
class Picture : public QWidget
{
public:
    Picture(int side, QWidget *parent)
        : QWidget(parent)
    {
        setFixedSize(side, side);
    }

    void setImage(const QImage &image)
    {
        m_image = image;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        paintPicture(painter, this, QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), m_image, true);
    }

private:
    QImage m_image;
};

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
    void paintBackground(QPainter &painter, const QColor &lit = QColor())
    {
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QColor fill = lit.isValid() ? lit : isDown() ? inkAt(this, 0.14) : underMouse() && isEnabled() ? inkAt(this, 0.08) : QColor();
        if (!fill.isValid()) return;
        painter.setPen(Qt::NoPen);
        painter.setBrush(fill);
        painter.drawPath(KisModernStyle::squircle(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), KisModernStyle::tileRadius));
    }
};

/// The blend mode: the layer over those below in it, and its name
class ModeTile : public Flat
{
public:
    using Flat::Flat;

    void setPicture(const QImage &picture)
    {
        m_picture = picture;
        update();
    }

    QSize sizeHint() const override { return QSize(contentWidth - 64 - KisModernStyle::space3, 36); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        paintBackground(painter);
        const int side = height() - 2 * KisModernStyle::hairline;
        paintPicture(painter, this, QRectF(KisModernStyle::hairline, KisModernStyle::hairline, side, side), m_picture, false);
        painter.setPen(palette().color(QPalette::WindowText));
        const QRect text = rect().adjusted(side + KisModernStyle::hairline + KisModernStyle::space2, 0, -KisModernStyle::space1, 0);
        painter.drawText(text, Qt::AlignLeft | Qt::AlignVCenter, fontMetrics().elidedText(this->text(), Qt::ElideRight, text.width()));
    }

private:
    QImage m_picture;
};

/// A layer's switch with its word, lit while on
class Switch : public Flat
{
public:
    Switch(const QString &text, const QString &onIcon, const QString &offIcon, QWidget *parent)
        : Flat(parent)
        , m_on(KisIconUtils::loadIcon(onIcon))
        , m_off(KisIconUtils::loadIcon(offIcon))
    {
        setText(text);
    }

    void setOn(bool on)
    {
        if (m_isOn == on) return;
        m_isOn = on;
        update();
    }

    QSize sizeHint() const override { return QSize(contentWidth / 2, 30); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        paintBackground(painter, m_isOn ? KisModernStyle::selectedRow(palette()) : QColor());
        if (m_isOn && underMouse()) paintBackground(painter, inkAt(this, 0.06));
        const int icon = 18;
        (m_isOn ? m_on : m_off).paint(&painter, QRect(KisModernStyle::space2, (height() - icon) / 2, icon, icon));
        painter.setPen(m_isOn ? KisModernStyle::selectedRowInk(palette()) : palette().color(QPalette::WindowText));
        painter.drawText(rect().adjusted(KisModernStyle::space2 + icon + KisModernStyle::space2, 0, 0, 0), Qt::AlignLeft | Qt::AlignVCenter, text());
    }

private:
    QIcon m_on;
    QIcon m_off;
    bool m_isOn {false};
};

/// An everyday command: its icon over its word
class Command : public Flat
{
public:
    using Flat::Flat;

    QSize sizeHint() const override { return QSize(56, 50); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        if (!isEnabled()) painter.setOpacity(0.35);
        paintBackground(painter);
        const int icon = 22;
        this->icon().paint(&painter, QRect((width() - icon) / 2, KisModernStyle::space1 + 2, icon, icon));
        painter.setFont(smaller(font()));
        painter.setPen(palette().color(QPalette::WindowText));
        const QRect text(0, icon + KisModernStyle::space1 + 2, width(), height() - icon - KisModernStyle::space1 - 2);
        painter.drawText(text, Qt::AlignHCenter | Qt::AlignVCenter, QFontMetrics(painter.font()).elidedText(this->text(), Qt::ElideRight, width() - 2));
    }
};

/// The rest: a word, opening its menu or running its command
class Quiet : public Flat
{
public:
    using Flat::Flat;

    QSize sizeHint() const override
    {
        return QSize(fontMetrics().horizontalAdvance(text()) + 2 * KisModernStyle::space2, 26);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        paintBackground(painter);
        painter.setPen(palette().color(QPalette::WindowText));
        painter.drawText(rect(), Qt::AlignCenter, text());
    }
};

/// A color label: the color, or for none an empty outline; the layer's own ringed
class Label : public Flat
{
public:
    Label(const QColor &color, QWidget *parent)
        : Flat(parent)
        , m_color(color)
    {
        setFixedSize(24, 24);
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
        const QRectF swatch = QRectF(rect()).adjusted(5, 5, -5, -5);
        const qreal radius = KisModernStyle::cornerRadius;
        if (m_color.alpha() == 0) {
            painter.setPen(QPen(inkAt(this, underMouse() ? 0.6 : 0.4), 1.2));
            painter.setBrush(Qt::NoBrush);
            painter.drawPath(KisModernStyle::squircle(swatch.adjusted(0.6, 0.6, -0.6, -0.6), radius));
        } else {
            painter.setPen(Qt::NoPen);
            painter.setBrush(m_color);
            painter.drawPath(KisModernStyle::squircle(underMouse() ? swatch.adjusted(-1, -1, 1, 1) : swatch, radius));
        }
        if (m_current) {
            painter.setPen(QPen(palette().color(QPalette::WindowText), 1.5));
            painter.setBrush(Qt::NoBrush);
            painter.drawPath(KisModernStyle::squircle(QRectF(rect()).adjusted(1.5, 1.5, -1.5, -1.5), radius + 3));
        }
    }

private:
    QColor m_color;
    bool m_current {false};
};

/// Whether the node's switches include \p id, and if so whether it is on
bool switchState(KisNodeSP node, const QString &id, bool *on)
{
    const KisBaseNode::PropertyList props = node->sectionModelProperties();
    for (const KisBaseNode::Property &prop : props) {
        if (prop.id == id && prop.isMutable) {
            *on = prop.state.toBool();
            return true;
        }
    }
    return false;
}

}

struct LayerCard::Private
{
    QPointer<LayerBox> box;
    KisNodeSP node;
    Picture *picture {nullptr};
    QLineEdit *name {nullptr};
    ModeTile *mode {nullptr};
    KisStudioValueRow *opacity {nullptr};
    QHash<QString, Switch *> switches;
    Command *group {nullptr};
    QVector<QPair<Command *, QPointer<QAction>>> commands;
    QVector<Label *> labels;
    KisBlendModePicker::Pictures pictures;
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
    KisLayer *layer = qobject_cast<KisLayer *>(node.data());

    auto *layout = new QVBoxLayout(this);
    fitLayout(layout, KisModernStyle::space3);
    layout->setSpacing(KisModernStyle::space2);
    setFixedWidth(contentWidth + 2 * (shadowMargin() + KisModernStyle::space3));
    setFocusPolicy(Qt::StrongFocus);

    // the picture, the name typed into, the blend mode
    auto *header = new QHBoxLayout();
    header->setSpacing(KisModernStyle::space3);
    d->picture = new Picture(64, this);
    header->addWidget(d->picture, 0, Qt::AlignTop);
    auto *beside = new QVBoxLayout();
    beside->setSpacing(KisModernStyle::hairline);
    d->name = new QLineEdit(this);
    d->name->setFrame(false);
    d->name->setFocusPolicy(Qt::ClickFocus);
    QFont nameFont = d->name->font();
    nameFont.setPointSizeF(nameFont.pointSizeF() * 1.08);
    nameFont.setWeight(QFont::DemiBold);
    d->name->setFont(nameFont);
    d->name->setToolTip(i18nc("@info:tooltip the layer's name in its card", "Name"));
    connect(d->name, &QLineEdit::editingFinished, this, [this]() {
        const QString typed = d->name->text().trimmed();
        if (d->box && d->box->m_nodeManager && !typed.isEmpty() && typed != d->node->name()) {
            d->box->m_nodeManager->setNodeName(d->node, typed);
        }
        if (typed.isEmpty()) d->name->setText(d->node->name());
        setFocus();
    });
    beside->addWidget(d->name);
    if (layer) {
        d->mode = new ModeTile(this);
        d->mode->setToolTip(i18nc("@info:tooltip a layer's blend mode in its card", "Blend mode"));
        connect(d->mode, &QAbstractButton::clicked, this, [this]() {
            if (d->box) d->box->m_wdgLayerBox->cmbComposite->showPopupBelow(d->mode);
        });
        beside->addWidget(d->mode);
        const QColor color = box->m_canvas && box->m_canvas->resourceManager() ? box->m_canvas->resourceManager()->foregroundColor().toQColor()
                                                                                : QColor(200, 85, 61);
        d->pictures = KisBlendModePicker::layerPictures(node, KisBlendModePicker::pictureSize(this), color);
    }
    beside->addStretch(1);
    header->addLayout(beside, 1);
    layout->addLayout(header);

    if (layer) {
        d->opacity = new KisStudioValueRow(i18nc("@label a layer's opacity in its card", "Opacity"), this);
        KisStudioValueRow::Range range;
        range.maximum = 100;
        range.suffix = QStringLiteral("%");
        d->opacity->setRange(range);
        d->opacity->setDefaultValue(100);
        // through the panel's own compressor, as the row's sideways drag
        connect(d->opacity, &KisStudioValueRow::valueChanged, this, [this](qreal value) {
            if (!d->box || !d->node->isEditable(false)) return;
            d->box->m_newOpacity = value;
            d->box->m_changedOpacityNode = d->node;
            d->box->m_nodeOpacityCompressor.start();
        });
        layout->addWidget(d->opacity);
    }

    // the switches this layer has
    auto *switches = new QGridLayout();
    switches->setSpacing(KisModernStyle::hairline);
    struct Kind {
        QString id;
        QString action;
        QString text;
        QString on;
        QString off;
    };
    const QVector<Kind> kinds = {
        {KisLayerPropertiesIcons::visible.id(), QStringLiteral("toggle_layer_visibility"), i18nc("@action:button a layer's switch", "Visible"),
         QStringLiteral("studio_visible"), QStringLiteral("studio_hidden")},
        {KisLayerPropertiesIcons::locked.id(), QStringLiteral("toggle_layer_lock"), i18nc("@action:button a layer's switch", "Locked"),
         QStringLiteral("studio_locked"), QStringLiteral("studio_unlocked")},
        {KisLayerPropertiesIcons::inheritAlpha.id(), QStringLiteral("toggle_layer_inherit_alpha"), i18nc("@action:button a layer's switch", "Clip"),
         QStringLiteral("studio_clip_on"), QStringLiteral("studio_clip")},
        {KisLayerPropertiesIcons::alphaLocked.id(), QStringLiteral("toggle_layer_alpha_lock"), i18nc("@action:button a layer's switch", "Alpha lock"),
         QStringLiteral("studio_alpha_lock"), QStringLiteral("studio_alpha_unlock")},
    };
    int at = 0;
    for (const Kind &kind : kinds) {
        bool on = false;
        if (!switchState(node, kind.id, &on)) continue;
        auto *button = new Switch(kind.text, kind.on, kind.off, this);
        QAction *action = d->action(kind.action);
        if (action && !action->shortcut().isEmpty()) button->setToolTip(action->shortcut().toString(QKeySequence::NativeText));
        connect(button, &QAbstractButton::clicked, this, [this, id = kind.action]() {
            if (QAction *action = d->action(id)) action->trigger();
        });
        d->switches.insert(kind.id, button);
        switches->addWidget(button, at / 2, at % 2);
        at++;
    }
    layout->addLayout(switches);
    layout->addSpacing(KisModernStyle::space1);

    // the everyday commands, always in the same places
    auto *commands = new QHBoxLayout();
    commands->setSpacing(KisModernStyle::hairline);
    auto addCommand = [&](const QString &icon, const QString &text, QAction *action) {
        auto *button = new Command(this);
        button->setIcon(KisIconUtils::loadIcon(icon));
        button->setText(text);
        // the longer words take the room the shorter ones leave
        button->setMinimumWidth(QFontMetrics(smaller(button->font())).horizontalAdvance(text) + KisModernStyle::space2);
        if (action && !action->shortcut().isEmpty()) button->setToolTip(action->shortcut().toString(QKeySequence::NativeText));
        QPointer<QAction> target(action);
        connect(button, &QAbstractButton::clicked, this, [this, target]() {
            // closed first: what it runs may open a dialog or take the layer away
            close();
            if (target) target->trigger();
        });
        d->commands.append({button, target});
        commands->addWidget(button, 1);
        return button;
    };
    addCommand(QStringLiteral("studio_duplicate"), i18nc("@action:button a layer's command", "Duplicate"), d->action(QStringLiteral("duplicatelayer")));
    addCommand(QStringLiteral("studio_merge_down"), i18nc("@action:button a layer's command", "Merge down"), d->action(QStringLiteral("merge_layer")));
    const bool group = node->inherits("KisGroupLayer") && box->m_nodeManager->selectedNodes().size() <= 1;
    addCommand(group ? QStringLiteral("studio_ungroup") : QStringLiteral("studio_group"),
               group ? i18nc("@action:button a layer's command", "Ungroup") : i18nc("@action:button a layer's command", "Group"),
               d->action(group ? QStringLiteral("quick_ungroup") : QStringLiteral("create_quick_group")));
    addCommand(QStringLiteral("studio_copy"), i18nc("@action:button a layer's command", "Copy"), d->action(QStringLiteral("copy_layer_clipboard")));
    addCommand(QStringLiteral("studio_delete"), i18nc("@action:button a layer's command", "Delete"), box->m_removeAction);
    layout->addLayout(commands);

    // the rest, in their menus
    auto *rest = new QGridLayout();
    rest->setSpacing(KisModernStyle::hairline);
    int restAt = 0;
    auto addQuiet = [&](const QString &text, std::function<void(QMenu *)> fill, QAction *run = nullptr) {
        auto *button = new Quiet(this);
        button->setText(text);
        QPointer<QAction> target(run);
        connect(button, &QAbstractButton::clicked, this, [this, button, fill, target]() {
            if (!fill) {
                close();
                if (target) target->trigger();
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
            if (menu.exec(button->mapToGlobal(QPoint(0, button->height()))) && card) card->close();
        });
        rest->addWidget(button, restAt / 4, restAt % 4);
        restAt++;
    };
    addQuiet(KisSentenceCase::of(i18nc("@action:button new layers, in a layer's card", "New")), [this](QMenu *menu) {
        d->box->fillNewLayerMenu(menu, true);
    });
    addQuiet(i18nc("@action:button a layer's command", "Paste"), nullptr, d->action(QStringLiteral("paste_layer_from_clipboard")));
    addQuiet(i18nc("@action:button a layer's menu", "Select"), [this](QMenu *menu) {
        LayerBox *box = d->box;
        box->addMenuEntry(menu, "selectopaque", i18nc("@action:inmenu", "Opaque pixels"));
        menu->addSeparator();
        box->addMenuEntry(menu, "select_all_layers", i18nc("@action:inmenu select layers", "All layers"));
        box->addMenuEntry(menu, "select_visible_layers", i18nc("@action:inmenu select layers", "Visible layers"));
        box->addMenuEntry(menu, "select_invisible_layers", i18nc("@action:inmenu select layers", "Hidden layers"));
        box->addMenuEntry(menu, "select_locked_layers", i18nc("@action:inmenu select layers", "Locked layers"));
        box->addMenuEntry(menu, "select_unlocked_layers", i18nc("@action:inmenu select layers", "Unlocked layers"));
    });
    if (layer) {
        addQuiet(i18nc("@action:button a layer's menu", "Style"), [this](QMenu *menu) {
            LayerBox *box = d->box;
            KisLayer *layer = qobject_cast<KisLayer *>(d->node.data());
            box->addMenuEntry(menu, "layer_style", i18nc("@action:inmenu layer style", "Edit…"));
            if (layer && layer->layerStyle()) box->addMenuEntry(menu, "copy_layer_style", i18nc("@action:inmenu layer style", "Copy"));
            if (KisClipboard::instance()->hasLayerStyles()) box->addMenuEntry(menu, "paste_layer_style", i18nc("@action:inmenu layer style", "Paste"));
        });
    }
    addQuiet(i18nc("@action:button a layer's menu", "Convert"), [this](QMenu *menu) {
        LayerBox *box = d->box;
        box->addMenuEntry(menu, "convert_to_paint_layer", i18nc("@action:inmenu convert to", "To paint layer"), true);
        box->addMenuEntry(menu, "convert_to_transparency_mask", i18nc("@action:inmenu convert to", "To transparency mask"), true);
        box->addMenuEntry(menu, "convert_to_filter_mask", i18nc("@action:inmenu convert to", "To filter mask"), true);
        box->addMenuEntry(menu, "convert_to_selection_mask", i18nc("@action:inmenu convert to", "To local selection"), true);
        box->addMenuEntry(menu, "convert_to_file_layer", i18nc("@action:inmenu convert to", "To file layer"), true);
        box->addMenuEntry(menu, "convert_group_to_animated", i18nc("@action:inmenu convert to", "To animated layer"), true);
        box->addMenuEntry(menu, "layercolorspaceconversion", i18nc("@action:inmenu", "Color space…"), true);
        menu->addSeparator();
        box->addMenuEntry(menu, "split_alpha_into_mask", i18nc("@action:inmenu", "Alpha into mask"), true);
        box->addMenuEntry(menu, "split_alpha_write", i18nc("@action:inmenu", "Write as alpha"), true);
        box->addMenuEntry(menu, "split_alpha_save_merged", i18nc("@action:inmenu", "Save merged…"), true);
    });
    addQuiet(i18nc("@action:button a layer's menu", "More"), [this](QMenu *menu) {
        LayerBox *box = d->box;
        box->addMenuEntry(menu, "create_quick_clipping_group", i18nc("@action:inmenu", "Clipping group"));
        box->addMenuEntry(menu, "flatten_layer", i18nc("@action:inmenu", "Flatten layer"));
        box->addMenuEntry(menu, "flatten_image", i18nc("@action:inmenu", "Flatten image"));
        if (d->node->inherits("KisCloneLayer")) box->addMenuEntry(menu, box->m_changeCloneSourceAction, i18nc("@action:inmenu", "Clone source…"));
        menu->addSeparator();
        if (!d->node->inherits("KisTransformMask")) {
            box->addMenuEntry(menu, "isolate_active_layer", i18nc("@action:inmenu", "Isolate layer"));
            box->addMenuEntry(menu, "isolate_active_group", i18nc("@action:inmenu", "Isolate group"));
        }
        box->addMenuEntry(menu, "pin_to_timeline", i18nc("@action:inmenu", "Pin to timeline"));
    });
    addQuiet(i18nc("@action:button a layer's command", "Properties…"), nullptr, box->m_propertiesAction);
    layout->addLayout(rest);

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
    labels->addStretch(1);
    layout->addLayout(labels);

    // what changes the layer (here, on its row, by a key) shows here too, once things settle
    d->later.setSingleShot(true);
    d->later.setInterval(40);
    connect(&d->later, &QTimer::timeout, this, [this]() { refresh(); });
    connect(box->m_nodeModel, &QAbstractItemModel::dataChanged, this, [this]() { d->later.start(); });
    connect(box->m_wdgLayerBox->cmbComposite, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() { d->later.start(); });
    refresh();
}

LayerCard::~LayerCard()
{
    delete d;
}

void LayerCard::refresh()
{
    KisNodeSP node = d->node;
    if (!node || !d->box) return;
    const qreal dpr = devicePixelRatioF();
    QImage thumbnail = node->createThumbnail(qRound(64 * dpr), qRound(64 * dpr), Qt::KeepAspectRatio);
    thumbnail.setDevicePixelRatio(dpr);
    d->picture->setImage(thumbnail);
    if (!d->name->hasFocus()) d->name->setText(node->name());

    if (d->mode) {
        const QString id = node->compositeOpId();
        d->mode->setText(KoCompositeOpRegistry::instance().getKoID(id).name());
        d->pictures.sourceOpacity = node->opacity() / 255.0;
        d->mode->setPicture(d->pictures.isValid() ? KisBlendModePicker::modePicture(d->pictures, id) : QImage());
    }
    // not while it is dragged: the layer gets the value a moment later
    if (d->opacity && QWidget::mouseGrabber() != d->opacity) d->opacity->setValue(qRound(node->opacity() * 100.0 / 255.0));
    for (auto it = d->switches.begin(); it != d->switches.end(); ++it) {
        bool on = false;
        if (switchState(node, it.key(), &on)) it.value()->setOn(on);
    }
    for (const auto &command : qAsConst(d->commands)) {
        command.first->setEnabled(command.second && command.second->isEnabled());
    }
    for (int i = 0; i < d->labels.size(); i++) {
        d->labels[i]->setCurrent(node->colorLabelIndex() == i);
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
