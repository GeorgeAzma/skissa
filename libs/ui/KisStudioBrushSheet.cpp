/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioBrushSheet.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QHBoxLayout>
#include <QHash>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPushButton>
#include <QSet>
#include <QTimer>
#include <QToolButton>
#include <QToolTip>
#include <QVBoxLayout>

#include <klocalizedstring.h>

#include <KisModernStyle.h>
#include <KoCanvasResourceProvider.h>
#include <brushengine/KisPaintOpPresetUpdateProxy.h>
#include <brushengine/kis_paintop_preset.h>
#include <brushengine/kis_paintop_registry.h>
#include <kis_cubic_curve.h>
#include <kis_icon_utils.h>
#include <kis_signal_auto_connection.h>
#include <kactioncollection.h>

#include <algorithm>

#include "KisLastStrokeReplay.h"
#include "KisMainWindow.h"
#include "KisStudioBarButtons.h"
#include "KisStudioBrushEditor.h"
#include "KisStudioBrushPicker.h"
#include "KisStudioGlass.h"
#include "KisStudioPenPopup.h"
#include "KisViewManager.h"
#include "kis_canvas_resource_provider.h"
#include "kis_paintop_box.h"
#include "kis_paintop_option.h"
#include "kis_paintop_settings_widget.h"
#include "widgets/KisStudioChips.h"
#include "widgets/KisStudioValueRow.h"
#include "widgets/kis_preset_live_preview_view.h"

namespace {

constexpr int columnWidth = 300;

QHash<KisMainWindow *, QPointer<KisStudioBrushSheet>> &sheets()
{
    static QHash<KisMainWindow *, QPointer<KisStudioBrushSheet>> all;
    return all;
}

QString partName(int part)
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
    }
    return QString();
}

/**
 * Which pen input drives which setting: a row for each setting the pen drives, a column for each
 * input, its curve where it is on. A click on a cell shows that input's curve, turning it on.
 */
