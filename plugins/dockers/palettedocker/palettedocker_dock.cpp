/*
 *  SPDX-FileCopyrightText: 2013 Sven Langkamp <sven.langkamp@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */


#include "palettedocker_dock.h"

#include <QApplication>
#include <QDrag>
#include <QDragEnterEvent>
#include <QFile>
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QTimer>
#include <QToolButton>
#include <QToolTip>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <functional>

#include <kundo2magicstring.h>
#include <kundo2stack.h>
#include <klocalizedstring.h>
#include <KoResourceServerProvider.h>
#include <KisResourceLocator.h>
#include <KoFileDialog.h>
#include <kis_floating_message.h>
#include <kis_icon.h>
#include <kis_config.h>
#include <kis_canvas_resource_provider.h>
#include <kis_node_manager.h>
#include <KisMainWindow.h>
#include <KisViewManager.h>
#include <kis_display_color_converter.h>
#include <kis_canvas2.h>
#include <KisDocument.h>
#include <KisPopupButton.h>
#include <KisResourceModel.h>
#include <KisStorageModel.h>
#include <KisStudioItemCard.h>

#include <KisPaletteModel.h>
#include <KisPaletteChooser.h>
#include <KisPaletteEditor.h>
#include <dialogs/KisDlgPaletteEditor.h>

namespace {

const QString swatchMimeType = QStringLiteral("application/x-krita-palette-swatch");

bool isOccupied(const QModelIndex &index)
{
    return index.isValid() && index.data(KisPaletteModel::CheckSlotRole).toBool()
           && !index.data(KisPaletteModel::IsGroupNameRole).toBool();
}

QColor swatchColor(const QModelIndex &index)
{
    return index.data(Qt::BackgroundRole).value<QBrush>().color();
}

} // namespace

/**
 * The swatches: the palette's columns across the panel's width, square, with
 * group names between the groups. See PaletteDockerDock for what it does.
 */
class PaletteGrid : public QWidget
{
public:
    std::function<void(const QModelIndex &)> picked;
    /// The "+": an empty slot to put the color in, or invalid for a new row
    std::function<void(const QModelIndex &emptySlot)> addRequested;
    std::function<void(const QModelIndex &, QWidget *anchor)> cardRequested;
    std::function<void(const QModelIndex &from, const QModelIndex &to)> moved;
    std::function<void(const QModelIndex &)> draggedOff;
    std::function<bool()> editable;

    PaletteGrid(KisPaletteModel *model, QWidget *parent)
        : QWidget(parent)
        , m_model(model)
    {
        setMouseTracking(true);
        setAcceptDrops(true);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

        // the palette changes underneath: added, removed, moved, another one loaded
        auto changed = [this]() { relayout(); };
        QObject::connect(model, &QAbstractItemModel::dataChanged, this, changed);
        QObject::connect(model, &QAbstractItemModel::modelReset, this, changed);
        QObject::connect(model, &QAbstractItemModel::layoutChanged, this, changed);
        QObject::connect(model, &QAbstractItemModel::rowsInserted, this, changed);
        QObject::connect(model, &QAbstractItemModel::rowsRemoved, this, changed);
        QObject::connect(model, &QAbstractItemModel::columnsInserted, this, changed);
        QObject::connect(model, &QAbstractItemModel::columnsRemoved, this, changed);
    }

    void setCurrentColor(const QColor &color)
    {
        m_current = color.rgb();
        update();
    }

