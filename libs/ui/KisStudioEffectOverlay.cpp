/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioEffectOverlay.h"

#include <QAbstractSpinBox>
#include <QApplication>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QScrollArea>
#include <QScrollBar>
#include <QSlider>
#include <QSpinBox>
#include <QTabletEvent>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <functional>

#include <filter/kis_filter.h>
#include <filter/kis_filter_configuration.h>
#include <klocalizedstring.h>
#include <kis_filter_mask.h>
#include <kis_layer.h>
#include <kis_paint_layer.h>
#include <kis_slider_spin_box.h>

#include "KisMainWindow.h"
#include "KisStudioMotion.h"
#include "KisStudioEffectsPicker.h"
#include "KisViewManager.h"
#include "canvas/kis_canvas2.h"
#include "input/KisInputActionGroup.h"
#include "kis_config.h"
#include "kis_filter_manager.h"
#include "kis_icon_utils.h"
#include "kis_node_manager.h"
#include "kis_node_commands_adapter.h"
#include "kis_selection.h"
#include "kis_signal_compressor.h"
#include "widgets/kis_filter_selector_widget.h"

namespace {

const QString editableKey = QStringLiteral("StudioEffectsEditable");
const QString expandedKey = QStringLiteral("StudioEffectsExpanded");
const QString hintsKey = QStringLiteral("StudioEffectsDragHints");
/// How many times the panel says what a drag on the canvas does, before it only shows the values
constexpr int hintsShown = 6;
/// A drag this long goes through a setting's whole (usual) range
constexpr qreal dragForWholeRange = 500.0;

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

QToolButton *iconButton(const QString &icon, const QString &toolTip, QWidget *parent)
{
    auto *button = new QToolButton(parent);
    button->setIcon(KisIconUtils::loadIcon(icon));
    button->setToolTip(toolTip);
    button->setAutoRaise(true);
    button->setFocusPolicy(Qt::NoFocus);
    button->setFixedSize(34, 30);
    return button;
}

/// One number setting of an effect that a drag on the canvas sets
struct Knob {
    QPointer<QWidget> widget; ///< a spin box or a slider
    QString label;
    qreal start {0};
    qreal span {1};

