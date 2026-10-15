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
#include <QGroupBox>
#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPointer>
#include <QScrollArea>
#include <QScrollBar>
#include <QSlider>
#include <QSpinBox>
#include <QTabletEvent>
#include <QTextEdit>
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
#include <KisGlobalResourcesInterface.h>
#include <kis_config_widget.h>

#include "KisMainWindow.h"
#include "KisStudioGlass.h"
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
#include <KisModernStyle.h>

namespace {

const QString editableKey = QStringLiteral("StudioEffectsEditable");
/// The drag distance, in pixels, that covers a setting's whole usual range
constexpr qreal dragForWholeRange = 500.0;

QToolButton *headerButton(QWidget *parent, const QString &toolTip)
{
    auto *button = new QToolButton(parent);
    button->setToolTip(toolTip);
    button->setAutoRaise(true);
    button->setFocusPolicy(Qt::NoFocus);
    button->setFixedSize(28, 26);
    return button;
}

QToolButton *glyphButton(const QString &glyph, const QString &toolTip, QWidget *parent)
{
    QToolButton *button = headerButton(parent, toolTip);
    button->setText(glyph);
    QFont font = button->font();
    font.setPointSizeF(font.pointSizeF() * 1.2);
    button->setFont(font);
    return button;
}

QToolButton *iconButton(const QString &icon, const QString &toolTip, QWidget *parent)
{
    QToolButton *button = headerButton(parent, toolTip);
    button->setIcon(KisIconUtils::loadIcon(icon));
    return button;
}

bool isTextInput(QWidget *widget)
{
    if (auto *combo = qobject_cast<QComboBox *>(widget)) return combo->isEditable();
    return qobject_cast<QLineEdit *>(widget) || qobject_cast<QAbstractSpinBox *>(widget)
        || qobject_cast<QTextEdit *>(widget) || qobject_cast<QPlainTextEdit *>(widget);
}

/// A numeric setting of the effect, controlled by dragging on the canvas
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
        // Krita's slider spin boxes keep their own state, so call their own (non-virtual) setValue
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

template<class T>
qreal softSpan(QWidget *widget)
{
    auto *slider = qobject_cast<T *>(widget);
    return slider ? slider->softMaximum() - slider->softMinimum() : 0;
}

/// The usual range of a setting: the soft range of Krita's sliders, else the full range capped at 200
qreal usualSpan(QWidget *widget)
{
    const qreal soft = qMax(softSpan<KisDoubleSliderSpinBox>(widget), softSpan<KisSliderSpinBox>(widget));
    if (soft > 0) return soft;
    qreal span = 100;
    if (auto *box = qobject_cast<QDoubleSpinBox *>(widget)) span = box->maximum() - box->minimum();
    else if (auto *box = qobject_cast<QSpinBox *>(widget)) span = box->maximum() - box->minimum();
    else if (auto *slider = qobject_cast<QSlider *>(widget)) span = slider->maximum() - slider->minimum();
    return qBound<qreal>(1.0, span, 200.0);
}

/// The label beside \p widget in its layout: its form label, the grid cell to its left, or the item before it
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

/// A setting's name: its label's text, without "&" and the trailing ":"
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
        // otherwise the nearest label to its left in the same row, or just above it
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
 * Decides whether the effect is simple (a few numbers and switches, so the panel opens compact)
 * and, if so, returns the settings a canvas drag controls: its first two numbers, top to bottom.
 */
QVector<Knob> findKnobs(QWidget *root, bool *simple)
{
    QVector<QWidget *> numbers;
    int others = 0;
    int switches = 0;
    Q_FOREACH (QWidget *widget, root->findChildren<QWidget *>()) {
        if (!widget->isVisibleTo(root) || !widget->isEnabled()) continue;
        if (qobject_cast<QAbstractSpinBox *>(widget->parentWidget())) continue;
        if (qobject_cast<QSpinBox *>(widget) || qobject_cast<QDoubleSpinBox *>(widget) || qobject_cast<QSlider *>(widget)) {
            numbers << widget;
        } else if (qobject_cast<QAbstractButton *>(widget)) {
            switches++;
        } else if (qobject_cast<QComboBox *>(widget)) {
            others++;
        } else if (!qobject_cast<QLabel *>(widget) && !qobject_cast<QScrollBar *>(widget) && !qobject_cast<QLineEdit *>(widget)
                   && (widget->focusPolicy() & Qt::ClickFocus) && widget->findChildren<QWidget *>().isEmpty()) {
            // custom editors (curves, gradients, levels) belong in the panel; a drag would set something arbitrary
            others++;
        }
    }
    *simple = !numbers.isEmpty() && numbers.size() <= 3 && others == 0 && switches <= 3;
    if (!*simple) return {};

    std::stable_sort(numbers.begin(), numbers.end(), [root](QWidget *a, QWidget *b) {
        const QPoint pa = a->mapTo(root, QPoint(0, 0));
        const QPoint pb = b->mapTo(root, QPoint(0, 0));
        return pa.y() != pb.y() ? pa.y() < pb.y() : pa.x() < pb.x();
    });
    QVector<Knob> knobs;
    for (int i = 0; i < numbers.size() && knobs.size() < 2; i++) {
        Knob knob;
        knob.widget = numbers[i];
        knob.label = labelFor(numbers[i], root);
        knobs << knob;
    }
    return knobs;
}

/// Makes scroll areas as tall as their contents, up to 400 px, instead of the size they ask for themselves
void fitScrolls(QWidget *root)
{
    Q_FOREACH (QScrollArea *scroll, root->findChildren<QScrollArea *>()) {
        if (!scroll->widget()) continue;
        const int held = scroll->widget()->sizeHint().height();
        const int height = qMin(400, held);
        if (scroll->minimumHeight() != height) scroll->setMinimumHeight(height);
        const int most = held > 400 ? QWIDGETSIZE_MAX : height;
        if (scroll->maximumHeight() != most) scroll->setMaximumHeight(most);
    }
}

/// Restyles an effect's settings to match the panel: no margins or spin buttons, quiet sliders, and a tight layout if \p simple
void styleSettings(QWidget *root, bool simple)
{
    // recurse into nested layouts: a widget only knows its top-level one
    std::function<void(QLayout *, bool)> tidy = [&](QLayout *layout, bool margins) {
        if (margins) layout->setContentsMargins(0, 0, 0, 0);
        if (simple) {
            if (auto *grid = qobject_cast<QGridLayout *>(layout)) {
                grid->setHorizontalSpacing(12);
                grid->setVerticalSpacing(2);
            } else if (auto *form = qobject_cast<QFormLayout *>(layout)) {
                form->setHorizontalSpacing(12);
                form->setVerticalSpacing(2);
                form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
            } else if (auto *box = qobject_cast<QBoxLayout *>(layout)) {
                const bool across = box->direction() == QBoxLayout::LeftToRight || box->direction() == QBoxLayout::RightToLeft;
                box->setSpacing(across ? 12 : 2);
            }
        }
        for (int i = 0; i < layout->count(); i++) {
            QLayoutItem *item = layout->itemAt(i);
            if (QLayout *inner = item->layout()) {
                tidy(inner, true);
            } else if (QSpacerItem *spacer = item->spacerItem(); spacer && simple) {
                spacer->changeSize(0, 0, spacer->sizePolicy().horizontalPolicy(), spacer->sizePolicy().verticalPolicy());
            }
        }
        layout->invalidate();
    };
    if (root->layout()) tidy(root->layout(), true);
    Q_FOREACH (QWidget *widget, root->findChildren<QWidget *>()) {
        if (QLayout *layout = widget->layout()) {
            tidy(layout, simple || !qobject_cast<QGroupBox *>(widget));
        }
        if (auto *box = qobject_cast<QAbstractSpinBox *>(widget)) {
            box->setButtonSymbols(QAbstractSpinBox::NoButtons);
        }
        if (qobject_cast<KisSliderSpinBox *>(widget) || qobject_cast<KisDoubleSliderSpinBox *>(widget)) {
            auto *box = static_cast<QAbstractSpinBox *>(widget);
            box->setFrame(false);
            box->setProperty(KisModernStyle::quietFieldProperty, true);
            box->setMinimumHeight(28);
            if (QLabel *label = layoutLabel(widget)) {
                QString text = label->text().trimmed();
                if (text.endsWith(QLatin1Char(':'))) text.chop(1);
                label->setText(text);
                label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
                QPalette palette = label->palette();
                palette.setColor(QPalette::WindowText, KisModernStyle::secondaryInk(palette.color(QPalette::WindowText)));
                label->setPalette(palette);
            }
        }
    }
    root->setAutoFillBackground(false);
    // size hints cached before the effect's widget was inserted are stale
    Q_FOREACH (QWidget *widget, root->findChildren<QWidget *>()) widget->updateGeometry();
    Q_FOREACH (QScrollArea *scroll, root->findChildren<QScrollArea *>()) {
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->viewport()->setAutoFillBackground(false);
        if (scroll->widget()) scroll->widget()->setAutoFillBackground(false);
    }
    fitScrolls(root);
    root->setMinimumSize(0, 0);
}

/// Shows the values next to the pointer while dragging on the canvas
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
        const QFontMetrics metrics(boldFont());
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
        painter.drawPath(KisModernStyle::squircle(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), height() / 2.0));
        painter.setFont(boldFont());
        painter.setPen(palette().color(QPalette::WindowText));
        painter.drawText(rect(), Qt::AlignCenter, m_text);
    }