    void relayout()
    {
        m_cells.clear();
        m_headers.clear();
        m_plus = QRect();
        m_plusSlot = QPersistentModelIndex();
        m_contentHeight = 0;

        const int columns = m_model->colorSet() ? m_model->columnCount() : 0;
        if (columns <= 0) {
            update();
            return;
        }
        // as wide as the panel; very few columns would make huge swatches
        m_cell = qBound(12, width() / columns, 36);
        const int left = (width() - columns * m_cell) / 2;
        const int headerHeight = fontMetrics().height() + 6;
        const bool canAdd = editable && editable();

        int y = 0;
        bool inGlobalGroup = true;
        // the "+" goes after the last swatch of the first group, in a row of its own if it is full
        auto endGlobalGroup = [&]() {
            inGlobalGroup = false;
            if (!canAdd) return;
            int last = -1;
            for (int i = 0; i < m_cells.size(); i++) {
                if (isOccupied(m_cells[i].index)) last = i;
            }
            if (last + 1 < m_cells.size()) {
                m_plus = m_cells[last + 1].rect;
                m_plusSlot = m_cells[last + 1].index;
            } else {
                m_plus = QRect(left, y, m_cell, m_cell);
                y += m_cell;
            }
        };

        for (int row = 0; row < m_model->rowCount(); row++) {
            const QModelIndex first = m_model->index(row, 0);
            if (first.data(KisPaletteModel::IsGroupNameRole).toBool()) {
                if (inGlobalGroup) endGlobalGroup();
                m_headers.append({first.data(KisPaletteModel::GroupNameRole).toString(), QRect(left, y, columns * m_cell, headerHeight)});
                y += headerHeight;
                continue;
            }
            for (int column = 0; column < columns; column++) {
                m_cells.append({QPersistentModelIndex(m_model->index(row, column)), QRect(left + column * m_cell, y, m_cell, m_cell)});
            }
            y += m_cell;
        }
        if (inGlobalGroup) endGlobalGroup();

        m_contentHeight = y;
        setScroll(m_scroll);
        updateGeometry();
        update();
    }

    QSize sizeHint() const override
    {
        return QSize(240, qBound(m_cell, m_contentHeight, 4 * m_cell));
    }

    QSize minimumSizeHint() const override
    {
        // one row: the panel can be made that short
        return QSize(60, 14);
    }

protected:
    void resizeEvent(QResizeEvent *) override
    {
        relayout();
    }

