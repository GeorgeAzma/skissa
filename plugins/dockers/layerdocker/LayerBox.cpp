/*
 *  LayerBox.cc - part of Krita aka Krayon aka KimageShop
 *
 *  SPDX-FileCopyrightText: 2002 Patrick Julien <freak@codepimps.org>
 *  SPDX-FileCopyrightText: 2006 GÃ¡bor Lehel <illissius@gmail.com>
 *  SPDX-FileCopyrightText: 2007 Thomas Zander <zander@kde.org>
 *  SPDX-FileCopyrightText: 2007 Boudewijn Rempt <boud@valdyas.org>
 *  SPDX-FileCopyrightText: 2011 JosÃ© Luis Vergara <pentalis@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "LayerBox.h"
#include <KisFrameRate.h>

#include <KoCanvasResourceProvider.h>
#include "KisBlendModePicker.h"
#include "KisStudioBrushMemory.h"

#include <QLabel>
#include <QApplication>
#include <QToolButton>
#include <QLayout>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPoint>
#include <QRect>
#include <QString>
#include <QToolTip>
#include <QWidget>
#include <QComboBox>
#include <QCheckBox>
#include <QVBoxLayout>
#include <QPixmap>
#include <QBitmap>
#include <QList>
#include <QMenu>
#include <QWidgetAction>
#include <QPointer>
#include <QProxyStyle>
#include <QStyleFactory>

#include <kis_debug.h>
#include <klocalizedstring.h>

#include <KisScreenMigrationTracker.h>

#include <kis_icon.h>
#include <KoColorSpace.h>
#include <KoCompositeOpRegistry.h>
#include <KisDocument.h>
#include <kis_time_span.h>

#include <kis_types.h>
#include <kis_image.h>
#include <kis_paint_device.h>
#include <kis_layer.h>
#include <kis_group_layer.h>
#include <kis_mask.h>
#include <kis_node.h>
#include <kis_base_node.h>
#include <kis_composite_ops_model.h>
#include <kis_keyframe_channel.h>
#include <kis_image_animation_interface.h>
#include <KoProperties.h>

#include <kis_action.h>
#include "kis_action_manager.h"
#include "widgets/kis_cmb_composite.h"
#include "kis_slider_spin_box.h"
#include "KisViewManager.h"
#include "kis_node_manager.h"
#include "kis_node_model.h"
#include <kis_clipboard.h>

#include "canvas/kis_canvas2.h"
#include "kis_dummies_facade_base.h"
#include "kis_shape_controller.h"
#include "kis_selection_mask.h"
#include "kis_config.h"
#include "KisView.h"
#include "krita_utils.h"
#include "kis_color_label_selector_widget.h"
#include "kis_signals_blocker.h"
#include "kis_color_filter_combo.h"
#include "kis_node_filter_proxy_model.h"
#include <KisSpinBoxI18nHelper.h>

#include "kis_selection.h"
#include "kis_processing_applicator.h"
#include "commands/kis_set_global_selection_command.h"
#include "KisSelectionActionsAdapter.h"
#include "KisIdleTasksManager.h"

#include "kis_layer_utils.h"

#include "ui_WdgLayerBox.h"
#include "NodeView.h"
#include "SyncButtonAndAction.h"
#include "KisMenuStyleDontCloseOnAlt.h"
#include "KisModernStyle.h"
#include "KisTouchMode.h"
#include "NodeDelegate.h"
#include <QStyleOption>
#include <QTimer>

class LayerBoxStyle : public QProxyStyle
{
public:
    LayerBoxStyle(QStyle *baseStyle = 0) : QProxyStyle(baseStyle) {}

    void drawPrimitive(PrimitiveElement element,
                       const QStyleOption *option,
                       QPainter *painter,
                       const QWidget *widget) const override
    {
        if (element == QStyle::PE_IndicatorItemViewItemDrop)
        {
            QColor color(widget->palette().color(QPalette::Highlight).lighter());

            if (option->rect.height() == 0) {
                QBrush brush(color);

                QRect r(option->rect);
                r.setTop(r.top() - 2);
                r.setBottom(r.bottom() + 2);

                painter->fillRect(r, brush);
            } else {
                color.setAlpha(200);
                QBrush brush(color);
                painter->fillRect(option->rect, brush);
            }
        }
        else
        {
            QProxyStyle::drawPrimitive(element, option, painter, widget);
        }
    }
};

/// The place of the list in the panel's layout: what goes above it goes there
int listRow(Ui_WdgLayerBox *ui)
{
    for (int i = 0; i < ui->verticalLayout->count(); i++) {
        QLayoutItem *item = ui->verticalLayout->itemAt(i);
        if (item->widget() == ui->listLayers || (item->layout() && item->layout()->indexOf(ui->listLayers) >= 0)) {
            return i;
        }
    }
    return 0;
}

/// A layer's opacity while it is dragged sideways on its row: the row fills up to it, with the number
class OpacityScrub : public QWidget
{
public:
    explicit OpacityScrub(QWidget *parent) : QWidget(parent)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
    }

    void setValue(qreal percent)
    {
        m_percent = percent;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QRectF r = QRectF(rect()).adjusted(1, 1, -1, -1);
        QColor ink = palette().color(QPalette::Highlight);
        QColor wash = ink;
        wash.setAlphaF(0.28);
        QPainterPath clip;
        clip.addPath(KisModernStyle::squircle(r, 6));
        painter.setClipPath(clip);
        painter.fillRect(r, QColor(0, 0, 0, 60));
        const qreal edge = r.left() + r.width() * m_percent / 100.0;
        painter.fillRect(QRectF(r.left(), r.top(), edge - r.left(), r.height()), wash);
        painter.fillRect(QRectF(edge - 1, r.top(), 2, r.height()), ink);
        QFont font = this->font();
        font.setBold(true);
        painter.setFont(font);
        painter.setPen(palette().color(QPalette::BrightText).isValid() ? QColor(Qt::white) : palette().color(QPalette::WindowText));
        painter.drawText(r.adjusted(0, 0, -10, 0), Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("%1%").arg(qRound(m_percent)));
    }

private:
    qreal m_percent {100};
};

/// Where a click between two rows adds a layer: a line with a "+" at its start
class GapIndicator : public QWidget
{
public:
    explicit GapIndicator(QWidget *parent) : QWidget(parent)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QColor ink = palette().color(QPalette::Highlight);
        const qreal y = height() / 2.0;
        painter.fillRect(QRectF(16, y - 1, width() - 20, 2), ink);
        painter.setPen(Qt::NoPen);
        painter.setBrush(ink);
        painter.drawEllipse(QPointF(9, y), 5.5, 5.5);
        painter.setPen(QPen(palette().color(QPalette::HighlightedText), 1.4));
        painter.drawLine(QPointF(6.5, y), QPointF(11.5, y));
        painter.drawLine(QPointF(9, y - 2.5), QPointF(9, y + 2.5));
    }
};

/**
 * Above the list, what the "+" under it was: a click adds a layer above the
 * selected one, a right click (or holding it) offers the other kinds.
 */
class AddLayerStrip : public QAbstractButton
{
public:
    explicit AddLayerStrip(QWidget *parent) : QAbstractButton(parent)
    {
        setAttribute(Qt::WA_Hover);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::NoFocus);
        setContextMenuPolicy(Qt::CustomContextMenu);
        setText(i18nc("@action:button", "New layer"));
        setToolTip(i18nc("@info:tooltip", "New layer (right-click for other kinds)"));
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        // a finger needs more to hit
        auto fit = [this]() { setFixedHeight(KisTouchMode::isActive() ? 34 : 26); };
        fit();
        QObject::connect(KisTouchMode::instance(), &KisTouchMode::changed, this, fit);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        QColor ink = palette().color(QPalette::WindowText);
        if (underMouse() || isDown()) {
            QColor wash = ink;
            wash.setAlphaF(isDown() ? 0.12 : 0.06);
            painter.setPen(Qt::NoPen);
            painter.setBrush(wash);
            painter.drawPath(KisModernStyle::squircle(QRectF(rect()).adjusted(1, 1, -2, -1), 6));
        }
        ink.setAlphaF(underMouse() ? 0.85 : 0.55);
        const qreal y = height() / 2.0;
        QPen pen(ink, 1.5);
        pen.setCapStyle(Qt::RoundCap);
        painter.setPen(pen);
        painter.drawLine(QPointF(12, y), QPointF(22, y));
        painter.drawLine(QPointF(17, y - 5), QPointF(17, y + 5));
        painter.drawText(QRect(32, 0, width() - 36, height()), Qt::AlignLeft | Qt::AlignVCenter, text());
    }
};

inline void LayerBox::connectActionToButton(KisViewManager* viewManager, QAbstractButton *button, const QString &id)
{
    if (!viewManager || !button) return;

    KisAction *action = viewManager->actionManager()->actionByName(id);

    if (!action) return;

    connect(button, SIGNAL(clicked()), action, SLOT(trigger()));
    connect(action, SIGNAL(sigEnableSlaves(bool)), button, SLOT(setEnabled(bool)));
    connect(viewManager->mainWindowAsQWidget(), SIGNAL(themeChanged()), this, SLOT(slotUpdateIcons()));
}

inline void LayerBox::addActionToMenu(QMenu *menu, const QString &id)
{
    if (m_canvas) {
        menu->addAction(m_canvas->viewManager()->actionManager()->actionByName(id));
    }
}

qint32 LayerBox::convertOpacityToInt(qreal opacity)
{
    /**
     * Scales opacity from the range 0...100
     * to the integer range 0...255
     */

    return qMin(255, int(opacity * 2.55 + 0.5));
}