private:
    QFont boldFont() const
    {
        QFont bold = font();
        bold.setBold(true);
        return bold;
    }

    QString m_text;
};

} // namespace

struct KisStudioEffectOverlay::Private
{
    Private() : compressor(200, KisSignalCompressor::FIRST_ACTIVE)
    {
        // as in the filter dialog: apply each change once the previous preview is done, waiting at most 200 ms
        compressor.setDelay([this]() {
            KisFilterManager *manager = filterManager();
            return !manager || manager->isIdle();
        }, 10, 200);
    }

    KisMainWindow *mainWindow {nullptr};
    KisFilterSP filter;
    KisNodeSP node;
    QPointer<KisFilterSelectorWidget> selector;
    QScrollArea *scroll {nullptr};
    QLabel *icon {nullptr};
    QLabel *title {nullptr};
    QToolButton *preview {nullptr};
    QToolButton *editable {nullptr};
    QToolButton *reset {nullptr};
    /// The settings at their defaults, as the panel writes them (it may add to them: curves per channel, a version);
    /// empty while that isn't known, as when the effect opened with its last used settings
    QString defaults;
    KisSignalCompressor compressor;
    /// Allows only zooming and panning, no painting, while the effect is adjusted
    QScopedPointer<KisInputActionGroupsMaskGuard> guard;