    bool event(QEvent *event) override
    {
        if (event->type() == QEvent::ToolTip) {
            auto *help = static_cast<QHelpEvent *>(event);
            const QPoint pos = help->pos() + QPoint(0, m_scroll);
            QString text;
            if (m_plus.contains(pos)) {
                text = i18nc("@info:tooltip", "Add the current color");
            } else {
                const QModelIndex index = indexAt(help->pos());
                if (isOccupied(index)) {
                    const KisSwatch swatch = m_model->getSwatch(index);
                    text = swatch.name().isEmpty() ? swatchColor(index).name() : swatch.name();
                }
            }
            if (text.isEmpty()) QToolTip::hideText();
            else QToolTip::showText(help->globalPos(), text, this);
            return true;
        }
        return QWidget::event(event);
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.translate(0, -m_scroll);
        const QColor ink = palette().color(QPalette::WindowText);

        QColor headerInk = ink;
        headerInk.setAlphaF(0.55);
        painter.setPen(headerInk);
        Q_FOREACH (const Header &header, m_headers) {
            painter.drawText(header.rect.adjusted(2, 2, 0, 0), Qt::AlignLeft | Qt::AlignVCenter,
                             fontMetrics().elidedText(header.name, Qt::ElideRight, header.rect.width() - 4));
        }

        QColor empty = ink;
        empty.setAlphaF(0.05);
        const qreal radius = qMin(4.0, m_cell / 5.0);
        Q_FOREACH (const Cell &cell, m_cells) {
            const QRectF rect = QRectF(cell.rect).adjusted(1, 1, -1, -1);
            if (!isOccupied(cell.index)) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(empty);
                painter.drawRoundedRect(rect, radius, radius);
                continue;
            }
            painter.setOpacity(m_dragging && cell.index == m_press ? 0.3 : 1.0);
            painter.setPen(Qt::NoPen);
            painter.setBrush(swatchColor(cell.index));
            painter.drawRoundedRect(rect, radius, radius);
            painter.setOpacity(1.0);
            // the color itself, not as the display shows it, as the current color is
            if (m_model->getSwatch(cell.index).color().toQColor().rgb() == m_current) {
                // the color painted with now: a ring that reads on any color
                painter.setBrush(Qt::NoBrush);
                painter.setPen(QPen(QColor(0, 0, 0, 170), 3));
                painter.drawRoundedRect(rect.adjusted(1.5, 1.5, -1.5, -1.5), radius, radius);
                painter.setPen(QPen(Qt::white, 1.5));
                painter.drawRoundedRect(rect.adjusted(1.5, 1.5, -1.5, -1.5), radius, radius);
            } else if (cell.index == m_hover && !m_dragging) {
                QColor outline = ink;
                outline.setAlphaF(0.6);
                painter.setBrush(Qt::NoBrush);
                painter.setPen(QPen(outline, 1));
                painter.drawRoundedRect(rect.adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
            }
        }

        if (!m_plus.isNull()) {
            QColor plusInk = ink;
            plusInk.setAlphaF(m_hoverPlus ? 0.9 : 0.45);
            QPen pen(plusInk, 1.5);
            pen.setCapStyle(Qt::RoundCap);
            painter.setPen(pen);
            const QPointF c = QRectF(m_plus).center();
            const qreal arm = qMin(5.0, m_cell * 0.25);
            painter.drawLine(c - QPointF(arm, 0), c + QPointF(arm, 0));
            painter.drawLine(c - QPointF(0, arm), c + QPointF(0, arm));
        }

        if (!m_dropRect.isNull()) {
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(palette().color(QPalette::Highlight), 2));
            painter.drawRoundedRect(QRectF(m_dropRect).adjusted(1, 1, -1, -1), radius, radius);
        }
    }

    void wheelEvent(QWheelEvent *event) override
    {
        const int steps = event->angleDelta().y() / 120;
        if (steps && m_contentHeight > height()) {
            // a row at a time, so that a short panel shows whole rows
            setScroll(m_scroll - steps * m_cell);
            event->accept();
            return;
        }
        event->ignore();
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        m_pressPos = event->pos();
        m_pressScroll = m_scroll;
        m_press = indexAt(event->pos());
        m_pressOnPlus = m_plus.contains(event->pos() + QPoint(0, m_scroll));
        m_scrolling = false;
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (!(event->buttons() & Qt::LeftButton)) {
            const QModelIndex hover = indexAt(event->pos());
            const bool hoverPlus = m_plus.contains(event->pos() + QPoint(0, m_scroll));
            if (QPersistentModelIndex(hover) != m_hover || hoverPlus != m_hoverPlus) {
                m_hover = hover;
                m_hoverPlus = hoverPlus;
                setCursor(isOccupied(hover) || hoverPlus ? Qt::PointingHandCursor : Qt::ArrowCursor);
                update();
            }
            return;
        }
        if (m_scrolling) {
            setScroll(m_pressScroll - (event->pos().y() - m_pressPos.y()));
            return;
        }
        if ((event->pos() - m_pressPos).manhattanLength() < QApplication::startDragDistance()) {
            return;
        }
        if (isOccupied(m_press) && editable && editable()) {
            startDrag();
        } else if (!m_pressOnPlus) {
            // from an empty place: the swatches follow the finger
            m_scrolling = true;
        }
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton || m_scrolling) {
            m_scrolling = false;
            return;
        }
        if (m_pressOnPlus && m_plus.contains(event->pos() + QPoint(0, m_scroll))) {
            if (addRequested) addRequested(m_plusSlot);
        } else if (isOccupied(m_press) && QPersistentModelIndex(indexAt(event->pos())) == m_press) {
            if (picked) picked(m_press);
        }
    }

    // a right click, or a finger or pen held on a swatch (KisTouchMode): its card
    void contextMenuEvent(QContextMenuEvent *event) override
    {
        // the card is the menu: not the main window's toolbar menu behind the panel
        event->accept();
        const QModelIndex index = indexAt(event->pos());
        if (isOccupied(index)) requestCard(index);
    }

    void leaveEvent(QEvent *) override
    {
        m_hover = QPersistentModelIndex();
        m_hoverPlus = false;
        update();
    }

    void dragEnterEvent(QDragEnterEvent *event) override
    {
        // only swatches of this palette, moved
        if (event->source() == this && event->mimeData()->hasFormat(swatchMimeType)) {
            event->acceptProposedAction();
        }
    }

    void dragMoveEvent(QDragMoveEvent *event) override
    {
        const QModelIndex target = indexAt(event->pos());
        const bool ok = target.isValid() && QPersistentModelIndex(target) != m_press;
        m_dropRect = ok ? cellRect(target) : QRect();
        update();
        if (ok) event->acceptProposedAction();
        else event->ignore();
    }

    void dragLeaveEvent(QDragLeaveEvent *) override
    {
        m_dropRect = QRect();
        update();
    }

    void dropEvent(QDropEvent *event) override
    {
        m_dropRect = QRect();
        const QModelIndex target = indexAt(event->pos());
        if (!target.isValid() || QPersistentModelIndex(target) == m_press) {
            event->ignore();
            return;
        }
        event->acceptProposedAction();
        const QModelIndex from = m_press;
        // the drag's own event loop is still running: change the palette after it
        QTimer::singleShot(0, this, [this, from = QPersistentModelIndex(from), to = QPersistentModelIndex(target)]() {
            if (moved && from.isValid() && to.isValid()) moved(from, to);
        });
    }