LayerBox::LayerBox()
    : QDockWidget(i18n("Layers"))
    , m_canvas(0)
    , m_wdgLayerBox(new Ui_WdgLayerBox)
    , m_colorLabelCompressor(500, KisSignalCompressor::FIRST_INACTIVE)
    , m_thumbnailSizeCompressor(100, KisSignalCompressor::FIRST_INACTIVE)
    , m_treeIndentationCompressor(100, KisSignalCompressor::FIRST_INACTIVE)
    , m_infoTextOpacityCompressor(100, KisSignalCompressor::FIRST_INACTIVE)
    , m_nodeOpacityCompressor(KisFrameRate::frameMs(), KisSignalCompressor::FIRST_ACTIVE, KisSignalCompressor::ADDITIVE_INTERVAL)
{
    KisConfig cfg(false);

    QWidget* mainWidget = new QWidget(this);
    setWidget(mainWidget);

    m_wdgLayerBox->setupUi(mainWidget);

    QStyle *newStyle = KisModernStyle::create(m_wdgLayerBox->listLayers->style()->objectName());
    // proxy style steals the ownership of the style and deletes it later
    LayerBoxStyle *proxyStyle = new LayerBoxStyle(newStyle);

    proxyStyle->setParent(m_wdgLayerBox->listLayers);
    m_wdgLayerBox->listLayers->setStyle(proxyStyle);

    connect(m_wdgLayerBox->listLayers,
            SIGNAL(contextMenuRequested(QPoint,QModelIndex)),
            this, SLOT(slotContextMenuRequested(QPoint,QModelIndex)));
    connect(m_wdgLayerBox->listLayers,
            SIGNAL(collapsed(QModelIndex)), SLOT(slotCollapsed(QModelIndex)));
    connect(m_wdgLayerBox->listLayers,
            SIGNAL(expanded(QModelIndex)), SLOT(slotExpanded(QModelIndex)));
    connect(m_wdgLayerBox->listLayers,
            SIGNAL(selectionChanged(QModelIndexList)), SLOT(selectionChanged(QModelIndexList)));

    slotUpdateIcons();

    m_wdgLayerBox->bnAdd->setIconSize(QSize(22, 22));
    m_wdgLayerBox->bnDelete->setIconSize(QSize(22, 22));
    m_wdgLayerBox->bnRaise->setIconSize(QSize(22, 22));
    m_wdgLayerBox->bnLower->setIconSize(QSize(22, 22));
    m_wdgLayerBox->bnProperties->setIconSize(QSize(22, 22));
    m_wdgLayerBox->bnDuplicate->setIconSize(QSize(22, 22));

    m_wdgLayerBox->bnLower->setEnabled(false);
    m_wdgLayerBox->bnRaise->setEnabled(false);

    m_wdgLayerBox->doubleOpacity->setRange(0, 100, 0);
    // the glyph next to it says what the number is
    KisSpinBoxI18nHelper::setText(m_wdgLayerBox->doubleOpacity,
                                  i18nc("{n} is the number value, % is the percent sign", "{n}%"));

    /**
     * The list is the panel. The selected layer's row holds its blend mode
     * and opacity; a strip above the list adds a layer, and so does a click
     * between two rows, right there; a layer dragged off the panel is
     * deleted; Ctrl+wheel sizes the rows. The rest (other kinds of layer,
     * filtering, the list's settings) is in the right click menu: no
     * buttons under the list. Their actions stay, with their shortcuts.
     */
    {
        Ui_WdgLayerBox *ui = m_wdgLayerBox;
        ui->hbox2->removeWidget(ui->cmbComposite);
        ui->hbox2->removeWidget(ui->bnLayerFilters);
        ui->opacityLayout->removeWidget(ui->doubleOpacity);
        ui->opacityLayout->removeWidget(ui->configureLayerDockerToolbar);
        ui->opacityLabel->hide();
        ui->verticalLayout->removeItem(ui->hbox2);
        ui->verticalLayout->removeItem(ui->opacityLayout);
        ui->verticalLayout->removeItem(ui->hbox1);
        // emptied and taken out: nothing owns them anymore; their buttons stay, hidden, for their actions
        delete ui->hbox2;
        ui->hbox2 = nullptr;
        delete ui->opacityLayout;
        ui->opacityLayout = nullptr;
        delete ui->hbox1;
        ui->hbox1 = nullptr;
        for (QWidget *button : std::initializer_list<QWidget *>{ui->bnAdd, ui->bnDuplicate, ui->bnRaise, ui->bnLower, ui->bnProperties,
                                                                  ui->bnDelete, ui->bnLayerFilters, ui->configureLayerDockerToolbar}) {
            button->hide();
        }

        // room around the rows, as the other panels have around what is in them
        ui->verticalLayout->setContentsMargins(6, 2, 6, 6);

        // in the selected layer's row, by its icons: see NodeDelegate::inlineControlsRect()
        ui->listLayers->setProperty("rowControls", true);
        m_rowControls = new QWidget(ui->listLayers->viewport());
        auto *row = new QHBoxLayout(m_rowControls);
        row->setContentsMargins(0, 3, 2, 3);
        row->setSpacing(2);
        // flat on the row's own highlight: a box only when pointed at; as wide as its mode
        ui->cmbComposite->setFrame(false);
        row->addWidget(ui->cmbComposite);
        ui->doubleOpacity->setToolTip(i18nc("@info:tooltip", "Layer opacity"));
        ui->doubleOpacity->setButtonSymbols(QAbstractSpinBox::NoButtons);
        // the number and a hairline of it, a slider when pointed at
        ui->doubleOpacity->setFrame(false);
        ui->doubleOpacity->setProperty(KisModernStyle::quietFieldProperty, true);
        ui->doubleOpacity->setFixedWidth(46);
        row->addWidget(ui->doubleOpacity);
        // the row's name ends where they begin, which is as wide as the mode's name makes them
        auto fit = [this]() {
            Ui_WdgLayerBox *ui = m_wdgLayerBox;
            QStyleOptionComboBox option;
            option.initFrom(ui->cmbComposite);
            option.frame = false;
            const QSize text(ui->cmbComposite->fontMetrics().horizontalAdvance(ui->cmbComposite->currentText()), ui->cmbComposite->height());
            const int mode = qMin(140, ui->cmbComposite->style()->sizeFromContents(QStyle::CT_ComboBox, &option, text, ui->cmbComposite).width());
            ui->cmbComposite->setFixedWidth(mode);
            // (its width() is the new one only once the layout has run)
            const QMargins margins = m_rowControls->layout()->contentsMargins();
            const int width = mode + m_rowControls->layout()->spacing() + 46 + margins.left() + margins.right();
            if (ui->listLayers->property("rowControlsWidth").toInt() != width) {
                ui->listLayers->setProperty("rowControlsWidth", width);
                ui->listLayers->viewport()->update();
            }
        };
        connect(ui->cmbComposite, QOverload<int>::of(&QComboBox::currentIndexChanged), this, fit);
        fit();
        // once its font is the panel's
        QTimer::singleShot(0, this, fit);
        // the layer's blend, quieter than its name next to it
        for (QWidget *quiet : std::initializer_list<QWidget *>{ui->cmbComposite, ui->doubleOpacity}) {
            QPalette palette = quiet->palette();
            for (QPalette::ColorRole role : {QPalette::ButtonText, QPalette::Text, QPalette::WindowText}) {
                QColor ink = palette.color(role);
                ink.setAlphaF(0.62);
                palette.setColor(role, ink);
            }
            quiet->setPalette(palette);
        }
        m_rowControls->hide();
        ui->listLayers->viewport()->installEventFilter(this);
        ui->listLayers->installEventFilter(this);
        connect(ui->listLayers, &NodeView::draggedOff, this, [this]() {
            if (m_removeAction) m_removeAction->trigger();
        });

        // a click between two rows adds a layer there
        m_opacityScrub = new OpacityScrub(m_wdgLayerBox->listLayers->viewport());
    m_opacityScrub->hide();
    m_gapIndicator = new GapIndicator(ui->listLayers->viewport());
        m_gapIndicator->hide();

        // and so does the strip above the list: above the selected layer; other kinds on a right click
        m_addStrip = new AddLayerStrip(mainWidget);
        connect(m_addStrip, &QAbstractButton::clicked, this, &LayerBox::slotAddLayerBnClicked);
        connect(m_addStrip, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
            m_newLayerMenu->popup(m_addStrip->mapToGlobal(pos));
        });
        ui->verticalLayout->insertWidget(listRow(ui), m_addStrip);
    }

    connect(m_wdgLayerBox->doubleOpacity, SIGNAL(valueChanged(qreal)), SLOT(slotOpacitySliderMoved(qreal)));
    connect(&m_nodeOpacityCompressor, SIGNAL(timeout()), SLOT(slotOpacityChanged()));

    connect(m_wdgLayerBox->cmbComposite, SIGNAL(activated(int)), SLOT(slotCompositeOpChanged(int)));
    connect(m_wdgLayerBox->cmbComposite, SIGNAL(sigPreviewBlendMode(QString)), SLOT(slotPreviewBlendMode(QString)));
    // the picker shows each mode on this layer over the ones below it
    m_wdgLayerBox->cmbComposite->setPreviewPictures([this]() {
        KisNodeSP node = m_nodeManager ? m_nodeManager->activeNode() : KisNodeSP();
        const QColor color = m_canvas && m_canvas->resourceManager() ? m_canvas->resourceManager()->foregroundColor().toQColor()
                                                                       : QColor(200, 85, 61);
        return KisBlendModePicker::layerPictures(node, KisBlendModePicker::pictureSize(this), color);
    });

    m_newLayerMenu = new QMenu(this);
    m_wdgLayerBox->bnAdd->setMenu(m_newLayerMenu);
    m_wdgLayerBox->bnAdd->setPopupMode(QToolButton::MenuButtonPopup);

    m_opLayerMenu = new QMenu(this);
    m_wdgLayerBox->bnProperties->setMenu(m_opLayerMenu);
    m_wdgLayerBox->bnProperties->setPopupMode(QToolButton::MenuButtonPopup);

    m_nodeModel = new KisNodeModel(this, 2);
    m_filteringModel = new KisNodeFilterProxyModel(this);
    m_filteringModel->setNodeModel(m_nodeModel);

    /**
     * Connect model updateUI() to enable/disable controls.
     * Note: nodeActivated() is connected separately in setImage(), because
     *       it needs particular order of calls: first the connection to the
     *       node manager should be called, then updateUI()
     */
    connect(m_nodeModel, SIGNAL(rowsInserted(QModelIndex,int,int)), SLOT(updateUI()));
    connect(m_nodeModel, SIGNAL(rowsRemoved(QModelIndex,int,int)), SLOT(updateUI()));
    connect(m_nodeModel, SIGNAL(rowsMoved(QModelIndex,int,int,QModelIndex,int)), SLOT(updateUI()));
    connect(m_nodeModel, SIGNAL(dataChanged(QModelIndex,QModelIndex)), SLOT(updateUI()));
    connect(m_nodeModel, SIGNAL(modelReset()), SLOT(slotModelReset()));

    connect(m_nodeModel, SIGNAL(rowsInserted(QModelIndex,int,int)), SLOT(slotForgetAboutSavedNodeBeforeEditSelectionMode()));
    connect(m_nodeModel, SIGNAL(rowsRemoved(QModelIndex,int,int)), SLOT(slotForgetAboutSavedNodeBeforeEditSelectionMode()));
    connect(m_nodeModel, SIGNAL(rowsMoved(QModelIndex,int,int,QModelIndex,int)), SLOT(slotForgetAboutSavedNodeBeforeEditSelectionMode()));
    connect(m_nodeModel, SIGNAL(modelReset()), SLOT(slotForgetAboutSavedNodeBeforeEditSelectionMode()));

    // we should update expanded state of the nodes on adding the nodes
    connect(m_nodeModel, SIGNAL(rowsInserted(QModelIndex,int,int)), SLOT(slotNodeCollapsedChanged()));
    connect(m_nodeModel, SIGNAL(modelReset()), SLOT(slotNodeCollapsedChanged()));

    m_showGlobalSelectionMask = new KisAction(i18n("&Show Global Selection Mask"), this);
    m_showGlobalSelectionMask->setObjectName("show-global-selection-mask");
    m_showGlobalSelectionMask->setActivationFlags(KisAction::ACTIVE_IMAGE);
    m_showGlobalSelectionMask->setToolTip(i18nc("@info:tooltip", "Shows global selection as a usual selection mask in <b>Layers</b> docker"));
    m_showGlobalSelectionMask->setCheckable(true);
    connect(m_showGlobalSelectionMask, SIGNAL(triggered(bool)), SLOT(slotEditGlobalSelection(bool)));

    m_showGlobalSelectionMask->setChecked(cfg.showGlobalSelection());

    m_colorSelector = new KisColorLabelSelectorWidgetMenuWrapper(this);
    MouseClickIgnore* mouseEater = new MouseClickIgnore(this);
    m_colorSelector->installEventFilter(mouseEater);
    connect(m_colorSelector->colorLabelSelector(), SIGNAL(currentIndexChanged(int)), SLOT(slotColorLabelChanged(int)));
    m_colorSelectorAction = new QWidgetAction(this);
    m_colorSelectorAction->setDefaultWidget(m_colorSelector);

    connect(m_nodeModel, SIGNAL(dataChanged(QModelIndex,QModelIndex)),
            &m_colorLabelCompressor, SLOT(start()));

    m_wdgLayerBox->listLayers->setModel(m_filteringModel);
    // this connection should be done *after* the setModel() call to
    // happen later than the internal selection model
    connect(m_filteringModel.data(), &KisNodeFilterProxyModel::sigBeforeBeginRemoveRows,
            this, &LayerBox::slotAdjustCurrentBeforeRemoveRows);


    // filtering by name and color label: a row above the list, there while it filters
    layerFilterWidget = new KisLayerFilterWidget(this);
    connect(layerFilterWidget, SIGNAL(filteringOptionsChanged()), this, SLOT(updateLayerFiltering()));
    connect(layerFilterWidget, &KisLayerFilterWidget::filteringOptionsChanged, this, [this]() {
        if (!layerFilterWidget->isCurrentlyFiltering() && !layerFilterWidget->isAncestorOf(QApplication::focusWidget())) {
            layerFilterWidget->hide();
        }
    });
    m_wdgLayerBox->verticalLayout->insertWidget(listRow(m_wdgLayerBox), layerFilterWidget);
    layerFilterWidget->hide();
    if (QLineEdit *field = layerFilterWidget->findChild<QLineEdit *>()) {
        field->installEventFilter(this);
    }

    setEnabled(false);

    connect(&m_colorLabelCompressor, SIGNAL(timeout()), SLOT(updateAvailableLabels()));


    // set up the configure menu for changing thumbnail size
    QMenu* configureMenu = new QMenu(i18nc("@item:inmenu the layers list's look", "List"), this);
    configureMenu->setContentsMargins(6, 6, 6, 6);
    // choosing a layer takes the brush last painted with on it (KisStudioBrushMemory)
    QAction *rememberBrushes = configureMenu->addAction(i18nc("@option:check", "Layers remember their brush"));
    rememberBrushes->setCheckable(true);
    rememberBrushes->setToolTip(i18nc("@info:tooltip", "Selecting a layer restores its brush"));
    connect(configureMenu, &QMenu::aboutToShow, rememberBrushes, [rememberBrushes]() {
        QSignalBlocker blocker(rememberBrushes);
        rememberBrushes->setChecked(KisStudioBrushMemory::layersRememberBrushes());
    });
    connect(rememberBrushes, &QAction::toggled, this, [](bool on) { KisStudioBrushMemory::setLayersRememberBrushes(on); });
    configureMenu->addSection(i18n("Thumbnail Size"));
    m_configureMenu = configureMenu;


    // add horizontal slider
    thumbnailSizeSlider = new QSlider(this);
    thumbnailSizeSlider->setOrientation(Qt::Horizontal);
    thumbnailSizeSlider->setRange(20, 80);

    thumbnailSizeSlider->setValue(cfg.layerThumbnailSize(false)); // grab this from the kritarc

    thumbnailSizeSlider->setMinimumHeight(20);
    thumbnailSizeSlider->setMinimumWidth(40);
    thumbnailSizeSlider->setTickInterval(5);
    m_nodeModel->setPreferredThumnalSize(cfg.layerThumbnailSize() * m_wdgLayerBox->listLayers->devicePixelRatioF());

    QWidgetAction *sliderAction= new QWidgetAction(this);
    sliderAction->setDefaultWidget(thumbnailSizeSlider);
    configureMenu->addAction(sliderAction);


    connect(thumbnailSizeSlider, SIGNAL(valueChanged(int)), &m_thumbnailSizeCompressor, SLOT(start()));
    connect(&m_thumbnailSizeCompressor, SIGNAL(timeout()), SLOT(slotUpdateThumbnailIconSize()));

    configureMenu->addSection(i18nc("@item:inmenu Layers Docker settings, slider", "Tree Indentation"));

    // add horizontal slider
    indentationSlider = new QSlider(Qt::Horizontal, this);
    indentationSlider->setRange(20, 100);
    indentationSlider->setMinimumSize(40, 20);
    indentationSlider->setSingleStep(5);
    indentationSlider->setPageStep(20);
    indentationSlider->setValue(cfg.layerTreeIndentation());


    sliderAction= new QWidgetAction(this);
    sliderAction->setDefaultWidget(indentationSlider);
    configureMenu->addAction(sliderAction);

    // NOTE: if KisConfig would just compress its file sync events, we wouldn't need
    // this extra compressor that juggles between slow UI and disk thrashing
    connect(indentationSlider, SIGNAL(valueChanged(int)), &m_treeIndentationCompressor, SLOT(start()));
    connect(&m_treeIndentationCompressor, SIGNAL(timeout()), SLOT(slotUpdateTreeIndentation()));


    // Layer info-text settings:
    // blending info-text style combobox
    configureMenu->addSection(i18nc("@item:inmenu Layers Docker settings, combobox", "Blending Info Style"));
    infoTextCombobox = new QComboBox(this);
    infoTextCombobox->setToolTip(i18nc("@item:tooltip", "None: Show nothing.\n"
                                                        "Simple: Show changed opacities or blending modes.\n"
                                                        "Balanced: Show both opacity and blending mode if either are changed.\n"
                                                        "Detailed: Show both opacity and blending mode even if unchanged."));
    infoTextCombobox->insertItems(0, QStringList ({
        i18nc("@item:inlistbox Layer Docker blending info style", "None"),
        i18nc("@item:inlistbox Layer Docker blending info style", "Simple"),
        i18nc("@item:inlistbox Layer Docker blending info style", "Balanced"),
        i18nc("@item:inlistbox Layer Docker blending info style", "Detailed"),
    }));
    infoTextCombobox->setCurrentIndex((int)cfg.layerInfoTextStyle());

    QWidgetAction *cmbboxAction = new QWidgetAction(this);
    cmbboxAction->setDefaultWidget(infoTextCombobox);
    configureMenu->addAction(cmbboxAction);
    connect(infoTextCombobox, SIGNAL(currentIndexChanged(int)), SLOT(slotUpdateLayerInfoTextStyle()));

    // info-text opacity slider
    infoTextOpacitySlider = new KisSliderSpinBox(this);
    KisSpinBoxI18nHelper::setText(infoTextOpacitySlider,
                                  i18nc("{n} is the number value, % is the percent sign", "Opacity: {n}%"));
    infoTextOpacitySlider->setToolTip(i18nc("@item:tooltip", "Blending info text opacity"));
    // 55% is the opacity of nonvisible layer text
    infoTextOpacitySlider->setRange(55, 100);
    infoTextOpacitySlider->setMinimumSize(40, 20);
    infoTextOpacitySlider->setSingleStep(5);
    infoTextOpacitySlider->setPageStep(15);
    infoTextOpacitySlider->setValue(cfg.layerInfoTextOpacity());
    if (infoTextCombobox->currentIndex() == 0) {
        infoTextOpacitySlider->setDisabled(true);
    }

    sliderAction= new QWidgetAction(this);
    sliderAction->setDefaultWidget(infoTextOpacitySlider);
    configureMenu->addAction(sliderAction);
    connect(infoTextOpacitySlider, SIGNAL(valueChanged(int)), &m_infoTextOpacityCompressor, SLOT(start()));
    connect(&m_infoTextOpacityCompressor, SIGNAL(timeout()), SLOT(slotUpdateLayerInfoTextOpacity()));

    // info-text inline checkbox
    infoTextInlineChkbox = new QCheckBox(i18nc("@item:inmenu Layers Docker settings, checkbox", "Inline"), this);
    infoTextInlineChkbox->setChecked(cfg.useInlineLayerInfoText());
    infoTextInlineChkbox->setToolTip(i18nc("@item:tooltip", "If enabled, show blending info beside layer names.\n"
                                                            "If disabled, show below layer names (when enough space)."));
    if (infoTextCombobox->currentIndex() == 0) {
        infoTextInlineChkbox->setDisabled(true);
    }

    QWidgetAction *chkboxAction = new QWidgetAction(this);
    chkboxAction->setDefaultWidget(infoTextInlineChkbox);
    configureMenu->addAction(chkboxAction);
    connect(infoTextInlineChkbox, SIGNAL(stateChanged(int)), SLOT(slotUpdateUseInlineLayerInfoText()));

    layerSelectionCheckBox = new QCheckBox(
        i18nc("@item:inmenu Layers Docker settings, checkbox", "Checkbox for Selecting Layers"), this);
    layerSelectionCheckBox->setToolTip(i18nc("@item:tooltip", "Show checkbox to select/unselect layers."));
    layerSelectionCheckBox->setChecked(cfg.useLayerSelectionCheckbox());

    QWidgetAction *layerSelectionAction = new QWidgetAction(this);
    layerSelectionAction->setDefaultWidget(layerSelectionCheckBox);
    configureMenu->addAction(layerSelectionAction);
    connect(layerSelectionCheckBox, SIGNAL(stateChanged(int)), SLOT(slotUpdateUseLayerSelectionCheckbox()));

    m_screenMigrationTracker.reset(new KisScreenMigrationTracker(m_wdgLayerBox->listLayers));
    connect(m_screenMigrationTracker.data(), &KisScreenMigrationTracker::sigScreenOrResolutionChanged,
            this, &LayerBox::slotSyncThumbnailIconSize);
}