    QVector<Knob> knobs;
    bool simple {false};
    QPointer<QWidget> canvasWidget;
    QPointer<ValuePill> pill;
    bool dragging {false};
    QPointF dragStart;
    bool fine {false};
    /// The second setting follows the first on its own (e.g. a blur's two radii)
    bool secondLinked {false};
    bool cursorSet {false};
    /// How far the panel was moved from its default place, and the move in progress
    QPoint moved;
    bool moving {false};
    QPoint moveGrab;
    int headerBottom {0};
    /// Space is held: the canvas pans, so drags don't change the effect
    bool spaceHeld {false};

    KisViewManager *viewManager() const { return mainWindow->viewManager(); }
    KisFilterManager *filterManager() const { return viewManager() ? viewManager()->filterManager() : nullptr; }
    bool maskPossible() const { return node && filter && !node->inherits("KisMask") && filter->supportsAdjustmentLayers(); }
    /// Whether the document it was opened on is still the one shown
    bool current() const
    {
        KisViewManager *view = viewManager();
        return node && view && view->image() && node->image().data() == view->image().data();
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
    layout->setContentsMargins(KisModernStyle::space4, KisModernStyle::space2, KisModernStyle::space3, KisModernStyle::space3);
    layout->setSpacing(KisModernStyle::space1);

    auto *header = new QHBoxLayout();
    header->setSpacing(KisModernStyle::hairline);
    d->icon = new QLabel(this);
    // let presses through to the panel, which is moved by its title bar
    d->icon->setAttribute(Qt::WA_TransparentForMouseEvents);
    header->addWidget(d->icon);
    header->addSpacing(4);
    d->title = new QLabel(this);
    QFont titleFont = d->title->font();
    titleFont.setWeight(QFont::DemiBold);
    d->title->setFont(titleFont);
    d->title->setAttribute(Qt::WA_TransparentForMouseEvents);
    header->addWidget(d->title, 1);

    // toggles the preview, like the dialog's Preview checkbox
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

    // back to the effect's own defaults; shown while the settings differ from them
    d->reset = iconButton(QStringLiteral("studio_reload"), i18nc("@info:tooltip", "Defaults"), this);
    connect(d->reset, &QToolButton::clicked, this, [this]() {
        KisConfigWidget *settings = d->selector ? d->selector->findChild<KisConfigWidget *>() : nullptr;
        if (!settings || !d->filter) return;
        settings->setConfiguration(d->filter->defaultConfiguration(KisGlobalResourcesInterface::instance()));
        d->defaults = d->selector->configuration()->toXML();
        d->compressor.start();
        showReset();
    });
    header->addWidget(d->reset);

    d->editable = iconButton(QStringLiteral("filterMask"), i18nc("@info:tooltip", "On: on a layer of its own, to change later\nOff: painted in"), this);
    d->editable->setCheckable(true);
    connect(d->editable, &QToolButton::toggled, this, [](bool on) { KisConfig(false).writeEntry(editableKey, on); });
    header->addWidget(d->editable);

    header->addSpacing(6);
    QToolButton *cancel = glyphButton(QStringLiteral("✕"), i18nc("@info:tooltip", "Cancel (Esc)"), this);
    connect(cancel, &QToolButton::clicked, this, &KisStudioEffectOverlay::reject);
    header->addWidget(cancel);
    QToolButton *apply = glyphButton(QStringLiteral("✓"), i18nc("@info:tooltip", "Apply (Enter)"), this);
    connect(apply, &QToolButton::clicked, this, &KisStudioEffectOverlay::accept);
    header->addWidget(apply);
    layout->addLayout(header);

    d->scroll = new QScrollArea(this);
    d->scroll->setFrameShape(QFrame::NoFrame);
    d->scroll->setWidgetResizable(true);
    d->scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    d->scroll->viewport()->setAutoFillBackground(false);
    layout->addWidget(d->scroll, 1);
    for (QWidget *widget : std::initializer_list<QWidget *>{d->preview, d->reset, d->editable, cancel, apply, d->scroll}) {
        KisStudioGlass::setSeeThrough(widget, true);
    }

    connect(&d->compressor, &KisSignalCompressor::timeout, this, &KisStudioEffectOverlay::startApplying);
    parentWidget()->installEventFilter(this);

    // the preview belongs to one layer of one document: switching either cancels it
    connect(mainWindow, &KisMainWindow::activeViewChanged, this, [this]() {
        if (isVisible()) reject();
    });
    if (KisViewManager *viewManager = mainWindow->viewManager()) {
        connect(viewManager->nodeManager(), &KisNodeManager::sigNodeActivated, this, [this](KisNodeSP node) {
            if (isVisible() && node != d->node) reject();
        });
        // also when the last document closes, as no other view becomes active then
        connect(viewManager, &KisViewManager::viewChanged, this, [this]() {
            if (isVisible() && !d->current()) reject();
        });
    }
    hide();
}

KisStudioEffectOverlay::~KisStudioEffectOverlay()
{
    d->setCursor(false);
}

void KisStudioEffectOverlay::showReset()
{
    KisFilterConfigurationSP current = d->selector ? d->selector->configuration() : KisFilterConfigurationSP();
    d->reset->setVisible(current && (d->defaults.isEmpty() || current->toXML() != d->defaults));
}

bool KisStudioEffectOverlay::open(KisFilterSP filter, KisFilterConfigurationSP overrideDefaultConfig)
{
    KisViewManager *viewManager = d->viewManager();
    KisFilterManager *filterManager = d->filterManager();
    if (!filter || !viewManager || !filterManager || !viewManager->activeNode() || !viewManager->activeNode()->paintDevice()
        || !viewManager->canvasBase()) {
        return false;
    }

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
    // show only the settings; presets and XML stay in the dialog
    for (const char *name : {"comboBoxPresets", "chkRememberPreset", "pushButtonEditPresets", "btnXML", "filtersSelector"}) {
        if (QWidget *widget = selector->findChild<QWidget *>(QLatin1String(name))) {
            widget->hide();
        }
    }
    connect(selector, &KisFilterSelectorWidget::configurationChanged, &d->compressor, &KisSignalCompressor::start);
    connect(selector, &KisFilterSelectorWidget::configurationChanged, this, &KisStudioEffectOverlay::showReset);
    connect(selector, &KisFilterSelectorWidget::sigSizeChanged, this, &KisStudioEffectOverlay::reposition);
    d->scroll->setWidget(selector);
    d->selector = selector;
    selector->adjustSize();

    d->title->setText(filter->name());
    d->icon->setPixmap(KisIconUtils::loadIcon(QStringLiteral("studio_effects")).pixmap(18, 18));
    {
        // read each time, as Settings may have changed it
        QSignalBlocker blocker(d->editable);
        d->editable->setChecked(KisConfig(true).readEntry<bool>(editableKey, true));
    }
    d->editable->setVisible(d->maskPossible());
    // opened at its defaults: what the panel makes of them is what the reset compares with
    KisFilterConfigurationSP defaults = filter->defaultConfiguration(KisGlobalResourcesInterface::instance());
    const bool atDefaults = !overrideDefaultConfig || overrideDefaultConfig->toXML() == defaults->toXML();
    d->defaults = atDefaults && selector->configuration() ? selector->configuration()->toXML() : QString();
    showReset();
    {
        QSignalBlocker blocker(d->preview);
        d->preview->setChecked(true);
        d->preview->setIcon(KisIconUtils::loadIcon(QStringLiteral("visible")));
    }

    d->knobs = findKnobs(selector, &d->simple);
    styleSettings(selector, d->simple);
    selector->adjustSize();
    // a scroll area absorbs its contents' size changes, so watch the contents too
    selector->installEventFilter(this);
    Q_FOREACH (QScrollArea *scroll, selector->findChildren<QScrollArea *>()) {
        if (scroll->widget()) scroll->widget()->installEventFilter(this);
    }
    reposition();

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

    // catch Enter and Esc wherever the focus is (a canvas drag moves it there), and Space for panning
    qApp->installEventFilter(this);
    d->guard.reset(new KisInputActionGroupsMaskGuard(viewManager->canvasBase()->inputActionGroupsMaskInterface(),
                                                     ViewTransformActionGroup));
    KisStudioMotion::showFaded(this);
    reposition();
    setFocus();
    startApplying();
    return true;
}

void KisStudioEffectOverlay::startApplying()
{
    KisFilterManager *filterManager = d->filterManager();
    if (!d->selector || !filterManager || !d->preview->isChecked() || !d->current()) {
        return;
    }
    KisFilterConfigurationSP config = d->selector->configuration();
    if (!config) {
        return;
    }
    if (auto *layer = qobject_cast<KisPaintLayer *>(d->node.data())) {
        config->setChannelFlags(layer->channelLockFlags());
    }
    filterManager->apply(config);
}

void KisStudioEffectOverlay::accept()
{
    KisFilterManager *filterManager = d->filterManager();
    if (!isVisible() || !filterManager) {
        return;
    }
    if (!d->current()) {
        reject();
        return;
    }
    if (d->editable->isChecked() && d->maskPossible() && d->selector) {
        KisFilterConfigurationSP config = d->selector->configuration();
        if (config && d->filter->configurationAllowedForMask(config)) {
            createMask(config);
            return;
        }
    }
    // apply a pending change now; otherwise keep the last preview
    const bool pending = d->compressor.isActive();
    d->compressor.stop();
    if (!d->preview->isChecked()) {
        QSignalBlocker blocker(d->preview);
        d->preview->setChecked(true);
    }
    if (pending || !filterManager->isStrokeRunning()) {
        startApplying();
    }
    if (filterManager->isStrokeRunning()) {
        // only the current frame: the overlay has no option for the other selected frames
        filterManager->setFilterAllSelectedFrames(false);
        filterManager->finish();
    }
    // finish() saved the settings as the filter's last used ones; also list the effect as recent
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

void KisStudioEffectOverlay::createMask(KisFilterConfigurationSP config)
{
    KisViewManager *viewManager = d->viewManager();
    KisFilterManager *filterManager = d->filterManager();
    KisLayer *layer = qobject_cast<KisLayer *>(d->node.data());
    if (!viewManager || !filterManager || !layer) {
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
        // its settings are the ones it opens with next time, as when painted in
        KisFilterManager::rememberLastUsed(config, d->node->paintDevice());
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
    if (!parentWidget()) {
        return;
    }
    const QSize area = parentWidget()->size();
    // as wide as the effect's settings want, but at least wide enough for a few sliders
    const int wanted = d->selector ? d->selector->sizeHint().width() + 32 : 0;
    const int width = qMin(qMax(520, wanted), area.width() - 40);
    const int contents = d->selector ? d->selector->sizeHint().height() : 0;
    const int height = qMin(46 + contents + (contents ? 14 : 0), int(area.height() * 0.55));
    // centered in the part of the canvas not covered by pinned panels
    const int left = parentWidget()->property("studioCanvasLeftInset").toInt();
    const int right = parentWidget()->property("studioCanvasRightInset").toInt();
    const int bottom = parentWidget()->property("studioCanvasBottomInset").toInt();
    QRect place(left + (area.width() - left - right - width) / 2, area.height() - bottom - height - 54, width, height);
    place.translate(d->moved);
    // keep the title bar inside the window so that the panel can't be dragged out of reach
    place.moveLeft(qBound(-width + 80, place.left(), area.width() - 80));
    place.moveTop(qBound(0, place.top(), area.height() - 40));
    setGeometry(place);
    d->headerBottom = d->scroll ? d->scroll->geometry().top() : 40;
    raise();
}

bool KisStudioEffectOverlay::handleCanvasDrag(QEvent *event)
{
    QPointF pos;
    Qt::MouseButton button = Qt::NoButton;
    switch (event->type()) {
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick:
    case QEvent::MouseMove:
    case QEvent::MouseButtonRelease:
        pos = static_cast<QMouseEvent *>(event)->localPos();
        button = static_cast<QMouseEvent *>(event)->button();
        break;
    case QEvent::TabletPress:
    case QEvent::TabletMove:
    case QEvent::TabletRelease:
        pos = static_cast<QTabletEvent *>(event)->posF();
        button = static_cast<QTabletEvent *>(event)->button();
        break;
    default:
        return false;
    }
    const QEvent::Type type = event->type();
    const bool press = type == QEvent::MouseButtonPress || type == QEvent::MouseButtonDblClick || type == QEvent::TabletPress;
    const bool release = type == QEvent::MouseButtonRelease || type == QEvent::TabletRelease;
    // Shift adjusts finely, at a quarter of the speed
    const Qt::KeyboardModifiers modifiers = static_cast<QInputEvent *>(event)->modifiers();
    const bool fine = modifiers & Qt::ShiftModifier;

    if (press) {
        const bool plain = modifiers == Qt::NoModifier || modifiers == Qt::ShiftModifier;
        if (button != Qt::LeftButton || !plain || d->spaceHeld || d->knobs.isEmpty()) return false;
        d->dragging = true;
        d->fine = fine;
        d->dragStart = pos;
        d->secondLinked = false;
        for (Knob &knob : d->knobs) {
            knob.start = knob.value();
            knob.span = usualSpan(knob.widget);
        }
        event->accept();
        return true;
    }
    if (!d->dragging || (release && button != Qt::LeftButton)) {
        return false;
    }
    if (release) {
        d->dragging = false;
        if (d->pill) d->pill->hide();
    } else {
        if (fine != d->fine) {
            // continue from the current values at the new speed
            d->fine = fine;
            d->dragStart = pos;
            for (Knob &knob : d->knobs) knob.start = knob.value();
        }
        // horizontal movement sets the first setting, vertical the second (up increases it)
        const QPointF moved = pos - d->dragStart;
        const QPointF delta = moved * (fine ? 0.25 : 1.0);
        QStringList shown;
        const Knob &first = d->knobs[0];
        if (first.widget) {
            first.setValue(first.start + delta.x() * first.span / dragForWholeRange);
            shown << QStringLiteral("%1 %2").arg(first.label, first.text());
        }
        if (d->knobs.size() > 1 && d->knobs[1].widget) {
            const Knob &second = d->knobs[1];
            // if it changed on its own before any vertical movement, it is linked to the first
            if (!d->secondLinked && second.value() != second.start && qAbs(moved.y()) <= 6) {
                d->secondLinked = true;
            }
            if (!d->secondLinked && qAbs(moved.y()) > 6) {
                second.setValue(second.start - delta.y() * second.span / dragForWholeRange);
                shown << QStringLiteral("%1 %2").arg(second.label, second.text());
            }
        }
        if (d->pill) d->pill->showAt(pos.toPoint(), shown.join(QStringLiteral("   ")));
    }
    event->accept();
    return true;
}

bool KisStudioEffectOverlay::eventFilter(QObject *watched, QEvent *event)
{
    // the settings changed size (a row shown or hidden): resize the panel to fit
    if (event->type() == QEvent::LayoutRequest && d->selector && isVisible()
        && (watched == d->selector || d->selector->isAncestorOf(qobject_cast<QWidget *>(watched)))) {
        QTimer::singleShot(0, this, [this]() {
            if (!d->selector) return;
            fitScrolls(d->selector);
            reposition();
        });
    }
    if (watched == parentWidget() && event->type() == QEvent::Resize && isVisible()) {
        reposition();
    }
    if (d->canvasWidget && watched == d->canvasWidget && isVisible()) {
        if (event->type() == QEvent::Enter) d->setCursor(true);
        if (event->type() == QEvent::Leave && !d->dragging) d->setCursor(false);
        if (handleCanvasDrag(event)) return true;
    }
    // Enter applies and Esc cancels wherever the focus is, except in the panel itself or in a text field
    if (event->type() == QEvent::KeyPress && isVisible()) {
        auto *key = static_cast<QKeyEvent *>(event);
        QWidget *focus = QApplication::focusWidget();
        const bool inPanel = focus && (focus == this || isAncestorOf(focus));
        // ignore keys meant for dialogs and other windows
        const bool here = focus ? focus->window() == window() : watched == window();
        if (!inPanel && here && (watched == focus || !focus) && !(focus && isTextInput(focus))) {
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
    // track Space, which pans the canvas: drags meanwhile don't change the effect
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

void KisStudioEffectOverlay::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && event->pos().y() < qMax(d->headerBottom, 30)) {
        d->moving = true;
        d->moveGrab = event->globalPos() - d->moved;
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void KisStudioEffectOverlay::mouseMoveEvent(QMouseEvent *event)
{
    if (d->moving) {
        d->moved = event->globalPos() - d->moveGrab;
        reposition();
        event->accept();
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void KisStudioEffectOverlay::mouseReleaseEvent(QMouseEvent *event)
{
    if (d->moving && event->button() == Qt::LeftButton) {
        d->moving = false;
        unsetCursor();
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void KisStudioEffectOverlay::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QPainterPath shape = KisStudioGlass::paint(painter, this, QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 14);
    QColor border = palette().color(QPalette::WindowText);
    border.setAlphaF(0.14);
    KisStudioGlass::paintBorder(painter, this, shape, border);
}