private:
    struct Cell {
        QPersistentModelIndex index;
        QRect rect; ///< in content coordinates, before scrolling
    };
    struct Header {
        QString name;
        QRect rect;
    };

    QModelIndex indexAt(const QPoint &pos) const
    {
        const QPoint content = pos + QPoint(0, m_scroll);
        Q_FOREACH (const Cell &cell, m_cells) {
            if (cell.rect.contains(content)) return cell.index;
        }
        return QModelIndex();
    }

    QRect cellRect(const QModelIndex &index) const
    {
        Q_FOREACH (const Cell &cell, m_cells) {
            if (cell.index == index) return cell.rect;
        }
        return QRect();
    }

    void setScroll(int scroll)
    {
        m_scroll = qBound(0, scroll, qMax(0, m_contentHeight - height()));
        update();
    }

    void requestCard(const QModelIndex &index)
    {
        if (!cardRequested) return;
        // the card goes beside the swatch
        auto *anchor = new QWidget(this);
        anchor->setGeometry(cellRect(index).translated(0, -m_scroll));
        cardRequested(index, anchor);
        anchor->deleteLater();
    }

    void startDrag()
    {
        const QPersistentModelIndex from = m_press;
        const qreal dpr = devicePixelRatioF();
        QPixmap pixmap(QSize(m_cell, m_cell) * dpr);
        pixmap.setDevicePixelRatio(dpr);
        pixmap.fill(Qt::transparent);
        {
            QPainter painter(&pixmap);
            painter.setRenderHint(QPainter::Antialiasing, true);
            painter.setPen(QPen(QColor(0, 0, 0, 90), 1));
            painter.setBrush(swatchColor(from));
            painter.drawRoundedRect(QRectF(0.5, 0.5, m_cell - 1, m_cell - 1), 4, 4);
        }
        auto *drag = new QDrag(this);
        auto *mime = new QMimeData();
        mime->setData(swatchMimeType, QByteArray::number(from.row()) + ',' + QByteArray::number(from.column()));
        drag->setMimeData(mime);
        drag->setPixmap(pixmap);
        drag->setHotSpot(QPoint(m_cell / 2, m_cell / 2));

        m_dragging = true;
        update();
        const Qt::DropAction result = drag->exec(Qt::MoveAction);
        m_dragging = false;
        m_dropRect = QRect();
        update();

        // let go outside the panel: taken out of the palette
        QWidget *panel = parentWidget() ? parentWidget() : this;
        if (result == Qt::IgnoreAction && !panel->rect().contains(panel->mapFromGlobal(QCursor::pos()))) {
            QTimer::singleShot(0, this, [this, from]() {
                if (draggedOff && from.isValid()) draggedOff(from);
            });
        }
    }

    KisPaletteModel *m_model;
    QVector<Cell> m_cells;
    QVector<Header> m_headers;
    QRect m_plus;
    QPersistentModelIndex m_plusSlot;
    int m_cell {20};
    int m_contentHeight {0};
    int m_scroll {0};
    QRgb m_current {0};

    QPersistentModelIndex m_hover;
    bool m_hoverPlus {false};
    QPersistentModelIndex m_press;
    QPoint m_pressPos;
    int m_pressScroll {0};
    bool m_pressOnPlus {false};
    bool m_scrolling {false};
    bool m_dragging {false};
    QRect m_dropRect;
};