LayerBox::~LayerBox()
{
    delete m_wdgLayerBox;
}


void expandNodesRecursively(KisNodeSP root, QPointer<KisNodeFilterProxyModel> filteringModel, NodeView *nodeView)
{
    if (!root) return;
    if (filteringModel.isNull()) return;
    if (!nodeView) return;

    nodeView->blockSignals(true);

    KisNodeSP node = root->firstChild();
    while (node) {
        QModelIndex idx = filteringModel->indexFromNode(node);
        if (idx.isValid()) {
            nodeView->setExpanded(idx, !node->collapsed());
        }
        if (!node->collapsed() && node->childCount() > 0) {
            expandNodesRecursively(node, filteringModel, nodeView);
        }
        node = node->nextSibling();
    }
    nodeView->blockSignals(false);
}

void LayerBox::slotAddLayerBnClicked()
{
    if (m_canvas) {
        KisNodeList nodes = m_nodeManager->selectedNodes();

        if (nodes.size() == 1) {
            KisAction *action = m_canvas->viewManager()->actionManager()->actionByName("add_new_paint_layer");
            action->trigger();
        } else {
            KisAction *action = m_canvas->viewManager()->actionManager()->actionByName("create_quick_group");
            action->trigger();
        }
    }
}

void LayerBox::setViewManager(KisViewManager* kisview)
{
    m_nodeManager = kisview->nodeManager();

    if (m_nodeManager) {
        connect(m_nodeManager, SIGNAL(sigNodeActivated(KisNodeSP)), SLOT(slotForgetAboutSavedNodeBeforeEditSelectionMode()));
    }

    kisview->actionManager()->
            addAction(m_showGlobalSelectionMask->objectName(),
                      m_showGlobalSelectionMask);

    connect(m_wdgLayerBox->bnAdd, SIGNAL(clicked()), this, SLOT(slotAddLayerBnClicked()));

    connectActionToButton(kisview, m_wdgLayerBox->bnDuplicate, "duplicatelayer");

    KisActionManager *actionManager = kisview->actionManager();

    KisAction *action = actionManager->createAction("RenameCurrentLayer");
    Q_ASSERT(action);
    connect(action, SIGNAL(triggered()), this, SLOT(slotRenameCurrentNode()));

    m_propertiesAction = actionManager->createAction("layer_properties");
    Q_ASSERT(m_propertiesAction);
    new SyncButtonAndAction(m_propertiesAction, m_wdgLayerBox->bnProperties, this);
    connect(m_propertiesAction, SIGNAL(triggered()), this, SLOT(slotPropertiesClicked()));

    connect(m_opLayerMenu, SIGNAL(aboutToShow()), this, SLOT(slotLayerOpMenuOpened()));

    // It's necessary to clear the layer operations menu when it closes, else
    // the color selector can't be shared with the right-click context menu
    connect(m_opLayerMenu, SIGNAL(aboutToHide()), this, SLOT(slotLayerOpMenuClosed()));

    m_removeAction = actionManager->createAction("remove_layer");
    Q_ASSERT(m_removeAction);
    new SyncButtonAndAction(m_removeAction, m_wdgLayerBox->bnDelete, this);
    connect(m_removeAction, SIGNAL(triggered()), this, SLOT(slotRmClicked()));

    action = actionManager->createAction("move_layer_up");
    Q_ASSERT(action);
    new SyncButtonAndAction(action, m_wdgLayerBox->bnRaise, this);
    connect(action, SIGNAL(triggered()), this, SLOT(slotRaiseClicked()));

    action = actionManager->createAction("move_layer_down");
    Q_ASSERT(action);
    new SyncButtonAndAction(action, m_wdgLayerBox->bnLower, this);
    connect(action, SIGNAL(triggered()), this, SLOT(slotLowerClicked()));

    m_changeCloneSourceAction = actionManager->createAction("set-copy-from");
    Q_ASSERT(m_changeCloneSourceAction);
    connect(m_changeCloneSourceAction, &KisAction::triggered,
            this, &LayerBox::slotChangeCloneSourceClicked);

    m_layerToggleSolo = actionManager->createAction("toggle_layer_soloing");
    connect(m_layerToggleSolo, SIGNAL(triggered(bool)), this, SLOT(toggleActiveLayerSolo()));
}

