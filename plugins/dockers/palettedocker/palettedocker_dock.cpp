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
#include <QPainterPath>
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

#include <KoColorSpaceRegistry.h>
#include <KisStrokeLog.h>
#include <KisStudioBrushMemory.h>
#include <kis_image.h>
#include <kis_node.h>
#include <KisPaletteModel.h>
#include <KisPaletteChooser.h>
#include <KisPaletteEditor.h>
#include <dialogs/KisDlgPaletteEditor.h>
#include <KisModernStyle.h>

namespace {

const QString swatchMimeType = QStringLiteral("application/x-krita-palette-swatch");
/// A recently used color, dragged into the palette: its place in the recent row
const QString recentMimeType = QStringLiteral("application/x-krita-recent-color");
const QString recentKey = QStringLiteral("StudioRecentColors");
constexpr int recentKept = 24;

bool isOccupied(const QModelIndex &index)
{
    return index.isValid() && index.data(KisPaletteModel::CheckSlotRole).toBool()
           && !index.data(KisPaletteModel::IsGroupNameRole).toBool();
}

QColor swatchColor(const QModelIndex &index)
{
    return index.data(Qt::BackgroundRole).value<QBrush>().color();
}

/// A swatch as a picture: what is dragged, a card's icon
QPixmap swatchPixmap(const QColor &color, int size, qreal radius, qreal dpr)
{
    QPixmap pixmap(QSize(size, size) * dpr);
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    {
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(QPen(QColor(0, 0, 0, 90), 1));
        painter.setBrush(color);
        painter.drawPath(KisModernStyle::squircle(QRectF(0.5, 0.5, size - 1, size - 1), radius));
    }
    return pixmap;
}

/// A swatch's color, with a ring that reads on any color when it is the one painted with, or an outline when pointed at
void paintSwatch(QPainter &painter, const QRectF &rect, qreal radius, const QColor &color, bool current, bool hovered, const QColor &ink)
{
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawPath(KisModernStyle::squircle(rect, radius));
    painter.setBrush(Qt::NoBrush);
    if (current) {
        const QPainterPath ring = KisModernStyle::squircle(rect.adjusted(1.5, 1.5, -1.5, -1.5), radius);
        painter.setPen(QPen(QColor(0, 0, 0, 170), 3));
        painter.drawPath(ring);
        painter.setPen(QPen(Qt::white, 1.5));
        painter.drawPath(ring);
    } else if (hovered) {
        QColor outline = ink;
        outline.setAlphaF(0.6);
        painter.setPen(QPen(outline, 1));
        painter.drawPath(KisModernStyle::squircle(rect.adjusted(0.5, 0.5, -0.5, -0.5), radius));
    }
}

} // namespace