    qreal value() const
    {
        if (auto *box = qobject_cast<QDoubleSpinBox *>(widget)) return box->value();
        if (auto *box = qobject_cast<QSpinBox *>(widget)) return box->value();
        if (auto *slider = qobject_cast<QSlider *>(widget)) return slider->value();
        return 0;
    }
    void setValue(qreal value) const
    {
        // Krita's slider spin boxes keep their own state: through their own setValue
        if (auto *box = qobject_cast<KisDoubleSliderSpinBox *>(widget)) box->setValue(value);
        else if (auto *box = qobject_cast<KisSliderSpinBox *>(widget)) box->setValue(qRound(value));
        else if (auto *box = qobject_cast<QDoubleSpinBox *>(widget)) box->setValue(value);
        else if (auto *box = qobject_cast<QSpinBox *>(widget)) box->setValue(qRound(value));
        else if (auto *slider = qobject_cast<QSlider *>(widget)) slider->setValue(qRound(value));
    }
    QString text() const
    {
        if (auto *box = qobject_cast<QDoubleSpinBox *>(widget)) {
            return QString::number(box->value(), 'f', qMin(box->decimals(), 2)) + box->suffix();
        }
        if (auto *box = qobject_cast<QSpinBox *>(widget)) return QString::number(box->value()) + box->suffix();
        return QString::number(qRound(value()));
    }
};

/// The usual range of a setting: the soft range of Krita's sliders, else the whole one, not absurdly wide
qreal usualSpan(QWidget *widget)
{
    if (auto *slider = qobject_cast<KisDoubleSliderSpinBox *>(widget)) {
        if (slider->softMaximum() > slider->softMinimum()) return slider->softMaximum() - slider->softMinimum();
    }
    if (auto *slider = qobject_cast<KisSliderSpinBox *>(widget)) {
        if (slider->softMaximum() > slider->softMinimum()) return slider->softMaximum() - slider->softMinimum();
    }
    qreal span = 100;
    if (auto *box = qobject_cast<QDoubleSpinBox *>(widget)) span = box->maximum() - box->minimum();
    else if (auto *box = qobject_cast<QSpinBox *>(widget)) span = box->maximum() - box->minimum();
    else if (auto *slider = qobject_cast<QSlider *>(widget)) span = slider->maximum() - slider->minimum();
    return qBound<qreal>(1.0, span, 200.0);
}

/// The label next to a widget in its layout: a form's label, the cell on its left, the item before it
QLabel *layoutLabel(QWidget *widget)
{
    QWidget *parent = widget->parentWidget();
    if (!parent || !parent->layout()) return nullptr;
    std::function<QLabel *(QLayout *)> search = [&](QLayout *layout) -> QLabel * {
        if (auto *form = qobject_cast<QFormLayout *>(layout)) {
            if (QWidget *label = form->labelForField(widget)) return qobject_cast<QLabel *>(label);
        }
        const int index = layout->indexOf(widget);
        if (index >= 0) {
            if (auto *grid = qobject_cast<QGridLayout *>(layout)) {
                int row, column, rowSpan, columnSpan;
                grid->getItemPosition(index, &row, &column, &rowSpan, &columnSpan);
                for (int c = column - 1; c >= 0; c--) {
                    QLayoutItem *item = grid->itemAtPosition(row, c);
                    if (item && qobject_cast<QLabel *>(item->widget())) return qobject_cast<QLabel *>(item->widget());
                }
                return nullptr;
            }
            for (int i = index - 1; i >= 0; i--) {
                QLayoutItem *item = layout->itemAt(i);
                if (item && qobject_cast<QLabel *>(item->widget())) return qobject_cast<QLabel *>(item->widget());
            }
            return nullptr;
        }
        for (int i = 0; i < layout->count(); i++) {
            if (QLayout *inner = layout->itemAt(i)->layout()) {
                if (QLabel *label = search(inner)) return label;
                if (inner->indexOf(widget) >= 0) return nullptr;
            }
        }
        return nullptr;
    };
    return search(parent->layout());
}

/// What a setting is called: its label's text, without "&" and ":"
QString labelFor(QWidget *widget, QWidget *root)
{
    QLabel *best = layoutLabel(widget);
    int bestDistance = INT_MAX;
    const QRect at(widget->mapTo(root, QPoint(0, 0)), widget->size());
    Q_FOREACH (QLabel *label, best ? QList<QLabel *>() : root->findChildren<QLabel *>()) {
        if (!label->isVisibleTo(root) || label->text().isEmpty()) continue;
        if (label->buddy() == widget) {
            best = label;
            break;
        }
        // the label on its left in the same row, or right above it
        const QRect place(label->mapTo(root, QPoint(0, 0)), label->size());
        const bool sameRow = qAbs(place.center().y() - at.center().y()) < 12 && place.right() <= at.left() + 4;
        const bool above = place.bottom() <= at.top() + 2 && at.top() - place.bottom() < 16 && place.left() < at.right();
        if (!sameRow && !above) continue;
        const int distance = sameRow ? at.left() - place.right() : 1000 + at.top() - place.bottom();
        if (distance < bestDistance) {
            bestDistance = distance;
            best = label;
        }
    }
    QString text = best ? best->text() : QString();
    text.remove(QLatin1Char('&'));
    text = text.trimmed();
    if (text.endsWith(QLatin1Char(':'))) text.chop(1);
    return text;
}

/**
 * The settings a drag on the canvas sets: the first two number settings,
 * top to bottom; and whether the effect is simple, i.e. made of a few
 * numbers (and a switch or two), so that the panel can open small.
 */
QVector<Knob> findKnobs(QWidget *root, bool *simple)
{
    QVector<QWidget *> numbers;
    int others = 0;
    int switches = 0;
    Q_FOREACH (QWidget *widget, root->findChildren<QWidget *>()) {
        if (!widget->isVisibleTo(root) || !widget->isEnabled()) continue;
        // the parts of a spin box are not settings of their own
        if (qobject_cast<QAbstractSpinBox *>(widget->parentWidget())) continue;
        if (qobject_cast<QSpinBox *>(widget) || qobject_cast<QDoubleSpinBox *>(widget) || qobject_cast<QSlider *>(widget)) {
            numbers << widget;
        } else if (qobject_cast<QAbstractButton *>(widget)) {
            switches++;
        } else if (qobject_cast<QComboBox *>(widget)) {
            others++;
        } else if (!qobject_cast<QLabel *>(widget) && !qobject_cast<QScrollBar *>(widget) && !qobject_cast<QLineEdit *>(widget)
                   && (widget->focusPolicy() & Qt::ClickFocus) && widget->findChildren<QWidget *>().isEmpty()) {
            // a curve, a gradient, levels: something to work on, not a number
            others++;
        }
    }
    std::stable_sort(numbers.begin(), numbers.end(), [root](QWidget *a, QWidget *b) {
        const QPoint pa = a->mapTo(root, QPoint(0, 0));
        const QPoint pb = b->mapTo(root, QPoint(0, 0));
        return pa.y() != pb.y() ? pa.y() < pb.y() : pa.x() < pb.x();
    });
    if (simple) {
        *simple = !numbers.isEmpty() && numbers.size() <= 3 && others == 0 && switches <= 3;
    }
    QVector<Knob> knobs;
    for (int i = 0; i < numbers.size() && knobs.size() < 2; i++) {
        Knob knob;
        knob.widget = numbers[i];
        knob.label = labelFor(numbers[i], root);
        knobs << knob;
    }
    return knobs;
}

/// The value by the pointer while dragging on the canvas
class ValuePill : public QWidget
{
public:
    explicit ValuePill(QWidget *parent) : QWidget(parent)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        hide();
    }

