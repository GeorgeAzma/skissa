/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioBrushEditor.h"

#include <QAction>
#include <QCursor>

#include <algorithm>

#include <QCloseEvent>
#include <QHBoxLayout>
#include <QHash>
#include <QLabel>
#include <QPainter>
#include <QPointer>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <klocalizedstring.h>

#include <KisModernStyle.h>
#include <KoCanvasResourceProvider.h>
#include <brushengine/kis_paintop_preset.h>
#include <brushengine/KisPaintOpPresetUpdateProxy.h>
#include <kis_icon_utils.h>
#include <kis_signal_auto_connection.h>
#include <kactioncollection.h>

#include <KoCanvasController.h>
#include <KoToolManager.h>

#include "KisLastStrokeReplay.h"
#include "KisMainWindow.h"
#include "KisStudioBarButtons.h"
#include "KisStudioBrushPicker.h"
#include "KisStudioBrushSheet.h"
#include "KisStudioMixing.h"
#include "KisStudioPenPopup.h"
#include "KisStudioSmoothing.h"
#include "KisViewManager.h"
#include "kis_canvas_resource_provider.h"
#include "kis_config.h"
#include "kis_paintop_box.h"
#include "kis_paintop_option.h"
#include "kis_paintop_settings_widget.h"
#include "widgets/KisStudioChips.h"
#include "widgets/KisStudioDabPictures.h"
#include "widgets/KisStudioValueRow.h"
#include "widgets/kis_preset_live_preview_view.h"

namespace {

constexpr int contentWidth = 368;
/// The tab of how the tool lays the stroke down (smoothing, shapes, snapping): not one of the brush's option categories
constexpr int strokePart = 1000;
const QString partKey = QStringLiteral("StudioBrushEditorPart");

QHash<KisMainWindow *, QPointer<KisStudioBrushEditor>> &editors()
{
    static QHash<KisMainWindow *, QPointer<KisStudioBrushEditor>> all;
    return all;
}


const QString pinPrefix = QStringLiteral("brushsetting:");

QString partTip(int part)
{
    switch (part) {
    case KisPaintOpOption::MASKING_BRUSH:
        return i18nc("@info:tooltip brush part", "A second tip that cuts into the first, for bristles or chalk");
    case KisPaintOpOption::TEXTURE:
        return i18nc("@info:tooltip brush part", "A paper grain the paint catches on");
    case KisPaintOpOption::COLOR:
        return i18nc("@info:tooltip brush part", "Where the color comes from and how it builds up");
    case KisPaintOpOption::FILTER:
        return i18nc("@info:tooltip brush part", "Paints with an effect, like blur or sharpen");
    case strokePart:
        return i18nc("@info:tooltip brush part", "How the line is laid down, with smoothing and shapes");
    }
    return QString();
}

/// The curve of the first pen input that drives \p option, if the pen drives it
QString penCurve(KisPaintOpOption *option)
{
    if (option->isCheckable() && !option->isChecked()) return QString();
    Q_FOREACH (const KisPaintOpOption::PenInput &input, option->penInputs()) {
        if (input.active) return input.curve.isEmpty() ? QStringLiteral("0,0;1,1;") : input.curve;
    }
    return QString();
}

} // namespace

struct KisStudioBrushEditor::Private
{
    QPointer<KisMainWindow> mainWindow;
    QPointer<KisPaintopBox> box;
    QPointer<KisPaintOpSettingsWidget> settings;

    QLabel *name {nullptr};
    QToolButton *reload {nullptr};
    QToolButton *pin {nullptr};
    QToolButton *follow {nullptr};
    KisPresetLivePreviewView *preview {nullptr};
    KisStudioChips *tabs {nullptr};
    QWidget *page {nullptr};
    QVBoxLayout *pageLayout {nullptr};

    QVector<int> parts;
    int part {KisPaintOpOption::GENERAL};
    QPointer<KisStudioPenPopup> pen;
    /// The brush's settings are in use (useOptionWidget()), from shown to closed
    bool usingSettings {false};
    /// The settings to add are shown, until another part is picked
    bool adding {false};
    QTimer refreshPage;
    /// The options shown as rows and as ones to add, to see whether a change moves any
    QList<KisPaintOpOption *> rowOptions;
    QList<KisPaintOpOption *> unusedOptions;
    KisSignalAutoConnectionsStore presetConnections;