void LayerBox::setCanvas(KoCanvasBase *canvas)
{
    if (m_canvas == canvas)
        return;

    setEnabled(canvas != 0);

    if (m_canvas) {
        m_canvas->disconnectCanvasObserver(this);
        m_nodeModel->setIdleTaskManager(0);
        m_nodeModel->setDummiesFacade(0, 0, 0, 0, 0);
        m_selectionActionsAdapter.reset();

        if (m_image) {
            KisImageAnimationInterface *animation = m_image->animationInterface();
            animation->disconnect(this);
        }

        disconnect(m_image, 0, this, 0);
        disconnect(m_nodeManager, 0, this, 0);
        disconnect(m_nodeModel, 0, m_nodeManager, 0);
        m_nodeManager->slotSetSelectedNodes(KisNodeList());
    }

    m_canvas = dynamic_cast<KisCanvas2*>(canvas);

    if (m_canvas) {
        m_image = m_canvas->image();
        Q_EMIT imageChanged();

        KisDocument* doc = static_cast<KisDocument*>(m_canvas->imageView()->document());
        KisShapeController *kritaShapeController =
                dynamic_cast<KisShapeController*>(doc->shapeController());
        KisDummiesFacadeBase *kritaDummiesFacade =
                static_cast<KisDummiesFacadeBase*>(kritaShapeController);


        m_selectionActionsAdapter.reset(new KisSelectionActionsAdapter(m_canvas->viewManager()->selectionManager()));
        m_nodeModel->setDummiesFacade(kritaDummiesFacade,
                                      m_image,
                                      kritaShapeController,
                                      m_selectionActionsAdapter.data(),
                                      m_nodeManager);

        if (isVisible()) {
            m_nodeModel->setIdleTaskManager(m_canvas->viewManager()->idleTasksManager());
        }

        connect(m_image, SIGNAL(sigAboutToBeDeleted()), SLOT(notifyImageDeleted()));
        connect(m_image, SIGNAL(sigNodeCollapsedChanged()), SLOT(slotNodeCollapsedChanged()));

        // cold start
        if (m_nodeManager) {
            setCurrentNode(m_nodeManager->activeNode());
            // Connection KisNodeManager -> LayerBox
            connect(m_nodeManager, SIGNAL(sigUiNeedChangeActiveNode(KisNodeSP)),
                    this, SLOT(setCurrentNode(KisNodeSP)));

            connect(m_nodeManager,
                    SIGNAL(sigUiNeedChangeSelectedNodes(QList<KisNodeSP>)),
                    SLOT(slotNodeManagerChangedSelection(QList<KisNodeSP>)));
        }
        else {
            setCurrentNode(m_canvas->imageView()->currentNode());
        }

        // Connection LayerBox -> KisNodeManager (isolate layer)
        connect(m_nodeModel, SIGNAL(toggleIsolateActiveNode()),
                m_nodeManager, SLOT(toggleIsolateActiveNode()));

        KisImageAnimationInterface *animation = m_image->animationInterface();
        connect(animation, &KisImageAnimationInterface::sigUiTimeChanged, this, &LayerBox::slotImageTimeChanged);

        expandNodesRecursively(m_image->rootLayer(), m_filteringModel, m_wdgLayerBox->listLayers);
        m_wdgLayerBox->listLayers->scrollTo(m_wdgLayerBox->listLayers->currentIndex());
        updateAvailableLabels();

        addActionToMenu(m_newLayerMenu, "add_new_paint_layer");
        addActionToMenu(m_newLayerMenu, "add_new_group_layer");
        addActionToMenu(m_newLayerMenu, "add_new_clone_layer");
        addActionToMenu(m_newLayerMenu, "add_new_shape_layer");
        addActionToMenu(m_newLayerMenu, "add_new_adjustment_layer");
        addActionToMenu(m_newLayerMenu, "add_new_fill_layer");
        addActionToMenu(m_newLayerMenu, "add_new_file_layer");
        m_newLayerMenu->addSeparator();
        addActionToMenu(m_newLayerMenu, "add_new_transparency_mask");
        addActionToMenu(m_newLayerMenu, "add_new_filter_mask");
        addActionToMenu(m_newLayerMenu, "add_new_fast_color_overlay_mask");
        addActionToMenu(m_newLayerMenu, "add_new_colorize_mask");
        addActionToMenu(m_newLayerMenu, "add_new_transform_mask");
        addActionToMenu(m_newLayerMenu, "add_new_selection_mask");
    }
}


void LayerBox::unsetCanvas()
{
    setEnabled(false);
    if (m_canvas) {
        m_newLayerMenu->clear();
    }

    m_filteringModel->unsetDummiesFacade();
    disconnect(m_image, 0, this, 0);
    disconnect(m_nodeManager, 0, this, 0);
    disconnect(m_nodeModel, 0, m_nodeManager, 0);
    m_nodeManager->slotSetSelectedNodes(KisNodeList());

    m_canvas = 0;
}

void LayerBox::showEvent(QShowEvent *event)
{
    QDockWidget::showEvent(event);

    if (m_canvas) {
        m_nodeModel->setIdleTaskManager(m_canvas->viewManager()->idleTasksManager());
    }
}

void LayerBox::hideEvent(QHideEvent *event)
{
    QDockWidget::hideEvent(event);
    m_nodeModel->setIdleTaskManager(0);
}

void LayerBox::notifyImageDeleted()
{
    setCanvas(0);
}

