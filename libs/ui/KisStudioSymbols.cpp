/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioSymbols.h"

#include <QApplication>
#include <QDrag>
#include <QHash>
#include <QLineEdit>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QScrollArea>
#include <QToolTip>
#include <QVBoxLayout>

#include <klocalizedstring.h>

#include <KisModernStyle.h>
#include <KisResourceModel.h>
#include <KisResourceTypes.h>
#include <KoShape.h>
#include <KoShapeController.h>
#include <kis_config.h>
#include <kis_coordinates_converter.h>
#include <kundo2command.h>
#include <resources/KoSvgSymbolCollectionResource.h>

#include "KisMainWindow.h"
#include "KisPart.h"
#include "KisSettingsRegistry.h"
#include "KisView.h"
#include "KisViewManager.h"
#include "canvas/kis_canvas2.h"
#include "widgets/KisStudioChips.h"
#include "widgets/KisStudioPopup.h"

namespace
{
const QString mimeType = QStringLiteral("application/x-skissa-symbol");
const QString recentKey = QStringLiteral("StudioRecentSymbols");
constexpr int recentKept = 24;
constexpr int cell = 64;

struct Symbol {
    QString key; ///< its collection's md5 and its id
    QString title;
    QSharedPointer<KoSvgSymbolCollectionResource> collection;
    KoSvgSymbol *symbol {nullptr};
};

struct Collection {
    QString title;
    QVector<Symbol> symbols;
};

QVector<Collection> collections()
{
    QVector<Collection> all;
    KisResourceModel model(ResourceType::Symbols);
    for (int row = 0; row < model.rowCount(); row++) {
        auto resource = model.resourceForIndex(model.index(row, 0)).dynamicCast<KoSvgSymbolCollectionResource>();
        if (!resource || resource->symbols().isEmpty()) continue;
        Collection collection;
        collection.title = resource->title().isEmpty() ? resource->name() : resource->title();
        for (KoSvgSymbol *symbol : resource->symbols()) {
            collection.symbols.append({resource->md5Sum() + QLatin1Char('\t') + symbol->id, symbol->title, resource, symbol});
        }
        all.append(collection);
    }
    return all;
}

Symbol symbolOf(const QString &key)
{
    for (const Collection &collection : collections()) {
        for (const Symbol &symbol : collection.symbols) {
            if (symbol.key == key) return symbol;
        }
    }
    return {};
}

void recordUse(const QString &key)
{
    QStringList recent = KisConfig(true).readEntry<QStringList>(recentKey, QStringList());
    recent.removeAll(key);
    recent.prepend(key);
    KisSettingsRegistry::writeLater(recentKey, recent.mid(0, recentKept));
}

/// Puts \p symbol on \p view's image with its middle at \p at (document coordinates), a quarter of the view tall
void place(KisView *view, const Symbol &symbol, const QPointF &at)
{
    KisCanvas2 *canvas = view ? view->canvasBase() : nullptr;
    if (!canvas || !symbol.symbol || !symbol.symbol->shape) return;
    KoShape *shape = symbol.symbol->shape->cloneShape();
    if (!shape) return;
    const QRectF bounds = shape->boundingRect();
    const QRectF seen = canvas->coordinatesConverter()->widgetToDocument(QRectF(canvas->canvasWidget()->rect()));
    const qreal side = qMax(bounds.width(), bounds.height());
    if (side > 0) {
        const qreal scale = qMin(seen.width(), seen.height()) / 4 / side;
        shape->applyAbsoluteTransformation(QTransform::fromScale(scale, scale));
    }
    shape->setAbsolutePosition(at, KoFlake::Center);
    // on the vector layer in use, or a new one
    if (KUndo2Command *command = canvas->shapeController()->addShapeDirect(shape, nullptr)) {
        canvas->addCommand(command);
    } else {
        delete shape;
    }
    recordUse(symbol.key);
}

/// The symbols as their pictures, on light tiles (most are drawn in black); a click places one, a drag carries it off
class SymbolGrid : public QWidget
{
public:
    std::function<void(const Symbol &)> picked;