    KisPaintOpPresetSP preset() const
    {
        KisViewManager *view = mainWindow ? mainWindow->viewManager() : nullptr;
        return view && view->canvasResourceProvider() ? view->canvasResourceProvider()->currentPreset() : KisPaintOpPresetSP();
    }

    QList<KisPaintOpOption *> options() const
    {
        return settings ? settings->options() : QList<KisPaintOpOption *>();
    }

    /// The options of \p part in use, the size first, and the ones that could be
    void classify(int part, QList<KisPaintOpOption *> *rows, QList<KisPaintOpOption *> *unused) const
    {
        // a part that is off (no mask, no texture) has no settings to show
        Q_FOREACH (KisPaintOpOption *option, options()) {
            if (option->category() == part && option->hasQuickPage(KisPaintOpOption::PaintopCategory(part))
                && option->isCheckable() && !option->isChecked()) {
                return;
            }
        }
        Q_FOREACH (KisPaintOpOption *option, options()) {
            if (option->category() != part || !option->isEnabled()) continue;
            const std::optional<KisPaintOpOption::RowValue> value = option->rowValue();
            if (!value) continue;
            if (value->appliesUnchecked || !option->isCheckable() || option->isChecked()) {
                rows->append(option);
            } else {
                unused->append(option);
            }
        }
        // the ones every brush has first, in the order the top bar has them
        static const QStringList first = {QStringLiteral("Size"), QStringLiteral("Opacity"), QStringLiteral("Flow"), QStringLiteral("Spacing")};
        std::stable_sort(rows->begin(), rows->end(), [](KisPaintOpOption *a, KisPaintOpOption *b) {
            const int ia = first.indexOf(a->id());
            const int ib = first.indexOf(b->id());
            return (ia < 0 ? first.size() : ia) < (ib < 0 ? first.size() : ib);
        });
    }

    static void applyRow(KisStudioValueRow *row, KisPaintOpOption *option, KisPaintOpSettingsWidget *settings)
    {
        const std::optional<KisPaintOpOption::RowValue> value = option->rowValue();
        if (!value) return;
        KisStudioValueRow::Range range;
        range.minimum = value->minimum;
        range.maximum = value->maximum;
        range.decimals = value->decimals;
        range.suffix = value->suffix;
        range.size = value->size;
        row->setRange(range);
        row->setValue(value->value);
        row->setValueText(value->text);
        row->setAutomatic(value->automatic.has_value(), value->automatic.value_or(false));
        const std::optional<qreal> picked = settings ? settings->pickedRowValue(option) : std::nullopt;
        row->setDefaultValue(picked ? *picked : qQNaN());
        row->setPenCurve(penCurve(option));
        row->setInactive(option->isCheckable() && !option->isChecked() && !value->appliesUnchecked);
    }
};

QString KisStudioBrushEditor::partName(int part)
{
    switch (part) {
    case KisPaintOpOption::GENERAL:
        return i18nc("brush part: its tip and how it is laid down", "Tip");
    case KisPaintOpOption::MASKING_BRUSH:
        return i18nc("brush part: a second tip that cuts into the first", "Mask");
    case KisPaintOpOption::TEXTURE:
        return i18nc("brush part", "Texture");
    case KisPaintOpOption::COLOR:
        return i18nc("brush part", "Color");
    case KisPaintOpOption::FILTER:
        return i18nc("brush part", "Filter");
    case strokePart:
        return i18nc("brush part: how the line is laid down (smoothing, shapes)", "Stroke");
    }
    return QString();
}

KisPaintOpSettingsWidget *KisStudioBrushEditor::brushSettings(KisMainWindow *mainWindow)
{
    KisViewManager *view = mainWindow ? mainWindow->viewManager() : nullptr;
    KisPaintopBox *box = view ? view->paintOpBox() : nullptr;
    return box ? dynamic_cast<KisPaintOpSettingsWidget *>(box->currentOptionWidget()) : nullptr;
}

KisPaintOpOption *KisStudioBrushEditor::brushOption(KisMainWindow *mainWindow, const QString &id)
{
    KisPaintOpSettingsWidget *settings = brushSettings(mainWindow);
    if (!settings) return nullptr;
    Q_FOREACH (KisPaintOpOption *option, settings->options()) {
        if (option->id() == id && option->rowValue()) return option;
    }
    return nullptr;
}