/// The swatches: the palette's columns across the panel's width, with group names between the groups
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
    /// The swatch size and columns once laid out, for the recent row to line up with
    std::function<void(int cell, int left, int columns)> laidOut;
    /// A recent color (\p recent in \p source's row) dropped on an empty slot, or on the "+" (an invalid slot: a new row)
    std::function<void(QObject *source, int recent, const QColor &color, const QModelIndex &slot)> recentDropped;

    PaletteGrid(KisPaletteModel *model, QWidget *parent)
        : QWidget(parent)
        , m_model(model)
    {
        setMouseTracking(true);
        setAcceptDrops(true);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

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
        // the foreground color changes at every move of a color wheel's drag, most often to one not here
        if (m_current == color.rgb()) return;
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
            if (laidOut) laidOut(m_cell, 0, qMax(1, width() / m_cell));
            update();
            return;
        }
        // as wide as the panel; very few columns would make huge swatches
        m_cell = qBound(4, width() / columns, 36);
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
        if (laidOut) laidOut(m_cell, left, columns);
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

    void paintEvent(QPaintEvent *event) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.translate(0, -m_scroll);
        // only the swatches in view: a large palette has thousands
        const QRect exposed = event->rect().translated(0, m_scroll);
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
            if (!cell.rect.intersects(exposed)) continue;
            const QRectF rect = QRectF(cell.rect).adjusted(1, 1, -1, -1);
            if (!isOccupied(cell.index)) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(empty);
                painter.drawPath(KisModernStyle::squircle(rect, radius));
                continue;
            }
            painter.setOpacity(m_dragging && cell.index == m_press ? 0.3 : 1.0);
            // the color itself, not as the display shows it, as the current color is
            const bool current = m_model->getSwatch(cell.index).color().toQColor().rgb() == m_current;
            paintSwatch(painter, rect, radius, swatchColor(cell.index), current, cell.index == m_hover && !m_dragging, ink);
            painter.setOpacity(1.0);
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
            painter.drawPath(KisModernStyle::squircle(QRectF(m_dropRect).adjusted(1, 1, -1, -1), radius));
        }
    }

    void wheelEvent(QWheelEvent *event) override
    {
        if (m_contentHeight > height()) {
            // a row at a time, so that a short panel shows whole rows
            if (const int steps = KisModernStyle::wheelSteps(event, this)) setScroll(m_scroll - steps * m_cell);
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

    // a right click, or a finger held on a swatch (KisTouchMode)
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
        // swatches of this palette, moved; recent colors, kept
        if ((event->source() == this && event->mimeData()->hasFormat(swatchMimeType))
            || (event->mimeData()->hasFormat(recentMimeType) && editable && editable())) {
            event->acceptProposedAction();
        }
    }

    void dragMoveEvent(QDragMoveEvent *event) override
    {
        const QModelIndex target = indexAt(event->pos());
        bool ok;
        if (event->mimeData()->hasFormat(recentMimeType)) {
            // a recent color goes in an empty place: an empty slot or the "+"
            const bool onPlus = m_plus.contains(event->pos() + QPoint(0, m_scroll));
            ok = onPlus || (target.isValid() && !isOccupied(target));
            m_dropRect = !ok ? QRect() : onPlus ? m_plus : cellRect(target);
        } else {
            ok = target.isValid() && QPersistentModelIndex(target) != m_press;
            m_dropRect = ok ? cellRect(target) : QRect();
        }
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
        if (event->mimeData()->hasFormat(recentMimeType)) {
            const int recent = event->mimeData()->data(recentMimeType).toInt();
            const QColor color = qvariant_cast<QColor>(event->mimeData()->colorData());
            const bool onPlus = m_plus.contains(event->pos() + QPoint(0, m_scroll));
            const QModelIndex target = onPlus ? QModelIndex(m_plusSlot) : indexAt(event->pos());
            if (!onPlus && (!target.isValid() || isOccupied(target))) {
                event->ignore();
                return;
            }
            event->acceptProposedAction();
            QTimer::singleShot(0, this, [this, source = QPointer<QObject>(event->source()), recent, color, slot = QPersistentModelIndex(target)]() {
                if (recentDropped) recentDropped(source, recent, color, slot);
            });
            return;
        }
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
        auto *drag = new QDrag(this);
        auto *mime = new QMimeData();
        mime->setData(swatchMimeType, QByteArray::number(from.row()) + ',' + QByteArray::number(from.column()));
        // its color too, so it fills where it is dropped on the canvas or recolors a layer it is dropped on
        mime->setColorData(swatchColor(from));
        drag->setMimeData(mime);
        drag->setPixmap(swatchPixmap(swatchColor(from), m_cell, 4, devicePixelRatioF()));
        drag->setHotSpot(QPoint(m_cell / 2, m_cell / 2));

        m_dragging = true;
        update();
        const Qt::DropAction result = drag->exec(Qt::MoveAction | Qt::CopyAction, Qt::MoveAction);
        m_dragging = false;
        m_dropRect = QRect();
        update();

        // let go outside the panel: taken out of the palette
        QWidget *panel = parentWidget() ? parentWidget() : this;
        if (result == Qt::IgnoreAction && !KisModernStyle::pointerHeld() && !panel->rect().contains(panel->mapFromGlobal(QCursor::pos()))) {
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

/// The colors last painted with, newest first, lined up with the palette's columns; dragged into the palette to keep one
class RecentStrip : public QWidget
{
public:
    std::function<void(int index)> picked;
    std::function<void(int index, QWidget *anchor)> cardRequested;

    explicit RecentStrip(QWidget *parent) : QWidget(parent)
    {
        setMouseTracking(true);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setToolTip(i18nc("@info:tooltip", "Recent colors"));
    }

    void setColors(const QVector<QColor> &colors, bool ofLayer = false)
    {
        m_colors = colors;
        setToolTip(ofLayer ? i18nc("@info:tooltip", "Colors on this layer")
                           : i18nc("@info:tooltip", "Recent colors"));
        fit();
    }

    void lineUp(int cell, int left, int columns)
    {
        if (cell == m_cell && left == m_left && columns == m_columns) return;
        m_cell = cell;
        m_left = left;
        m_columns = qMax(1, columns);
        fit();
    }

    void setCurrentColor(const QColor &color)
    {
        m_current = color.rgb();
        update();
    }

protected:
    void fit()
    {
        // as tall as a swatch and a little gap under it; nothing when there is nothing
        setFixedHeight(m_colors.isEmpty() ? 0 : m_cell + 5);
        setVisible(!m_colors.isEmpty());
        update();
    }

    int indexAt(const QPoint &pos) const
    {
        if (pos.y() < 0 || pos.y() >= m_cell || pos.x() < m_left) return -1;
        const int i = (pos.x() - m_left) / m_cell;
        return i < qMin(m_columns, int(m_colors.size())) ? i : -1;
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const qreal radius = qMin(4.0, m_cell / 5.0);
        const QColor ink = palette().color(QPalette::WindowText);
        for (int i = 0; i < m_colors.size() && i < m_columns; i++) {
            const QRectF rect = QRectF(m_left + i * m_cell, 0, m_cell, m_cell).adjusted(1, 1, -1, -1);
            paintSwatch(painter, rect, radius, m_colors[i], m_colors[i].rgb() == m_current, i == m_hover, ink);
        }
        // a hairline between the colors used and the palette
        QColor line = ink;
        line.setAlphaF(0.1);
        painter.setPen(QPen(line, 1));
        const int width = qMin(m_columns, 1000) * m_cell;
        painter.drawLine(QPointF(m_left + 2, height() - 1.5), QPointF(m_left + width - 2, height() - 1.5));
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        m_press = event->button() == Qt::LeftButton ? indexAt(event->pos()) : -1;
        m_pressPos = event->pos();
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (!(event->buttons() & Qt::LeftButton)) {
            const int hover = indexAt(event->pos());
            if (hover != m_hover) {
                m_hover = hover;
                setCursor(hover >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
                update();
            }
            return;
        }
        if (m_press < 0 || (event->pos() - m_pressPos).manhattanLength() < QApplication::startDragDistance()) return;
        // into the palette
        const int index = m_press;
        m_press = -1;
        auto *drag = new QDrag(this);
        auto *mime = new QMimeData();
        mime->setData(recentMimeType, QByteArray::number(index));
        mime->setColorData(m_colors.value(index));
        drag->setMimeData(mime);
        drag->setPixmap(swatchPixmap(m_colors.value(index), m_cell, 4, devicePixelRatioF()));
        drag->setHotSpot(QPoint(m_cell / 2, m_cell / 2));
        drag->exec(Qt::CopyAction);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && m_press >= 0 && indexAt(event->pos()) == m_press && picked) {
            picked(m_press);
        }
        m_press = -1;
    }

    void contextMenuEvent(QContextMenuEvent *event) override
    {
        event->accept();
        const int index = indexAt(event->pos());
        if (index < 0 || !cardRequested) return;
        auto *anchor = new QWidget(this);
        anchor->setGeometry(m_left + index * m_cell, 0, m_cell, m_cell);
        cardRequested(index, anchor);
        anchor->deleteLater();
    }

    void leaveEvent(QEvent *) override
    {
        m_hover = -1;
        update();
    }

private:
    QVector<QColor> m_colors;
    int m_cell {20};
    int m_left {0};
    int m_columns {8};
    int m_hover {-1};
    int m_press {-1};
    QPoint m_pressPos;
    QRgb m_current {0};
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
    // the list opens on the palette shown: the one highlighted is the one in use
    connect(m_paletteButton, &KisPopupButton::sigPopupWidgetShown, this, [this]() {
        if (m_currentColorSet) m_paletteChooser->highlight(m_currentColorSet);
    });
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
    m_undo = smallButton("edit-undo", i18nc("@info:tooltip", "Undo"));
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

    m_recentStrip = new RecentStrip(mainWidget);
    m_recentUpdate.setSingleShot(true);
    m_recentUpdate.setInterval(0);
    connect(&m_recentUpdate, &QTimer::timeout, this, &PaletteDockerDock::updateRecentStrip);
    m_recentStrip->picked = [this](int index) { pickRecent(index); };
    m_recentStrip->cardRequested = [this](int index, QWidget *anchor) { showRecentCard(index, anchor); };
    layout->addWidget(m_recentStrip);

    m_grid = new PaletteGrid(m_model, mainWidget);
    m_grid->laidOut = [this](int cell, int left, int columns) { m_recentStrip->lineUp(cell, left, columns); };
    m_grid->recentDropped = [this](QObject *source, int recent, const QColor &color, const QModelIndex &slot) {
        // from another window's row: the color the drag carries
        if (source == m_recentStrip && recent >= 0 && recent < m_shownRecent.size()) {
            addColor(m_shownRecent[recent], slot);
        } else if (color.isValid()) {
            addColor(KoColor(color, KoColorSpaceRegistry::instance()->rgb8()), slot);
        }
    };
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
    // one picked: the list is done
    connect(m_paletteChooser, &KisPaletteChooser::sigPaletteSelected, m_paletteButton, &KisPopupButton::hidePopupWidget);
    connect(m_paletteChooser, SIGNAL(sigAddPalette()), SLOT(slotAddPalette()));
    connect(m_paletteChooser, SIGNAL(sigImportPalette()), SLOT(slotImportPalette()));
    connect(m_paletteChooser, SIGNAL(sigRemovePalette(KoColorSetSP)), SLOT(slotRemovePalette(KoColorSetSP)));
    connect(m_paletteChooser, SIGNAL(sigExportPalette(KoColorSetSP)), SLOT(slotExportPalette(KoColorSetSP)));

    // the colors last painted with, kept between sessions; saved once a run of strokes is over
    Q_FOREACH (const QString &name, KisConfig(true).readEntry<QStringList>(recentKey, QStringList())) {
        const QColor color(name);
        if (color.isValid()) m_recent << KoColor(color, KoColorSpaceRegistry::instance()->rgb8());
    }
    m_recentSave.setSingleShot(true);
    m_recentSave.setInterval(3000);
    connect(&m_recentSave, &QTimer::timeout, this, &PaletteDockerDock::saveRecent);
    updateRecentStrip();

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
    if (m_recentSave.isActive()) {
        saveRecent();
    }
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
    connect(m_resourceProvider, &KisCanvasResourceProvider::sigFGColorUsed, this, &PaletteDockerDock::slotColorUsed);
    // another layer: its colors
    connect(kisview->nodeManager(), &KisNodeManager::sigNodeActivated, this, [this]() { updateRecentStrip(); });
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

    // a stroke ended (or was undone): the layer's colors may have changed; once for many at a time
    disconnect(m_logConnection);
    m_logConnection = connect(KisStrokeLog::notifier(), &KisStrokeLogNotifier::changed, this, [this]() {
        if (!m_recentUpdate.isActive()) m_recentUpdate.start();
    });
    updateRecentStrip();

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
    // the list of palettes highlights the one shown, however it came to be shown
    if (colorSet) m_paletteChooser->highlight(colorSet);
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
        m_paletteButton->setToolTip(unsaved ? i18nc("@info:tooltip", "Unsaved changes") : QString());
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
    if (m_resourceProvider) {
        addColor(m_resourceProvider->fgColor(), emptySlot);
    }
}

void PaletteDockerDock::addColor(const KoColor &color, const QModelIndex &emptySlot)
{
    if (!canEdit() || !m_resourceProvider) {
        return;
    }
    const KisSwatch swatch = newSwatch(color);
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
    auto *card = new KisStudioItemCard(mainWindow, QIcon(swatchPixmap(swatchColor(index), 28, 6, devicePixelRatioF())),
                                       swatch.name().isEmpty() ? swatchColor(index).name() : swatch.name(), nullptr);
    if (canEdit()) {
        const QPersistentModelIndex target = index;
        card->addCommand({KisIconUtils::loadIcon(QStringLiteral("format-fill-color")),
                          i18nc("@info:tooltip", "Set to current color"), [this, target]() {
                              if (!canEdit() || !isOccupied(target) || !m_resourceProvider) return;
                              KisSwatch replaced = m_model->getSwatch(target);
                              replaced.setColor(m_resourceProvider->fgColor());
                              m_model->setSwatch(replaced, target);
                              updateHeader();
                          }});
        card->addCommand({KisIconUtils::loadIcon(QStringLiteral("document-edit")),
                          i18nc("@info:tooltip", "Edit"), [this, target]() {
                              if (!canEdit() || !isOccupied(target)) return;
                              m_paletteEditor->modifyEntry(target);
                              updateHeader();
                          }});
        card->addCommand({KisIconUtils::loadIcon(QStringLiteral("edit-delete")),
                          i18nc("@info:tooltip", "Remove"), [this, target]() {
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
                          i18nc("@info:tooltip", "Edit palette"), [this]() {
                              if (!m_currentColorSet || !m_view) return;
                              KisDlgPaletteEditor dialog(m_paletteEditor.data(), m_view->mainWindowAsQWidget());
                              dialog.initialize(m_model);
                              m_paletteEditor->startEditing();
                              m_paletteEditor->endEditing(dialog.exec() == QDialog::Accepted);
                              updateHeader();
                          }});
    }
    card->addCommand({KisIconUtils::loadIcon(locked ? QStringLiteral("locked") : QStringLiteral("unlocked")),
                      locked ? i18nc("@info:tooltip", "Unlock") : i18nc("@info:tooltip", "Lock"),
                      [this, locked]() {
                          if (!m_currentColorSet) return;
                          m_currentColorSet->setLocked(!locked);
                          updateHeader();
                      },
                      true, locked});
    if (m_paletteEditor->isModified() && isGlobal()) {
        card->addCommand({KisIconUtils::loadIcon(QStringLiteral("document-save")),
                          i18nc("@info:tooltip", "Save"), [this]() {
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
    m_recentStrip->setCurrentColor(color.toQColor());
}

void PaletteDockerDock::slotColorUsed(const KoColor &color)
{
    // newest first, each once
    const QRgb rgba = color.toQColor().rgba();
    for (int i = 0; i < m_recent.size(); i++) {
        if (m_recent[i].toQColor().rgba() == rgba) {
            if (i == 0) return;
            m_recent.removeAt(i);
            break;
        }
    }
    m_recent.prepend(color);
    if (m_recent.size() > recentKept) m_recent.resize(recentKept);
    m_recentSave.start();
    updateRecentStrip();
}

void PaletteDockerDock::saveRecent()
{
    m_recentSave.stop();
    QStringList names;
    Q_FOREACH (const KoColor &recent, m_recent) names << recent.toQColor().name(QColor::HexArgb);
    KisConfig(false).writeEntry(recentKey, names);
}

void PaletteDockerDock::updateRecentStrip()
{
    m_shownRecent.clear();
    // the colors painted on the active layer, when it has some and layers remember theirs
    if (m_view && m_view->image() && m_view->activeNode() && KisStudioBrushMemory::layersRememberBrushes()) {
        const QVector<QColor> onLayer = m_view->activeNode()->strokeLog().colors(recentKept);
        if (!onLayer.isEmpty()) {
            Q_FOREACH (const QColor &color, onLayer) {
                m_shownRecent << KoColor(color, KoColorSpaceRegistry::instance()->rgb8());
            }
            m_recentStrip->setColors(onLayer, true);
            return;
        }
    }
    QVector<QColor> colors;
    Q_FOREACH (const KoColor &color, m_recent) colors << color.toQColor();
    m_shownRecent = m_recent;
    m_recentStrip->setColors(colors);
}

void PaletteDockerDock::pickRecent(int index)
{
    if (!m_resourceProvider || index < 0 || index >= m_shownRecent.size()) {
        return;
    }
    m_resourceProvider->setFGColor(m_shownRecent[index]);
}

void PaletteDockerDock::showRecentCard(int index, QWidget *anchor)
{
    KisMainWindow *mainWindow = m_view ? m_view->mainWindow() : nullptr;
    if (!mainWindow || index < 0 || index >= m_shownRecent.size()) {
        return;
    }
    const KoColor color = m_shownRecent[index];
    auto *card = new KisStudioItemCard(mainWindow, QIcon(swatchPixmap(color.toQColor(), 28, 6, devicePixelRatioF())),
                                       color.toQColor().name(), nullptr);
    if (canEdit()) {
        card->addCommand({KisIconUtils::loadIcon(QStringLiteral("list-add")), i18nc("@info:tooltip", "Add to palette"),
                          [this, color]() { addColor(color, QModelIndex()); }});
    }
    card->addCommand({KisIconUtils::loadIcon(QStringLiteral("edit-delete")), i18nc("@info:tooltip", "Forget it"), [this, color]() {
                          for (int i = 0; i < m_recent.size(); i++) {
                              if (m_recent[i].toQColor().rgba() == color.toQColor().rgba()) {
                                  m_recent.removeAt(i);
                                  break;
                              }
                          }
                          saveRecent();
                          updateRecentStrip();
                      }});
    card->popupBeside(anchor);
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