PaletteDockerDock::PaletteDockerDock( )
    : QDockWidget(i18n("Palette"))
    , m_model(new KisPaletteModel(this))
    , m_paletteChooser(new KisPaletteChooser(this))
    , m_rServer(KoResourceServerProvider::instance()->paletteServer())
    , m_paletteEditor(new KisPaletteEditor)
{
    QWidget *mainWidget = new QWidget(this);
    setWidget(mainWidget);
    auto *layout = new QVBoxLayout(mainWidget);
    layout->setContentsMargins(6, 0, 6, 4);
    layout->setSpacing(2);

    // the name, which opens the other palettes; undo and redo while there is something to undo
    auto *header = new QHBoxLayout();
    header->setContentsMargins(0, 0, 0, 0);
    header->setSpacing(2);
    m_paletteButton = new KisPopupButton(mainWidget);
    m_paletteButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_paletteButton->setAutoRaise(true);
    m_paletteButton->setArrowVisible(false);
    m_paletteButton->setPopupWidget(m_paletteChooser);
    m_paletteButton->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_paletteButton, &QWidget::customContextMenuRequested, this, [this]() { showPaletteCard(); });
    header->addWidget(m_paletteButton);
    header->addStretch(1);
    auto smallButton = [mainWidget](const char *icon, const QString &toolTip) {
        auto *button = new QToolButton(mainWidget);
        button->setAutoRaise(true);
        button->setFocusPolicy(Qt::NoFocus);
        button->setIcon(KisIconUtils::loadIcon(QLatin1String(icon)));
        button->setIconSize(QSize(14, 14));
        button->setToolTip(toolTip);
        button->hide();
        return button;
    };
    m_undo = smallButton("edit-undo", i18nc("@info:tooltip", "Undo the last change to the palette"));
    m_redo = smallButton("edit-redo", i18nc("@info:tooltip", "Redo"));
    connect(m_undo, &QToolButton::clicked, this, [this]() {
        if (m_currentColorSet) m_currentColorSet->undoStack()->undo();
        updateHeader();
    });
    connect(m_redo, &QToolButton::clicked, this, [this]() {
        if (m_currentColorSet) m_currentColorSet->undoStack()->redo();
        updateHeader();
    });
    header->addWidget(m_undo);
    header->addWidget(m_redo);
    layout->addLayout(header);

    m_grid = new PaletteGrid(m_model, mainWidget);
    m_grid->editable = [this]() { return canEdit(); };
    m_grid->picked = [this](const QModelIndex &index) { pickSwatch(index); };
    m_grid->addRequested = [this](const QModelIndex &slot) { addCurrentColor(slot); };
    m_grid->moved = [this](const QModelIndex &from, const QModelIndex &to) { moveSwatch(from, to); };
    m_grid->draggedOff = [this](const QModelIndex &index) { removeSwatch(index); };
    m_grid->cardRequested = [this](const QModelIndex &index, QWidget *anchor) { showSwatchCard(index, anchor); };
    layout->addWidget(m_grid, 1);

    m_paletteEditor->setPaletteModel(m_model);
    connect(m_model, SIGNAL(sigPaletteModified()), SLOT(updateHeader()));
    connect(KisResourceLocator::instance(), SIGNAL(storageRemoved(QString)), this, SLOT(slotStoragesChanged(QString)));

    connect(m_paletteChooser, SIGNAL(sigPaletteSelected(KoColorSetSP)), SLOT(slotSetColorSet(KoColorSetSP)));
    connect(m_paletteChooser, SIGNAL(sigAddPalette()), SLOT(slotAddPalette()));
    connect(m_paletteChooser, SIGNAL(sigImportPalette()), SLOT(slotImportPalette()));
    connect(m_paletteChooser, SIGNAL(sigRemovePalette(KoColorSetSP)), SLOT(slotRemovePalette(KoColorSetSP)));
    connect(m_paletteChooser, SIGNAL(sigExportPalette(KoColorSetSP)), SLOT(slotExportPalette(KoColorSetSP)));

    KisConfig cfg(true);
    KoColorSetSP defaultPalette = m_rServer->resource("", "", cfg.defaultPalette());
    if (defaultPalette) {
        slotSetColorSet(defaultPalette);
        m_paletteChooser->setCurrentItem(defaultPalette);
    }
    updateHeader();

    m_rServer->addObserver(this);
}

PaletteDockerDock::~PaletteDockerDock()
{
    if (m_paletteEditor->isModified()) {
        m_paletteEditor->saveNewPaletteVersion();
    }
    KoResourceServer<KoColorSet> *srv = KoResourceServerProvider::instance()->paletteServer();
    srv->removeObserver(this);
}

void PaletteDockerDock::setViewManager(KisViewManager* kisview)
{
    m_view = kisview;
    m_resourceProvider = kisview->canvasResourceProvider();
    connect(m_resourceProvider, SIGNAL(sigFGColorChanged(KoColor)),
            this, SLOT(slotFGColorResourceChanged(KoColor)));
    kisview->nodeManager()->disconnect(m_model);
}

void PaletteDockerDock::unsetResourceServer()
{
    KoResourceServer<KoColorSet> *srv = KoResourceServerProvider::instance()->paletteServer();
    srv->removeObserver(this);
}