QString KisStudioBrushEditor::optionTip(KisPaintOpOption *option)
{
    static const QHash<QString, QString> tips = {
        {QStringLiteral("Opacity"), i18nc("@info:tooltip brush setting", "0% see-through → 100% solid")},
        {QStringLiteral("Flow"), i18nc("@info:tooltip brush setting", "10% builds up slowly → 100% lays it on full")},
        {QStringLiteral("Spacing"), i18nc("@info:tooltip brush setting", "The gap between dabs, from a smooth line at 5% to beads at 100%")},
        {QStringLiteral("Ratio"), i18nc("@info:tooltip brush setting", "100% round → 20% flat")},
        {QStringLiteral("Softness"), i18nc("@info:tooltip brush setting", "0% crisp → 100% soft")},
        {QStringLiteral("Rotation"), i18nc("@info:tooltip brush setting", "Turns the tip, for example along the stroke")},
        {QStringLiteral("Mirror"), i18nc("@info:tooltip brush setting", "Flips dabs, for example at random")},
        {QStringLiteral("Sharpness"), i18nc("@info:tooltip brush setting", "Puts edges on whole pixels, for pixel art")},
        {QStringLiteral("Scatter"), i18nc("@info:tooltip brush setting", "Scatters dabs off the line. At 0% they stay on it")},
        {QStringLiteral("Darken"), i18nc("@info:tooltip brush setting", "Darkens the color, for example with pressure")},
        {QStringLiteral("Mix"), i18nc("@info:tooltip brush setting", "Mixes from the foreground to the background color, for example with pressure")},
        {QStringLiteral("h"), i18nc("@info:tooltip brush setting", "Shifts the hue, for example at random each dab")},
        {QStringLiteral("s"), i18nc("@info:tooltip brush setting", "Shifts the saturation, for example at random each dab")},
        {QStringLiteral("v"), i18nc("@info:tooltip brush setting", "Shifts the lightness, for example at random each dab")},
        {QStringLiteral("Rate"), i18nc("@info:tooltip brush setting", "Airbrush: dabs a second while the pen is held still")},
        {QStringLiteral("LightnessStrength"), i18nc("@info:tooltip brush setting", "The tip's lights and darks shade the paint")},
        {QStringLiteral("Texture/Strength/"), i18nc("@info:tooltip brush setting", "0% no texture → 100% full")},
    };
    // the mask's own settings have a prefix before the same names
    const QString id = option->id();
    if (tips.contains(id)) return tips.value(id);
    return tips.value(id.section(QLatin1Char('/'), -1));
}

QString KisStudioBrushEditor::pinKey(KisPaintOpOption *option)
{
    return pinPrefix + option->id();
}

void KisStudioBrushEditor::bindRow(KisStudioValueRow *row, KisPaintOpOption *option, KisPaintOpSettingsWidget *settings)
{
    const QPointer<KisPaintOpOption> target(option);
    const QPointer<KisPaintOpSettingsWidget> owner(settings);
    auto apply = [row, target, owner]() {
        if (target) Private::applyRow(row, target, owner);
    };
    row->setToolTip(optionTip(option));
    apply();
    QObject::connect(row, &KisStudioValueRow::valueChanged, option, [target](qreal value) {
        if (!target) return;
        // an effect that is off (Scatter, Rotation) comes on when its value is set
        const std::optional<KisPaintOpOption::RowValue> row = target->rowValue();
        if (target->isCheckable() && !target->isChecked() && row && !row->appliesUnchecked) target->setChecked(true);
        target->setRowValue(value);
    });
    QObject::connect(row, &KisStudioValueRow::automaticToggled, option, [target](bool on) {
        if (target) target->setRowAutomatic(on);
    });
    QObject::connect(option, &KisPaintOpOption::sigRowValueChanged, row, apply);
    QObject::connect(option, &KisPaintOpOption::sigSettingChanged, row, apply);
}

void KisStudioBrushEditor::toggle(KisMainWindow *mainWindow, QWidget *anchor, Qt::Edge edge)
{
    QPointer<KisStudioBrushEditor> &editor = editors()[mainWindow];
    if (editor) {
        editor->close();
        return;
    }
    editor = new KisStudioBrushEditor(mainWindow);
    if (anchor) {
        editor->popupBeside(anchor, edge);
    } else {
        editor->popupAt(mainWindow->mapToGlobal(QPoint(80, 80)));
    }
}