class PenGrid : public QWidget
{
    // a class of its own: a press on it picks a cell, it doesn't move the window
    Q_OBJECT
public:
    PenGrid(std::function<QList<KisPaintOpOption *>()> options, QWidget *parent)
        : QWidget(parent)
        , m_options(options)
    {
        setMouseTracking(true);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

    std::function<void(KisPaintOpOption *, const QString &, QWidget *)> onCell;

    QList<KisPaintOpOption *> rows() const
    {
        QList<KisPaintOpOption *> rows;
        // a part that is off (no mask, no texture) drives nothing
        QSet<int> off;
        Q_FOREACH (KisPaintOpOption *option, m_options()) {
            const auto category = option->category();
            if (option->hasQuickPage(category) && option->isCheckable() && !option->isChecked()) off << category;
        }
        Q_FOREACH (KisPaintOpOption *option, m_options()) {
            if (off.contains(option->category())) continue;
            if (!option->isEnabled() || (option->isCheckable() && !option->isChecked())) continue;
            Q_FOREACH (const KisPaintOpOption::PenInput &input, option->penInputs()) {
                if (input.active) {
                    rows << option;
                    break;
                }
            }
        }
        return rows;
    }

    QVector<KisPaintOpOption::PenInput> inputs() const
    {
        Q_FOREACH (KisPaintOpOption *option, m_options()) {
            const QVector<KisPaintOpOption::PenInput> inputs = option->penInputs();
            if (!inputs.isEmpty()) return inputs;
        }
        return {};
    }

    void refresh()
    {
        setFixedHeight(header + rowHeight * qMax(1, rows().size()));
        update();
    }

    QSize sizeHint() const override
    {
        return QSize(600, header + rowHeight * qMax(1, rows().size()));
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QColor ink = palette().color(QPalette::WindowText);
        const QColor quiet = KisModernStyle::secondaryInk(ink);
        const QVector<KisPaintOpOption::PenInput> columns = inputs();
        const QList<KisPaintOpOption *> lines = rows();
        if (columns.isEmpty()) return;
        const qreal cell = (width() - label) / qreal(columns.size());
        painter.setFont(KisModernStyle::textFont(font(), 0.85));
        for (int c = 0; c < columns.size(); c++) {
            painter.setPen(quiet);
            const QRectF head(label + c * cell, 0, cell, header);
            painter.drawText(head, Qt::AlignCenter | Qt::TextWordWrap, KisStudioPenPopup::inputName(columns[c].id, columns[c].name));
        }
        painter.setFont(font());
        for (int r = 0; r < lines.size(); r++) {
            KisPaintOpOption *option = lines[r];
            const qreal y = header + r * rowHeight;
            painter.setPen(ink);
            // the mask's and the texture's own settings say whose they are
            const QString name = option->category() == KisPaintOpOption::GENERAL || option->category() == KisPaintOpOption::COLOR
                ? option->label() : partName(option->category()) + QLatin1Char(' ') + option->label().toLower();
            painter.drawText(QRectF(0, y, label - 8, rowHeight), Qt::AlignVCenter | Qt::AlignLeft, name);
            const QVector<KisPaintOpOption::PenInput> inputs = option->penInputs();
            for (int c = 0; c < columns.size() && c < inputs.size(); c++) {
                const QRectF box = QRectF(label + c * cell, y, cell, rowHeight).adjusted(1.5, 1.5, -1.5, -1.5);
                QColor ground = ink;
                ground.setAlphaF(inputs[c].active ? 0.09 : QRectF(box).contains(m_hover) ? 0.06 : 0.025);
                painter.fillPath(KisModernStyle::squircle(box, 5), ground);
                if (!inputs[c].active) continue;
                const KisCubicCurve curve(inputs[c].curve.isEmpty() ? QStringLiteral("0,0;1,1;") : inputs[c].curve);
                const QRectF glyph = box.adjusted(box.width() / 2 - 13, 6, -(box.width() / 2 - 13), -6);
                QPainterPath path;
                for (int i = 0; i <= 12; i++) {
                    const qreal x = i / 12.0;
                    const QPointF point(glyph.left() + x * glyph.width(), glyph.bottom() - qBound(0.0, curve.value(x), 1.0) * glyph.height());
                    if (i) path.lineTo(point);
                    else path.moveTo(point);
                }
                painter.setPen(QPen(ink, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                painter.setBrush(Qt::NoBrush);
                painter.drawPath(path);
            }
        }
        if (lines.isEmpty()) {
            painter.setPen(quiet);
            painter.drawText(QRectF(0, header, width(), rowHeight), Qt::AlignVCenter | Qt::AlignLeft,
                             i18nc("the pen grid when no setting follows the pen", "Nothing follows the pen"));
        }
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        m_hover = event->pos();
        update();
    }

    void leaveEvent(QEvent *) override
    {
        m_hover = QPointF(-1, -1);
        update();
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        event->accept();
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        const QVector<KisPaintOpOption::PenInput> columns = inputs();
        const QList<KisPaintOpOption *> lines = rows();
        if (columns.isEmpty() || event->pos().x() < label || event->pos().y() < header) return;
        const int c = int((event->pos().x() - label) / ((width() - label) / qreal(columns.size())));
        const int r = (event->pos().y() - header) / rowHeight;
        if (r < 0 || r >= lines.size() || c < 0 || c >= columns.size()) return;
        // the cell itself is what its popup opens beside
        const qreal cell = (width() - label) / qreal(columns.size());
        auto *anchor = new QWidget(this);
        anchor->setGeometry(QRectF(label + c * cell, header + r * rowHeight, cell, rowHeight).toRect());
        if (onCell) onCell(lines[r], columns[c].id, anchor);
        anchor->deleteLater();
    }

private:
    static constexpr int label = 130;
    static constexpr int header = 30;
    static constexpr int rowHeight = 30;
    std::function<QList<KisPaintOpOption *>()> m_options;
    QPointF m_hover {-1, -1};
};

/// A setting's full page, as Krita shows it, lent by the full editor while this is open
class LentPage : public KisStudioPopup
{
public:
    LentPage(KisPaintOpSettingsWidget *settings, KisPaintOpOption *option, QWidget *page, QWidget *parent)
        : KisStudioPopup(parent)
        , m_settings(settings)
        , m_option(option)
    {
        auto *layout = new QVBoxLayout(this);
        fitLayout(layout, KisModernStyle::space3);
        layout->addWidget(page);
        // on the glass here, as it was in the full editor's window
        KisStudioGlass::setSeeThrough(page, true);
        m_page = page;
        page->show();
    }

    ~LentPage() override
    {
        // given back before this window's children go
        if (m_page) KisStudioGlass::setSeeThrough(m_page, false);
        if (m_settings && m_option) m_settings->returnPage(m_option);
    }

private:
    QPointer<KisPaintOpSettingsWidget> m_settings;
    QPointer<KisPaintOpOption> m_option;
    QPointer<QWidget> m_page;
};

} // namespace

struct KisStudioBrushSheet::Private
{
    QPointer<KisMainWindow> mainWindow;
    QPointer<KisPaintopBox> box;
    QPointer<KisPaintOpSettingsWidget> settings;

    QLabel *name {nullptr};
    QToolButton *engine {nullptr};
    /// What is done with the brush: left out of its picture
    QList<QWidget *> actions;
    QToolButton *reload {nullptr};
    QToolButton *follow {nullptr};
    KisPresetLivePreviewView *preview {nullptr};
    QWidget *columns {nullptr};
    QHBoxLayout *columnsLayout {nullptr};
    PenGrid *grid {nullptr};

    QPointer<KisStudioPenPopup> pen;
    QPointer<KisStudioPopup> details;
    QTimer refreshGrid;
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
};

void KisStudioBrushSheet::toggle(KisMainWindow *mainWindow)
{
    QPointer<KisStudioBrushSheet> &sheet = sheets()[mainWindow];
    if (sheet) {
        sheet->close();
        return;
    }
    sheet = new KisStudioBrushSheet(mainWindow);
    sheet->adjustSize();
    // in the middle of the window
    const QPoint middle = mainWindow->mapToGlobal(mainWindow->rect().center());
    sheet->popupAt(middle - QPoint(sheet->width() / 2, sheet->height() / 2) + QPoint(sheet->shadowMargin(), sheet->shadowMargin()));
}

KisStudioBrushSheet::KisStudioBrushSheet(KisMainWindow *mainWindow)
    : KisStudioPopup(mainWindow)
    , d(new Private)
{
    d->mainWindow = mainWindow;
    d->box = mainWindow->viewManager() ? mainWindow->viewManager()->paintOpBox() : nullptr;
    setObjectName(QStringLiteral("studioBrushSheet"));

    auto *layout = new QVBoxLayout(this);
    fitLayout(layout, KisModernStyle::space4);
    layout->setSpacing(KisModernStyle::space3);

    // the brush: its name and engine, its stroke, and what is done with it
    auto *header = new QHBoxLayout();
    header->setSpacing(KisModernStyle::space3);
    auto *title = new QVBoxLayout();
    title->setSpacing(KisModernStyle::hairline);
    d->name = new QLabel(this);
    d->name->setFont(KisModernStyle::textFont(font(), 1.2, QFont::DemiBold));
    title->addWidget(d->name);
    d->engine = new QToolButton(this);
    d->engine->setAutoRaise(true);
    d->engine->setPopupMode(QToolButton::InstantPopup);
    d->engine->setToolTip(i18nc("@info:tooltip", "A new brush with another engine"));
    title->addWidget(d->engine, 0, Qt::AlignLeft);
    header->addLayout(title);
    d->preview = new KisPresetLivePreviewView(this);
    d->preview->setFixedSize(460, 64);
    d->preview->setFrameShape(QFrame::NoFrame);
    if (mainWindow->viewManager() && mainWindow->viewManager()->canvasResourceProvider()) {
        d->preview->setup(mainWindow->viewManager()->canvasResourceProvider()->resourceManager());
    }
    header->addWidget(d->preview);
    header->addStretch(1);

    auto iconButton = [this](const QString &icon, const QString &toolTip) {
        auto *button = new QToolButton(this);
        button->setAutoRaise(true);
        button->setIcon(KisIconUtils::loadIcon(icon));
        button->setToolTip(toolTip);
        return button;
    };
    QToolButton *copy = iconButton(QStringLiteral("edit-copy"), i18nc("@info:tooltip", "Copy as a picture"));
    connect(copy, &QToolButton::clicked, this, &KisStudioBrushSheet::copyPicture);
    header->addWidget(copy);
    d->reload = iconButton(QStringLiteral("studio_reload"), i18nc("@info:tooltip", "Back to the saved brush"));
    connect(d->reload, &QToolButton::clicked, this, [this]() {
        if (QAction *reload = d->mainWindow ? d->mainWindow->actionCollection()->action(QStringLiteral("reload_preset_action")) : nullptr) {
            reload->trigger();
        }
    });
    header->addWidget(d->reload);
    auto *saveNew = new QPushButton(i18nc("@action:button", "Save as new"), this);
    connect(saveNew, &QPushButton::clicked, this, [this]() {
        if (d->box) d->box->savePreset(true);
    });
    header->addWidget(saveNew);
    auto *save = new QPushButton(i18nc("@action:button save the brush over itself", "Save"), this);
    save->setDefault(true);
    connect(save, &QPushButton::clicked, this, [this]() {
        if (d->box) d->box->savePreset(false);
    });
    header->addWidget(save);
    // Krita's own editor, for what only it has (the scratchpad, renaming, Instant Preview)
    QToolButton *more = iconButton(QStringLiteral("studio_more"), QString());
    more->setPopupMode(QToolButton::InstantPopup);
    auto *moreMenu = new QMenu(more);
    moreMenu->addAction(i18nc("@action Krita's own brush editor", "Classic editor"), this, [this]() {
        QPointer<KisPaintopBox> box = d->box;
        close();
        if (box) box->showFullEditor();
    });
    more->setMenu(moreMenu);
    header->addWidget(more);
    d->follow = iconButton(QStringLiteral("studio_restroke"), i18nc("@info:tooltip", "Your last stroke follows the brush"));
    d->follow->setCheckable(true);
    d->follow->setChecked(KisLastStrokeReplay::isEnabled());
    connect(d->follow, &QToolButton::toggled, this, [](bool on) { KisLastStrokeReplay::setEnabled(on); });
    header->addWidget(d->follow);
    QToolButton *pin = iconButton(QStringLiteral("studio_pin"), i18nc("@info:tooltip", "Keep open while painting"));
    pin->setCheckable(true);
    connect(pin, &QToolButton::toggled, this, &KisStudioPopup::setPinned);
    header->addWidget(pin);
    d->actions = {copy, d->reload, saveNew, save, more, d->follow, pin};
    layout->addLayout(header);

    d->columns = new QWidget(this);
    d->columnsLayout = new QHBoxLayout(d->columns);
    d->columnsLayout->setContentsMargins(0, 0, 0, 0);
    d->columnsLayout->setSpacing(KisModernStyle::space4);
    layout->addWidget(d->columns);

    d->grid = new PenGrid([this]() { return d->options(); }, this);
    d->grid->onCell = [this](KisPaintOpOption *option, const QString &input, QWidget *anchor) {
        if (d->pen) d->pen->close();
        const bool wasOn = [&]() {
            Q_FOREACH (const KisPaintOpOption::PenInput &each, option->penInputs()) {
                if (each.id == input) return each.active;
            }
            return false;
        }();
        if (!wasOn) option->setPenInputActive(input, true);
        auto *pen = new KisStudioPenPopup(option, this, input);
        d->pen = pen;
        pen->popupBeside(anchor, Qt::TopEdge);
    };
    layout->addWidget(d->grid);

    d->refreshGrid.setSingleShot(true);
    connect(&d->refreshGrid, &QTimer::timeout, this, [this]() {
        d->grid->refresh();
        adjustSize();
    });

    if (d->box) {
        connect(d->box, &KisPaintopBox::sigOptionWidgetChanged, this, &KisStudioBrushSheet::rebuild);
    }
    rebuild();
}

KisStudioBrushSheet::~KisStudioBrushSheet()
{
}

void KisStudioBrushSheet::showEvent(QShowEvent *event)
{
    if (d->box) d->box->setQuickEditorShown(true);
    KisStudioPopup::showEvent(event);
}

void KisStudioBrushSheet::closeEvent(QCloseEvent *event)
{
    if (d->box) d->box->setQuickEditorShown(false);
    if (d->pen) d->pen->close();
    if (d->details) d->details->close();
    KisStudioPopup::closeEvent(event);
}

void KisStudioBrushSheet::updateHeader()
{
    KisPaintOpPresetSP preset = d->preset();
    d->name->setText(preset ? KisStudioBrushPicker::title(preset->name()) : QString());
    d->reload->setVisible(preset && preset->isDirty());

    const QString current = preset ? preset->paintOp().id() : QString();
    KisPaintOpFactory *factory = current.isEmpty() ? nullptr : KisPaintOpRegistry::instance()->get(current);
    d->engine->setText(factory ? factory->name() : QString());
}

void KisStudioBrushSheet::rebuild()
{
    if (d->pen) d->pen->close();
    if (d->details) d->details->close();
    d->settings = KisStudioBrushEditor::brushSettings(d->mainWindow);

    d->presetConnections.clear();
    if (KisPaintOpPresetSP preset = d->preset()) {
        d->preview->setCurrentPreset(preset);
        d->preview->requestUpdateStroke();
        if (preset->updateProxy()) {
            // the last stroke shows the change on the picture, as soon as it is made
            d->presetConnections.addConnection(preset->updateProxy(), &KisPaintOpPresetUpdateProxy::sigSettingsChangedUncompressed, this,
                                               [this]() {
                                                   KisPaintOpPresetSP current = d->preset();
                                                   if (current && d->follow->isChecked()) KisLastStrokeReplay::instance()->brushChanged(current->name(), current->settings()->toXML());
                                               });
            d->presetConnections.addConnection(preset->updateProxy(), &KisPaintOpPresetUpdateProxy::sigSettingsChanged, this, [this]() {
                d->preview->requestUpdateStroke();
                updateHeader();
                d->refreshGrid.start();
            });
        }
    }

    // a new brush of another engine
    auto *engines = new QMenu(d->engine);
    QList<KisPaintOpFactory *> factories;
    Q_FOREACH (const QString &id, KisPaintOpRegistry::instance()->keys()) {
        if (KisPaintOpFactory *factory = KisPaintOpRegistry::instance()->get(id)) factories << factory;
    }
    std::sort(factories.begin(), factories.end(), [](KisPaintOpFactory *a, KisPaintOpFactory *b) {
        return a->name().localeAwareCompare(b->name()) < 0;
    });
    Q_FOREACH (KisPaintOpFactory *factory, factories) {
        const QString id = factory->id();
        engines->addAction(factory->name(), this, [this, id]() {
            if (d->box) d->box->slotCreatePresetFromScratch(id);
        });
    }
    delete d->engine->menu();
    d->engine->setMenu(engines);
    updateHeader();

    while (QLayoutItem *item = d->columnsLayout->takeAt(0)) {
        if (QWidget *widget = item->widget()) {
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }

    const QList<KisPaintOpOption *> options = d->options();
    for (int part : {KisPaintOpOption::GENERAL, KisPaintOpOption::MASKING_BRUSH, KisPaintOpOption::TEXTURE,
                     KisPaintOpOption::COLOR, KisPaintOpOption::FILTER}) {
        const auto category = KisPaintOpOption::PaintopCategory(part);
        bool has = false;
        Q_FOREACH (KisPaintOpOption *option, options) {
            if (option->category() == part || option->hasQuickPage(category)) has = true;
        }
        if (!has) continue;

        auto *column = new QWidget(d->columns);
        column->setFixedWidth(columnWidth);
        auto *columnLayout = new QVBoxLayout(column);
        columnLayout->setContentsMargins(0, 0, 0, 0);
        columnLayout->setSpacing(KisModernStyle::space1);
        auto *title = new QLabel(partName(part), column);
        title->setFont(KisModernStyle::textFont(font(), 1.0, QFont::DemiBold));
        columnLayout->addWidget(title);

        for (bool own : {true, false}) {
            Q_FOREACH (KisPaintOpOption *option, options) {
                if ((option->category() == part) != own || !option->hasQuickPage(category)) continue;
                if (QWidget *quick = option->createQuickPage(category, column)) {
                    columnLayout->addWidget(quick);
                    columnLayout->addSpacing(KisModernStyle::space1);
                }
            }
        }

        // a part that is off (no mask, no texture) keeps only its pictures, to turn it on
        KisPaintOpOption *lead = nullptr;
        Q_FOREACH (KisPaintOpOption *option, options) {
            if (option->category() == part && option->hasQuickPage(category) && option->isCheckable()) lead = option;
        }
        QList<QWidget *> partSettings;

        // every setting of the part, the ones that are off quiet
        QList<KisPaintOpOption *> details;
        QList<KisPaintOpOption *> inPart;
        Q_FOREACH (KisPaintOpOption *option, options) {
            if (option->category() == part) inPart << option;
        }
        // the ones every brush has first, as in the quick editor
        static const QStringList first = {QStringLiteral("Size"), QStringLiteral("Opacity"), QStringLiteral("Flow"), QStringLiteral("Spacing")};
        std::stable_sort(inPart.begin(), inPart.end(), [](KisPaintOpOption *a, KisPaintOpOption *b) {
            const int ia = first.indexOf(a->id());
            const int ib = first.indexOf(b->id());
            return (ia < 0 ? first.size() : ia) < (ib < 0 ? first.size() : ib);
        });
        Q_FOREACH (KisPaintOpOption *option, inPart) {
            if (!option->rowValue()) {
                details << option;
                continue;
            }
            auto *row = new KisStudioValueRow(option->label(), column);
            row->setCompact(true);
            KisStudioBrushEditor::bindRow(row, option, d->settings);
            const QPointer<KisPaintOpOption> target(option);
            const QString key = KisStudioBrushEditor::pinKey(option);
            row->setPinnable(true, KisStudioBarButtons::has(d->mainWindow, key));
            connect(row, &KisStudioValueRow::pinClicked, this, [this, row, key]() {
                KisStudioBarButtons::toggle(d->mainWindow, key);
                row->setPinnable(true, KisStudioBarButtons::has(d->mainWindow, key));
            });
            connect(row, &KisStudioValueRow::pulledOut, this, [row, key]() { KisStudioBarButtons::dragItem(key, row); });
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
                pen->popupBeside(row, Qt::RightEdge);
            });
            columnLayout->addWidget(row);
            partSettings << row;
        }

        // the rest of a part, as Krita shows it
        if (!details.isEmpty()) {
            auto *more = new KisStudioChips(column);
            QVector<KisStudioChips::Chip> chips;
            QList<QPointer<KisPaintOpOption>> targets;
            Q_FOREACH (KisPaintOpOption *option, details) {
                KisStudioChips::Chip chip;
                chip.text = option->label() + QStringLiteral(" ›");
                chips << chip;
                targets << option;
            }
            more->setChips(chips);
            connect(more, &KisStudioChips::clicked, this, [this, more, targets](int index) {
                if (index < 0 || index >= targets.size() || !targets[index] || !d->settings) return;
                if (d->details) d->details->close();
                KisPaintOpOption *option = targets[index];
                QWidget *page = d->settings->lendPage(option);
                if (!page) return;
                auto *popup = new LentPage(d->settings, option, page, this);
                d->details = popup;
                popup->popupBeside(more, Qt::RightEdge);
            });
            columnLayout->addSpacing(KisModernStyle::space2);
            columnLayout->addWidget(more);
            partSettings << more;
        }
        if (lead) {
            const QPointer<KisPaintOpOption> on(lead);
            auto show = [this, on, partSettings]() {
                Q_FOREACH (QWidget *widget, partSettings) {
                    widget->setVisible(on && on->isChecked());
                }
                QTimer::singleShot(0, this, [this]() { adjustSize(); });
            };
            show();
            connect(lead, &KisPaintOpOption::sigCheckedChanged, column, show);
        }
        columnLayout->addStretch(1);
        d->columnsLayout->addWidget(column, 0, Qt::AlignTop);

        Q_FOREACH (KisPaintOpOption *option, options) {
            connect(option, &KisPaintOpOption::sigCheckedChanged, &d->refreshGrid, qOverload<>(&QTimer::start), Qt::UniqueConnection);
            connect(option, &KisPaintOpOption::sigSettingChanged, &d->refreshGrid, qOverload<>(&QTimer::start), Qt::UniqueConnection);
        }
    }
    d->grid->refresh();
    adjustSize();
    if (isVisible()) keepOnScreen();
}

void KisStudioBrushSheet::copyPicture()
{
    // the sheet as shown, on its own color rather than on what is behind the glass
    const int margin = shadowMargin();
    const QRect panel = rect().adjusted(margin, margin, -margin, -margin);
    const qreal ratio = devicePixelRatioF();
    QPixmap picture(panel.size() * ratio);
    picture.setDevicePixelRatio(ratio);
    QColor ground = palette().color(QPalette::Window);
    ground.setAlpha(255);
    picture.fill(ground);
    QList<QWidget *> shown;
    Q_FOREACH (QWidget *action, d->actions) {
        if (action->isVisible()) shown << action;
        action->setVisible(false);
    }
    QPainter painter(&picture);
    render(&painter, QPoint(), QRegion(panel), QWidget::DrawChildren);
    painter.end();
    Q_FOREACH (QWidget *action, shown) {
        action->setVisible(true);
    }
    QApplication::clipboard()->setPixmap(picture);
    QToolTip::showText(QCursor::pos(), i18nc("@info:tooltip", "Copied"), this);
}

#include "KisStudioBrushSheet.moc"