void LayerBox::updateUI()
{
    if (!m_canvas) return;
    if (!m_nodeManager) return;

    KisNodeSP activeNode = m_nodeManager->activeNode();

    if (activeNode != m_activeNode) {
        m_activeNodeConnections.clear();
        m_activeNode = activeNode;

        if (activeNode) {
            KisPaintDeviceSP parentLayerDevice = activeNode->parent() ? activeNode->parent()->original() : 0;
            if (parentLayerDevice) {
                // update blending modes availability
                m_activeNodeConnections.addConnection(
                     parentLayerDevice, SIGNAL(colorSpaceChanged(const KoColorSpace*)),
                     this, SLOT(updateUI()));
            }

            m_activeNodeConnections.addConnection(
                    activeNode, SIGNAL(opacityChanged(quint8)),
                    this, SLOT(slotUpdateOpacitySlider(quint8)));
        }
    }

    m_wdgLayerBox->bnRaise->setEnabled(activeNode && activeNode->isEditable(false) && (activeNode->nextSibling()
                                                                                       || (activeNode->parent() && activeNode->parent() != m_image->root())));
    m_wdgLayerBox->bnLower->setEnabled(activeNode && activeNode->isEditable(false) && (activeNode->prevSibling()
                                                                                       || (activeNode->parent() && activeNode->parent() != m_image->root())));

    m_wdgLayerBox->doubleOpacity->setEnabled(activeNode && activeNode->isEditable(false));

    m_wdgLayerBox->cmbComposite->setEnabled(activeNode && activeNode->isEditable(false));

    if (activeNode) {
        if (activeNode->inherits("KisColorizeMask") || activeNode->inherits("KisLayer")) {

            m_wdgLayerBox->doubleOpacity->setEnabled(true);

            if (!m_wdgLayerBox->doubleOpacity->isDragging()) {
                slotSetOpacity(activeNode->opacity() * 100.0 / 255);
            }

            const KoCompositeOp* compositeOp = activeNode->compositeOp();
            if (compositeOp) {
                /// the composite op works in the color space of the parent layer,
                /// not the active one.
                m_wdgLayerBox->cmbComposite->validate(compositeOp->colorSpace());
                slotSetCompositeOp(compositeOp);
            } else {
                m_wdgLayerBox->cmbComposite->setEnabled(false);
            }

            const KisGroupLayer *group = qobject_cast<const KisGroupLayer*>(activeNode.data());
            bool compositeSelectionActive = !(group && group->passThroughMode());

            m_wdgLayerBox->cmbComposite->setEnabled(compositeSelectionActive);
        } else if (activeNode->inherits("KisMask")) {
            m_wdgLayerBox->cmbComposite->setEnabled(false);
            m_wdgLayerBox->doubleOpacity->setEnabled(false);
        }
    }
}


/**
 * This method is called *only* when non-GUI code requested the
 * change of the current node
 */
void LayerBox::setCurrentNode(KisNodeSP node)
{
    /**
     * setCanvas() calls to node managed and layer box are coming out-of-sync,
     * so it might happen that setCurrentNode() call will arrive with a node
     * from a different image. We should check that and ignore such calls.
     *
     * TODO: make bootstrapping of layers-related structures a bit more
     * straight-forward. We need to have a single source of truth for the
     * "active layer", and all the initialization should happen in one
     * direction without cycles.
     */
    if (node && m_image && node->graphListener() != m_image.data()) {
        return;
    }

    m_filteringModel->setActiveNode(node);

    QModelIndex index = node ? m_filteringModel->indexFromNode(node) : QModelIndex();
    m_filteringModel->setData(index, true, KisNodeModel::ActiveRole);
    updateUI();
}

void LayerBox::slotModelReset()
{
    if(m_nodeModel->hasDummiesFacade()) {
        QItemSelection selection;
        Q_FOREACH (const KisNodeSP node, m_nodeManager->selectedNodes()) {
            const QModelIndex &idx = m_filteringModel->indexFromNode(node);
            if(idx.isValid()){
                QItemSelectionRange selectionRange(idx);
                selection << selectionRange;
            }
        }

        m_wdgLayerBox->listLayers->selectionModel()->select(selection, QItemSelectionModel::ClearAndSelect);
    }

    updateUI();
}

void LayerBox::slotSetCompositeOp(const KoCompositeOp* compositeOp)
{
    KoID opId = KoCompositeOpRegistry::instance().getKoID(compositeOp->id());

    m_wdgLayerBox->cmbComposite->blockSignals(true);
    m_wdgLayerBox->cmbComposite->selectCompositeOp(opId);
    m_wdgLayerBox->cmbComposite->blockSignals(false);
}

// range: 0-100
void LayerBox::slotSetOpacity(double opacity)
{
    Q_ASSERT(opacity >= 0 && opacity <= 100);
    m_wdgLayerBox->doubleOpacity->blockSignals(true);
    m_wdgLayerBox->doubleOpacity->setValue(opacity);
    m_wdgLayerBox->doubleOpacity->blockSignals(false);
}

void LayerBox::slotUpdateOpacitySlider(quint8 value) {
    double percentage = value * 100.0 / 255.0;

    m_wdgLayerBox->doubleOpacity->blockSignals(true);
    m_wdgLayerBox->doubleOpacity->setValue(percentage);
    m_wdgLayerBox->doubleOpacity->blockSignals(false);
}

void LayerBox::slotContextMenuRequested(const QPoint &pos, const QModelIndex &index)
{
    if (!m_canvas || !m_nodeManager) return;
    KisNodeList nodes = m_nodeManager->selectedNodes();
    KisNodeSP activeNode = m_nodeManager->activeNode();

    QMenu menu;
    if (index.isValid() && !nodes.isEmpty() && activeNode) {
        updateLayerOpMenu(index, menu);
    } else {
        // the list itself
        addMenuEntry(&menu, "add_new_paint_layer", i18nc("@action:inmenu", "New Layer"));
        addNewLayerMenu(&menu, false);
        addMenuEntry(&menu, "paste_layer_from_clipboard", i18nc("@action:inmenu", "Paste"));
        menu.addSeparator();
        QAction *filter = menu.addAction(i18nc("@action:inmenu", "Filter…") + QLatin1Char('\t') + QKeySequence(QKeySequence::Find).toString(QKeySequence::NativeText));
        connect(filter, &QAction::triggered, this, [this]() { startFiltering(QString()); });
        QMenu *select = menu.addMenu(i18nc("@title:menu", "Select"));
        addMenuEntry(select, "select_all_layers", i18nc("@action:inmenu select layers", "All"));
        addMenuEntry(select, "select_visible_layers", i18nc("@action:inmenu select layers", "Visible"));
        addMenuEntry(select, "select_invisible_layers", i18nc("@action:inmenu select layers", "Hidden"));
        addMenuEntry(select, "select_locked_layers", i18nc("@action:inmenu select layers", "Locked"));
        addMenuEntry(select, "select_unlocked_layers", i18nc("@action:inmenu select layers", "Unlocked"));
        menu.addMenu(m_configureMenu);
    }
    menu.exec(pos);
}

QAction *LayerBox::addMenuEntry(QMenu *menu, QAction *action, const QString &label, bool onlyIfEnabled)
{
    /**
     * A short entry for the action: its name in the fewest words (it keeps
     * its full name elsewhere, in Find and the main menu), the shortcut
     * beside it, and no icon (a menu is read, not looked over).
     */
    if (!action || (onlyIfEnabled && !action->isEnabled())) return nullptr;
    QString text = label.isEmpty() ? action->text() : label;
    if (!action->shortcut().isEmpty()) {
        text += QLatin1Char('\t') + action->shortcut().toString(QKeySequence::NativeText);
    }
    QAction *entry = new QAction(text, menu);
    entry->setEnabled(action->isEnabled());
    // its shortcut is the action's: shown as a key cap, and changed there
    entry->setProperty("kisShortcutTarget", QVariant::fromValue<QObject *>(action));
    entry->setCheckable(action->isCheckable());
    entry->setChecked(action->isChecked());
    QPointer<QAction> target(action);
    connect(entry, &QAction::triggered, this, [target]() {
        if (target) target->trigger();
    });
    menu->addAction(entry);
    return entry;
}

QAction *LayerBox::addMenuEntry(QMenu *menu, const QString &id, const QString &label, bool onlyIfEnabled)
{
    if (!m_canvas) return nullptr;
    return addMenuEntry(menu, m_canvas->viewManager()->actionManager()->actionByName(id), label, onlyIfEnabled);
}

void LayerBox::addNewLayerMenu(QMenu *menu, bool below)
{
    // every kind of layer and mask: one list, not "New" and "Add" with half of it each
    QMenu *kinds = menu->addMenu(i18nc("@title:menu other kinds of layer", "New"));
    addMenuEntry(kinds, "add_new_paint_layer", i18nc("@action:inmenu", "Paint Layer"));
    if (below) {
        QAction *entry = kinds->addAction(i18nc("@action:inmenu", "Paint Layer Below"));
        connect(entry, &QAction::triggered, this, &LayerBox::addLayerBelow);
    }
    addMenuEntry(kinds, "add_new_group_layer", i18nc("@action:inmenu", "Group"));
    addMenuEntry(kinds, "add_new_shape_layer", i18nc("@action:inmenu", "Vector Layer"));
    addMenuEntry(kinds, "add_new_adjustment_layer", i18nc("@action:inmenu", "Filter Layer…"));
    addMenuEntry(kinds, "add_new_fill_layer", i18nc("@action:inmenu", "Fill Layer…"));
    addMenuEntry(kinds, "add_new_file_layer", i18nc("@action:inmenu", "File Layer…"));
    addMenuEntry(kinds, "add_new_clone_layer", i18nc("@action:inmenu", "Clone"));
    addMenuEntry(kinds, "new_from_visible", i18nc("@action:inmenu", "From Visible"));
    kinds->addSeparator();
    addMenuEntry(kinds, "add_new_transparency_mask", i18nc("@action:inmenu", "Transparency Mask"));
    addMenuEntry(kinds, "add_new_filter_mask", i18nc("@action:inmenu", "Filter Mask…"));
    addMenuEntry(kinds, "add_new_fast_color_overlay_mask", i18nc("@action:inmenu", "Color Overlay Mask"));
    addMenuEntry(kinds, "add_new_colorize_mask", i18nc("@action:inmenu", "Colorize Mask"));
    addMenuEntry(kinds, "add_new_transform_mask", i18nc("@action:inmenu", "Transform Mask"));
    addMenuEntry(kinds, "add_new_selection_mask", i18nc("@action:inmenu", "Local Selection"));
}

void LayerBox::addLayerBelow()
{
    // a new layer goes above the selected one: then one step down
    slotAddLayerBnClicked();
    if (m_canvas) {
        if (KisAction *down = m_canvas->viewManager()->actionManager()->actionByName("move_layer_down")) {
            down->trigger();
        }
    }
}

