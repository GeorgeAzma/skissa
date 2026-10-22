/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioPenPopup.h"

#include <QCheckBox>
#include <QHash>
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QVBoxLayout>

#include <klocalizedstring.h>

#include <KisModernStyle.h>
#include <kis_cubic_curve.h>
#include <kis_signals_blocker.h>

#include "kis_paintop_option.h"
#include "widgets/KisCurvePresetStrip.h"
#include "widgets/KisStudioChips.h"
#include "widgets/kis_curve_widget.h"

struct KisStudioPenPopup::Private
{
    QPointer<KisPaintOpOption> option;
    QString selected;
    bool showAll {false};
    QVector<KisPaintOpOption::PenInput> shown;

    QLabel *title {nullptr};
    QCheckBox *on {nullptr};
    KisStudioChips *inputs {nullptr};
    KisCurveWidget *curve {nullptr};
    KisCurvePresetStrip *saved {nullptr};
    KisStudioChips *modes {nullptr};
    QWidget *inputPageHolder {nullptr};
    QWidget *inputPage {nullptr};
    QString inputPageId;

    KisPaintOpOption::PenInput input(const QString &id) const
    {
        Q_FOREACH (const KisPaintOpOption::PenInput &input, option->penInputs()) {
            if (input.id == id) return input;
        }
        return {};
    }
};

QString KisStudioPenPopup::inputName(const QString &id, const QString &fallback)
{
    static const QHash<QString, QString> names = {
        {QStringLiteral("pressure"), i18nc("pen input", "Pressure")},
        {QStringLiteral("pressurein"), i18nc("pen input: the highest pressure so far in the stroke", "Peak pressure")},
        {QStringLiteral("declination"), i18nc("pen input: how far the pen leans", "Tilt")},
        {QStringLiteral("speed"), i18nc("pen input", "Speed")},
        {QStringLiteral("drawingangle"), i18nc("pen input: the way the stroke goes", "Direction")},
        {QStringLiteral("fade"), i18nc("pen input: along the stroke", "Fade")},
        {QStringLiteral("fuzzy"), i18nc("pen input: a new random value each dab", "Random")},
        {QStringLiteral("fuzzystroke"), i18nc("pen input: a random value each stroke", "Random per stroke")},
        {QStringLiteral("xtilt"), i18nc("pen input", "X tilt")},
        {QStringLiteral("ytilt"), i18nc("pen input", "Y tilt")},
        {QStringLiteral("ascension"), i18nc("pen input: the way the pen leans", "Tilt direction")},
        {QStringLiteral("rotation"), i18nc("pen input: turning the pen in the fingers", "Twist")},
        {QStringLiteral("distance"), i18nc("pen input", "Distance")},
        {QStringLiteral("time"), i18nc("pen input", "Time")},
        {QStringLiteral("perspective"), i18nc("pen input", "Perspective")},
        {QStringLiteral("tangentialpressure"), i18nc("pen input: an airbrush pen's wheel", "Wheel")},
    };
    return names.value(id, fallback);
}

QString KisStudioPenPopup::inputTip(const QString &id)
{
    static const QHash<QString, QString> tips = {
        {QStringLiteral("pressure"), i18nc("@info:tooltip pen input", "Light → firm press")},
        {QStringLiteral("pressurein"), i18nc("@info:tooltip pen input", "The hardest you've pressed so far, so it never lightens mid-stroke")},
        {QStringLiteral("declination"), i18nc("@info:tooltip pen input", "Pen upright → lying flat")},
        {QStringLiteral("speed"), i18nc("@info:tooltip pen input", "Slow → fast strokes")},
        {QStringLiteral("drawingangle"), i18nc("@info:tooltip pen input", "Which way the stroke is heading, → ↓ ← ↑")},
        {QStringLiteral("fade"), i18nc("@info:tooltip pen input", "Start of the stroke → further along it")},
        {QStringLiteral("fuzzy"), i18nc("@info:tooltip pen input", "A new random amount every dab")},
        {QStringLiteral("fuzzystroke"), i18nc("@info:tooltip pen input", "One random amount for the whole stroke")},
        {QStringLiteral("xtilt"), i18nc("@info:tooltip pen input", "Leaning left ↔ right")},
        {QStringLiteral("ytilt"), i18nc("@info:tooltip pen input", "Leaning toward ↕ away")},
        {QStringLiteral("ascension"), i18nc("@info:tooltip pen input", "Which way the pen leans, like a compass")},
        {QStringLiteral("rotation"), i18nc("@info:tooltip pen input", "Turning the pen in your fingers (art pens)")},
        {QStringLiteral("distance"), i18nc("@info:tooltip pen input", "How far along the stroke, repeating")},
        {QStringLiteral("time"), i18nc("@info:tooltip pen input", "How long the stroke has lasted, repeating")},
        {QStringLiteral("perspective"), i18nc("@info:tooltip pen input", "Near → far on the perspective grid")},
        {QStringLiteral("tangentialpressure"), i18nc("@info:tooltip pen input", "The airbrush pen's finger wheel")},
    };
    return tips.value(id);
}

