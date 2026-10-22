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
#include <QStyleOptionToolButton>
#include <QStylePainter>
#include <QToolButton>
#include <QToolTip>
#include <QHelpEvent>
#include <ksharedconfig.h>
#include <kconfiggroup.h>
#include <QVBoxLayout>

#include <klocalizedstring.h>

#include <KisModernStyle.h>
#include <KoCanvasResourceProvider.h>
#include <KoCanvasResourcesIds.h>
#include <KoID.h>
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
#include "KisStudioMixing.h"
#include "KisStudioBrushPicker.h"
#include "KisStudioGlass.h"
#include "KisStudioPenPopup.h"
#include "KisStudioTips.h"
#include "KisViewManager.h"
#include "kis_canvas_resource_provider.h"
#include "kis_node.h"
#include "kis_paintop_box.h"
#include "kis_paintop_option.h"
#include "kis_paintop_settings_widget.h"
#include "widgets/KisStudioChips.h"
#include "widgets/KisStudioValueRow.h"
#include "widgets/kis_cmb_composite.h"
#include "widgets/kis_preset_live_preview_view.h"

namespace {

/// A name that opens a menu, drawn as the name alone: no arrow beside it
class MenuNameButton : public QToolButton
{
public:
    using QToolButton::QToolButton;

protected:
    void paintEvent(QPaintEvent *) override
    {
        QStylePainter painter(this);
        QStyleOptionToolButton option;
        initStyleOption(&option);
        option.features &= ~QStyleOptionToolButton::HasMenu;
        painter.drawComplexControl(QStyle::CC_ToolButton, option);
    }
};


constexpr int columnWidth = 300;

QHash<KisMainWindow *, QPointer<KisStudioBrushSheet>> &sheets()
{
    static QHash<KisMainWindow *, QPointer<KisStudioBrushSheet>> all;
    return all;
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
        m_extraColumns = KSharedConfig::openConfig()->group(QStringLiteral("StudioBrushSheet")).readEntry("PenColumns", QStringList());
    }

    /// A cell to edit: its curve's popup, opened beside \p anchor (the input turned on first if it was off)
    std::function<void(KisPaintOpOption *, const QString &, QWidget *)> onCell;
    /// A cell to turn off
    std::function<void(KisPaintOpOption *, const QString &)> onCellOff;

    /// Settings in use the pen can drive: those it does, the main ones and any added; a part that is off drives nothing
    QList<KisPaintOpOption *> rows() const
    {
        QSet<int> off;
        Q_FOREACH (KisPaintOpOption *option, m_options()) {
            const auto category = option->category();
            if (option->hasQuickPage(category) && option->isCheckable() && !option->isChecked()) off << category;
        }
        static const QStringList main = {QStringLiteral("Size"), QStringLiteral("Opacity"), QStringLiteral("Flow")};
        QList<KisPaintOpOption *> rows;
        Q_FOREACH (KisPaintOpOption *option, candidates()) {
            if (off.contains(option->category())) continue;
            if (followsPen(option) || main.contains(option->id()) || m_extraRows.contains(option->id())) rows << option;
        }
        return rows;
    }

    /// The pen inputs shown: the main ones, those any row uses and any added
    QVector<KisPaintOpOption::PenInput> columns() const
    {
        QVector<KisPaintOpOption::PenInput> all;
        Q_FOREACH (KisPaintOpOption *option, m_options()) {
            all = option->penInputs();
            if (!all.isEmpty()) break;
        }
        QSet<QString> used;
        Q_FOREACH (KisPaintOpOption *option, rows()) {
            Q_FOREACH (const KisPaintOpOption::PenInput &input, option->penInputs()) {
                if (input.active) used << input.id;
            }
        }
        const QStringList main = KisStudioPenPopup::mainInputs();
        QVector<KisPaintOpOption::PenInput> shown;
        Q_FOREACH (const KisPaintOpOption::PenInput &input, all) {
            if (main.contains(input.id) || used.contains(input.id) || m_extraColumns.contains(input.id)) shown << input;
        }
        return shown;
    }

    void refresh()
    {
        setFixedHeight(header + rowHeight * (rows().size() + 1));
        update();
    }

