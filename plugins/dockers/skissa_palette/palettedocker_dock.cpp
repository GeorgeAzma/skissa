/*
 *  SPDX-FileCopyrightText: 2013 Sven Langkamp <sven.langkamp@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */


#include "palettedocker_dock.h"

#include <KisColorDrop.h>

#include <QApplication>
#include <QDrag>
#include <QDragEnterEvent>
#include <QFile>
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QLineEdit>
#include <QScrollArea>
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
#include <KisSettingsRegistry.h>
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
#include <KisResourceUserOperations.h>
#include <widgets/KisStudioChips.h>
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

/// A swatch's shape: a squircle, or round for a drop of paint (the colors painted with, as on the rail)
QPainterPath swatchShape(const QRectF &rect, qreal radius, bool round)
{
    if (!round) return KisModernStyle::squircle(rect, radius);
    QPainterPath path;
    const qreal side = qMin(rect.width(), rect.height());
    path.addEllipse(QRectF(rect.center() - QPointF(side, side) / 2, QSizeF(side, side)));
    return path;
}

/// A swatch's color, edged faintly so white and pale colors keep their shape on light glass, with a ring that reads
/// on any color when it is the one painted with, or an outline when pointed at
void paintSwatch(QPainter &painter, const QRectF &rect, qreal radius, const QColor &color, bool current, bool hovered, const QColor &ink,
                 bool round = false)
{
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawPath(swatchShape(rect, radius, round));
    painter.setBrush(Qt::NoBrush);
    if (current) {
        const QPainterPath ring = swatchShape(rect.adjusted(1.5, 1.5, -1.5, -1.5), radius, round);
        painter.setPen(QPen(QColor(0, 0, 0, 170), 3));
        painter.drawPath(ring);
        painter.setPen(QPen(Qt::white, 1.5));
        painter.drawPath(ring);
    } else {
        QColor outline = ink;
        outline.setAlphaF(hovered ? 0.6 : 0.12);
        painter.setPen(QPen(outline, 1));
        painter.drawPath(swatchShape(rect.adjusted(0.5, 0.5, -0.5, -0.5), radius, round));
    }
}

/// The same palette, however it was loaded: the list reads each one again from the database
bool samePalette(const KoColorSetSP &a, const KoColorSetSP &b)
{
    if (!a || !b) return false;
    return a == b || a->filename().compare(b->filename(), Qt::CaseInsensitive) == 0;
}

} // namespace

/// The swatches: the palette's columns across the panel's width, with group names between the groups
class PaletteGrid : public QWidget
{
public:
    std::function<void(const QModelIndex &)> picked;
    /// A color dragged in from elsewhere (the color chip, another palette, the image) dropped on an empty slot, or on the
    /// slot after the last swatch (an invalid slot: a new row)
    std::function<void(const KoColor &color, const QModelIndex &slot)> colorDropped;
    std::function<void(const QModelIndex &, QWidget *anchor)> cardRequested;
    std::function<void(const QModelIndex &from, const QModelIndex &to)> moved;
    std::function<void(const QModelIndex &)> draggedOff;
    std::function<bool()> editable;
    /// A recent color (\p recent in \p source's row) dropped on an empty slot, or after the last swatch (an invalid slot: a new row)
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
        m_next = QRect();
        m_nextSlot = QPersistentModelIndex();
        m_contentHeight = 0;