QStringList KisStudioPenPopup::mainInputs()
{
    return {QStringLiteral("pressure"), QStringLiteral("declination"), QStringLiteral("speed"),
            QStringLiteral("drawingangle"), QStringLiteral("fade"), QStringLiteral("fuzzy")};
}

KisStudioPenPopup::KisStudioPenPopup(KisPaintOpOption *option, QWidget *parent, const QString &input)
    : KisStudioPopup(parent)
    , d(new Private)
{
    d->option = option;
    d->selected = input;
    // opened on one input (a cell of the pen grid, whose column is the input): only that curve
    const bool oneInput = !input.isEmpty();
    if (d->selected.isEmpty()) {
        Q_FOREACH (const KisPaintOpOption::PenInput &each, option->penInputs()) {
            if (each.active) {
                d->selected = each.id;
                break;
            }
        }
    }
    if (d->selected.isEmpty()) d->selected = QStringLiteral("pressure");

    auto *layout = new QVBoxLayout(this);
    fitLayout(layout, KisModernStyle::space3);
    layout->setSpacing(KisModernStyle::space2 + KisModernStyle::hairline);

    auto *header = new QHBoxLayout();
    header->setSpacing(KisModernStyle::space2);
    d->title = new QLabel(oneInput ? i18nc("a pen input drives a brush setting: %1 the input, %2 the setting", "%1 → %2",
                                           inputName(input, d->input(input).name), option->label())
                                   : option->label(),
                          this);
    d->title->setFont(KisModernStyle::textFont(font(), 1.0, QFont::DemiBold));
    header->addWidget(d->title, 1);
    const std::optional<KisPaintOpOption::RowValue> value = option->rowValue();
    if (option->isCheckable() && value && !value->appliesUnchecked) {
        // an effect the brush can go without (Rotation, Scatter): its own switch
        d->on = new QCheckBox(this);
        d->on->setToolTip(i18nc("@info:tooltip a brush effect's switch", "Off: the brush paints without it"));
        header->addWidget(d->on);
        connect(d->on, &QCheckBox::toggled, this, [this](bool checked) {
            if (d->option) d->option->setChecked(checked);
        });
    }
    layout->addLayout(header);

    d->inputs = new KisStudioChips(this);
    layout->addWidget(d->inputs);
    d->inputs->setVisible(!oneInput);
    connect(d->inputs, &KisStudioChips::clicked, this, [this](int index) {
        if (!d->option) return;
        if (index >= d->shown.size()) {
            d->showAll = !d->showAll;
            refresh();
            return;
        }
        const KisPaintOpOption::PenInput picked = d->shown[index];
        if (picked.id == d->selected) {
            d->option->setPenInputActive(picked.id, !picked.active);
        } else {
            d->selected = picked.id;
            if (!picked.active) d->option->setPenInputActive(picked.id, true);
        }
        refresh();
    });

    d->curve = new KisCurveWidget(this);
    d->curve->setFixedHeight(150);
    d->curve->setToolTip(i18nc("@info:tooltip a pen curve", "Across: the pen's input\nUp: how much"));
    layout->addWidget(d->curve);
    connect(d->curve, &KisCurveWidget::modified, this, [this]() {
        if (!d->option) return;
        const QString curve = d->curve->curve().toString();
        d->option->setPenCurve(d->selected, curve);
        if (!d->input(d->selected).active) d->option->setPenInputActive(d->selected, true);
        d->saved->setCurrent(curve);
    });

    d->saved = new KisCurvePresetStrip(this);
    layout->addWidget(d->saved);
    connect(d->saved, &KisCurvePresetStrip::picked, this, [this](const QString &curve) {
        if (!d->option) return;
        d->option->setPenCurve(d->selected, curve);
        if (!d->input(d->selected).active) d->option->setPenInputActive(d->selected, true);
        refresh();
    });

    d->modes = new KisStudioChips(this);
    d->modes->setTabs(true);
    layout->addWidget(d->modes);
    connect(d->modes, &KisStudioChips::clicked, this, [this](int index) {
        if (d->option) d->option->setPenMode(index);
        refresh();
    });

    d->inputPageHolder = new QWidget(this);
    auto *holderLayout = new QVBoxLayout(d->inputPageHolder);
    holderLayout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(d->inputPageHolder);

    setFixedWidth(320 + 2 * shadowMargin());
    connect(option, &KisPaintOpOption::sigSettingChanged, this, &KisStudioPenPopup::refresh);
    connect(option, &QObject::destroyed, this, &QWidget::close);
    refresh();
}