    QSize sizeHint() const override
    {
        return QSize(600, header + rowHeight * (rows().size() + 1));
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QColor ink = palette().color(QPalette::WindowText);
        const QColor quiet = KisModernStyle::secondaryInk(ink);
        const QVector<KisPaintOpOption::PenInput> heads = columns();
        const QList<KisPaintOpOption *> lines = rows();
        if (heads.isEmpty()) return;
        const qreal cell = cellWidth(heads.size());
        const qreal left = labelWidth();

        painter.setFont(KisModernStyle::textFont(font(), 0.85));
        for (int c = 0; c < heads.size(); c++) {
            painter.setPen(quiet);
            const QRectF head(left + c * cell, 0, cell, header);
            const QString name = KisStudioPenPopup::inputName(heads[c].id, heads[c].name);
            painter.drawText(head.adjusted(2, 0, -2, 0), Qt::AlignCenter, painter.fontMetrics().elidedText(name, Qt::ElideRight, int(cell) - 4));
        }
        // more inputs and more settings, each a "+"
        drawPlus(painter, plusColumnRect(heads.size()), plusColumnRect(heads.size()).contains(m_hover));
        painter.setFont(font());
        for (int r = 0; r < lines.size(); r++) {
            KisPaintOpOption *option = lines[r];
            const qreal y = header + r * rowHeight;
            painter.setPen(followsPen(option) ? ink : quiet);
            painter.drawText(QRectF(0, y, left - KisModernStyle::space2, rowHeight), Qt::AlignVCenter | Qt::AlignLeft,
                             painter.fontMetrics().elidedText(rowName(option), Qt::ElideRight, int(left) - KisModernStyle::space2));
            const QVector<KisPaintOpOption::PenInput> inputs = option->penInputs();
            for (int c = 0; c < heads.size(); c++) {
                const KisPaintOpOption::PenInput input = inputOf(inputs, heads[c].id);
                const QRectF box = cellRect(r, c, heads.size());
                const bool hovered = box.contains(m_hover);
                QColor ground = ink;
                ground.setAlphaF(input.active ? (hovered ? 0.13 : 0.10) : hovered ? 0.07 : 0.03);
                painter.fillPath(KisModernStyle::squircle(box, KisModernStyle::cornerRadius), ground);
                if (!input.active) {
                    if (hovered) drawPlus(painter, box, false);
                    continue;
                }
                // the curve, as the input drives the setting
                const KisCubicCurve curve(input.curve.isEmpty() ? QStringLiteral("0,0;1,1;") : input.curve);
                const qreal side = qMin(box.height() - 8, 26.0);
                const QRectF glyph(box.center().x() - side * 0.7, box.center().y() - side / 2, side * 1.4, side);
                QPainterPath path;
                for (int i = 0; i <= 16; i++) {
                    const qreal x = i / 16.0;
                    const QPointF point(glyph.left() + x * glyph.width(), glyph.bottom() - qBound(0.0, curve.value(x), 1.0) * glyph.height());
                    if (i) path.lineTo(point);
                    else path.moveTo(point);
                }
                painter.setPen(QPen(ink, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                painter.setBrush(Qt::NoBrush);
                painter.drawPath(path);
            }
        }
        const QRectF addRow = plusRowRect(lines.size());
        drawPlus(painter, QRectF(addRow.left(), addRow.top(), rowHeight, rowHeight), addRow.contains(m_hover));
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
        const QVector<KisPaintOpOption::PenInput> heads = columns();
        const QList<KisPaintOpOption *> lines = rows();
        if (heads.isEmpty()) return;
        if (event->button() == Qt::LeftButton && plusColumnRect(heads.size()).contains(event->pos())) {
            addColumn(event->globalPos());
            return;
        }
        if (event->button() == Qt::LeftButton && plusRowRect(lines.size()).contains(event->pos())) {
            addRow(event->globalPos());
            return;
        }
        for (int r = 0; r < lines.size(); r++) {
            for (int c = 0; c < heads.size(); c++) {
                if (!cellRect(r, c, heads.size()).contains(event->pos())) continue;
                const KisPaintOpOption::PenInput input = inputOf(lines[r]->penInputs(), heads[c].id);
                if (event->button() == Qt::RightButton) {
                    if (input.active && onCellOff) onCellOff(lines[r], heads[c].id);
                } else if (event->button() == Qt::LeftButton && onCell) {
                    // the cell itself is what its popup opens beside, and a press on it closes the popup
                    auto *anchor = new QWidget(this);
                    anchor->setGeometry(cellRect(r, c, heads.size()).toRect());
                    anchor->show();
                    onCell(lines[r], heads[c].id, anchor);
                }
                update();
                return;
            }
        }
    }

    bool event(QEvent *event) override
    {
        if (event->type() == QEvent::ToolTip) {
            auto *help = static_cast<QHelpEvent *>(event);
            const QString tip = toolTipAt(help->pos());
            if (tip.isEmpty()) QToolTip::hideText();
            else QToolTip::showText(help->globalPos(), KisStudioTips::html(tip), this);
            return true;
        }
        return QWidget::event(event);
    }

private:
    static constexpr int header = 24;
    static constexpr int rowHeight = 30;

    QList<KisPaintOpOption *> candidates() const
    {
        QList<KisPaintOpOption *> all;
        Q_FOREACH (KisPaintOpOption *option, m_options()) {
            if (option->penInputs().isEmpty() || !option->isEnabled()) continue;
            // an effect that is off has nothing to drive; a setting whose switch is only the pen's still does
            const std::optional<KisPaintOpOption::RowValue> value = option->rowValue();
            if (option->isCheckable() && !option->isChecked() && !(value && value->appliesUnchecked)) continue;
            all << option;
        }
        return all;
    }

    static bool followsPen(KisPaintOpOption *option)
    {
        if (option->isCheckable() && !option->isChecked()) return false;
        Q_FOREACH (const KisPaintOpOption::PenInput &input, option->penInputs()) {
            if (input.active) return true;
        }
        return false;
    }

    static KisPaintOpOption::PenInput inputOf(const QVector<KisPaintOpOption::PenInput> &inputs, const QString &id)
    {
        Q_FOREACH (const KisPaintOpOption::PenInput &input, inputs) {
            if (input.id == id) return input;
        }
        return {};
    }

    static QString rowName(KisPaintOpOption *option)
    {
        // the mask's and the texture's own settings say whose they are
        if (option->category() == KisPaintOpOption::GENERAL || option->category() == KisPaintOpOption::COLOR) return option->label();
        return i18nc("a brush setting of a part, e.g. Mask opacity: %1 the part, %2 the setting", "%1 %2",
                     KisStudioBrushEditor::partName(option->category()), option->label().toLower());
    }

    qreal labelWidth() const
    {
        int widest = 0;
        Q_FOREACH (KisPaintOpOption *option, candidates()) widest = qMax(widest, fontMetrics().horizontalAdvance(rowName(option)));
        return qBound(90, widest + KisModernStyle::space3, 170);
    }

    qreal cellWidth(int count) const
    {
        return qMin(84.0, (width() - labelWidth() - rowHeight) / qreal(qMax(1, count)));
    }

    QRectF cellRect(int row, int column, int count) const
    {
        const qreal cell = cellWidth(count);
        return QRectF(labelWidth() + column * cell, header + row * rowHeight, cell, rowHeight).adjusted(1.5, 1.5, -1.5, -1.5);
    }

    QRectF plusColumnRect(int count) const
    {
        return QRectF(labelWidth() + count * cellWidth(count), 0, rowHeight, header);
    }

    QRectF plusRowRect(int count) const
    {
        return QRectF(0, header + count * rowHeight, labelWidth(), rowHeight);
    }

    void drawPlus(QPainter &painter, const QRectF &box, bool hovered) const
    {
        QColor color = KisModernStyle::secondaryInk(palette().color(QPalette::WindowText));
        if (hovered) color = palette().color(QPalette::WindowText);
        painter.setPen(QPen(color, 1.4, Qt::SolidLine, Qt::RoundCap));
        const QPointF c = box.center();
        painter.drawLine(c - QPointF(4.5, 0), c + QPointF(4.5, 0));
        painter.drawLine(c - QPointF(0, 4.5), c + QPointF(0, 4.5));
    }

    QString toolTipAt(const QPoint &pos) const
    {
        const QVector<KisPaintOpOption::PenInput> heads = columns();
        const QList<KisPaintOpOption *> lines = rows();
        if (plusColumnRect(heads.size()).contains(pos)) return i18nc("@info:tooltip", "More pen inputs");
        if (plusRowRect(lines.size()).contains(pos)) return i18nc("@info:tooltip", "More settings");
        for (int c = 0; c < heads.size(); c++) {
            if (QRectF(labelWidth() + c * cellWidth(heads.size()), 0, cellWidth(heads.size()), header).contains(pos)) {
                return KisStudioPenPopup::inputTip(heads[c].id);
            }
        }
        for (int r = 0; r < lines.size(); r++) {
            for (int c = 0; c < heads.size(); c++) {
                if (!cellRect(r, c, heads.size()).contains(pos)) continue;
                const QString what = i18nc("@info:tooltip a pen input drives a brush setting: %1 the input, %2 the setting", "%1 → %2",
                                           KisStudioPenPopup::inputName(heads[c].id, heads[c].name), rowName(lines[r]));
                if (!inputOf(lines[r]->penInputs(), heads[c].id).active) {
                    return KisStudioTips::tip(what, {i18nc("@info:tooltip a gesture", "Click: on")});
                }
                return KisStudioTips::tip(what, {i18nc("@info:tooltip a gesture", "Click: curve"), i18nc("@info:tooltip a gesture", "Right-click: off")});
            }
        }
        return QString();
    }

    void addColumn(const QPoint &at)
    {
        QVector<KisPaintOpOption::PenInput> all;
        Q_FOREACH (KisPaintOpOption *option, m_options()) {
            all = option->penInputs();
            if (!all.isEmpty()) break;
        }
        QStringList shown;
        Q_FOREACH (const KisPaintOpOption::PenInput &input, columns()) shown << input.id;
        // the sheet as its parent, so a pick counts as inside it and doesn't close it
        QMenu menu(this);
        Q_FOREACH (const KisPaintOpOption::PenInput &input, all) {
            if (shown.contains(input.id)) continue;
            QAction *action = menu.addAction(KisStudioPenPopup::inputName(input.id, input.name));
            action->setData(input.id);
            action->setToolTip(KisStudioPenPopup::inputTip(input.id));
        }
        if (menu.isEmpty()) return;
        if (QAction *picked = menu.exec(at)) {
            m_extraColumns << picked->data().toString();
            KSharedConfig::openConfig()->group(QStringLiteral("StudioBrushSheet")).writeEntry("PenColumns", m_extraColumns);
            refresh();
        }
    }

    void addRow(const QPoint &at)
    {
        const QList<KisPaintOpOption *> shown = rows();
        // the sheet as its parent, so a pick counts as inside it and doesn't close it
        QMenu menu(this);
        Q_FOREACH (KisPaintOpOption *option, candidates()) {
            if (shown.contains(option)) continue;
            menu.addAction(rowName(option))->setData(option->id());
        }
        if (menu.isEmpty()) return;
        if (QAction *picked = menu.exec(at)) {
            m_extraRows << picked->data().toString();
            refresh();
            if (QWidget *sheet = window()) sheet->adjustSize();
        }
    }

    std::function<QList<KisPaintOpOption *>()> m_options;
    QStringList m_extraColumns;
    QStringList m_extraRows;
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

/// The ground the brush's stroke is shown on, a tile as the rows are
class StrokeTile : public QWidget
{
public:
    using QWidget::QWidget;

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        QColor ground = palette().color(QPalette::WindowText);
        ground.setAlphaF(0.05);
        painter.fillPath(KisModernStyle::squircle(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), KisModernStyle::tileRadius), ground);
    }
};

/// A part's column, asking to be as tall as its contents are at its width: chips that wrap take more rows
class Column : public QWidget
{
public:
    using QWidget::QWidget;