void LayerBox::addLayerAbove(const QModelIndex &index)
{
    // a new layer goes above the selected one: select the one it goes above first
    NodeView *view = m_wdgLayerBox->listLayers;
    view->setCurrentIndex(index);
    view->selectionModel()->select(index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    slotAddLayerBnClicked();
}

void LayerBox::startFiltering(const QString &typed)
{
    layerFilterWidget->show();
    layerFilterWidget->startTextFilter(typed);
}

void LayerBox::placeRowControls()
{
    NodeView *view = m_wdgLayerBox->listLayers;
    const QModelIndex current = view->currentIndex();
    const QModelIndex main = current.isValid() ? current.sibling(current.row(), NodeView::DEFAULT_COL) : QModelIndex();
    const QRect row = main.isValid() ? view->visualRect(main) : QRect();
    if (!m_canvas || row.isEmpty() || !view->viewport()->rect().intersects(row)) {
        if (m_rowControls->isVisible()) m_rowControls->hide();
        return;
    }
    auto *delegate = qobject_cast<NodeDelegate *>(view->itemDelegate());
    const QRect rect = delegate ? delegate->inlineControlsRect(main, row, view->layoutDirection()) : QRect();
    // tall rows: the opacity as long as the row; in one line, as wide as its number
    QWidget *opacity = m_wdgLayerBox->doubleOpacity;
    const bool tall = delegate && delegate->isTall();
    if (tall && opacity->maximumWidth() == 46) {
        opacity->setMinimumWidth(46);
        opacity->setMaximumWidth(QWIDGETSIZE_MAX);
    } else if (!tall && opacity->maximumWidth() != 46) {
        opacity->setFixedWidth(46);
    }
    if (rect.isEmpty()) {
        if (m_rowControls->isVisible()) m_rowControls->hide();
        return;
    }
    if (m_rowControls->geometry() != rect) m_rowControls->setGeometry(rect);
    if (!m_rowControls->isVisible()) m_rowControls->show();
}

QModelIndex LayerBox::gapAt(const QPoint &pos, int *gapY) const
{
    // within a few pixels of the line between two rows: the row under it, which the new layer goes above
    NodeView *view = m_wdgLayerBox->listLayers;
    const QModelIndex at = static_cast<QAbstractItemView *>(view)->indexAt(pos);
    if (!at.isValid()) return QModelIndex();
    const QModelIndex main = at.sibling(at.row(), NodeView::DEFAULT_COL);
    const QRect row = view->visualRect(main);
    const int band = 3;
    if (pos.y() - row.top() <= band) {
        *gapY = row.top();
        return main;
    }
    if (row.bottom() - pos.y() <= band) {
        const QModelIndex below = view->indexBelow(main);
        if (below.isValid()) {
            *gapY = row.bottom() + 1;
            return below.sibling(below.row(), NodeView::DEFAULT_COL);
        }
    }
    return QModelIndex();
}

bool LayerBox::eventFilter(QObject *watched, QEvent *event)
{
    NodeView *view = m_wdgLayerBox->listLayers;
    if (watched == view->viewport()) {
        switch (event->type()) {
        case QEvent::Paint:
            // scrolled, resized, another layer selected, rows added: the controls follow the row
            placeRowControls();
            break;
        case QEvent::MouseMove: {
            auto *mouse = static_cast<QMouseEvent *>(event);
            if (m_opacityPress.isValid() && (mouse->buttons() & Qt::LeftButton) && handleOpacityDrag(mouse->pos(), mouse->modifiers() & Qt::ShiftModifier)) {
                return true;
            }
            int y = 0;
            // a mouse only: a finger has the strip and "New Layer Above/Below" in its menu
            const QModelIndex gap = (mouse->buttons() == Qt::NoButton && mouse->source() == Qt::MouseEventNotSynthesized)
                                        ? gapAt(mouse->pos(), &y) : QModelIndex();
            m_gapIndex = gap;
            if (gap.isValid()) {
                const QRect row = view->visualRect(gap);
                m_gapIndicator->setGeometry(row.left(), y - 6, view->viewport()->width() - row.left(), 12);
                m_gapIndicator->show();
                m_gapIndicator->raise();
            } else {
                m_gapIndicator->hide();
            }
            break;
        }
        case QEvent::Leave:
            m_gapIndicator->hide();
            m_gapIndex = QModelIndex();
            break;
        case QEvent::MouseButtonPress: {
            auto *mouse = static_cast<QMouseEvent *>(event);
            int y = 0;
            if (mouse->button() == Qt::LeftButton && m_gapIndicator->isVisible() && m_gapIndex.isValid()
                && gapAt(mouse->pos(), &y).isValid()) {
                const QModelIndex gap = m_gapIndex;
                m_gapIndicator->hide();
                addLayerAbove(gap);
                return true;
            }
            // maybe the start of a drag sideways: the layer's opacity
            m_opacityPress = mouse->button() == Qt::LeftButton ? QPersistentModelIndex(static_cast<QAbstractItemView *>(view)->indexAt(mouse->pos())) : QPersistentModelIndex();
            m_opacityPressPos = mouse->pos();
            m_opacityScrubbing = false;
            break;
        }
        case QEvent::MouseButtonRelease:
            if (m_opacityScrubbing) {
                m_opacityScrubbing = false;
                m_opacityPress = QModelIndex();
                m_opacityScrub->hide();
                view->viewport()->unsetCursor();
                // the last value now, not after the compressor's wait
                if (m_nodeOpacityCompressor.isActive()) {
                    m_nodeOpacityCompressor.stop();
                    slotOpacityChanged();
                }
                return true;
            }
            m_opacityPress = QModelIndex();
            break;
        case QEvent::Wheel: {
            // Ctrl+wheel: smaller or larger rows, as the list's settings set them
            auto *wheel = static_cast<QWheelEvent *>(event);
            if (wheel->modifiers() & Qt::ControlModifier) {
                const int steps = KisModernStyle::wheelSteps(wheel, watched);
                if (steps) thumbnailSizeSlider->setValue(thumbnailSizeSlider->value() + 5 * steps);
                return true;
            }
            break;
        }
        default:
            break;
        }
    } else if (watched == view && event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_F && key->modifiers() == Qt::ControlModifier) {
            startFiltering(QString());
            return true;
        }
    } else if (event->type() == QEvent::KeyPress && static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape
               && layerFilterWidget->isAncestorOf(qobject_cast<QWidget *>(watched))) {
        // Escape: no filter, back to the list
        layerFilterWidget->reset();
        layerFilterWidget->hide();
        view->setFocus();
        return true;
    }
    return QDockWidget::eventFilter(watched, event);
}

bool LayerBox::handleOpacityDrag(const QPoint &pos, bool fine)
{
    NodeView *view = m_wdgLayerBox->listLayers;
    if (m_opacityScrubbing && fine != m_opacityFine) {
        // on from where it is, at the other speed
        m_opacityStart = m_newOpacity;
        m_opacityPressPos = pos;
    }
    m_opacityFine = fine;
    const QPoint delta = pos - m_opacityPressPos;
    if (!m_opacityScrubbing) {
        if (delta.manhattanLength() < QApplication::startDragDistance()) {
            return false;
        }
        // mostly sideways: the opacity; anything else is the list's (moving the layer, scrolling)
        KisNodeSP node = m_filteringModel ? m_filteringModel->nodeFromIndex(m_opacityPress) : KisNodeSP();
        if (qAbs(delta.x()) < 2 * qAbs(delta.y()) || !node || !node->isEditable(false)) {
            m_opacityPress = QModelIndex();
            return false;
        }
        m_opacityScrubbing = true;
        m_opacityNode = node;
        m_opacityStart = node->opacity() * 100.0 / 255.0;
        view->viewport()->setCursor(Qt::SizeHorCursor);
    }
    if (!m_opacityNode) {
        return true;
    }
    // the row's width is all of it, from where it was
    const QRect row = view->visualRect(m_opacityPress);
    const qreal percent = qBound(0.0, m_opacityStart + (fine ? 0.25 : 1.0) * delta.x() * 100.0 / qMax(80, view->viewport()->width() - row.left()), 100.0);
    m_newOpacity = percent;
    m_changedOpacityNode = m_opacityNode;
    m_nodeOpacityCompressor.start();
    if (m_opacityNode == m_activeNode) {
        QSignalBlocker blocker(m_wdgLayerBox->doubleOpacity);
        m_wdgLayerBox->doubleOpacity->setValue(percent);
    }
    m_opacityScrub->setGeometry(row.left(), row.top(), view->viewport()->width() - row.left(), qMin(row.height(), 30));
    static_cast<OpacityScrub *>(m_opacityScrub)->setValue(percent);
    m_opacityScrub->show();
    m_opacityScrub->raise();
    return true;
}

void LayerBox::slotRmClicked()
{
    if (!m_canvas) return;
    m_nodeManager->removeNode();
}

void LayerBox::slotRaiseClicked()
{
    if (!m_canvas) return;
    m_nodeManager->raiseNode();
}

void LayerBox::slotLowerClicked()
{
    if (!m_canvas) return;
    m_nodeManager->lowerNode();
}

void LayerBox::slotPropertiesClicked()
{
    if (!m_canvas) return;
    if (KisNodeSP active = m_nodeManager->activeNode()) {
        m_nodeManager->nodeProperties(active);
    }
}

void LayerBox::slotLayerOpMenuOpened()
{
    if (!m_canvas) return;
    updateLayerOpMenu(m_wdgLayerBox->listLayers->currentIndex(), *m_opLayerMenu);
}

void LayerBox::slotLayerOpMenuClosed()
{
    if (!m_canvas) return;
    m_opLayerMenu->clear();
}

void LayerBox::slotChangeCloneSourceClicked()
{
    if (!m_canvas) return;
    m_nodeManager->changeCloneSource();
}

void LayerBox::slotCompositeOpChanged(int index)
{
    Q_UNUSED(index);
    if (!m_canvas) return;

    QString compositeOp = m_wdgLayerBox->cmbComposite->selectedCompositeOp().id();
    m_nodeManager->nodeCompositeOpChanged(m_nodeManager->activeColorSpace()->compositeOp(compositeOp));
}

void LayerBox::slotPreviewBlendMode(const QString &id)
{
    /**
     * Shows a blend mode on the canvas while it is highlighted in the picker,
     * without undo commands. The original mode is back before a picked one
     * is applied through slotCompositeOpChanged(), which makes it one undo step.
     */
    if (!id.isEmpty()) {
        if (!m_canvas || !m_activeNode || !m_activeNode->isEditable(false)) {
            return;
        }
        if (!m_previewNode) {
            m_previewNode = m_activeNode;
            m_previewImage = m_image;
            m_previewOriginalOp = m_activeNode->compositeOpId();
        }
        // the merge threads read the op: change it only while they are stopped,
        // and try again in a moment rather than wait for a heavy update to finish
        KisImageSP image = m_previewImage;
        m_pendingPreviewOp.clear();
        if (m_previewNode->compositeOpId() != id && image) {
            if (image->tryBarrierLock()) {
                m_previewNode->setCompositeOpId(id);
                image->unlock();
                m_previewNode->setDirty();
            } else {
                m_pendingPreviewOp = id;
                QTimer::singleShot(16, this, [this, id]() {
                    if (m_pendingPreviewOp == id) slotPreviewBlendMode(id);
                });
            }
        }
    } else if (m_previewNode) {
        m_pendingPreviewOp.clear();
        // the image the preview was made in, even if another one is shown now
        KisImageSP image = m_previewImage;
        if (m_previewNode->compositeOpId() != m_previewOriginalOp && image) {
            // the original must come back, so this one waits
            image->barrierLock();
            m_previewNode->setCompositeOpId(m_previewOriginalOp);
            image->unlock();
            m_previewNode->setDirty();
        }
        m_previewNode = nullptr;
        m_previewImage = nullptr;
    }
}