    void showAt(const QPoint &pos, const QString &text)
    {
        m_text = text;
        QFont font = this->font();
        font.setBold(true);
        const QFontMetrics metrics(font);
        const QSize size(metrics.horizontalAdvance(text) + 22, metrics.height() + 12);
        QPoint place = pos + QPoint(18, -size.height() - 10);
        if (place.x() + size.width() > parentWidget()->width() - 4) place.setX(pos.x() - size.width() - 18);
        if (place.y() < 4) place.setY(pos.y() + 18);
        setGeometry(QRect(place, size));
        show();
        raise();
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        QColor background = palette().color(QPalette::Window);
        background.setAlphaF(0.94);
        painter.setPen(Qt::NoPen);
        painter.setBrush(background);
        painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), height() / 2.0, height() / 2.0);
        QFont font = this->font();
        font.setBold(true);
        painter.setFont(font);
        painter.setPen(palette().color(QPalette::WindowText));
        painter.drawText(rect(), Qt::AlignCenter, m_text);
    }

private:
    QString m_text;
};

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
    QLabel *hint {nullptr};
    QToolButton *preview {nullptr};
    QToolButton *editable {nullptr};
    QToolButton *expand {nullptr};
    KisSignalCompressor compressor;
    /// No painting while an effect is being adjusted, only zooming and panning
    QScopedPointer<KisInputActionGroupsMaskGuard> guard;

    /// The settings a drag on the canvas sets
    QVector<Knob> knobs;
    bool simple {false};
    bool expanded {true};
    bool showHint {false};
    QPointer<QWidget> canvasWidget;
    QPointer<ValuePill> pill;
    bool dragging {false};
    QPointF dragStart;
    /// The second setting follows the first by itself
    bool secondLinked {false};
    bool cursorSet {false};
    /// Space held: the canvas pans, the drag is not the effect's
    bool spaceHeld {false};

    KisViewManager *viewManager() const { return mainWindow->viewManager(); }
    KisFilterManager *filterManager() const { return viewManager() ? viewManager()->filterManager() : nullptr; }
    bool maskPossible() const { return node && filter && !node->inherits("KisMask") && filter->supportsAdjustmentLayers(); }

    /// "↔ Radius 5 px · ↕ Amount 0.50", with what a drag does the first few times
    void updateHint()
    {
        if (!hint) return;
        QStringList parts;
        for (int i = 0; i < knobs.size(); i++) {
            if (!knobs[i].widget) continue;
            parts << QStringLiteral("%1 %2 %3").arg(i == 0 ? QStringLiteral("↔") : QStringLiteral("↕"), knobs[i].label, knobs[i].text());
        }
        QString text = parts.join(QStringLiteral("   "));
        if (showHint && !text.isEmpty()) {
            text = i18nc("@info what a drag on the canvas sets, %1 the settings", "Drag on the canvas: %1", text);
        }
        // one line, as much of it as fits
        hint->setToolTip(text);
        hint->setText(hint->fontMetrics().elidedText(text, Qt::ElideRight, qMax(120, hint->width() - hint->contentsMargins().left() - 8)));
        hint->setVisible(!text.isEmpty());
    }

    void setCursor(bool on)
    {
        if (on == cursorSet) return;
        cursorSet = on;
        if (on) QApplication::setOverrideCursor(knobs.size() > 1 ? Qt::SizeAllCursor : Qt::SizeHorCursor);
        else QApplication::restoreOverrideCursor();
    }
};