    explicit SymbolGrid(QWidget *parent)
        : QWidget(parent)
    {
        setMouseTracking(true);
    }

    void setSymbols(const QVector<Symbol> &symbols)
    {
        m_symbols = symbols;
        m_pictures.clear();
        m_hover = -1;
        updateGeometry();
        resize(width(), heightForWidth(width()));
        update();
    }

    bool hasHeightForWidth() const override { return true; }

    int heightForWidth(int width) const override
    {
        const int columns = qMax(1, width / cell);
        return ((m_symbols.size() + columns - 1) / columns) * cell;
    }

    QSize sizeHint() const override { return QSize(5 * cell, heightForWidth(5 * cell)); }

protected:
    int at(const QPoint &pos) const
    {
        const int columns = qMax(1, width() / cell);
        const int column = pos.x() / cell;
        if (column >= columns || pos.x() < 0 || pos.y() < 0) return -1;
        const int index = (pos.y() / cell) * columns + column;
        return index < m_symbols.size() ? index : -1;
    }

    QRect rectOf(int index) const
    {
        const int columns = qMax(1, width() / cell);
        return QRect((index % columns) * cell, (index / columns) * cell, cell, cell);
    }

    void paintEvent(QPaintEvent *event) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        const qreal ratio = devicePixelRatioF();
        for (int i = 0; i < m_symbols.size(); i++) {
            const QRect rect = rectOf(i);
            if (!rect.intersects(event->rect())) continue;
            const QRectF tile = QRectF(rect).adjusted(3, 3, -3, -3);
            QColor paper(245, 243, 238);
            if (i != m_hover) paper.setAlphaF(0.88);
            painter.fillPath(KisModernStyle::squircle(tile, KisModernStyle::tileRadius), paper);
            QImage &picture = m_pictures[i];
            if (picture.isNull()) picture = m_symbols[i].symbol->icon(qRound((cell - 16) * ratio));
            const QSizeF size = QSizeF(picture.size()) / ratio;
            painter.drawImage(QRectF(tile.center() - QPointF(size.width(), size.height()) / 2, size), picture);
        }
    }

    bool event(QEvent *event) override
    {
        if (event->type() == QEvent::ToolTip) {
            auto *help = static_cast<QHelpEvent *>(event);
            const int index = at(help->pos());
            if (index < 0) QToolTip::hideText();
            else QToolTip::showText(help->globalPos(), m_symbols[index].title, this);
            return true;
        }
        return QWidget::event(event);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        m_press = event->button() == Qt::LeftButton ? at(event->pos()) : -1;
        m_pressPos = event->pos();
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (!(event->buttons() & Qt::LeftButton)) {
            const int hover = at(event->pos());
            if (hover != m_hover) {
                m_hover = hover;
                setCursor(hover >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
                update();
            }
            return;
        }
        if (m_press < 0 || (event->pos() - m_pressPos).manhattanLength() < KisModernStyle::dragDistance(event)) return;
        // carried to the canvas: put where it is dropped
        const Symbol dragged = m_symbols[m_press];
        m_press = -1;
        auto *drag = new QDrag(qApp);
        auto *mime = new QMimeData();
        mime->setData(mimeType, dragged.key.toUtf8());
        drag->setMimeData(mime);
        drag->setPixmap(QPixmap::fromImage(dragged.symbol->icon(48)));
        QPointer<QWidget> popup = window();
        popup->hide();
        drag->exec(Qt::CopyAction);
        drag->deleteLater();
        if (popup) popup->close();
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        const int index = at(event->pos());
        if (event->button() == Qt::LeftButton && index >= 0 && index == m_press && picked) picked(m_symbols[index]);
        m_press = -1;
    }

    void leaveEvent(QEvent *) override
    {
        m_hover = -1;
        update();
    }

private:
    QVector<Symbol> m_symbols;
    QHash<int, QImage> m_pictures;
    int m_hover {-1};
    int m_press {-1};
    QPoint m_pressPos;
};
} // namespace