bool KisStudioBrushEditor::isOpen(KisMainWindow *mainWindow)
{
    return editors().value(mainWindow);
}

KisStudioBrushEditor::KisStudioBrushEditor(KisMainWindow *mainWindow)
    : KisStudioPopup(mainWindow)
    , d(new Private)
{
    d->mainWindow = mainWindow;
    d->box = mainWindow->viewManager() ? mainWindow->viewManager()->paintOpBox() : nullptr;
    d->part = KisConfig(true).readEntry(partKey, int(KisPaintOpOption::GENERAL));
    setObjectName(QStringLiteral("studioBrushEditor"));

    auto *layout = new QVBoxLayout(this);
    fitLayout(layout, KisModernStyle::space3);
    layout->setSpacing(KisModernStyle::space2);

    auto *header = new QHBoxLayout();
    header->setContentsMargins(KisModernStyle::space1, 0, 0, 0);
    header->setSpacing(KisModernStyle::hairline);
    d->name = new QLabel(this);
    d->name->setFont(KisModernStyle::textFont(font(), 1.1, QFont::DemiBold));
    header->addWidget(d->name, 1);
    d->reload = new QToolButton(this);
    d->reload->setAutoRaise(true);
    d->reload->setIcon(KisIconUtils::loadIcon(QStringLiteral("studio_reload")));
    d->reload->setToolTip(i18nc("@info:tooltip", "Back to the saved brush"));
    connect(d->reload, &QToolButton::clicked, this, [this]() {
        if (QAction *reload = d->mainWindow ? d->mainWindow->actionCollection()->action(QStringLiteral("reload_preset_action")) : nullptr) {
            reload->trigger();
        }
    });
    header->addWidget(d->reload);
    d->follow = new QToolButton(this);
    d->follow->setAutoRaise(true);
    d->follow->setCheckable(true);
    d->follow->setChecked(KisLastStrokeReplay::isEnabled());
    d->follow->setIcon(KisIconUtils::loadIcon(QStringLiteral("studio_restroke")));
    d->follow->setToolTip(i18nc("@info:tooltip", "Redraw the last stroke while editing"));
    connect(d->follow, &QToolButton::toggled, this, [](bool on) { KisLastStrokeReplay::setEnabled(on); });
    header->addWidget(d->follow);
    d->pin = new QToolButton(this);
    d->pin->setAutoRaise(true);
    d->pin->setCheckable(true);
    d->pin->setIcon(KisIconUtils::loadIcon(QStringLiteral("studio_pin")));
    d->pin->setToolTip(i18nc("@info:tooltip", "Keep open while painting"));
    connect(d->pin, &QToolButton::toggled, this, &KisStudioPopup::setPinned);
    header->addWidget(d->pin);
    layout->addLayout(header);

    d->preview = new KisPresetLivePreviewView(this);
    d->preview->setFixedSize(contentWidth, 64);
    d->preview->setFrameShape(QFrame::NoFrame);
    {
        // the stroke right on the glass, with no box behind it: the preview paints over its window color
        QPalette clear;
        clear.setColor(QPalette::Window, Qt::transparent);
        clear.setColor(QPalette::Base, Qt::transparent);
        d->preview->setPalette(clear);
        d->preview->setBackgroundBrush(Qt::NoBrush);
        d->preview->viewport()->setAutoFillBackground(false);
    }
    if (mainWindow->viewManager() && mainWindow->viewManager()->canvasResourceProvider()) {
        d->preview->setup(mainWindow->viewManager()->canvasResourceProvider()->resourceManager());
    }
    layout->addWidget(d->preview);

    d->tabs = new KisStudioChips(this);
    d->tabs->setTabs(true);
    connect(d->tabs, &KisStudioChips::clicked, this, [this](int index) {
        if (index < 0 || index >= d->parts.size()) return;
        d->part = d->parts[index];
        d->adding = false;
        KisConfig(false).writeEntry(partKey, d->part);
        showPart(d->part);
    });
    layout->addWidget(d->tabs);

    d->page = new QWidget(this);
    d->pageLayout = new QVBoxLayout(d->page);
    d->pageLayout->setContentsMargins(0, 0, 0, 0);
    d->pageLayout->setSpacing(KisModernStyle::space1);
    layout->addWidget(d->page);

    auto *everything = new KisStudioValueRow(i18nc("the whole brush editor", "Everything"), this);
    everything->setDraggable(false);
    everything->setValueText(QStringLiteral("›"));
    connect(everything, &KisStudioValueRow::clicked, this, [this]() {
        QPointer<KisMainWindow> mainWindow = d->mainWindow;
        close();
        if (mainWindow) KisStudioBrushSheet::toggle(mainWindow);
    });
    layout->addWidget(everything);

    setFixedWidth(contentWidth + 2 * (shadowMargin() + KisModernStyle::space3));

    d->refreshPage.setSingleShot(true);
    connect(&d->refreshPage, &QTimer::timeout, this, [this]() {
        QList<KisPaintOpOption *> rows;
        QList<KisPaintOpOption *> unused;
        d->classify(d->part, &rows, &unused);
        if (rows != d->rowOptions || unused != d->unusedOptions) showPart(d->part);
    });

    if (d->box) {
        connect(d->box, &KisPaintopBox::sigOptionWidgetChanged, this, &KisStudioBrushEditor::rebuild);
    }
    // the last stroke follows the smoothing (the Stroke tab, the chip) as it does the brush
    connect(KisStudioSmoothing::instance(), &KisStudioSmoothing::sigChanged, this, &KisStudioBrushEditor::followLastStroke);
    // the Stroke tab is there for the tools that paint freehand
    connect(KoToolManager::instance(), &KoToolManager::changedTool, this, [this]() {
        if (d->parts.contains(strokePart) != KisStudioStrokePage::applies(d->mainWindow)) rebuild();
    });
    rebuild();
}