KisStudioEffectOverlay::KisStudioEffectOverlay(KisMainWindow *mainWindow)
    : QWidget(mainWindow->centralWidget())
    , d(new Private)
{
    d->mainWindow = mainWindow;
    setObjectName(QStringLiteral("studioEffectOverlay"));
    setFocusPolicy(Qt::StrongFocus);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(14, 8, 10, 10);
    layout->setSpacing(4);

    auto *header = new QHBoxLayout();
    header->setSpacing(4);
    d->icon = new QLabel(this);
    header->addWidget(d->icon);
    header->addSpacing(4);
    d->title = new QLabel(this);
    QFont titleFont = d->title->font();
    titleFont.setWeight(QFont::DemiBold);
    d->title->setFont(titleFont);
    header->addWidget(d->title, 1);

    d->expand = new QToolButton(this);
    d->expand->setAutoRaise(true);
    d->expand->setFocusPolicy(Qt::NoFocus);
    d->expand->setCheckable(true);
    d->expand->setText(i18nc("@action:button the effect's settings", "Settings"));
    d->expand->setToolTip(i18nc("@info:tooltip", "All its settings"));
    d->expand->setFixedHeight(30);
    connect(d->expand, &QToolButton::toggled, this, [this](bool on) {
        setExpanded(on);
        if (d->filter) {
            QStringList expanded = KisConfig(true).readEntry<QStringList>(expandedKey, QStringList());
            expanded.removeAll(d->filter->id());
            if (on) expanded << d->filter->id();
            KisConfig(false).writeEntry(expandedKey, expanded);
        }
    });
    header->addWidget(d->expand);

    // the layer without it, and back: what the dialog's Preview box did
    d->preview = iconButton(QStringLiteral("visible"), i18nc("@info:tooltip", "Preview"), this);
    d->preview->setCheckable(true);
    d->preview->setChecked(true);
    connect(d->preview, &QToolButton::toggled, this, [this](bool on) {
        d->preview->setIcon(KisIconUtils::loadIcon(on ? QStringLiteral("visible") : QStringLiteral("novisible")));
        KisFilterManager *filterManager = d->filterManager();
        if (!filterManager) return;
        if (on) {
            startApplying();
        } else if (filterManager->isStrokeRunning()) {
            d->compressor.stop();
            filterManager->cancelRunningStroke();
        }
    });
    header->addWidget(d->preview);

    // kept as a filter mask, to change or remove later; remembered
    d->editable = iconButton(QStringLiteral("filterMask"), i18nc("@info:tooltip", "Editable: keep it as a filter mask"), this);
    d->editable->setCheckable(true);
    d->editable->setChecked(KisConfig(true).readEntry<bool>(editableKey, true));
    connect(d->editable, &QToolButton::toggled, this, [](bool on) { KisConfig(false).writeEntry(editableKey, on); });
    header->addWidget(d->editable);

    QToolButton *cancel = roundButton(QStringLiteral("✕"), i18nc("@info:tooltip", "Drop it (Esc)"), this);
    connect(cancel, &QToolButton::clicked, this, &KisStudioEffectOverlay::reject);
    header->addWidget(cancel);
    QToolButton *apply = roundButton(QStringLiteral("✓"), i18nc("@info:tooltip", "Keep it (Enter)"), this);
    connect(apply, &QToolButton::clicked, this, &KisStudioEffectOverlay::accept);
    header->addWidget(apply);
    layout->addLayout(header);

    d->hint = new QLabel(this);
    QPalette hintPalette = d->hint->palette();
    QColor hintInk = hintPalette.color(QPalette::WindowText);
    hintInk.setAlphaF(0.6);
    hintPalette.setColor(QPalette::WindowText, hintInk);
    d->hint->setPalette(hintPalette);
    d->hint->setContentsMargins(26, 0, 0, 2);
    layout->addWidget(d->hint);

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

KisStudioEffectOverlay::~KisStudioEffectOverlay()
{
    d->setCursor(false);
}

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
    connect(selector, &KisFilterSelectorWidget::configurationChanged, this, [this]() { d->updateHint(); });
    connect(selector, &KisFilterSelectorWidget::sigSizeChanged, this, &KisStudioEffectOverlay::reposition);
    d->scroll->setWidget(selector);
    d->selector = selector;
    selector->adjustSize();

    d->title->setText(filter->name());
    d->icon->setPixmap(KisIconUtils::loadIcon(QStringLiteral("studio_effects")).pixmap(18, 18));
    d->editable->setVisible(d->maskPossible());
    {
        QSignalBlocker blocker(d->preview);
        d->preview->setChecked(true);
        d->preview->setIcon(KisIconUtils::loadIcon(QStringLiteral("visible")));
    }

    // what a drag on the canvas sets; a few numbers only: the panel opens small
    d->knobs = findKnobs(selector, &d->simple);
    // a curve or levels are worked on in the panel: a drag would set something arbitrary
    if (!d->simple) {
        d->knobs.clear();
    }
    const int hintsSoFar = KisConfig(true).readEntry<int>(hintsKey, 0);
    d->showHint = !d->knobs.isEmpty() && hintsSoFar < hintsShown;
    if (d->showHint) {
        KisConfig(false).writeEntry(hintsKey, hintsSoFar + 1);
    }
    d->updateHint();
    const bool expanded = !d->simple || KisConfig(true).readEntry<QStringList>(expandedKey, QStringList()).contains(filter->id());
    {
        QSignalBlocker blocker(d->expand);
        d->expand->setChecked(expanded);
    }
    d->expand->setVisible(d->simple);
    setExpanded(expanded);

    if (!d->knobs.isEmpty()) {
        d->canvasWidget = viewManager->canvasBase()->canvasWidget();
        if (d->canvasWidget) {
            d->canvasWidget->installEventFilter(this);
            d->pill = new ValuePill(d->canvasWidget);
            if (d->canvasWidget->rect().contains(d->canvasWidget->mapFromGlobal(QCursor::pos()))) {
                d->setCursor(true);
            }
        }
    }

    // Enter and Esc wherever the focus went (a drag on the canvas takes it there), Space for panning
    qApp->installEventFilter(this);
    d->guard.reset(new KisInputActionGroupsMaskGuard(viewManager->canvasBase()->inputActionGroupsMaskInterface(),
                                                     ViewTransformActionGroup));
    KisStudioMotion::showFaded(this);
    raise();
    reposition();
    setFocus();
    startApplying();
    return true;
}