void PaletteDockerDock::resourceAdded(QSharedPointer<KoColorSet> resource)
{
    Q_UNUSED(resource);
}

void PaletteDockerDock::removingResource(QSharedPointer<KoColorSet> resource)
{
    Q_UNUSED(resource);
}

void PaletteDockerDock::resourceChanged(QSharedPointer<KoColorSet> resource)
{
    m_model->slotExternalPaletteModified(resource);
}

void PaletteDockerDock::slotAddPalette()
{
    KoColorSetSP palette = m_paletteEditor->addPalette();

    if (palette) {
        m_paletteChooser->setCurrentItem(palette);
    }
}

void PaletteDockerDock::slotRemovePalette(KoColorSetSP cs)
{
    m_paletteEditor->removePalette(cs);
}

void PaletteDockerDock::slotImportPalette()
{
    KoColorSetSP palette = m_paletteEditor->importPalette();
    if (palette) {
        m_paletteChooser->setCurrentItem(palette);
    }
}

void PaletteDockerDock::slotExportPalette(KoColorSetSP palette)
{
    if (!palette) {
        return;
    }
    KoFileDialog dialog(this, KoFileDialog::SaveFile, "Save Palette");
    dialog.setCaption(i18n("Export Palette"));
    dialog.setDefaultDir(palette->filename());
    dialog.setMimeTypeFilters(QStringList() << "application/x-krita-palette");
    QString newPath;
    if ((newPath = dialog.filename()).isEmpty()) { return; }

    QFile file(newPath);
    if (!file.open(QIODevice::WriteOnly)) {
        warnKrita << "Could not open the file for writing:" << newPath;
        return;
    }
    if (palette->saveToDevice(&file)) {
        if (m_view) {
            m_view->showFloatingMessage(
                i18nc("Floating message about exporting successful", "Palette exported successfully"), QIcon(),
                500, KisFloatingMessage::Low);
        }
    } else {
        warnKrita << "Could export to the file:" << newPath;
    }
}

void PaletteDockerDock::setCanvas(KoCanvasBase *canvas)
{
    setEnabled(canvas != 0);
    if (canvas) {
        KisCanvas2 *cv = qobject_cast<KisCanvas2*>(canvas);
        m_model->setDisplayRenderer(cv->displayColorConverter()->displayRendererInterface());
    }

    if (m_view && m_view->document()) {
        m_activeDocument = m_view->document();
        m_paletteEditor->setView(m_view);
    }
    if (m_resourceProvider) {
        m_grid->setCurrentColor(m_resourceProvider->fgColor().toQColor());
    }

    if (!m_currentColorSet) {
        slotSetColorSet(0);
    }
    updateHeader();
}

void PaletteDockerDock::unsetCanvas()
{
    setEnabled(false);
    m_model->setDisplayRenderer(0);
    m_paletteEditor->setView(0);

    if (!m_currentColorSet) {
        slotSetColorSet(0);
    }
}

void PaletteDockerDock::slotSetColorSet(KoColorSetSP colorSet)
{
    if (m_currentColorSet == colorSet) {
        updateHeader();
        return;
    }

    Q_FOREACH (const QMetaObject::Connection &connection, m_paletteConnections) {
        disconnect(connection);
    }
    m_paletteConnections.clear();

    // needs to save the palette before switching to another one
    if (m_paletteEditor->isModified()) {
        m_paletteEditor->saveNewPaletteVersion();
    }
    if (colorSet) {
        m_model->setColorSet(colorSet);
        m_paletteConnections << connect(colorSet->undoStack(), SIGNAL(canUndoChanged(bool)), this, SLOT(updateHeader()));
        m_paletteConnections << connect(colorSet->undoStack(), SIGNAL(canRedoChanged(bool)), this, SLOT(updateHeader()));
        KisConfig(false).setDefaultPalette(colorSet->name());
    }

    m_currentColorSet = colorSet;
    updateHeader();
}

bool PaletteDockerDock::canEdit() const
{
    // a document palette is saved with the document: one has to be open
    return m_currentColorSet && !m_currentColorSet->isLocked() && m_view && m_view->document();
}

