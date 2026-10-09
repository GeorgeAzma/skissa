/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioEffectOverlay.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QPointer>
#include <QScrollArea>
#include <QToolButton>
#include <QVBoxLayout>

#include <filter/kis_filter.h>
#include <filter/kis_filter_configuration.h>
#include <klocalizedstring.h>
#include <kis_filter_mask.h>
#include <kis_layer.h>
#include <kis_paint_layer.h>

#include "KisMainWindow.h"
#include "KisStudioMotion.h"
#include "KisStudioEffectsPicker.h"
#include "KisViewManager.h"
#include "canvas/kis_canvas2.h"
#include "input/KisInputActionGroup.h"
#include "kis_filter_manager.h"
#include "kis_icon_utils.h"
#include "kis_node_manager.h"
#include "kis_node_commands_adapter.h"
#include "kis_selection.h"
#include "kis_signal_compressor.h"
#include "widgets/kis_filter_selector_widget.h"

namespace {

QToolButton *roundButton(const QString &glyph, const QString &toolTip, QWidget *parent)
{
    auto *button = new QToolButton(parent);
    button->setText(glyph);
    button->setToolTip(toolTip);
    button->setAutoRaise(true);
    button->setFocusPolicy(Qt::NoFocus);
    button->setFixedSize(34, 30);
    QFont font = button->font();
    font.setPointSizeF(font.pointSizeF() * 1.2);
    button->setFont(font);
    return button;
}

} // namespace

struct KisStudioEffectOverlay::Private
{
    Private() : compressor(200, KisSignalCompressor::FIRST_ACTIVE) {}

    KisMainWindow *mainWindow {nullptr};
    KisFilterSP filter;
    KisNodeSP node;
    QPointer<KisFilterSelectorWidget> selector;
    QScrollArea *scroll {nullptr};
    QLabel *icon {nullptr};
    QLabel *title {nullptr};
    QToolButton *mask {nullptr};
    KisSignalCompressor compressor;
    /// No painting while an effect is being adjusted, only zooming and panning
    QScopedPointer<KisInputActionGroupsMaskGuard> guard;

    KisViewManager *viewManager() const { return mainWindow->viewManager(); }
    KisFilterManager *filterManager() const { return viewManager() ? viewManager()->filterManager() : nullptr; }
};

KisStudioEffectOverlay::KisStudioEffectOverlay(KisMainWindow *mainWindow)
    : QWidget(mainWindow->centralWidget())
    , d(new Private)
{
    d->mainWindow = mainWindow;
    setObjectName(QStringLiteral("studioEffectOverlay"));
    setFocusPolicy(Qt::StrongFocus);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(14, 8, 10, 12);
    layout->setSpacing(6);

    auto *header = new QHBoxLayout();
    header->setSpacing(8);
    d->icon = new QLabel(this);
    header->addWidget(d->icon);
    d->title = new QLabel(this);
    QFont titleFont = d->title->font();
    titleFont.setWeight(QFont::DemiBold);
    d->title->setFont(titleFont);
    header->addWidget(d->title, 1);

    d->mask = new QToolButton(this);
    d->mask->setIcon(KisIconUtils::loadIcon(QStringLiteral("filterMask")));
    d->mask->setToolTip(i18nc("@info:tooltip", "Keep it editable: add it as a filter mask"));
    d->mask->setAutoRaise(true);
    d->mask->setFocusPolicy(Qt::NoFocus);
    d->mask->setFixedSize(34, 30);
    connect(d->mask, &QToolButton::clicked, this, &KisStudioEffectOverlay::createMask);
    header->addWidget(d->mask);

    QToolButton *cancel = roundButton(QStringLiteral("✕"), i18nc("@info:tooltip", "Drop it (Esc)"), this);
    connect(cancel, &QToolButton::clicked, this, &KisStudioEffectOverlay::reject);
    header->addWidget(cancel);
    QToolButton *apply = roundButton(QStringLiteral("✓"), i18nc("@info:tooltip", "Keep it (Enter)"), this);
    connect(apply, &QToolButton::clicked, this, &KisStudioEffectOverlay::accept);
    header->addWidget(apply);
    layout->addLayout(header);

    d->scroll = new QScrollArea(this);
    d->scroll->setFrameShape(QFrame::NoFrame);
    d->scroll->setWidgetResizable(true);
    d->scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    d->scroll->viewport()->setAutoFillBackground(false);
    layout->addWidget(d->scroll, 1);

    connect(&d->compressor, &KisSignalCompressor::timeout, this, &KisStudioEffectOverlay::startApplying);
    parentWidget()->installEventFilter(this);

    // the preview belongs to one layer of one document: leaving either drops it
    connect(mainWindow, &KisMainWindow::activeViewChanged, this, [this]() {
        if (isVisible()) reject();
    });
    if (KisViewManager *viewManager = mainWindow->viewManager()) {
        connect(viewManager->nodeManager(), &KisNodeManager::sigNodeActivated, this, [this](KisNodeSP node) {
            if (isVisible() && node != d->node) reject();
        });
    }
    hide();
}

KisStudioEffectOverlay::~KisStudioEffectOverlay() = default;