void KisStudioEffectOverlay::setExpanded(bool expanded)
{
    d->expanded = expanded;
    d->scroll->setVisible(expanded);
    reposition();
}

void KisStudioEffectOverlay::startApplying()
{
    KisFilterManager *filterManager = d->filterManager();
    if (!d->selector || !filterManager || !d->preview->isChecked()) {
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
    // editable: a filter mask instead of new pixels
    if (d->editable->isChecked() && d->maskPossible()) {
        createMask();
        return;
    }
    d->compressor.stop();
    if (!d->preview->isChecked()) {
        QSignalBlocker blocker(d->preview);
        d->preview->setChecked(true);
    }
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
    d->setCursor(false);
    d->dragging = false;
    if (d->canvasWidget) {
        d->canvasWidget->removeEventFilter(this);
    }
    qApp->removeEventFilter(this);
    delete d->pill;
    d->canvasWidget = nullptr;
    d->knobs.clear();
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
    QTimer::singleShot(0, this, [this]() { d->updateHint(); });
    if (!parentWidget()) {
        return;
    }
    const QSize area = parentWidget()->size();
    const int width = qMin(520, area.width() - 40);
    int height = 46 + (d->hint && !d->hint->isHidden() ? d->hint->sizeHint().height() + 4 : 0);
    if (d->expanded) {
        const int contents = d->selector ? d->selector->sizeHint().height() : 0;
        height = qMin(height + contents + 20, int(area.height() * 0.55));
    }
    setGeometry((area.width() - width) / 2, area.height() - height - 54, width, height);
    raise();
}

bool KisStudioEffectOverlay::handleCanvasDrag(QEvent *event)
{
    QPointF pos;
    bool press = false;
    bool move = false;
    bool release = false;
    switch (event->type()) {
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick: {
        auto *mouse = static_cast<QMouseEvent *>(event);
        if (mouse->button() != Qt::LeftButton || mouse->modifiers() != Qt::NoModifier) return false;
        pos = mouse->localPos();
        press = true;
        break;
    }
    case QEvent::MouseMove:
        pos = static_cast<QMouseEvent *>(event)->localPos();
        move = true;
        break;
    case QEvent::MouseButtonRelease:
        if (static_cast<QMouseEvent *>(event)->button() != Qt::LeftButton) return false;
        pos = static_cast<QMouseEvent *>(event)->localPos();
        release = true;
        break;
    case QEvent::TabletPress: {
        auto *tablet = static_cast<QTabletEvent *>(event);
        if (tablet->button() != Qt::LeftButton || tablet->modifiers() != Qt::NoModifier) return false;
        pos = tablet->posF();
        press = true;
        break;
    }
    case QEvent::TabletMove:
        pos = static_cast<QTabletEvent *>(event)->posF();
        move = true;
        break;
    case QEvent::TabletRelease:
        pos = static_cast<QTabletEvent *>(event)->posF();
        release = true;
        break;
    default:
        return false;
    }

    if (press) {
        if (d->spaceHeld || d->knobs.isEmpty()) return false;
        d->dragging = true;
        d->dragStart = pos;
        d->secondLinked = false;
        for (Knob &knob : d->knobs) {
            knob.start = knob.value();
            knob.span = usualSpan(knob.widget);
        }
        event->accept();
        return true;
    }
    if (!d->dragging) {
        return false;
    }
    if (move) {
        // sideways the first setting, up and down (up is more) the second
        const QPointF delta = pos - d->dragStart;
        QStringList shown;
        const Knob &first = d->knobs[0];
        if (first.widget) {
            first.setValue(first.start + delta.x() * first.span / dragForWholeRange);
            shown << QStringLiteral("%1 %2").arg(first.label, first.text());
        }
        if (d->knobs.size() > 1 && d->knobs[1].widget) {
            const Knob &second = d->knobs[1];
            // linked to the first (e.g. both radii of a blur): it follows, the drag does not set it too
            if (!d->secondLinked && second.value() != second.start && qAbs(delta.y()) <= 6) {
                d->secondLinked = true;
            }
            if (!d->secondLinked && qAbs(delta.y()) > 6) {
                second.setValue(second.start - delta.y() * second.span / dragForWholeRange);
                shown << QStringLiteral("%1 %2").arg(second.label, second.text());
            }
        }
        if (d->pill) d->pill->showAt(pos.toPoint(), shown.join(QStringLiteral("   ")));
    }
    if (release) {
        d->dragging = false;
        if (d->pill) d->pill->hide();
    }
    event->accept();
    return true;
}

bool KisStudioEffectOverlay::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == parentWidget() && event->type() == QEvent::Resize && isVisible()) {
        reposition();
    }
    if (d->canvasWidget && watched == d->canvasWidget && isVisible()) {
        if (event->type() == QEvent::Enter) d->setCursor(true);
        if (event->type() == QEvent::Leave && !d->dragging) d->setCursor(false);
        if (handleCanvasDrag(event)) return true;
    }
    // Enter keeps it, Esc drops it, also when the focus is on the canvas (but not in a field of the panel)
    if (event->type() == QEvent::KeyPress && isVisible()) {
        auto *key = static_cast<QKeyEvent *>(event);
        QWidget *focus = QApplication::focusWidget();
        const bool inPanel = focus && (focus == this || isAncestorOf(focus));
        if (!inPanel && (watched == focus || (!focus && watched == window()))) {
            if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
                accept();
                return true;
            }
            if (key->key() == Qt::Key_Escape) {
                reject();
                return true;
            }
        }
    }
    // Space pans the canvas: a drag meanwhile is not the effect's
    if ((event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease)
        && static_cast<QKeyEvent *>(event)->key() == Qt::Key_Space && !static_cast<QKeyEvent *>(event)->isAutoRepeat()) {
        d->spaceHeld = event->type() == QEvent::KeyPress;
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