bool PaletteDockerDock::isGlobal() const
{
    if (!m_currentColorSet) {
        return false;
    }
    KisResourceModel model(ResourceType::Palettes);
    const QModelIndex index = model.indexForResource(m_currentColorSet);
    if (index.isValid()) {
        bool ok;
        const int storageId = model.data(index, Qt::UserRole + KisAllResourcesModel::StorageId).toInt(&ok);
        if (ok) {
            KisStorageModel storageModel;
            KisResourceStorageSP storage = storageModel.storageForId(storageId);
            return storage && storage->type() != KisResourceStorage::StorageType::Memory;
        }
    }
    return true;
}

void PaletteDockerDock::updateHeader()
{
    if (m_currentColorSet) {
        // an unsaved palette is saved when Krita closes; a document's one with the document
        const bool unsaved = m_paletteEditor->isModified() && isGlobal();
        const QString name = fontMetrics().elidedText(m_currentColorSet->name(), Qt::ElideMiddle, 180);
        m_paletteButton->setText(unsaved ? name + QStringLiteral(" •") : name);
        m_paletteButton->setToolTip(unsaved ? i18nc("@info:tooltip", "Changed, saved when Krita closes. Click for other palettes, right click for this one")
                                            : i18nc("@info:tooltip", "Click for other palettes, right click for this one"));
    } else {
        m_paletteButton->setText(i18nc("@action:button choose a palette", "Palettes"));
        m_paletteButton->setToolTip(i18nc("@info:tooltip", "Choose a palette"));
    }

    const bool editable = canEdit();
    KUndo2Stack *stack = m_currentColorSet ? m_currentColorSet->undoStack() : nullptr;
    m_undo->setVisible(editable && stack && stack->canUndo());
    m_redo->setVisible(editable && stack && stack->canRedo());
    m_grid->relayout();
}

KisSwatch PaletteDockerDock::newSwatch(const KoColor &color) const
{
    KisSwatch swatch(color);
    const QString number = QString::number(m_currentColorSet->colorCount() + 1);
    swatch.setId(number);
    swatch.setName(i18nc("Default name for a color swatch", "Color %1", number));
    return swatch;
}

void PaletteDockerDock::pickSwatch(const QModelIndex &index)
{
    if (!m_resourceProvider) {
        return;
    }
    const KoColor color = m_model->getSwatch(index).color();
    m_colorSelfUpdate = true;
    m_resourceProvider->setFGColor(color);
    m_colorSelfUpdate = false;
    m_grid->setCurrentColor(color.toQColor());
}

void PaletteDockerDock::addCurrentColor(const QModelIndex &emptySlot)
{
    if (!canEdit() || !m_resourceProvider) {
        return;
    }
    const KisSwatch swatch = newSwatch(m_resourceProvider->fgColor());
    if (emptySlot.isValid()) {
        m_model->setSwatch(swatch, emptySlot);
    } else {
        // the first group is full: a new row under it
        KisSwatchGroupSP global = m_currentColorSet->getGlobalGroup();
        m_currentColorSet->addSwatch(swatch, KoColorSet::GLOBAL_GROUP_NAME, 0, global ? global->rowCount() : 0);
    }
    m_grid->setCurrentColor(m_resourceProvider->fgColor().toQColor());
    updateHeader();
}

void PaletteDockerDock::moveSwatch(const QModelIndex &from, const QModelIndex &to)
{
    if (!canEdit() || !isOccupied(from) || !to.isValid() || from == to) {
        return;
    }
    const KisSwatch moving = m_model->getSwatch(from);
    const bool swap = isOccupied(to);
    const KisSwatch other = swap ? m_model->getSwatch(to) : KisSwatch();

    // one step to undo
    KUndo2Stack *stack = m_currentColorSet->undoStack();
    stack->beginMacro(kundo2_i18n("Move Swatch"));
    m_model->setSwatch(moving, to);
    if (swap) {
        m_model->setSwatch(other, from);
    } else {
        m_model->removeSwatch(from, false);
    }
    stack->endMacro();
    updateHeader();
}

void PaletteDockerDock::removeSwatch(const QModelIndex &index)
{
    if (!canEdit() || !isOccupied(index)) {
        return;
    }
    m_model->removeSwatch(index, false);
    updateHeader();
}