void LayerBox::slotOpacityChanged()
{
    if (!m_canvas) return;
    m_nodeManager->setNodeOpacity(m_changedOpacityNode, convertOpacityToInt(m_newOpacity));
}

void LayerBox::slotOpacitySliderMoved(qreal opacity)
{
    m_newOpacity = opacity;
    m_changedOpacityNode = m_activeNode;
    m_nodeOpacityCompressor.start();
}

void LayerBox::slotCollapsed(const QModelIndex &index)
{
    KisNodeSP node = m_filteringModel->nodeFromIndex(index);
    if (node) {
        node->setCollapsed(true);
    }
}

void LayerBox::slotExpanded(const QModelIndex &index)
{
    KisNodeSP node = m_filteringModel->nodeFromIndex(index);
    if (node) {
        node->setCollapsed(false);
    }
}

void LayerBox::slotSelectOpaque()
{
    if (!m_canvas) return;
    QAction *action = m_canvas->viewManager()->actionManager()->actionByName("selectopaque");
    if (action) {
        action->trigger();
    }
}

void LayerBox::slotNodeCollapsedChanged()
{
    if (m_nodeModel->hasDummiesFacade()) {
        expandNodesRecursively(m_image->rootLayer(), m_filteringModel, m_wdgLayerBox->listLayers);
    }
}

inline bool isSelectionMask(KisNodeSP node)
{
    return dynamic_cast<KisSelectionMask*>(node.data());
}

KisNodeSP LayerBox::findNonHidableNode(KisNodeSP startNode)
{
    if (KisNodeManager::isNodeHidden(startNode, true) &&
            startNode->parent() &&
            !startNode->parent()->parent()) {


        KisNodeSP node = startNode->prevSibling();
        while (node && KisNodeManager::isNodeHidden(node, true)) {
            node = node->prevSibling();
        }

        if (!node) {
            node = startNode->nextSibling();
            while (node && KisNodeManager::isNodeHidden(node, true)) {
                node = node->nextSibling();
            }
        }

        if (!node) {
            node = m_image->root()->lastChild();
            while (node && KisNodeManager::isNodeHidden(node, true)) {
                node = node->prevSibling();
            }
        }

        KIS_ASSERT_RECOVER_NOOP(node && "cannot activate any node!");
        startNode = node;
    }

    return startNode;
}

void LayerBox::slotEditGlobalSelection(bool showSelections)
{
    KisNodeSP lastActiveNode = m_nodeManager->activeNode();
    KisNodeSP activateNode = lastActiveNode;
    KisSelectionMaskSP globalSelectionMask;

    if (!showSelections) {
        activateNode =
            m_savedNodeBeforeEditSelectionMode ?
                KisNodeSP(m_savedNodeBeforeEditSelectionMode) :
                findNonHidableNode(activateNode);
    }

    m_nodeModel->setShowGlobalSelection(showSelections);

    globalSelectionMask = m_image->rootLayer()->selectionMask();

    // try to find deactivated, but visible masks
    if (!globalSelectionMask) {
        KoProperties properties;
        properties.setProperty("visible", true);
        QList<KisNodeSP> masks = m_image->rootLayer()->childNodes(QStringList("KisSelectionMask"), properties);
        if (!masks.isEmpty()) {
            globalSelectionMask = dynamic_cast<KisSelectionMask*>(masks.first().data());
        }
    }

    // try to find at least any selection mask
    if (!globalSelectionMask) {
        KoProperties properties;
        QList<KisNodeSP> masks = m_image->rootLayer()->childNodes(QStringList("KisSelectionMask"), properties);
        if (!masks.isEmpty()) {
            globalSelectionMask = dynamic_cast<KisSelectionMask*>(masks.first().data());
        }
    }

    if (globalSelectionMask) {
        if (showSelections) {
            activateNode = globalSelectionMask;
        }
    }

    if (activateNode != lastActiveNode) {
        m_nodeManager->slotNonUiActivatedNode(activateNode);
    } else if (lastActiveNode) {
        setCurrentNode(lastActiveNode);
    }

    if (showSelections && !globalSelectionMask) {
        KisProcessingApplicator applicator(m_image, 0,
                                           KisProcessingApplicator::NONE,
                                           KisImageSignalVector(),
                                           kundo2_i18n("Quick Selection Mask"));

        applicator.applyCommand(
            new KisLayerUtils::KeepNodesSelectedCommand(
                m_nodeManager->selectedNodes(), KisNodeList(),
                lastActiveNode, 0, m_image, false),
            KisStrokeJobData::SEQUENTIAL, KisStrokeJobData::EXCLUSIVE);
        applicator.applyCommand(new KisSetEmptyGlobalSelectionCommand(m_image),
                                KisStrokeJobData::SEQUENTIAL,
                                KisStrokeJobData::EXCLUSIVE);
        applicator.applyCommand(new KisLayerUtils::SelectGlobalSelectionMask(m_image),
                                KisStrokeJobData::SEQUENTIAL,
                                KisStrokeJobData::EXCLUSIVE);

        applicator.end();
    } else if (!showSelections &&
               globalSelectionMask &&
               globalSelectionMask->selection()->selectedRect().isEmpty()) {

        KisProcessingApplicator applicator(m_image, 0,
                                           KisProcessingApplicator::NONE,
                                           KisImageSignalVector(),
                                           kundo2_i18n("Cancel Quick Selection Mask"));
        applicator.applyCommand(new KisSetGlobalSelectionCommand(m_image, 0), KisStrokeJobData::SEQUENTIAL, KisStrokeJobData::EXCLUSIVE);
        applicator.end();
    }

    if (showSelections) {
        m_savedNodeBeforeEditSelectionMode = lastActiveNode;
    }
}

void LayerBox::selectionChanged(const QModelIndexList &selection)
{
    if (!m_nodeManager) return;

    /**
     * When the user clears the extended selection by clicking on the
     * empty area of the docker, the selection should be reset on to
     * the active layer, which might be even unselected(!).
     */
    if (selection.isEmpty() && m_nodeManager->activeNode()) {
        QModelIndex selectedIndex =
                m_filteringModel->indexFromNode(m_nodeManager->activeNode());

        m_wdgLayerBox->listLayers->selectionModel()->
                setCurrentIndex(selectedIndex, QItemSelectionModel::ClearAndSelect);
        return;
    }

    QList<KisNodeSP> selectedNodes;
    Q_FOREACH (const QModelIndex &idx, selection) {
        // Precaution because node manager doesn't like duplicates in that list.
        // NodeView Selection behavior is SelectRows, although currently only column 0 allows selections.
        if (idx.column() != 0) {
            continue;
        }
        selectedNodes << m_filteringModel->nodeFromIndex(idx);
    }

    m_nodeManager->slotSetSelectedNodes(selectedNodes);
    updateUI();
}

void LayerBox::slotAdjustCurrentBeforeRemoveRows(const QModelIndex &parent, int start, int end)
{
    /**
     * Qt has changed its behavior when deleting an item. Previously
     * the selection priority was on the next item in the list, and
     * now it has changed to the previous item. Here we just adjust
     * the selected item after the node removal.
     *
     * This method is called right before the Qt's beginRemoveRows()
     * is called, that is we make sure that Qt will never have to
     * adjust the position of the removed cursor.
     *
     * See bug: https://bugs.kde.org/show_bug.cgi?id=345601
     */

    QModelIndex currentIndex = m_wdgLayerBox->listLayers->currentIndex();
    QAbstractItemModel *model = m_filteringModel;

    if (currentIndex.isValid() && parent == currentIndex.parent()
            && currentIndex.row() >= start && currentIndex.row() <= end) {
        QModelIndex old = currentIndex;

        if (model && end < model->rowCount(parent) - 1) // there are rows left below the change
            currentIndex = model->index(end + 1, old.column(), parent);
        else if (model && start > 0) // there are rows left above the change
            currentIndex = model->index(start - 1, old.column(), parent);
        else // there are no rows left in the table
            currentIndex = QModelIndex();

        if (currentIndex.isValid() && currentIndex != old) {
            m_wdgLayerBox->listLayers->setCurrentIndex(currentIndex);
        }
    }
}