void KisStudioSymbols::openPicker(QWidget *anchor)
{
    const QVector<Collection> all = collections();
    auto *popup = new KisStudioPopup(anchor->window());
    popup->setAttribute(Qt::WA_DeleteOnClose);
    auto *layout = new QVBoxLayout(popup);
    popup->fitLayout(layout, KisModernStyle::space2);
    layout->setSpacing(KisModernStyle::space2);

    auto *search = new QLineEdit(popup);
    search->setPlaceholderText(i18nc("@info:placeholder", "Search"));
    search->setProperty(KisModernStyle::quietFieldProperty, true);
    layout->addWidget(search);

    // the ones used lately, then each collection
    QVector<QVector<Symbol>> groups;
    QVector<KisStudioChips::Chip> chips;
    QVector<Symbol> recent;
    for (const QString &key : KisConfig(true).readEntry<QStringList>(recentKey, QStringList())) {
        const Symbol symbol = symbolOf(key);
        if (symbol.symbol) recent.append(symbol);
    }
    if (!recent.isEmpty()) {
        chips.append({i18nc("symbols used lately", "Recent"), QString()});
        groups.append(recent);
    }
    for (const Collection &collection : all) {
        chips.append({collection.title, QString()});
        groups.append(collection.symbols);
    }
    auto *tabs = new KisStudioChips(popup);
    tabs->setChips(chips);
    layout->addWidget(tabs);

    auto *scroll = new QScrollArea(popup);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->viewport()->setAutoFillBackground(false);
    auto *grid = new SymbolGrid(scroll);
    scroll->setWidget(grid);
    layout->addWidget(scroll, 1);

    QPointer<KisStudioPopup> guarded = popup;
    grid->picked = [guarded](const Symbol &symbol) {
        KisView *view = KisPart::instance()->currentMainwindow() ? KisPart::instance()->currentMainwindow()->activeView() : nullptr;
        if (view && view->canvasBase()) {
            KisCanvas2 *canvas = view->canvasBase();
            place(view, symbol, canvas->coordinatesConverter()->widgetToDocument(QPointF(canvas->canvasWidget()->rect().center())));
        }
        if (guarded) guarded->close();
    };
    auto showGroup = [grid, tabs, groups](int index) {
        if (index < 0 || index >= groups.size()) return;
        tabs->setCurrent(index);
        grid->setSymbols(groups[index]);
    };
    QObject::connect(tabs, &KisStudioChips::clicked, popup, showGroup);
    // a search looks through every collection
    QObject::connect(search, &QLineEdit::textChanged, popup, [grid, tabs, all, showGroup](const QString &text) {
        if (text.trimmed().isEmpty()) {
            showGroup(0);
            return;
        }
        QVector<Symbol> found;
        for (const Collection &collection : all) {
            for (const Symbol &symbol : collection.symbols) {
                if (symbol.title.contains(text.trimmed(), Qt::CaseInsensitive)) found.append(symbol);
            }
        }
        tabs->setCurrent(-1);
        grid->setSymbols(found);
    });
    showGroup(0);

    popup->resize(QSize(5 * cell + 2 * KisModernStyle::space2 + 16, 440) + QSize(2, 2) * popup->shadowMargin());
    popup->popupBeside(anchor, Qt::BottomEdge);
    search->setFocus();
}

bool KisStudioSymbols::carriesSymbol(const QMimeData *data)
{
    return data && data->hasFormat(mimeType);
}

void KisStudioSymbols::drop(KisView *view, const QMimeData *data, const QPoint &pos)
{
    KisCanvas2 *canvas = view ? view->canvasBase() : nullptr;
    if (!canvas || !carriesSymbol(data)) return;
    const Symbol symbol = symbolOf(QString::fromUtf8(data->data(mimeType)));
    const QPointF onCanvas = canvas->canvasWidget()->mapFrom(view, pos);
    place(view, symbol, canvas->coordinatesConverter()->widgetToDocument(onCanvas));
}