KisStudioPenPopup::~KisStudioPenPopup()
{
}

KisPaintOpOption *KisStudioPenPopup::option() const
{
    return d->option;
}

void KisStudioPenPopup::refresh()
{
    if (!d->option) return;
    const QVector<KisPaintOpOption::PenInput> all = d->option->penInputs();
    const QStringList main = mainInputs();

    d->shown.clear();
    QVector<KisStudioChips::Chip> chips;
    int activeCount = 0;
    int current = -1;
    // the main inputs in their order, then any other that is on or picked, then the rest when asked
    auto add = [&](const KisPaintOpOption::PenInput &input) {
        KisStudioChips::Chip chip;
        chip.text = inputName(input.id, input.name);
        chip.toolTip = inputTip(input.id);
        chip.on = input.active;
        chip.dot = input.active;
        if (input.id == d->selected) current = chips.size();
        chips << chip;
        d->shown << input;
    };
    Q_FOREACH (const QString &id, main) {
        Q_FOREACH (const KisPaintOpOption::PenInput &input, all) {
            if (input.id == id) add(input);
        }
    }
    Q_FOREACH (const KisPaintOpOption::PenInput &input, all) {
        if (input.active) activeCount++;
        if (main.contains(input.id)) continue;
        if (d->showAll || input.active || input.id == d->selected) add(input);
    }
    KisStudioChips::Chip more;
    more.text = d->showAll ? i18nc("show fewer pen inputs", "Less") : QStringLiteral("···");
    more.toolTip = d->showAll ? QString() : i18nc("@info:tooltip", "More pen inputs");
    chips << more;
    d->inputs->setChips(chips);
    d->inputs->setCurrent(current);

    const KisPaintOpOption::PenInput selected = d->input(d->selected);
    const QString curve = selected.curve.isEmpty() ? KisCubicCurve().toString() : selected.curve;
    if (d->curve->curve().toString() != curve) {
        KisSignalsBlocker blocker(d->curve);
        d->curve->setCurve(KisCubicCurve(curve));
    }
    d->saved->setCurrent(curve);

    const int mode = d->option->penMode();
    // decided here, not by isVisible(): the popup itself may not be shown yet
    const bool combining = mode >= 0 && activeCount > 1;
    d->modes->setVisible(combining);
    if (combining) {
        QVector<KisStudioChips::Chip> modes;
        // how two inputs make one amount, with an example
        const QStringList tips = {i18nc("@info:tooltip pen inputs combined", "50% × 50% → 25%"),
                                  i18nc("@info:tooltip pen inputs combined", "50% + 30% → 80%"),
                                  i18nc("@info:tooltip pen inputs combined", "The stronger: 50%, 30% → 50%"),
                                  i18nc("@info:tooltip pen inputs combined", "The weaker: 50%, 30% → 30%"),
                                  i18nc("@info:tooltip pen inputs combined", "The gap: 50%, 30% → 20%")};
        const QStringList names = KisPaintOpOption::penModeNames();
        for (int i = 0; i < names.size(); i++) {
            KisStudioChips::Chip chip;
            chip.text = names[i];
            chip.toolTip = tips.value(i);
            modes << chip;
        }
        d->modes->setChips(modes);
        d->modes->setCurrent(mode);
    }

    if (d->on) {
        KisSignalsBlocker blocker(d->on);
        d->on->setChecked(d->option->isChecked());
    }

    if (d->inputPageId != d->selected) {
        delete d->inputPage;
        d->inputPageId = d->selected;
        d->inputPage = d->option->createPenInputPage(d->selected, d->inputPageHolder);
        if (d->inputPage) d->inputPageHolder->layout()->addWidget(d->inputPage);
        d->inputPageHolder->setVisible(d->inputPage);
    }
    // fitted again only when it grew or shrank, not with each step of a curve dragged
    if (layout()) layout()->activate();
    if (size() != sizeHint()) {
        adjustSize();
        if (isVisible()) keepOnScreen();
    }
}