void LayerBox::slotNodeManagerChangedSelection(const KisNodeList &nodes)
{
    if (!m_nodeManager) return;

    QModelIndexList newSelection;
    Q_FOREACH(KisNodeSP node, nodes) {
        newSelection << m_filteringModel->indexFromNode(node);
    }

    QItemSelectionModel *model = m_wdgLayerBox->listLayers->selectionModel();

    if (KritaUtils::compareListsUnordered(newSelection, model->selectedRows())) {
        return;
    }

    QItemSelection selection;
    Q_FOREACH(const QModelIndex &idx, newSelection) {
        selection.select(idx, idx);
    }

    model->select(selection, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
}

void LayerBox::slotRenameCurrentNode()
{
    m_wdgLayerBox->listLayers->edit(m_wdgLayerBox->listLayers->currentIndex());
}

void LayerBox::slotColorLabelChanged(int label)
{
    KisNodeList selectedNodes = m_nodeManager->selectedNodes();

    Q_FOREACH(KisNodeSP selectedNode, selectedNodes) {
        //Always apply label to selected nodes..
        selectedNode->setColorLabelIndex(label);

        //Apply label only to unlabelled children..
        KisNodeList children = selectedNode->childNodes(QStringList(), KoProperties());

        auto applyLabelFunc =
                [label](KisNodeSP child) {
            if (child->colorLabelIndex() == 0) {
                child->setColorLabelIndex(label);
            }
        };

        Q_FOREACH(KisNodeSP child, children) {
            KisLayerUtils::recursiveApplyNodes(child, applyLabelFunc);
        }
    }
}

void LayerBox::updateAvailableLabels()
{
    if (!m_image) return;
    layerFilterWidget->updateColorLabels(m_image->root());
}

void LayerBox::updateLayerFiltering()
{
    m_filteringModel->setAcceptedLabels(layerFilterWidget->getActiveColors());
    m_filteringModel->setTextFilter(layerFilterWidget->getTextFilter());
}

void LayerBox::slotImageTimeChanged(int time)
{
    Q_UNUSED(time);
    updateUI();
}

void LayerBox::updateLayerOpMenu(const QModelIndex &index, QMenu &menu) {

    KisNodeList nodes = m_nodeManager->selectedNodes();
    KisNodeSP activeNode = m_nodeManager->activeNode();
    const bool singleNode = nodes.size() == 1;

    if (!index.isValid()) {
        return;
    }

    /**
     * What is done to layers every day, by name, at the top; the rest in a
     * few lists by what they are about. Nothing that was here is gone.
     */
    addMenuEntry(&menu, "add_new_paint_layer", i18nc("@action:inmenu", "New Layer"));
    addNewLayerMenu(&menu, true);
    menu.addSeparator();

    addMenuEntry(&menu, "duplicatelayer", i18nc("@action:inmenu", "Duplicate"));
    addMenuEntry(&menu, "merge_layer", i18nc("@action:inmenu", "Merge Down"));
    addMenuEntry(&menu, "create_quick_group", i18nc("@action:inmenu", "Group"));
    if (singleNode && activeNode->inherits("KisGroupLayer")) {
        addMenuEntry(&menu, "quick_ungroup", i18nc("@action:inmenu", "Ungroup"));
    }
    addMenuEntry(&menu, m_removeAction, i18nc("@action:inmenu", "Delete"));
    menu.addSeparator();

    addMenuEntry(&menu, "copy_layer_clipboard", i18nc("@action:inmenu", "Copy"));
    addMenuEntry(&menu, "cut_layer_clipboard", i18nc("@action:inmenu", "Cut"));
    addMenuEntry(&menu, "paste_layer_from_clipboard", i18nc("@action:inmenu", "Paste"));
    menu.addSeparator();

    addMenuEntry(&menu, m_propertiesAction, i18nc("@action:inmenu", "Properties…"));
    KisLayerSP singleLayer = dynamic_cast<KisLayer*>(activeNode.data());
    if (singleLayer) {
        QMenu *style = menu.addMenu(i18nc("@title:menu layer style", "Style"));
        addMenuEntry(style, "layer_style", i18nc("@action:inmenu layer style", "Edit…"));
        if (singleLayer->layerStyle()) {
            addMenuEntry(style, "copy_layer_style", i18nc("@action:inmenu layer style", "Copy"));
        }
        if (KisClipboard::instance()->hasLayerStyles()) {
            addMenuEntry(style, "paste_layer_style", i18nc("@action:inmenu layer style", "Paste"));
        }
    }
    {
        KisSignalsBlocker b(m_colorSelector->colorLabelSelector());
        m_colorSelector->colorLabelSelector()->setCurrentIndex(singleNode ? activeNode->colorLabelIndex() : -1);
    }
    menu.addAction(m_colorSelectorAction);
    menu.addSeparator();

    QMenu *select = menu.addMenu(i18nc("@title:menu", "Select"));
    addMenuEntry(select, "select_all_layers", i18nc("@action:inmenu select layers", "All"));
    addMenuEntry(select, "select_visible_layers", i18nc("@action:inmenu select layers", "Visible"));
    addMenuEntry(select, "select_invisible_layers", i18nc("@action:inmenu select layers", "Hidden"));
    addMenuEntry(select, "select_locked_layers", i18nc("@action:inmenu select layers", "Locked"));
    addMenuEntry(select, "select_unlocked_layers", i18nc("@action:inmenu select layers", "Unlocked"));
    if (singleNode) {
        select->addSeparator();
        // a selection of the layer's painted pixels (not of layers)
        addMenuEntry(select, "selectopaque", i18nc("@action:inmenu", "Opaque Pixels"));
    }

    // only what can be done to this layer: the rest would be grayed lines to read past
    if (singleNode) {
        QMenu *convert = menu.addMenu(i18nc("@title:menu", "Convert"));
        addMenuEntry(convert, "convert_to_paint_layer", i18nc("@action:inmenu convert to", "To Paint Layer"), true);
        addMenuEntry(convert, "convert_to_transparency_mask", i18nc("@action:inmenu convert to", "To Transparency Mask"), true);
        addMenuEntry(convert, "convert_to_filter_mask", i18nc("@action:inmenu convert to", "To Filter Mask"), true);
        addMenuEntry(convert, "convert_to_selection_mask", i18nc("@action:inmenu convert to", "To Local Selection"), true);
        addMenuEntry(convert, "convert_to_file_layer", i18nc("@action:inmenu convert to", "To File Layer"), true);
        addMenuEntry(convert, "convert_group_to_animated", i18nc("@action:inmenu convert to", "To Animated Layer"), true);
        addMenuEntry(convert, "layercolorspaceconversion", i18nc("@action:inmenu", "Color Space…"), true);
        convert->addSeparator();
        addMenuEntry(convert, "split_alpha_into_mask", i18nc("@action:inmenu", "Alpha into Mask"), true);
        addMenuEntry(convert, "split_alpha_write", i18nc("@action:inmenu", "Write as Alpha"), true);
        addMenuEntry(convert, "split_alpha_save_merged", i18nc("@action:inmenu", "Save Merged…"), true);
    }

    QMenu *more = menu.addMenu(i18nc("@title:menu", "More"));
    addMenuEntry(more, "create_quick_clipping_group", i18nc("@action:inmenu", "Clipping Group"));
    if (singleNode) {
        addMenuEntry(more, "flatten_layer", i18nc("@action:inmenu", "Flatten Layer"));
        addMenuEntry(more, "flatten_image", i18nc("@action:inmenu", "Flatten Image"));
    }
    Q_FOREACH(KisNodeSP node, nodes) {
        if (node && node->inherits("KisCloneLayer")) {
            addMenuEntry(more, m_changeCloneSourceAction, i18nc("@action:inmenu", "Clone Source…"));
            break;
        }
    }
    more->addSeparator();
    addMenuEntry(more, "toggle_layer_visibility", i18nc("@action:inmenu", "Show or Hide"));
    addMenuEntry(more, "toggle_layer_lock", i18nc("@action:inmenu", "Lock or Unlock"));
    addMenuEntry(more, "toggle_layer_inherit_alpha", i18nc("@action:inmenu", "Clip to Layer Below"));
    addMenuEntry(more, "toggle_layer_alpha_lock", i18nc("@action:inmenu", "Lock Alpha"));
    more->addSeparator();
    if (singleNode) {
        KisNodeSP node = m_filteringModel->nodeFromIndex(index);
        if (node && !node->inherits("KisTransformMask")) {
            addMenuEntry(more, "isolate_active_layer", i18nc("@action:inmenu", "Isolate Layer"));
            addMenuEntry(more, "isolate_active_group", i18nc("@action:inmenu", "Isolate Group"));
        }
    }
    addMenuEntry(more, "pin_to_timeline", i18nc("@action:inmenu", "Pin to Timeline"));
}

void LayerBox::slotForgetAboutSavedNodeBeforeEditSelectionMode()
{
    m_savedNodeBeforeEditSelectionMode = 0;
}

void LayerBox::slotUpdateIcons() {
    m_wdgLayerBox->bnAdd->setIcon(KisIconUtils::loadIcon("addlayer"));
    m_wdgLayerBox->bnRaise->setIcon(KisIconUtils::loadIcon("arrowup"));
    m_wdgLayerBox->bnDelete->setIcon(KisIconUtils::loadIcon("deletelayer"));
    m_wdgLayerBox->bnLower->setIcon(KisIconUtils::loadIcon("arrowdown"));
    m_wdgLayerBox->bnProperties->setIcon(KisIconUtils::loadIcon("properties"));
    m_wdgLayerBox->bnDuplicate->setIcon(KisIconUtils::loadIcon("duplicatelayer"));
    m_wdgLayerBox->configureLayerDockerToolbar->setIcon(KisIconUtils::loadIcon("view-choose"));

    // call child function about needing to update icons
    m_wdgLayerBox->listLayers->slotUpdateIcons();
}

void LayerBox::toggleActiveLayerSolo() {
    NodeView* view = m_wdgLayerBox->listLayers;
    if (!view)
        return;

    KisNodeSP node = m_nodeManager->activeNode();
    if (!node)
        return;

    QModelIndex index = m_filteringModel->indexFromNode(node);
    if (!index.isValid())
        return;

    view->toggleSolo(index);
}

void LayerBox::slotUpdateThumbnailIconSize()
{
    KisConfig cfg(false);
    cfg.setLayerThumbnailSize(thumbnailSizeSlider->value());

    m_nodeModel->setPreferredThumnalSize(thumbnailSizeSlider->value() * m_wdgLayerBox->listLayers->devicePixelRatioF());
    m_wdgLayerBox->listLayers->slotConfigurationChanged();
}

void LayerBox::slotSyncThumbnailIconSize()
{
    m_nodeModel->setPreferredThumnalSize(thumbnailSizeSlider->value() * m_wdgLayerBox->listLayers->devicePixelRatioF());
}

void LayerBox::slotUpdateTreeIndentation()
{
    KisConfig cfg(false);
    if (indentationSlider->value() == cfg.layerTreeIndentation()) {
        return;
    }
    cfg.setLayerTreeIndentation(indentationSlider->value());
    m_wdgLayerBox->listLayers->slotConfigurationChanged();
}

void LayerBox::slotUpdateLayerInfoTextStyle()
{
    KisConfig cfg(false);
    if (infoTextCombobox->currentIndex() == cfg.layerInfoTextStyle()) {
        return;
    }
    cfg.setLayerInfoTextStyle((KisConfig::LayerInfoTextStyle)infoTextCombobox->currentIndex());
    m_wdgLayerBox->listLayers->slotConfigurationChanged();
    m_wdgLayerBox->listLayers->viewport()->update();
    if (infoTextCombobox->currentIndex() == 0) {
        infoTextOpacitySlider->setDisabled(true);
        infoTextInlineChkbox->setDisabled(true);
    }
    else {
        infoTextOpacitySlider->setDisabled(false);
        infoTextInlineChkbox->setDisabled(false);
    }
}

void LayerBox::slotUpdateLayerInfoTextOpacity()
{
    KisConfig cfg(false);
    if (infoTextOpacitySlider->value() == cfg.layerInfoTextOpacity()) {
        return;
    }
    cfg.setLayerInfoTextOpacity(infoTextOpacitySlider->value());
    m_wdgLayerBox->listLayers->slotConfigurationChanged();
    m_wdgLayerBox->listLayers->viewport()->update();
}

void LayerBox::slotUpdateUseInlineLayerInfoText()
{
    KisConfig cfg(false);
    if (infoTextInlineChkbox->isChecked() == cfg.useInlineLayerInfoText()) {
        return;
    }
    cfg.setUseInlineLayerInfoText(infoTextInlineChkbox->isChecked());
    m_wdgLayerBox->listLayers->slotConfigurationChanged();
    m_wdgLayerBox->listLayers->viewport()->update();
}


void LayerBox::slotUpdateUseLayerSelectionCheckbox()
{
    KisConfig cfg(false);
    if (layerSelectionCheckBox->isChecked() == cfg.useLayerSelectionCheckbox()) {
        return;
    }
    cfg.setUseLayerSelectionCheckbox(layerSelectionCheckBox->isChecked());
    m_wdgLayerBox->listLayers->slotConfigurationChanged();
    m_wdgLayerBox->listLayers->viewport()->update();
}

#include "moc_LayerBox.cpp"