KisStudioBrushEditor::~KisStudioBrushEditor()
{
}

void KisStudioBrushEditor::showEvent(QShowEvent *event)
{
    if (d->box && !d->usingSettings) {
        d->box->useOptionWidget(true);
        d->usingSettings = true;
    }
    KisStudioPopup::showEvent(event);
}

void KisStudioBrushEditor::closeEvent(QCloseEvent *event)
{
    if (d->box && d->usingSettings) {
        d->box->useOptionWidget(false);
        d->usingSettings = false;
    }
    if (d->pen) d->pen->close();
    KisStudioPopup::closeEvent(event);
}

void KisStudioBrushEditor::rebuild()
{
    d->settings = d->box ? dynamic_cast<KisPaintOpSettingsWidget *>(d->box->currentOptionWidget()) : nullptr;

    d->presetConnections.clear();
    if (KisPaintOpPresetSP preset = d->preset()) {
        d->preview->setCurrentPreset(preset);
        d->preview->requestUpdateStroke();
        if (preset->updateProxy()) {
            // the last stroke shows the change on the picture, as soon as it is made
            d->presetConnections.addConnection(preset->updateProxy(), &KisPaintOpPresetUpdateProxy::sigSettingsChangedUncompressed, this,
                                               [this]() { followLastStroke(); });
            d->presetConnections.addConnection(preset->updateProxy(), &KisPaintOpPresetUpdateProxy::sigSettingsChanged, this,
                                               [this]() {
                                                   d->preview->requestUpdateStroke();
                                                   updateHeader();
                                               });
        }
    }

    // the parts this brush has, in the order a brush is made
    d->parts.clear();
    const QList<KisPaintOpOption *> options = d->options();
    for (int part : {KisPaintOpOption::GENERAL, KisPaintOpOption::MASKING_BRUSH, KisPaintOpOption::TEXTURE,
                     KisPaintOpOption::COLOR, KisPaintOpOption::FILTER}) {
        bool has = false;
        Q_FOREACH (KisPaintOpOption *option, options) {
            if ((option->category() == part && option->rowValue()) || option->hasQuickPage(KisPaintOpOption::PaintopCategory(part))) {
                has = true;
                break;
            }
        }
        if (has) d->parts << part;
    }
    if (KisStudioStrokePage::applies(d->mainWindow)) d->parts << strokePart;
    if (!d->parts.contains(d->part)) d->part = d->parts.isEmpty() ? KisPaintOpOption::GENERAL : d->parts.first();

    QVector<KisStudioChips::Chip> tabs;
    Q_FOREACH (int part, d->parts) {
        KisStudioChips::Chip chip;
        chip.text = partName(part);
        chip.toolTip = partTip(part);
        tabs << chip;
    }
    d->tabs->setChips(tabs);
    d->tabs->setVisible(d->parts.size() > 1);
    updateHeader();
    showPart(d->part);
}

