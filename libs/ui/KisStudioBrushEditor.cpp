/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioBrushEditor.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QCursor>
#include <QHelpEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QSet>
#include <QToolTip>

#include <algorithm>

#include <QCloseEvent>
#include <QHBoxLayout>
#include <QGuiApplication>
#include <QHash>
#include <QLabel>
#include <QPainter>
#include <QPointer>
#include <QScreen>
#include <QScrollArea>
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
#include <kconfiggroup.h>
#include <ksharedconfig.h>
#include <brushengine/kis_paintop_registry.h>
#include <kis_cubic_curve.h>

#include <KoCanvasController.h>
#include <KoToolManager.h>

#include "KisLastStrokeReplay.h"
#include "KisSettingsRegistry.h"
#include "KisStudioGlass.h"
#include "KisStudioTips.h"
#include "KisMainWindow.h"
#include "KisStudioBarButtons.h"
#include "KisStudioBrushPicker.h"
#include "KisStudioTools.h"
#include "KisStudioMixing.h"
#include "KisStudioPenPopup.h"
#include "KisStudioSmoothing.h"
#include "KisViewManager.h"
#include "kis_canvas_resource_provider.h"
#include <QPainterPath>
#include "widgets/KisBlendModePicker.h"
#include "widgets/kis_cmb_composite.h"
#include "kis_node.h"
#include <KoID.h>
#include <KoCanvasResourcesIds.h>
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
constexpr int strokePart = KisStudioBrushEditor::strokePart;
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
        return i18nc("@info:tooltip brush part", "A second tip cut into the first, for bristles or chalk");
    case KisPaintOpOption::TEXTURE:
        return i18nc("@info:tooltip brush part", "A paper grain the paint catches on");
    case KisPaintOpOption::COLOR:
        return i18nc("@info:tooltip brush part", "Where the color comes from and how it builds up");
    case KisPaintOpOption::FILTER:
        return i18nc("@info:tooltip brush part", "Paints with an effect, like blur or sharpen");
    case KisStudioBrushEditor::penPart:
        return i18nc("@info:tooltip brush part", "What pressure, tilt and speed change");
    case strokePart:
        return i18nc("@info:tooltip brush part", "Smoothing and shapes");
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

    /// The pen inputs shown: pressure, those any row uses and any added
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
        // pressure always, the rest behind "+" until used: an editor's width holds a few inputs by name
        QVector<KisPaintOpOption::PenInput> shown;
        Q_FOREACH (const KisPaintOpOption::PenInput &input, all) {
            if (KisStudioPenPopup::kritaInput(input.id) == QLatin1String("pressure") || used.contains(input.id) || m_extraColumns.contains(input.id)) {
                shown << input;
            }
        }
        return shown;
    }

    /// The grid gained or lost a row
    std::function<void()> onResized;

    void refresh()
    {
        m_labelWidth = -1;
        const int tall = header() + rowHeight * (rows().size() + 1);
        const bool resized = tall != minimumHeight();
        setFixedHeight(tall);
        update();
        if (resized && onResized) onResized();
    }

    QSize sizeHint() const override
    {
        return QSize(contentWidth, header() + rowHeight * (rows().size() + 1));
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
        m_cell = cell;

        painter.setFont(KisModernStyle::textFont(font(), 0.85));
        for (int c = 0; c < heads.size(); c++) {
            painter.setPen(quiet);
            const QRectF head(left + c * cell, 0, cell, header());
            const QString name = KisStudioPenPopup::inputName(heads[c].id, heads[c].name);
            painter.drawText(head.adjusted(2, 0, -2, -KisModernStyle::hairline), Qt::AlignHCenter | Qt::AlignBottom | Qt::TextWordWrap, name);
        }
        // more inputs and more settings, each a "+"
        drawPlus(painter, plusColumnRect(heads.size()), plusColumnRect(heads.size()).contains(m_hover));
        painter.setFont(font());
        for (int r = 0; r < lines.size(); r++) {
            KisPaintOpOption *option = lines[r];
            const qreal y = header() + r * rowHeight;
            painter.setPen(followsPen(option) ? ink : quiet);
            painter.drawText(QRectF(0, y, left - KisModernStyle::space2, rowHeight), Qt::AlignVCenter | Qt::AlignLeft,
                             painter.fontMetrics().elidedText(rowName(option), Qt::ElideRight, int(left) - KisModernStyle::space2));
            const QVector<KisPaintOpOption::PenInput> inputs = option->penInputs();
            for (int c = 0; c < heads.size(); c++) {
                const KisPaintOpOption::PenInput input = inputOf(inputs, heads[c].id);
                const QRectF box = cellRect(r, c, heads.size());
                const bool hovered = box.contains(m_hover);
                QColor ground = ink;
                ground.setAlphaF(input.active ? (hovered ? 0.18 : 0.10) : hovered ? 0.10 : 0.03);
                painter.fillPath(KisModernStyle::squircle(box, KisModernStyle::cornerRadius), ground);
                if (hovered) {
                    // pointed at: outlined, as a swatch is
                    QColor outline = ink;
                    outline.setAlphaF(0.35);
                    painter.setPen(QPen(outline, 1));
                    painter.setBrush(Qt::NoBrush);
                    painter.drawPath(KisModernStyle::squircle(box.adjusted(0.5, 0.5, -0.5, -0.5), KisModernStyle::cornerRadius));
                }
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
                if (KisStudioPenPopup::curvesAroundZero(input.id)) {
                    // where the curve leaves the setting as it is
                    painter.setPen(QPen(quiet, 1.0));
                    painter.drawLine(QPointF(glyph.left(), glyph.center().y()), QPointF(glyph.right(), glyph.center().y()));
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
        // painted again only when the pointer goes over another cell or "+", not at every move
        const QPointF at = m_hover - QPointF(labelWidth(), header());
        const QPoint place(at.x() < 0 ? -1 : int(at.x() / qMax(1.0, m_cell)), at.y() < 0 ? -1 : int(at.y() / rowHeight));
        if (place != m_hoverPlace) {
            m_hoverPlace = place;
            update();
        }
        setCursor(clickableAt(event->pos()) ? Qt::PointingHandCursor : Qt::ArrowCursor);
    }

    void leaveEvent(QEvent *) override
    {
        m_hover = QPointF(-1, -1);
        m_hoverPlace = QPoint(-2, -2);
        update();
    }

    void changeEvent(QEvent *event) override
    {
        if (event->type() == QEvent::FontChange) m_labelWidth = -1;
        QWidget::changeEvent(event);
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
    /// The inputs' names, on two lines when one doesn't fit its column
    int header() const
    {
        const QFontMetrics metrics(KisModernStyle::textFont(font(), 0.85));
        const QVector<KisPaintOpOption::PenInput> heads = columns();
        const qreal cell = cellWidth(heads.size());
        int lines = 1;
        Q_FOREACH (const KisPaintOpOption::PenInput &input, heads) {
            if (metrics.horizontalAdvance(KisStudioPenPopup::inputName(input.id, input.name)) > cell - 4) lines = 2;
        }
        return lines * metrics.lineSpacing() + KisModernStyle::space2;
    }
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

    /// Measured once until the rows change: every cell's place asks for it
    qreal labelWidth() const
    {
        if (m_labelWidth < 0) {
            int widest = 0;
            Q_FOREACH (KisPaintOpOption *option, candidates()) widest = qMax(widest, fontMetrics().horizontalAdvance(rowName(option)));
            m_labelWidth = qBound(90, widest + KisModernStyle::space3, 170);
        }
        return m_labelWidth;
    }

    qreal cellWidth(int count) const
    {
        return qMin(84.0, (width() - labelWidth() - rowHeight) / qreal(qMax(1, count)));
    }

    QRectF cellRect(int row, int column, int count) const
    {
        const qreal cell = cellWidth(count);
        return QRectF(labelWidth() + column * cell, header() + row * rowHeight, cell, rowHeight).adjusted(1.5, 1.5, -1.5, -1.5);
    }

    QRectF plusColumnRect(int count) const
    {
        return QRectF(labelWidth() + count * cellWidth(count), 0, rowHeight, header());
    }

    QRectF plusRowRect(int count) const
    {
        return QRectF(0, header() + count * rowHeight, labelWidth(), rowHeight);
    }

    /// A cell or a "+": what a click does something to
    bool clickableAt(const QPoint &pos) const
    {
        const QVector<KisPaintOpOption::PenInput> heads = columns();
        const int count = rows().size();
        if (heads.isEmpty()) return false;
        if (plusColumnRect(heads.size()).contains(pos) || plusRowRect(count).contains(pos)) return true;
        for (int r = 0; r < count; r++) {
            for (int c = 0; c < heads.size(); c++) {
                if (cellRect(r, c, heads.size()).contains(pos)) return true;
            }
        }
        return false;
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
            if (QRectF(labelWidth() + c * cellWidth(heads.size()), 0, cellWidth(heads.size()), header()).contains(pos)) {
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
        // the grid as its parent, so a pick counts as inside the editor and doesn't close it
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
            KisSettingsRegistry::instance()->scheduleSync();
            refresh();
        }
    }

    void addRow(const QPoint &at)
    {
        const QList<KisPaintOpOption *> shown = rows();
        // the grid as its parent, so a pick counts as inside the editor and doesn't close it
        QMenu menu(this);
        Q_FOREACH (KisPaintOpOption *option, candidates()) {
            if (shown.contains(option)) continue;
            menu.addAction(rowName(option))->setData(option->id());
        }
        if (menu.isEmpty()) return;
        if (QAction *picked = menu.exec(at)) {
            m_extraRows << picked->data().toString();
            refresh();
        }
    }

    std::function<QList<KisPaintOpOption *>()> m_options;
    QStringList m_extraColumns;
    QStringList m_extraRows;
    QPointF m_hover {-1, -1};
    QPoint m_hoverPlace {-2, -2};
    mutable qreal m_labelWidth {-1};
    qreal m_cell {1};
};

/// A setting's full page, as Krita shows it, lent by Krita's editor while this is open
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
    /// The page scrolls when a brush has more rows than the screen is tall (MyPaint brushes have dozens)
    QScrollArea *pageScroll {nullptr};

    QVector<int> parts;
    int part {KisPaintOpOption::GENERAL};
    QPointer<KisStudioPenPopup> pen;
    /// A setting's own page, from its chip
    QPointer<KisStudioPopup> details;
    QPointer<PenGrid> grid;
    QTimer refreshGrid;
    /// What is done with the brush: left out of its picture
    QList<QWidget *> actions;
    /// The brush's settings are in use (useOptionWidget()), from shown to closed
    bool usingSettings {false};
    /// The settings to add are shown, until another part is picked
    bool adding {false};
    QTimer refreshPage;
    /// Settings at their defaults the user added as rows while the editor is open
    QSet<KisPaintOpOption *> added;
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

    /// A part that is off (no mask, no texture): it keeps only its pictures, to turn it on
    bool isOff(int part) const
    {
        Q_FOREACH (KisPaintOpOption *option, options()) {
            if (option->category() == part && option->hasQuickPage(KisPaintOpOption::PaintopCategory(part))
                && option->isCheckable() && !option->isChecked()) {
                return true;
            }
        }
        return false;
    }

    /// The options of \p part in use, the size first, and the ones that could be
    void classify(int part, QList<KisPaintOpOption *> *rows, QList<KisPaintOpOption *> *unused) const
    {
        if (isOff(part)) return;
        Q_FOREACH (KisPaintOpOption *option, options()) {
            if (option->category() != part || !option->isEnabled()) continue;
            const std::optional<KisPaintOpOption::RowValue> value = option->rowValue();
            if (!value) continue;
            // at its engine's default and not driven by the pen: nothing the brush does with it, offered under Add
            const bool idle = value->neutral && qAbs(value->value - *value->neutral) <= 1e-4 * qMax<qreal>(1, qAbs(*value->neutral))
                && penCurve(option).isEmpty() && !added.contains(option);
            if (idle) {
                unused->append(option);
            } else if (value->appliesUnchecked || !option->isCheckable() || option->isChecked()) {
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
    case penPart:
        return i18nc("brush part: which pen input drives which setting", "Pen");
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
        {QStringLiteral("Flow"), i18nc("@info:tooltip brush setting", "Low flow builds color up gradually")},
        {QStringLiteral("Spacing"), i18nc("@info:tooltip brush setting", "Wide spacing turns the line into dots")},
        {QStringLiteral("Ratio"), i18nc("@info:tooltip brush setting", "Squashes the tip flat")},
        {QStringLiteral("Rotation"), i18nc("@info:tooltip brush setting", "Turns the tip as you draw")},
        {QStringLiteral("Mirror"), i18nc("@info:tooltip brush setting", "Flips dabs as you draw")},
        {QStringLiteral("Sharpness"), i18nc("@info:tooltip brush setting", "Snaps edges to whole pixels, for pixel art")},
        {QStringLiteral("Scatter"), i18nc("@info:tooltip brush setting", "Throws dabs off the line")},
        {QStringLiteral("Mix"), i18nc("@info:tooltip brush setting", "Blends toward the background color as you draw")},
        {QStringLiteral("h"), i18nc("@info:tooltip brush setting", "Varies the hue, usually at random per dab")},
        {QStringLiteral("s"), i18nc("@info:tooltip brush setting", "Varies the saturation, usually at random per dab")},
        {QStringLiteral("v"), i18nc("@info:tooltip brush setting", "Varies the lightness, usually at random per dab")},
        {QStringLiteral("Rate"), i18nc("@info:tooltip brush setting", "How fast the airbrush sprays while the pen holds still")},
        {QStringLiteral("LightnessStrength"), i18nc("@info:tooltip brush setting", "The tip's lights and darks shade the paint")},
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

KisStudioValueRow *KisStudioBrushEditor::optionRow(KisMainWindow *mainWindow, KisPaintOpOption *option, KisPaintOpSettingsWidget *settings,
                                                   QWidget *host, QPointer<KisStudioPenPopup> &pen,
                                                   const std::function<void(KisStudioPenPopup *, KisStudioValueRow *)> &place, bool removable,
                                                   QWidget *parent)
{
    auto *row = new KisStudioValueRow(option->label(), parent);
    bindRow(row, option, settings);
    const QPointer<KisPaintOpOption> target(option);
    const QPointer<KisMainWindow> window(mainWindow);
    // pinned to the top bar by its pin, or pulled up onto it
    const QString key = pinKey(option);
    auto showPinned = [row, window, key]() {
        row->setPinnable(true, window && KisStudioBarButtons::has(window, key));
    };
    showPinned();
    if (KisStudioBarButtons *bar = KisStudioBarButtons::of(mainWindow)) {
        QObject::connect(bar, &KisStudioBarButtons::sigItemsChanged, row, showPinned);
    }
    QObject::connect(row, &KisStudioValueRow::pinClicked, row, [window, key]() {
        if (window) KisStudioBarButtons::toggle(window, key);
    });
    row->setRemovable(removable);
    QObject::connect(row, &KisStudioValueRow::removeClicked, row, [target]() {
        if (target) target->setChecked(false);
    });
    QObject::connect(row, &KisStudioValueRow::pulledOut, row, [host, row, key, target, removable]() {
        const Qt::DropAction done = KisStudioBarButtons::dragItem(key, row);
        // dropped on the bar: pinned; let go over the editor, or the drag cancelled: nothing
        if (done != Qt::IgnoreAction || !removable || !target || KisModernStyle::pointerHeld()) return;
        if (host->frameGeometry().contains(QCursor::pos())) return;
        target->setChecked(false);
    });
    QObject::connect(row, &KisStudioValueRow::typingStarted, row, [&pen, target]() {
        if (pen && pen->option() == target) pen->close();
    });
    QObject::connect(row, &KisStudioValueRow::clicked, row, [host, &pen, place, row, target]() {
        if (!target || target->penInputs().isEmpty()) return;
        const bool same = pen && pen->option() == target;
        if (pen) pen->close();
        if (same) return;
        auto *popup = new KisStudioPenPopup(target, host);
        pen = popup;
        row->setSelected(true);
        QObject::connect(popup, &QObject::destroyed, row, [row]() { row->setSelected(false); });
        place(popup, row);
    });
    return row;
}

bool KisStudioBrushEditor::blends(KisMainWindow *mainWindow, const QList<KisPaintOpOption *> &options)
{
    // MyPaint brushes always paint as Normal (or erase): the brush box turns their blend mode off
    KisViewManager *view = mainWindow ? mainWindow->viewManager() : nullptr;
    const KisPaintOpPresetSP preset = view && view->canvasResourceProvider() ? view->canvasResourceProvider()->currentPreset() : KisPaintOpPresetSP();
    if (preset && preset->paintOp().id() == QLatin1String("mypaintbrush")) return false;
    Q_FOREACH (KisPaintOpOption *option, options) {
        if (option->objectName() == QLatin1String("KisCompositeOpOption")) return true;
    }
    return false;
}

QWidget *KisStudioBrushEditor::blendRow(KisMainWindow *mainWindow, QWidget *parent)
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
    mode->setForBrush(true);
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
    // what is done with the brush itself
    auto *more = new QToolButton(this);
    more->setAutoRaise(true);
    more->setIcon(KisIconUtils::loadIcon(QStringLiteral("studio_more")));
    more->setPopupMode(QToolButton::InstantPopup);
    auto *moreMenu = new QMenu(more);
    moreMenu->addAction(i18nc("@action save the brush over itself", "Save"), this, [this]() {
        if (d->box) d->box->savePreset(false);
    });
    moreMenu->addAction(i18nc("@action", "Save as new"), this, [this]() {
        if (d->box) d->box->savePreset(true);
    });
    // a new brush of another engine: made once, as the engines don't change
    QMenu *engines = moreMenu->addMenu(i18nc("@action a new brush of the engine picked", "New brush"));
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
            // after the menu is done: the new brush rebuilds the editor
            QTimer::singleShot(0, this, [this, id]() {
                if (d->box) d->box->slotCreatePresetFromScratch(id);
            });
        });
    }
    moreMenu->addAction(i18nc("@action", "Copy as picture"), this, &KisStudioBrushEditor::copyPicture);
    moreMenu->addSeparator();
    // Krita's own editor, for what only it has (the scratchpad, renaming, Instant Preview)
    moreMenu->addAction(i18nc("@action Krita's own brush editor", "Classic editor"), this, [this]() {
        QPointer<KisPaintopBox> box = d->box;
        close();
        if (box) box->showFullEditor();
    });
    more->setMenu(moreMenu);
    header->addWidget(more);
    d->actions = {d->reload, d->follow, d->pin, more};
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
    d->pageScroll = new QScrollArea(this);
    d->pageScroll->setFrameShape(QFrame::NoFrame);
    d->pageScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    d->pageScroll->setWidgetResizable(true);
    d->pageScroll->viewport()->setAutoFillBackground(false);
    d->pageScroll->setWidget(d->page);
    // after setWidget, which fills it: the page is glass like the rest
    d->page->setAutoFillBackground(false);
    layout->addWidget(d->pageScroll);

    setFixedWidth(contentWidth + 2 * (shadowMargin() + KisModernStyle::space3));

    d->refreshPage.setSingleShot(true);
    connect(&d->refreshPage, &QTimer::timeout, this, [this]() {
        QList<KisPaintOpOption *> rows;
        QList<KisPaintOpOption *> unused;
        d->classify(d->part, &rows, &unused);
        if (rows != d->rowOptions || unused != d->unusedOptions) showPart(d->part);
    });

    d->refreshGrid.setSingleShot(true);
    connect(&d->refreshGrid, &QTimer::timeout, this, [this]() {
        if (d->grid) d->grid->refresh();
    });

    if (d->box) {
        connect(d->box, &KisPaintopBox::sigOptionWidgetChanged, this, &KisStudioBrushEditor::rebuild);
    }
    KisLastStrokeReplay::instance()->watch(this);
    // the last stroke follows the smoothing (the Stroke tab, the chip) as it does the brush
    connect(KisStudioSmoothing::instance(), &KisStudioSmoothing::sigChanged, this, &KisStudioBrushEditor::followLastStroke);
    // the Stroke tab is there for the tools that paint freehand
    if (KoToolManager *tools = KisStudioTools::manager(mainWindow)) {
        connect(tools, &KoToolManager::changedTool, this, [this]() {
            if (d->parts.contains(strokePart) != KisStudioStrokePage::applies(d->mainWindow)) rebuild();
        });
    }
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
    if (d->details) d->details->close();
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
        // the blend mode and the mixing are the Color part's, with or without other color settings
        if (part == KisPaintOpOption::COLOR && blends(d->mainWindow, options)) has = true;
        if (has) d->parts << part;
    }
    // the pen's tab once there is a setting it can drive
    Q_FOREACH (KisPaintOpOption *option, options) {
        if (!option->penInputs().isEmpty()) {
            d->parts << penPart;
            break;
        }
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
    if (d->details) d->details->close();
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
        connect(stroke, &KisStudioStrokePage::sigResized, this, [this]() { QTimer::singleShot(0, this, [this]() { fitPage(); }); });
        d->pageLayout->addWidget(stroke);
        d->rowOptions.clear();
        d->unusedOptions.clear();
        fitPage();
        return;
    }

    if (part == penPart) {
        auto *grid = new PenGrid([this]() { return d->options(); }, d->page);
        d->grid = grid;
        grid->onCell = [this](KisPaintOpOption *option, const QString &input, QWidget *anchor) {
            if (d->pen) d->pen->close();
            bool wasOn = false;
            Q_FOREACH (const KisPaintOpOption::PenInput &each, option->penInputs()) {
                if (each.id == input) wasOn = each.active;
            }
            if (!wasOn) option->setPenInputActive(input, true);
            auto *pen = new KisStudioPenPopup(option, this, input);
            d->pen = pen;
            connect(pen, &QObject::destroyed, anchor, &QObject::deleteLater);
            // beside the editor, level with the cell, as a row's is
            const QPoint at(mapToGlobal(QPoint(width() - shadowMargin(), 0)).x() + KisModernStyle::space2,
                            anchor->mapToGlobal(QPoint(0, 0)).y() - 2 * KisModernStyle::space4);
            pen->popupAt(at, anchor);
            d->refreshGrid.start();
        };
        grid->onCellOff = [this](KisPaintOpOption *option, const QString &input) {
            if (d->pen && d->pen->option() == option) d->pen->close();
            option->setPenInputActive(input, false);
            d->refreshGrid.start();
        };
        grid->onResized = [this]() { QTimer::singleShot(0, this, [this]() { fitPage(); }); };
        Q_FOREACH (KisPaintOpOption *option, d->options()) {
            connect(option, &KisPaintOpOption::sigCheckedChanged, &d->refreshGrid, qOverload<>(&QTimer::start), Qt::UniqueConnection);
            connect(option, &KisPaintOpOption::sigSettingChanged, &d->refreshGrid, qOverload<>(&QTimer::start), Qt::UniqueConnection);
        }
        d->pageLayout->addWidget(grid);
        grid->refresh();
        d->rowOptions.clear();
        d->unusedOptions.clear();
        fitPage();
        return;
    }

    const QList<KisPaintOpOption *> options = d->options();
    const auto category = KisPaintOpOption::PaintopCategory(part);

    // how its paint goes onto the layer and mixes with paint: the blend mode, then the mixing
    if (category == KisPaintOpOption::COLOR) {
        if (QWidget *blend = blends(d->mainWindow, options) ? blendRow(d->mainWindow, d->page) : nullptr) d->pageLayout->addWidget(blend);
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
        // an effect the brush can go without (Rotation, Scatter) comes off by its cross, or pulled off the editor
        const std::optional<KisPaintOpOption::RowValue> rowValue = option->rowValue();
        const bool removable = option->isCheckable() && rowValue && !rowValue->appliesUnchecked;
        auto *row = optionRow(d->mainWindow, option, d->settings, this, d->pen, [this](KisStudioPenPopup *pen, KisStudioValueRow *row) {
            // beside the editor, level with the row
            const QPoint at(mapToGlobal(QPoint(width() - shadowMargin(), 0)).x() + KisModernStyle::space2,
                            row->mapToGlobal(QPoint(0, 0)).y() - 2 * KisModernStyle::space4);
            pen->popupAt(at, row);
        }, removable, d->page);
        d->pageLayout->addWidget(row);
    }

    // the rest of the part, as Krita shows it
    QList<QPointer<KisPaintOpOption>> pages;
    if (!d->isOff(part)) {
        Q_FOREACH (KisPaintOpOption *option, options) {
            if (option->category() != part || option->rowValue()) continue;
            // shown whole above, or as the blend mode
            if (option->quickPageIsWhole() && option->hasQuickPage(category)) continue;
            if (option->objectName() == QLatin1String("KisCompositeOpOption")) continue;
            pages << option;
        }
    }
    if (!pages.isEmpty()) {
        auto *chips = new KisStudioChips(d->page);
        QVector<KisStudioChips::Chip> named;
        Q_FOREACH (KisPaintOpOption *option, pages) {
            KisStudioChips::Chip chip;
            chip.text = option->label() + QStringLiteral(" ›");
            chip.toolTip = optionTip(option);
            named << chip;
        }
        chips->setChips(named);
        connect(chips, &KisStudioChips::clicked, this, [this, chips, pages](int index) {
            if (index < 0 || index >= pages.size() || !pages[index] || !d->settings) return;
            if (d->details) d->details->close();
            KisPaintOpOption *option = pages[index];
            QWidget *page = d->settings->lendPage(option);
            if (!page) return;
            auto *popup = new LentPage(d->settings, option, page, this);
            d->details = popup;
            popup->popupBeside(chips, Qt::RightEdge);
        });
        d->pageLayout->addSpacing(KisModernStyle::space1);
        d->pageLayout->addWidget(chips);
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
        connect(more, &KisStudioChips::clicked, this, [this, targets](int index) {
            if (index < 0 || index >= targets.size() || !targets[index]) return;
            // one that is always on (a MyPaint setting at its default) is shown as a row to change
            if (targets[index]->isCheckable()) {
                targets[index]->setChecked(true);
            } else {
                d->added.insert(targets[index]);
                d->refreshPage.start();
            }
        });
        d->pageLayout->addSpacing(KisModernStyle::space1);
        d->pageLayout->addWidget(more);
    }

    fitPage();
}

void KisStudioBrushEditor::fitPage()
{
    // the window fits the new page, smaller or larger, and no taller than the room beside its button: the page scrolls past that
    // What a shown layout was just given shows only a moment later, and until then it isn't measured: shown now
    for (int i = 0; i < d->pageLayout->count(); i++) {
        QWidget *widget = d->pageLayout->itemAt(i)->widget();
        if (widget && widget->isHidden() && !widget->testAttribute(Qt::WA_WState_ExplicitShowHide)) widget->show();
    }
    d->pageLayout->activate();
    // at its width: rows of chips wrap
    const int pageWidth = d->pageScroll->viewport()->width() > 0 ? d->pageScroll->viewport()->width() : d->page->sizeHint().width();
    const int wanted = d->page->hasHeightForWidth() ? d->page->heightForWidth(pageWidth) : d->page->sizeHint().height();
    d->pageScroll->setFixedHeight(wanted);
    layout()->activate();
    const int room = heightRoom() - KisModernStyle::space4;
    // at the window's width, as the tabs wrap there: the size hint counts them on one line
    const int tall = layout()->hasHeightForWidth() ? layout()->totalHeightForWidth(width()) : sizeHint().height();
    const int over = tall - room;
    if (over > 0) {
        d->pageScroll->setFixedHeight(qMax(qMin(wanted, 120), wanted - over));
        layout()->activate();
    }
    adjustSize();
    if (isVisible()) keepOnScreen();
    QTimer::singleShot(0, this, [this]() {
        adjustSize();
        if (isVisible()) keepOnScreen();
    });
}

void KisStudioBrushEditor::copyPicture()
{
    // the editor as shown, on its own color rather than on what is behind the glass
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

#include "KisStudioBrushEditor.moc"