        const int columns = m_model->colorSet() ? m_model->columnCount() : 0;
        if (columns <= 0) {
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
        // the slot a dropped color goes to: after the last swatch of the first group, in a row of its own if it is full
        auto endGlobalGroup = [&]() {
            inGlobalGroup = false;
            if (!canAdd) return;
            int last = -1;
            for (int i = 0; i < m_cells.size(); i++) {
                if (isOccupied(m_cells[i].index)) last = i;
            }
            if (last + 1 < m_cells.size()) {
                m_next = m_cells[last + 1].rect;
                m_nextSlot = m_cells[last + 1].index;
            } else if (m_offering) {
                // a full palette gets a row for it only while a color is dragged over it
                m_next = QRect(left, y, m_cell, m_cell);
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
            if (m_next.contains(pos)) {
                text = i18nc("@info:tooltip", "Drop a color here");
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

        // empty places are only outlined: a fill read as a pale color on glass
        QColor empty = ink;
        empty.setAlphaF(0.08);
        const qreal radius = qMin<qreal>(KisModernStyle::tileRadius, m_cell / 4.0);
        Q_FOREACH (const Cell &cell, m_cells) {
            if (!cell.rect.intersects(exposed)) continue;
            const QRectF rect = QRectF(cell.rect).adjusted(1, 1, -1, -1);
            if (!isOccupied(cell.index)) {
                painter.setPen(QPen(empty, 1));
                painter.setBrush(Qt::NoBrush);
                painter.drawPath(KisModernStyle::squircle(rect.adjusted(0.5, 0.5, -0.5, -0.5), radius));
                continue;
            }
            painter.setOpacity(m_dragging && cell.index == m_press ? 0.3 : 1.0);
            // the color itself, not as the display shows it, as the current color is
            const bool current = m_model->getSwatch(cell.index).color().toQColor().rgb() == m_current;
            paintSwatch(painter, rect, radius, swatchColor(cell.index), current, cell.index == m_hover && !m_dragging, ink);
            painter.setOpacity(1.0);
        }

        if (!m_next.isNull()) {
            // where the next color dropped goes: an empty place, its edge dashed so it reads as one
            QColor edge = ink;
            edge.setAlphaF(0.35);
            QPen dashed(edge, 1, Qt::CustomDashLine);
            dashed.setDashPattern({3, 2});
            painter.setPen(dashed);
            painter.setBrush(Qt::NoBrush);
            painter.drawPath(KisModernStyle::squircle(QRectF(m_next).adjusted(1.5, 1.5, -1.5, -1.5), radius));
        }

        if (!m_dropRect.isNull()) {
            // on a swatch: the color it would become
            if (m_dropColor.isValid()) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(m_dropColor);
                painter.drawPath(KisModernStyle::squircle(QRectF(m_dropRect).adjusted(1, 1, -1, -1), radius));
            }
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
        m_scrolling = false;
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (!(event->buttons() & Qt::LeftButton)) {
            const QModelIndex hover = indexAt(event->pos());
            if (QPersistentModelIndex(hover) != m_hover) {
                m_hover = hover;
                setCursor(isOccupied(hover) ? Qt::PointingHandCursor : Qt::ArrowCursor);
                update();
            }
            return;
        }
        if (m_scrolling) {
            setScroll(m_pressScroll - (event->pos().y() - m_pressPos.y()));
            return;
        }
        if ((event->pos() - m_pressPos).manhattanLength() < KisModernStyle::dragDistance(event)) {
            return;
        }
        // a locked palette's colors still go onto the canvas; only moving and removing them needs it unlocked
        if (isOccupied(m_press)) {
            startDrag();
        } else {
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
        if (isOccupied(m_press) && QPersistentModelIndex(indexAt(event->pos())) == m_press) {
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
        update();
    }

    static QColor draggedColor(const QMimeData *mime)
    {
        if (mime->hasColor()) return qvariant_cast<QColor>(mime->colorData());
        KoColor color;
        return KisColorDrop::colorOf(mime, &color) ? color.toQColor() : QColor();
    }

    void dragEnterEvent(QDragEnterEvent *event) override
    {
        // swatches of this palette, moved; any other color (recent, the color chip, another palette), kept
        if (editable && editable() && (m_dragging || KisColorDrop::carriesColor(event->mimeData()))) {
            event->acceptProposedAction();
            if (!m_dragging) offer(true);
        }
    }

    void dragMoveEvent(QDragMoveEvent *event) override
    {
        const QModelIndex target = indexAt(event->pos());
        bool ok;
        if (!m_dragging) {
            // a color from elsewhere goes in the empty slot under it, in place of the swatch under it, or else after
            // the last swatch
            const bool onCell = target.isValid();
            ok = onCell || !m_next.isNull();
            m_dropRect = !ok ? QRect() : onCell ? cellRect(target) : m_next;
            m_dropColor = onCell && isOccupied(target) ? draggedColor(event->mimeData()) : QColor();
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
        m_dropColor = QColor();
        offer(false);
        update();
    }

    void offer(bool on)
    {
        if (m_offering == on) return;
        m_offering = on;
        relayout();
    }

    void dropEvent(QDropEvent *event) override
    {
        m_dropRect = QRect();
        m_dropColor = QColor();
        // the row offered for it stays until the drop is handled, then goes with the next layout
        QTimer::singleShot(0, this, [this]() { offer(false); });
        if (!m_dragging) {
            const QModelIndex under = indexAt(event->pos());
            if (!under.isValid() && m_next.isNull()) {
                event->ignore();
                return;
            }
            const QModelIndex target = under.isValid() ? under : QModelIndex(m_nextSlot);
            event->acceptProposedAction();
            // the drag's own event loop is still running: change the palette after it
            if (event->mimeData()->hasFormat(recentMimeType)) {
                const int recent = event->mimeData()->data(recentMimeType).toInt();
                const QColor color = qvariant_cast<QColor>(event->mimeData()->colorData());
                QTimer::singleShot(0, this, [this, source = QPointer<QObject>(event->source()), recent, color, slot = QPersistentModelIndex(target)]() {
                    if (recentDropped) recentDropped(source, recent, color, slot);
                });
                return;
            }
            KoColor color;
            if (!KisColorDrop::colorOf(event->mimeData(), &color)) return;
            QTimer::singleShot(0, this, [this, color, slot = QPersistentModelIndex(target)]() {
                if (colorDropped) colorDropped(color, slot);
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
        const bool canMove = editable && editable();
        auto *drag = new QDrag(qApp);
        auto *mime = new QMimeData();
        mime->setData(swatchMimeType, QByteArray::number(from.row()) + ',' + QByteArray::number(from.column()));
        // its color too, so it fills where it is dropped on the canvas or recolors a layer it is dropped on
        mime->setColorData(swatchColor(from));
        drag->setMimeData(mime);
        drag->setPixmap(swatchPixmap(swatchColor(from), m_cell, 4, devicePixelRatioF()));
        drag->setHotSpot(QPoint(m_cell / 2, m_cell / 2));

        m_dragging = true;
        update();
        const Qt::DropAction result = canMove ? drag->exec(Qt::MoveAction | Qt::CopyAction, Qt::MoveAction) : drag->exec(Qt::CopyAction);
        drag->deleteLater();
        m_dragging = false;
        m_dropRect = QRect();
        update();

        // let go outside the windows: taken out of the palette; anywhere in them that doesn't take it, kept
        const QPoint at = QCursor::pos();
        bool overWindow = false;
        Q_FOREACH (QWidget *top, QApplication::topLevelWidgets()) {
            if (top->isVisible() && top->frameGeometry().contains(at)) overWindow = true;
        }
        if (canMove && result == Qt::IgnoreAction && !KisModernStyle::pointerHeld() && !overWindow) {
            QTimer::singleShot(0, this, [this, from]() {
                if (draggedOff && from.isValid()) draggedOff(from);
            });
        }
    }

    KisPaletteModel *m_model;
    QVector<Cell> m_cells;
    QVector<Header> m_headers;
    QRect m_next;
    bool m_offering {false};
    QPersistentModelIndex m_nextSlot;
    int m_cell {20};
    int m_contentHeight {0};
    int m_scroll {0};
    QRgb m_current {0};

    QPersistentModelIndex m_hover;
    QPersistentModelIndex m_press;
    QPoint m_pressPos;
    int m_pressScroll {0};
    bool m_scrolling {false};
    bool m_dragging {false};
    QRect m_dropRect;
    /// The color a drop would put in place of the swatch under it
    QColor m_dropColor;
};

/// The colors last painted with (or on the layer), newest first, as small drops beside the palette's name; dragged into the palette to keep one
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

    void setCurrentColor(const QColor &color)
    {
        m_current = color.rgb();
        update();
    }

protected:
    void fit()
    {
        setFixedHeight(m_cell);
        update();
    }

    /// As many as fit
    int shown() const
    {
        return qMin(int(m_colors.size()), width() / m_cell);
    }

    int indexAt(const QPoint &pos) const
    {
        if (pos.y() < 0 || pos.y() >= m_cell || pos.x() < 0) return -1;
        const int i = pos.x() / m_cell;
        return i < shown() ? i : -1;
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QColor ink = palette().color(QPalette::WindowText);
        const qreal inset = KisModernStyle::hairline;
        for (int i = 0; i < shown(); i++) {
            const QRectF rect = QRectF(i * m_cell, 0, m_cell, m_cell).adjusted(inset, inset, -inset, -inset);
            paintSwatch(painter, rect, 0, m_colors[i], m_colors[i].rgb() == m_current, i == m_hover, ink, true);
        }
    }

    void resizeEvent(QResizeEvent *) override
    {
        update();
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
        if (m_press < 0 || (event->pos() - m_pressPos).manhattanLength() < KisModernStyle::dragDistance(event)) return;
        // into the palette
        const int index = m_press;
        m_press = -1;
        auto *drag = new QDrag(qApp);
        auto *mime = new QMimeData();
        mime->setData(recentMimeType, QByteArray::number(index));
        mime->setColorData(m_colors.value(index));
        drag->setMimeData(mime);
        drag->setPixmap(swatchPixmap(m_colors.value(index), m_cell, 4, devicePixelRatioF()));
        drag->setHotSpot(QPoint(m_cell / 2, m_cell / 2));
        drag->exec(Qt::CopyAction);
        drag->deleteLater();
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
        anchor->setGeometry(index * m_cell, 0, m_cell, m_cell);
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
    int m_cell {KisModernStyle::space4};
    int m_hover {-1};
    int m_press {-1};
    QPoint m_pressPos;
    QRgb m_current {0};
};

/// The palettes, each shown by its own colors under its name: picked by a click, its card on a right click
class PaletteList : public QWidget
{
public:
    std::function<void(KoColorSetSP)> picked;
    std::function<void(KoColorSetSP, QWidget *anchor)> cardRequested;
    std::function<void()> newRequested;
    std::function<void()> importRequested;

    explicit PaletteList(QWidget *parent)
        : QWidget(parent)
    {
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(KisModernStyle::space2);

        m_filter = new QLineEdit(this);
        m_filter->addAction(KisIconUtils::loadIcon(QStringLiteral("edit-find")), QLineEdit::LeadingPosition);
        m_filter->setClearButtonEnabled(true);
        connect(m_filter, &QLineEdit::textChanged, this, [this]() { fill(); });
        connect(m_filter, &QLineEdit::returnPressed, this, [this]() {
            if (!m_shown.isEmpty() && picked) picked(m_shown.first().set);
        });
        layout->addWidget(m_filter);

        m_scroll = new QScrollArea(this);
        m_scroll->setFrameShape(QFrame::NoFrame);
        m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        m_scroll->setWidgetResizable(true);
        m_scroll->viewport()->setAutoFillBackground(false);
        m_rows = new Rows(this);
        m_scroll->setWidget(m_rows);
        m_rows->setAutoFillBackground(false);
        layout->addWidget(m_scroll);

        auto *actions = new KisStudioChips(this);
        KisStudioChips::Chip add;
        add.text = QStringLiteral("+ ") + i18nc("@action:button a new palette", "New");
        KisStudioChips::Chip import;
        import.text = i18nc("@action:button a palette from a file", "Import");
        actions->setChips({add, import});
        connect(actions, &KisStudioChips::clicked, this, [this](int index) {
            if (index == 0 && newRequested) newRequested();
            if (index == 1 && importRequested) importRequested();
        });
        layout->addWidget(actions);

        setFixedWidth(listWidth);
    }

    void setCurrent(KoColorSetSP palette)
    {
        m_current = palette;
        m_rows->update();
    }

protected:
    void showEvent(QShowEvent *event) override
    {
        reload();
        QWidget::showEvent(event);
    }

private:
    static constexpr int listWidth = 260;
    static constexpr int rowsShown = 7;

    struct Entry {
        KoColorSetSP set;
        QVector<QColor> colors;
    };

    class Rows : public QWidget
    {
    public:
        explicit Rows(PaletteList *list)
            : QWidget(list)
            , m_list(list)
        {
            setMouseTracking(true);
        }

        int rowHeight() const
        {
            return fontMetrics().height() + KisModernStyle::space1 + stripHeight + 2 * KisModernStyle::space2;
        }

        int indexAt(const QPoint &pos) const
        {
            const int i = pos.y() / rowHeight();
            return pos.y() >= 0 && i < m_list->m_shown.size() ? i : -1;
        }

    protected:
        void paintEvent(QPaintEvent *) override
        {
            QPainter painter(this);
            painter.setRenderHint(QPainter::Antialiasing, true);
            const QColor ink = palette().color(QPalette::WindowText);
            const int tall = rowHeight();
            for (int i = 0; i < m_list->m_shown.size(); i++) {
                const Entry &entry = m_list->m_shown[i];
                const QRectF row(0, i * tall, width(), tall);
                const bool current = m_list->m_current && samePalette(entry.set, m_list->m_current);
                if (current || i == m_hover) {
                    // the ink at low alpha: on glass the base color selectedRow() mixes from is see-through
                    QColor ground = ink;
                    ground.setAlphaF(current ? 0.12 : 0.06);
                    painter.fillPath(KisModernStyle::squircle(row.adjusted(0, 1, 0, -1), KisModernStyle::tileRadius), ground);
                }
                const QRectF inside = row.adjusted(KisModernStyle::space2, KisModernStyle::space2, -KisModernStyle::space2, -KisModernStyle::space2);
                painter.setPen(ink);
                const QRectF name(inside.left(), inside.top(), inside.width(), fontMetrics().height());
                painter.drawText(name, Qt::AlignLeft | Qt::AlignVCenter, fontMetrics().elidedText(entry.set->name(), Qt::ElideMiddle, int(name.width())));
                paintStrip(painter, QRectF(inside.left(), inside.bottom() - stripHeight, inside.width(), stripHeight), entry.colors, ink);
            }
        }

        void mouseMoveEvent(QMouseEvent *event) override
        {
            const int hover = indexAt(event->pos());
            if (hover != m_hover) {
                m_hover = hover;
                update();
            }
        }

        void leaveEvent(QEvent *) override
        {
            m_hover = -1;
            update();
        }

        void mousePressEvent(QMouseEvent *event) override
        {
            // taken here, so the release comes here too
            event->accept();
        }

        void mouseReleaseEvent(QMouseEvent *event) override
        {
            const int index = indexAt(event->pos());
            if (event->button() == Qt::LeftButton && index >= 0 && m_list->picked) m_list->picked(m_list->m_shown[index].set);
        }

        void contextMenuEvent(QContextMenuEvent *event) override
        {
            event->accept();
            const int index = indexAt(mapFromGlobal(event->globalPos()));
            if (index < 0 || !m_list->cardRequested) return;
            auto *anchor = new QWidget(this);
            anchor->setGeometry(0, index * rowHeight(), width(), rowHeight());
            m_list->cardRequested(m_list->m_shown[index].set, anchor);
            anchor->deleteLater();
        }

    private:
        static constexpr int stripHeight = KisModernStyle::space3;

        /// The palette's colors side by side, as many as the width holds, taken evenly from all of them
        static void paintStrip(QPainter &painter, const QRectF &rect, const QVector<QColor> &colors, const QColor &ink)
        {
            const QPainterPath shape = KisModernStyle::squircle(rect, KisModernStyle::cornerRadius);
            QColor empty = ink;
            empty.setAlphaF(0.06);
            painter.fillPath(shape, empty);
            if (!colors.isEmpty()) {
                const int count = qMin(int(colors.size()), qMax(1, int(rect.width() / KisModernStyle::space1)));
                painter.save();
                painter.setClipPath(shape);
                const qreal step = rect.width() / count;
                for (int i = 0; i < count; i++) {
                    // a hair over into the next, so no seam shows between them
                    painter.fillRect(QRectF(rect.left() + i * step, rect.top(), step + 0.5, rect.height()), colors[i * colors.size() / count]);
                }
                painter.restore();
            }
            QColor edge = ink;
            edge.setAlphaF(0.12);
            painter.setPen(QPen(edge, 1));
            painter.setBrush(Qt::NoBrush);
            painter.drawPath(KisModernStyle::squircle(rect.adjusted(0.5, 0.5, -0.5, -0.5), KisModernStyle::cornerRadius));
        }

        PaletteList *m_list;
        int m_hover {-1};
    };

    /// Every palette, read again at each opening: palettes come and go with documents and bundles
    void reload()
    {
        m_all.clear();
        KisResourceModel model(ResourceType::Palettes);
        for (int row = 0; row < model.rowCount(); row++) {
            KoColorSetSP set = model.resourceForIndex(model.index(row, 0)).dynamicCast<KoColorSet>();
            if (!set) continue;
            Entry entry;
            entry.set = set;
            QList<KisSwatchGroup::SwatchInfo> infos;
            Q_FOREACH (const QString &group, set->swatchGroupNames()) {
                if (KisSwatchGroupSP swatches = set->getGroup(group)) infos += swatches->infoList();
            }
            std::stable_sort(infos.begin(), infos.end(), [](const KisSwatchGroup::SwatchInfo &a, const KisSwatchGroup::SwatchInfo &b) {
                return a.row != b.row ? a.row < b.row : a.column < b.column;
            });
            Q_FOREACH (const KisSwatchGroup::SwatchInfo &info, infos) entry.colors << info.swatch.color().toQColor();
            m_all << entry;
        }
        // a field to narrow them only when there are more than show at once
        m_filter->setVisible(m_all.size() > rowsShown);
        m_filter->clear();
        fill();
        if (m_filter->isVisible()) m_filter->setFocus();
    }

    void fill()
    {
        m_shown.clear();
        const QString words = m_filter->text().trimmed();
        Q_FOREACH (const Entry &entry, m_all) {
            if (words.isEmpty() || entry.set->name().contains(words, Qt::CaseInsensitive)) m_shown << entry;
        }
        const int tall = m_rows->rowHeight();
        m_rows->setFixedHeight(tall * m_shown.size());
        m_scroll->setFixedHeight(tall * qBound(1, int(m_all.size()), rowsShown));
        m_rows->update();
        adjustSize();
    }

    QLineEdit *m_filter {nullptr};
    QScrollArea *m_scroll {nullptr};
    Rows *m_rows {nullptr};
    QVector<Entry> m_all;
    QVector<Entry> m_shown;
    KoColorSetSP m_current;
};

PaletteDockerDock::PaletteDockerDock( )
    : QDockWidget(i18n("Palette"))
    , m_model(new KisPaletteModel(this))
    , m_paletteList(new PaletteList(this))
    , m_rServer(KoResourceServerProvider::instance()->paletteServer())
    , m_paletteEditor(new KisPaletteEditor)
{
    QWidget *mainWidget = new QWidget(this);
    setWidget(mainWidget);
    auto *layout = new QVBoxLayout(mainWidget);
    layout->setContentsMargins(6, 0, 6, 4);
    layout->setSpacing(KisModernStyle::space1);

    // one line: the name, which opens the other palettes; the colors last used; undo and redo while there is something to undo
    auto *header = new QHBoxLayout();
    header->setContentsMargins(0, 0, 0, 0);
    header->setSpacing(2);
    m_paletteButton = new KisPopupButton(mainWidget);
    m_paletteButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_paletteButton->setAutoRaise(true);
    m_paletteButton->setArrowVisible(false);
    m_paletteButton->setPopupWidget(m_paletteList);
    m_paletteButton->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_paletteButton, &QWidget::customContextMenuRequested, this, [this]() { showPaletteCard(m_currentColorSet, m_paletteButton); });
    header->addWidget(m_paletteButton);
    header->addSpacing(KisModernStyle::space2);
    m_recentStrip = new RecentStrip(mainWidget);
    header->addWidget(m_recentStrip, 1, Qt::AlignVCenter);
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

    m_recentUpdate.setSingleShot(true);
    m_recentUpdate.setInterval(0);
    connect(&m_recentUpdate, &QTimer::timeout, this, &PaletteDockerDock::updateRecentStrip);
    m_recentStrip->picked = [this](int index) { pickRecent(index); };
    m_recentStrip->cardRequested = [this](int index, QWidget *anchor) { showRecentCard(index, anchor); };

    m_grid = new PaletteGrid(m_model, mainWidget);
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
    m_grid->colorDropped = [this](const KoColor &color, const QModelIndex &slot) { addColor(color, slot); };
    m_grid->moved = [this](const QModelIndex &from, const QModelIndex &to) { moveSwatch(from, to); };
    m_grid->draggedOff = [this](const QModelIndex &index) { removeSwatch(index); };
    m_grid->cardRequested = [this](const QModelIndex &index, QWidget *anchor) { showSwatchCard(index, anchor); };
    layout->addWidget(m_grid, 1);

    m_paletteEditor->setPaletteModel(m_model);
    connect(m_model, SIGNAL(sigPaletteModified()), SLOT(updateHeader()));
    connect(KisResourceLocator::instance(), SIGNAL(storageRemoved(QString)), this, SLOT(slotStoragesChanged(QString)));

    // one picked: the list is done
    m_paletteList->picked = [this](KoColorSetSP palette) {
        m_paletteButton->hidePopupWidget();
        slotSetColorSet(palette);
    };
    m_paletteList->cardRequested = [this](KoColorSetSP palette, QWidget *anchor) { showPaletteCard(palette, anchor); };
    m_paletteList->newRequested = [this]() {
        m_paletteButton->hidePopupWidget();
        slotAddPalette();
    };
    m_paletteList->importRequested = [this]() {
        m_paletteButton->hidePopupWidget();
        slotImportPalette();
    };

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
    // nothing to ask: an empty palette, named, shown at once for colors to be dropped in
    QString name = i18nc("Default name for a new palette", "Palette");
    for (int number = 2; m_rServer->resource(QString(), QString(), name); number++) {
        name = i18nc("Default name for a new palette: %1 a number", "Palette %1", number);
    }
    KoColorSetSP palette(new KoColorSet());
    palette->setPaletteType(KoColorSet::KPL);
    palette->setValid(true);
    palette->setName(name);
    palette->setFilename(name.split(QLatin1Char(' ')).join(QLatin1Char('_')) + palette->defaultFileExtension());
    if (KisResourceUserOperations::addResourceWithUserInput(this, palette)) {
        slotSetColorSet(palette);
    }
}

void PaletteDockerDock::slotRemovePalette(KoColorSetSP cs)
{
    if (!cs) return;
    const bool shown = m_currentColorSet && samePalette(cs, m_currentColorSet);
    m_rServer->removeResourceFromServer(cs);
    if (!shown) return;
    // the next one shows in its place
    KisResourceModel model(ResourceType::Palettes);
    slotSetColorSet(model.rowCount() > 0 ? model.resourceForIndex(model.index(0, 0)).dynamicCast<KoColorSet>() : KoColorSetSP());
}

void PaletteDockerDock::slotImportPalette()
{
    KoColorSetSP palette = m_paletteEditor->importPalette();
    if (palette) {
        slotSetColorSet(palette);
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
    // the file is the sign it worked
    if (!palette->saveToDevice(&file)) {
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
    m_paletteList->setCurrent(colorSet);
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
        const QString name = fontMetrics().elidedText(m_currentColorSet->name(), Qt::ElideMiddle, 120);
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

void PaletteDockerDock::addColor(const KoColor &color, const QModelIndex &emptySlot)
{
    if (!canEdit() || !m_resourceProvider) {
        return;
    }
    KisSwatch swatch = newSwatch(color);
    if (emptySlot.isValid() && isOccupied(emptySlot)) {
        swatch.setName(m_model->getSwatch(emptySlot).name());
    }
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

void PaletteDockerDock::showPaletteCard(KoColorSetSP palette, QWidget *anchor)
{
    KisMainWindow *mainWindow = m_view ? m_view->mainWindow() : nullptr;
    if (!mainWindow || !palette) {
        return;
    }
    auto *card = new KisStudioItemCard(mainWindow, KisIconUtils::loadIcon(QStringLiteral("palette-library")),
                                       palette->name(), nullptr);
    const bool shown = m_currentColorSet && samePalette(palette, m_currentColorSet);
    const bool locked = palette->isLocked();
    if (shown && !locked && m_view->document()) {
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
                      [this, palette, locked]() {
                          palette->setLocked(!locked);
                          updateHeader();
                      },
                      true, locked});
    if (shown && m_paletteEditor->isModified() && isGlobal()) {
        card->addCommand({KisIconUtils::loadIcon(QStringLiteral("document-save")),
                          i18nc("@info:tooltip", "Save"), [this]() {
                              m_paletteEditor->saveNewPaletteVersion();
                              updateHeader();
                          }});
    }
    card->addCommand({KisIconUtils::loadIcon(QStringLiteral("document-export")),
                      i18nc("@info:tooltip", "Export to a file"), [this, palette]() {
                          m_paletteButton->hidePopupWidget();
                          slotExportPalette(palette);
                      }});
    if (!locked) {
        card->addCommand({KisIconUtils::loadIcon(QStringLiteral("edit-delete")), i18nc("@info:tooltip", "Remove"), [this, palette]() {
                              m_paletteButton->hidePopupWidget();
                              slotRemovePalette(palette);
                          }});
    }
    card->popupBeside(anchor);
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
    KisSettingsRegistry::writeLater(recentKey, names);
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