void KisStudioBrushEditor::followLastStroke()
{
    KisPaintOpPresetSP current = d->preset();
    if (current && d->follow->isChecked() && KisLastStrokeReplay::instance()->follows(current->name())) {
        KisLastStrokeReplay::instance()->brushChanged(current->name(), current->settings()->toXML());
    }
}

void KisStudioBrushEditor::updateHeader()
{
    KisPaintOpPresetSP preset = d->preset();
    d->name->setText(preset ? KisStudioBrushPicker::title(preset->name()) : QString());
    d->reload->setVisible(preset && preset->isDirty());
}

void KisStudioBrushEditor::showPart(int part)
{
    if (d->pen) d->pen->close();
    d->tabs->setCurrent(d->parts.indexOf(part));

    while (QLayoutItem *item = d->pageLayout->takeAt(0)) {
        if (QWidget *widget = item->widget()) {
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }

    if (part == strokePart) {
        auto *stroke = new KisStudioStrokePage(d->mainWindow, d->page);
        connect(stroke, &KisStudioStrokePage::sigResized, this, [this]() {
            QTimer::singleShot(0, this, [this]() {
                adjustSize();
                if (isVisible()) keepOnScreen();
            });
        });
        d->pageLayout->addWidget(stroke);
        d->rowOptions.clear();
        d->unusedOptions.clear();
        fitPage();
        return;
    }

    const QList<KisPaintOpOption *> options = d->options();
    const auto category = KisPaintOpOption::PaintopCategory(part);

    // how its paint mixes with paint, for every brush that has colors
    if (category == KisPaintOpOption::COLOR) {
        if (QWidget *mixing = KisStudioMixing::chooser(d->mainWindow, d->page)) d->pageLayout->addWidget(mixing);
    }

    // the pictures first: the tip to shape, the patterns to pick
    // the part's own first (the patterns), then what others add to it (the tip's grain)
    for (bool own : {true, false}) {
        Q_FOREACH (KisPaintOpOption *option, options) {
            if ((option->category() == category) != own || !option->hasQuickPage(category)) continue;
            if (QWidget *quick = option->createQuickPage(category, d->page)) {
                d->pageLayout->addWidget(quick);
            }
        }
    }

    // then what is in use as rows, the size first; what isn't, to add
    QList<KisPaintOpOption *> rows;
    QList<KisPaintOpOption *> unused;
    d->classify(part, &rows, &unused);
    d->rowOptions = rows;
    d->unusedOptions = unused;
    Q_FOREACH (KisPaintOpOption *option, options) {
        // something turned on or off may move between the rows and the ones to add
        connect(option, &KisPaintOpOption::sigCheckedChanged, &d->refreshPage, qOverload<>(&QTimer::start), Qt::UniqueConnection);
        connect(option, &KisPaintOpOption::sigEnabledChanged, &d->refreshPage, qOverload<>(&QTimer::start), Qt::UniqueConnection);
    }

    Q_FOREACH (KisPaintOpOption *option, rows) {
        auto *row = new KisStudioValueRow(option->label(), d->page);
        bindRow(row, option, d->settings);
        const QPointer<KisPaintOpOption> target(option);
        // pinned to the top bar by its pin, or pulled up onto it
        const QString key = pinKey(option);
        auto showPinned = [this, row, key]() {
            row->setPinnable(true, KisStudioBarButtons::has(d->mainWindow, key));
        };
        showPinned();
        if (KisStudioBarButtons *bar = KisStudioBarButtons::of(d->mainWindow)) {
            connect(bar, &KisStudioBarButtons::sigItemsChanged, row, showPinned);
        }
        connect(row, &KisStudioValueRow::pinClicked, this, [this, key]() { KisStudioBarButtons::toggle(d->mainWindow, key); });
        // an effect the brush can go without (Rotation, Scatter) comes off by its cross, or pulled off the editor
        const std::optional<KisPaintOpOption::RowValue> rowValue = option->rowValue();
        const bool removable = option->isCheckable() && rowValue && !rowValue->appliesUnchecked;
        row->setRemovable(removable);
        connect(row, &KisStudioValueRow::removeClicked, this, [target]() {
            if (target) target->setChecked(false);
        });
        connect(row, &KisStudioValueRow::pulledOut, this, [this, row, key, target, removable]() {
            const Qt::DropAction done = KisStudioBarButtons::dragItem(key, row);
            // dropped on the bar: pinned; let go over the editor, or the drag cancelled: nothing
            if (done != Qt::IgnoreAction || !removable || !target || KisModernStyle::pointerHeld()) return;
            if (frameGeometry().contains(QCursor::pos())) return;
            target->setChecked(false);
        });
        connect(row, &KisStudioValueRow::typingStarted, this, [this, target]() {
            if (d->pen && d->pen->option() == target) d->pen->close();
        });
        connect(row, &KisStudioValueRow::clicked, this, [this, row, target]() {
            if (!target || target->penInputs().isEmpty()) return;
            const bool same = d->pen && d->pen->option() == target;
            if (d->pen) d->pen->close();
            if (same) return;
            auto *pen = new KisStudioPenPopup(target, this);
            d->pen = pen;
            row->setSelected(true);
            connect(pen, &QObject::destroyed, row, [row]() { row->setSelected(false); });
            // beside the editor, level with the row
            const QPoint at(mapToGlobal(QPoint(width() - shadowMargin(), 0)).x() + KisModernStyle::space2,
                            row->mapToGlobal(QPoint(0, 0)).y() - 2 * KisModernStyle::space4);
            pen->popupAt(at, row);
        });
        d->pageLayout->addWidget(row);
    }

    if (!unused.isEmpty() && !d->adding) {
        // what could be added stays out of the way until asked for
        auto *add = new KisStudioChips(d->page);
        KisStudioChips::Chip chip;
        chip.text = QStringLiteral("+ ") + i18nc("@action:button add a brush setting", "Add");
        add->setChips({chip});
        connect(add, &KisStudioChips::clicked, this, [this]() {
            d->adding = true;
            showPart(d->part);
        });
        d->pageLayout->addSpacing(KisModernStyle::space1);
        d->pageLayout->addWidget(add);
    } else if (!unused.isEmpty()) {
        // each drawn as what it does to a stroke
        auto *more = new KisStudioChips(d->page);
        more->setTileColumns(5, 58);
        KisViewManager *view = d->mainWindow ? d->mainWindow->viewManager() : nullptr;
        const QColor fg = view && view->canvasResourceProvider() ? view->canvasResourceProvider()->fgColor().toQColor() : QColor();
        QVector<KisStudioChips::Chip> chips;
        Q_FOREACH (KisPaintOpOption *option, unused) {
            KisStudioChips::Chip chip;
            chip.text = option->label();
            chip.toolTip = optionTip(option);
            const QString id = option->id().section(QLatin1Char('/'), -1).isEmpty() ? option->id() : option->id().section(QLatin1Char('/'), -1);
            chip.picture = [more, id, fg](QPainter &painter, const QRectF &rect) {
                const QColor ink = more->palette().color(QPalette::WindowText);
                const QColor paint = KisStudioDabPictures::paintColor(fg, more->palette().color(QPalette::Highlight));
                if (KisStudioDabPictures::effect(painter, rect, id, ink, paint)) return;
                painter.setPen(QPen(KisModernStyle::secondaryInk(ink), 1.5, Qt::SolidLine, Qt::RoundCap));
                const QPointF c = rect.center();
                painter.drawLine(c - QPointF(5, 0), c + QPointF(5, 0));
                painter.drawLine(c - QPointF(0, 5), c + QPointF(0, 5));
            };
            chips << chip;
        }
        more->setChips(chips);
        QList<QPointer<KisPaintOpOption>> targets;
        Q_FOREACH (KisPaintOpOption *option, unused) {
            targets << option;
        }
        connect(more, &KisStudioChips::clicked, this, [targets](int index) {
            if (index >= 0 && index < targets.size() && targets[index]) targets[index]->setChecked(true);
        });
        d->pageLayout->addSpacing(KisModernStyle::space1);
        d->pageLayout->addWidget(more);
    }

    fitPage();
}

void KisStudioBrushEditor::fitPage()
{
    // the window fits the new page, smaller or larger
    d->pageLayout->activate();
    layout()->activate();
    adjustSize();
    if (isVisible()) keepOnScreen();
    QTimer::singleShot(0, this, [this]() {
        adjustSize();
        if (isVisible()) keepOnScreen();
    });
}