    QSize sizeHint() const override
    {
        return fitted(QWidget::sizeHint());
    }

    QSize minimumSizeHint() const override
    {
        return fitted(QWidget::minimumSizeHint());
    }

private:
    QSize fitted(QSize size) const
    {
        if (layout() && layout()->hasHeightForWidth()) size.setHeight(qMax(size.height(), layout()->totalHeightForWidth(minimumWidth())));
        return size;
    }
};

} // namespace

struct KisStudioBrushSheet::Private
{
    QPointer<KisMainWindow> mainWindow;
    QPointer<KisPaintopBox> box;
    QPointer<KisPaintOpSettingsWidget> settings;
    /// The brush's settings are in use (useOptionWidget()), from shown to closed
    bool usingSettings {false};

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

    /// The brush's blend mode, as the top bar has it, and its eraser mode
    QWidget *blendRow(QWidget *parent)
    {
        KisViewManager *view = mainWindow ? mainWindow->viewManager() : nullptr;
        KisCanvasResourceProvider *resources = view ? view->canvasResourceProvider() : nullptr;
        if (!resources) return nullptr;
        auto *row = new QWidget(parent);
        auto *layout = new QHBoxLayout(row);
        layout->setContentsMargins(KisModernStyle::space2, 0, 0, 0);
        layout->setSpacing(KisModernStyle::hairline);
        row->setToolTip(i18nc("brush setting", "Blend mode"));
        // the mode at a glance: a stroke of the color in it, over the image
        auto *glance = new QLabel(row);
        const qreal ratio = row->devicePixelRatioF();
        const QSize glanceSize(36, 22);
        glance->setFixedSize(glanceSize);
        const KisBlendModePicker::Pictures pictures =
            KisBlendModePicker::brushPictures(view->image(), glanceSize * ratio, resources->fgColor().toQColor());
        auto showGlance = [glance, pictures, ratio, glanceSize](const QString &id) {
            QImage picture = KisBlendModePicker::modePicture(pictures, id);
            if (picture.isNull()) return glance->clear();
            QPixmap rounded(glanceSize * ratio);
            rounded.fill(Qt::transparent);
            QPainter painter(&rounded);
            painter.setRenderHint(QPainter::Antialiasing, true);
            painter.setClipPath(KisModernStyle::squircle(QRectF(QPointF(0, 0), QSizeF(glanceSize) * ratio), KisModernStyle::cornerRadius * ratio));
            painter.drawImage(0, 0, picture);
            painter.end();
            rounded.setDevicePixelRatio(ratio);
            glance->setPixmap(rounded);
        };
        showGlance(resources->currentCompositeOp());
        layout->addWidget(glance);
        auto *mode = new KisCompositeOpComboBox(row);
        mode->setFrame(false);
        mode->setMaximumWidth(150);
        if (KisNodeSP node = view->activeNode()) mode->validate(node->colorSpace());
        mode->selectCompositeOp(KoID(resources->currentCompositeOp()));
        QPointer<KisViewManager> owner(view);
        mode->setPreviewPictures([owner, mode, resources]() {
            if (!owner) return KisBlendModePicker::Pictures();
            return KisBlendModePicker::brushPictures(owner->image(), KisBlendModePicker::pictureSize(mode), resources->fgColor().toQColor());
        });
        QObject::connect(mode, QOverload<int>::of(&QComboBox::currentIndexChanged), mode, [mode, resources]() {
            resources->setCurrentCompositeOp(mode->selectedCompositeOp().id());
        });
        QObject::connect(resources->resourceManager(), &KoCanvasResourceProvider::canvasResourceChanged, mode, [mode, showGlance](int key, const QVariant &value) {
            if (key != KoCanvasResource::CurrentCompositeOp) return;
            QSignalBlocker blocker(mode);
            mode->selectCompositeOp(KoID(value.toString()));
            showGlance(value.toString());
        });
        layout->addWidget(mode);
        layout->addStretch(1);
        if (QAction *eraser = mainWindow->actionCollection()->action(QStringLiteral("erase_action"))) {
            auto *button = new QToolButton(row);
            button->setAutoRaise(true);
            button->setDefaultAction(eraser);
            button->setToolButtonStyle(Qt::ToolButtonIconOnly);
            layout->addWidget(button);
        }
        return row;
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
    d->engine = new MenuNameButton(this);
    d->engine->setAutoRaise(true);
    d->engine->setPopupMode(QToolButton::InstantPopup);
    d->engine->setToolTip(i18nc("@info:tooltip", "A new brush with another engine"));
    title->addWidget(d->engine, 0, Qt::AlignLeft);
    // a new brush of another engine: the menu is made once, as the engines don't change
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
            // after the menu is done: the new brush rebuilds the sheet
            QTimer::singleShot(0, this, [this, id]() {
                if (d->box) d->box->slotCreatePresetFromScratch(id);
            });
        });
    }
    d->engine->setMenu(engines);
    header->addLayout(title);
    auto *strokeTile = new StrokeTile(this);
    auto *strokeLayout = new QHBoxLayout(strokeTile);
    strokeLayout->setContentsMargins(KisModernStyle::space2, KisModernStyle::space1, KisModernStyle::space2, KisModernStyle::space1);
    d->preview = new KisPresetLivePreviewView(strokeTile);
    strokeLayout->addWidget(d->preview);
    d->preview->setFixedSize(444, 60);
    d->preview->setFrameShape(QFrame::NoFrame);
    {
        // the stroke on its tile: the preview paints over its window color
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
    header->addWidget(strokeTile);
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
    d->follow = iconButton(QStringLiteral("studio_restroke"), i18nc("@info:tooltip", "Redraw the last stroke while editing"));
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
        connect(pen, &QObject::destroyed, anchor, &QObject::deleteLater);
        pen->popupBeside(anchor, Qt::TopEdge);
        d->refreshGrid.start();
    };
    d->grid->onCellOff = [this](KisPaintOpOption *option, const QString &input) {
        if (d->pen && d->pen->option() == option) d->pen->close();
        option->setPenInputActive(input, false);
        d->refreshGrid.start();
    };
    layout->addWidget(d->grid);

    d->refreshGrid.setSingleShot(true);
    connect(&d->refreshGrid, &QTimer::timeout, this, [this]() {
        // the sheet only grows or shrinks when the grid gains or loses a row, not with each value dragged
        const int before = d->grid->minimumHeight();
        d->grid->refresh();
        if (d->grid->minimumHeight() != before) adjustSize();
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
    if (d->box && !d->usingSettings) {
        d->box->useOptionWidget(true);
        d->usingSettings = true;
    }
    KisStudioPopup::showEvent(event);
}

void KisStudioBrushSheet::closeEvent(QCloseEvent *event)
{
    if (d->box && d->usingSettings) {
        d->box->useOptionWidget(false);
        d->usingSettings = false;
    }
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
                                                   if (current && d->follow->isChecked() && KisLastStrokeReplay::instance()->follows(current->name())) {
                                                       KisLastStrokeReplay::instance()->brushChanged(current->name(), current->settings()->toXML());
                                                   }
                                               });
            d->presetConnections.addConnection(preset->updateProxy(), &KisPaintOpPresetUpdateProxy::sigSettingsChanged, this, [this]() {
                d->preview->requestUpdateStroke();
                updateHeader();
                d->refreshGrid.start();
            });
        }
    }

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

        auto *column = new Column(d->columns);
        column->setFixedWidth(columnWidth);
        auto *columnLayout = new QVBoxLayout(column);
        columnLayout->setContentsMargins(0, 0, 0, 0);
        columnLayout->setSpacing(KisModernStyle::space1);
        auto *title = new QLabel(KisStudioBrushEditor::partName(part), column);
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
                // shown whole above, or as the blend mode below
                if (option->quickPageIsWhole() && option->hasQuickPage(category)) continue;
                if (option->objectName() == QLatin1String("KisCompositeOpOption")) continue;
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

        if (part == KisPaintOpOption::GENERAL) {
            bool blends = false;
            Q_FOREACH (KisPaintOpOption *option, inPart) {
                if (option->objectName() == QLatin1String("KisCompositeOpOption")) blends = true;
            }
            if (QWidget *blend = blends ? d->blendRow(column) : nullptr) {
                columnLayout->addSpacing(KisModernStyle::space1);
                columnLayout->addWidget(blend);
                if (QWidget *mixing = KisStudioMixing::chooser(d->mainWindow, column)) columnLayout->addWidget(mixing);
            }
        }

        // the rest of a part, as Krita shows it
        if (!details.isEmpty()) {
            auto *more = new KisStudioChips(column);
            QVector<KisStudioChips::Chip> chips;
            QList<QPointer<KisPaintOpOption>> targets;
            Q_FOREACH (KisPaintOpOption *option, details) {
                KisStudioChips::Chip chip;
                chip.text = option->label() + QStringLiteral(" ›");
                chip.toolTip = KisStudioBrushEditor::optionTip(option);
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
    }
    Q_FOREACH (KisPaintOpOption *option, options) {
        connect(option, &KisPaintOpOption::sigCheckedChanged, &d->refreshGrid, qOverload<>(&QTimer::start), Qt::UniqueConnection);
        connect(option, &KisPaintOpOption::sigSettingChanged, &d->refreshGrid, qOverload<>(&QTimer::start), Qt::UniqueConnection);
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