void PaletteDockerDock::showSwatchCard(const QModelIndex &index, QWidget *anchor)
{
    KisMainWindow *mainWindow = m_view ? m_view->mainWindow() : nullptr;
    if (!mainWindow || !isOccupied(index)) {
        return;
    }
    const KisSwatch swatch = m_model->getSwatch(index);

    const qreal dpr = devicePixelRatioF();
    QPixmap chip(QSize(28, 28) * dpr);
    chip.setDevicePixelRatio(dpr);
    chip.fill(Qt::transparent);
    {
        QPainter painter(&chip);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(QPen(QColor(0, 0, 0, 90), 1));
        painter.setBrush(swatchColor(index));
        painter.drawRoundedRect(QRectF(0.5, 0.5, 27, 27), 6, 6);
    }

    auto *card = new KisStudioItemCard(mainWindow, QIcon(chip),
                                       swatch.name().isEmpty() ? swatchColor(index).name() : swatch.name(), nullptr);
    if (canEdit()) {
        const QPersistentModelIndex target = index;
        card->addCommand({KisIconUtils::loadIcon(QStringLiteral("format-fill-color")),
                          i18nc("@info:tooltip", "Put the current color here"), [this, target]() {
                              if (!canEdit() || !isOccupied(target) || !m_resourceProvider) return;
                              KisSwatch replaced = m_model->getSwatch(target);
                              replaced.setColor(m_resourceProvider->fgColor());
                              m_model->setSwatch(replaced, target);
                              updateHeader();
                          }});
        card->addCommand({KisIconUtils::loadIcon(QStringLiteral("document-edit")),
                          i18nc("@info:tooltip", "Edit its name and color"), [this, target]() {
                              if (!canEdit() || !isOccupied(target)) return;
                              m_paletteEditor->modifyEntry(target);
                              updateHeader();
                          }});
        card->addCommand({KisIconUtils::loadIcon(QStringLiteral("edit-delete")),
                          i18nc("@info:tooltip", "Remove (or drag it off the panel)"), [this, target]() {
                              removeSwatch(target);
                          }});
    }
    card->popupBeside(anchor);
}

void PaletteDockerDock::showPaletteCard()
{
    KisMainWindow *mainWindow = m_view ? m_view->mainWindow() : nullptr;
    if (!mainWindow || !m_currentColorSet) {
        return;
    }
    auto *card = new KisStudioItemCard(mainWindow, KisIconUtils::loadIcon(QStringLiteral("palette-library")),
                                       m_currentColorSet->name(), nullptr);
    const bool locked = m_currentColorSet->isLocked();
    if (!locked && m_view->document()) {
        card->addCommand({KisIconUtils::loadIcon(QStringLiteral("palette-edit")),
                          i18nc("@info:tooltip", "Edit the palette: its name, columns and groups"), [this]() {
                              if (!m_currentColorSet || !m_view) return;
                              KisDlgPaletteEditor dialog(m_paletteEditor.data(), m_view->mainWindowAsQWidget());
                              dialog.initialize(m_model);
                              m_paletteEditor->startEditing();
                              m_paletteEditor->endEditing(dialog.exec() == QDialog::Accepted);
                              updateHeader();
                          }});
    }
    card->addCommand({KisIconUtils::loadIcon(locked ? QStringLiteral("locked") : QStringLiteral("unlocked")),
                      locked ? i18nc("@info:tooltip", "Locked: click to allow changes") : i18nc("@info:tooltip", "Lock it against changes"),
                      [this, locked]() {
                          if (!m_currentColorSet) return;
                          m_currentColorSet->setLocked(!locked);
                          updateHeader();
                      },
                      true, locked});
    if (m_paletteEditor->isModified() && isGlobal()) {
        card->addCommand({KisIconUtils::loadIcon(QStringLiteral("document-save")),
                          i18nc("@info:tooltip", "Save now (it is also saved when Krita closes)"), [this]() {
                              m_paletteEditor->saveNewPaletteVersion();
                              updateHeader();
                          }});
    }
    card->addCommand({KisIconUtils::loadIcon(QStringLiteral("document-export")),
                      i18nc("@info:tooltip", "Export to a file"), [this]() { slotExportPalette(m_currentColorSet); }});
    card->popupBeside(m_paletteButton);
}

void PaletteDockerDock::slotFGColorResourceChanged(const KoColor &color)
{
    if (!m_colorSelfUpdate) {
        m_grid->setCurrentColor(color.toQColor());
    }
}

void PaletteDockerDock::slotStoragesChanged(const QString &/*location*/)
{
    if (m_activeDocument.isNull()) {
        slotSetColorSet(0);
    }
    if (m_currentColorSet) {
        if (!m_rServer->resource(m_currentColorSet->md5Sum(), "", "")) {
            slotSetColorSet(0);
        }
    }
}