bool KisStudioEffectOverlay::open(KisFilterSP filter, KisFilterConfigurationSP overrideDefaultConfig)
{
    KisViewManager *viewManager = d->viewManager();
    KisFilterManager *filterManager = d->filterManager();
    if (!filter || !viewManager || !filterManager || !viewManager->activeNode() || !viewManager->activeNode()->paintDevice()
        || !viewManager->canvasBase()) {
        return false;
    }

    // another effect replaces the one being adjusted
    if (isVisible()) {
        reject();
    }

    d->filter = filter;
    d->node = viewManager->activeNode();

    auto *selector = new KisFilterSelectorWidget(d->scroll);
    selector->setView(viewManager);
    selector->showFilterGallery(false);
    selector->setPaintDevice(true, d->node->paintDevice());
    selector->setFilter(filter, overrideDefaultConfig);
    // the settings only: presets and XML stay in the dialog
    for (const char *name : {"comboBoxPresets", "chkRememberPreset", "pushButtonEditPresets", "btnXML", "filtersSelector"}) {
        if (QWidget *widget = selector->findChild<QWidget *>(QLatin1String(name))) {
            widget->hide();
        }
    }
    connect(selector, &KisFilterSelectorWidget::configurationChanged, &d->compressor, &KisSignalCompressor::start);
    connect(selector, &KisFilterSelectorWidget::sigSizeChanged, this, &KisStudioEffectOverlay::reposition);
    d->scroll->setWidget(selector);
    d->selector = selector;

    d->title->setText(filter->name());
    d->icon->setPixmap(KisIconUtils::loadIcon(QStringLiteral("studio_effects")).pixmap(18, 18));
    d->mask->setVisible(!d->node->inherits("KisMask") && filter->supportsAdjustmentLayers());

    d->guard.reset(new KisInputActionGroupsMaskGuard(viewManager->canvasBase()->inputActionGroupsMaskInterface(),
                                                     ViewTransformActionGroup));
    KisStudioMotion::showFaded(this);
    raise();
    reposition();
    setFocus();
    startApplying();
    return true;
}

void KisStudioEffectOverlay::startApplying()
{
    KisFilterManager *filterManager = d->filterManager();
    if (!d->selector || !filterManager) {
        return;
    }
    KisFilterConfigurationSP config = d->selector->configuration();
    if (!config) {
        return;
    }
    if (d->node && d->node->inherits("KisPaintLayer")) {
        config->setChannelFlags(qobject_cast<KisPaintLayer *>(d->node.data())->channelLockFlags());
    }
    filterManager->apply(config);
}

void KisStudioEffectOverlay::accept()
{
    KisFilterManager *filterManager = d->filterManager();
    if (!isVisible() || !filterManager) {
        return;
    }
    d->compressor.stop();
    if (!filterManager->isStrokeRunning()) {
        startApplying();
    }
    if (filterManager->isStrokeRunning()) {
        // only the frame on screen: the overlay has no switch for the other selected frames
        filterManager->setFilterAllSelectedFrames(false);
        filterManager->finish();
    }
    // finish() remembers the settings as the filter's last used ones
    if (d->filter) {
        KisStudioEffectsPicker::recordUse(d->filter->id());
    }
    finishEditing();
}

void KisStudioEffectOverlay::reject()
{
    KisFilterManager *filterManager = d->filterManager();
    d->compressor.stop();
    if (filterManager && filterManager->isStrokeRunning()) {
        filterManager->cancelRunningStroke();
    }
    finishEditing();
}

void KisStudioEffectOverlay::createMask()
{
    KisViewManager *viewManager = d->viewManager();
    KisFilterManager *filterManager = d->filterManager();
    if (!viewManager || !filterManager || !d->selector || !d->node || d->node->inherits("KisMask")) {
        return;
    }
    KisFilterConfigurationSP config = d->selector->configuration();
    KisLayer *layer = qobject_cast<KisLayer *>(d->node.data());
    if (!config || !layer) {
        return;
    }
    d->compressor.stop();
    if (filterManager->isStrokeRunning()) {
        filterManager->cancelRunningStroke();
        if (!viewManager->blockUntilOperationsFinished(viewManager->image())) {
            startApplying();
            return;
        }
    }

    KisFilterMaskSP mask = new KisFilterMask(viewManager->image(), d->filter->name());
    mask->initSelection(viewManager->selection(), layer);
    mask->setFilter(config->cloneWithResourcesSnapshot());
    if (layer->allowAsChild(mask)) {
        KisNodeCommandsAdapter adapter(viewManager);
        adapter.addNode(mask, layer, layer->lastChild());
        KisStudioEffectsPicker::recordUse(d->filter->id());
    }
    finishEditing();
}

void KisStudioEffectOverlay::finishEditing()
{
    d->guard.reset();
    hide();
    if (d->selector) {
        d->scroll->takeWidget();
        d->selector->deleteLater();
        d->selector = nullptr;
    }
    d->filter = nullptr;
    d->node = nullptr;
}

void KisStudioEffectOverlay::reposition()
{
    if (!parentWidget()) {
        return;
    }
    const QSize area = parentWidget()->size();
    const int width = qMin(520, area.width() - 40);
    const int contents = d->selector ? d->selector->sizeHint().height() : 0;
    const int height = qMin(contents + 70, int(area.height() * 0.55));
    setGeometry((area.width() - width) / 2, area.height() - height - 54, width, height);
    raise();
}

bool KisStudioEffectOverlay::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == parentWidget() && event->type() == QEvent::Resize && isVisible()) {
        reposition();
    }
    return QWidget::eventFilter(watched, event);
}

void KisStudioEffectOverlay::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape) {
        reject();
        return;
    }
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void KisStudioEffectOverlay::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    QColor background = palette().color(QPalette::Window);
    background.setAlphaF(0.95);
    QColor border = palette().color(QPalette::WindowText);
    border.setAlphaF(0.14);
    painter.setPen(QPen(border, 1));
    painter.setBrush(background);
    painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 14, 14);
}
