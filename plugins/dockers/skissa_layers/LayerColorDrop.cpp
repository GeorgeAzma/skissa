/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "LayerColorDrop.h"

#include <QApplication>
#include <QDropEvent>
#include <QPainter>
#include <QPointer>
#include <QStyleOptionViewItem>
#include <QThreadPool>
#include <QtMath>

#include <kundo2command.h>

#include <KisColorDrop.h>
#include <KisView.h>
#include <canvas/kis_canvas2.h>
#include <canvas/kis_canvas_decoration.h>
#include <kis_coordinates_converter.h>
#include <kis_image.h>
#include <kis_node.h>
#include <kis_node_filter_proxy_model.h>
#include <kis_node_view_color_scheme.h>
#include <kis_undo_adapter.h>

#include "LayerBox.h"
#include "NodeDelegate.h"
#include "NodeView.h"
#include "ui_WdgLayerBox.h"

namespace {

const QString previewId = QStringLiteral("layerRecolorPreview");

/// The layer's paint in the dropped color, over the canvas while the color is held on its picture
class RecolorPreview : public KisCanvasDecoration
{
public:
    explicit RecolorPreview(QPointer<KisView> view)
        : KisCanvasDecoration(previewId, view)
    {
        setVisible(true);
    }

    void set(const QImage &shape, const QRect &rect, const QColor &color, qreal opacity)
    {
        if (shape.cacheKey() != m_shapeKey || color != m_color) {
            m_shapeKey = shape.cacheKey();
            m_color = color;
            m_picture = shape;
            if (!m_picture.isNull()) {
                QPainter painter(&m_picture);
                painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
                painter.fillRect(m_picture.rect(), color);
            }
        }
        m_rect = rect;
        m_opacity = opacity;
    }

protected:
    void drawDecoration(QPainter &gc, const QRectF &, const KisCoordinatesConverter *converter, KisCanvas2 *) override
    {
        if (m_picture.isNull()) return;
        gc.save();
        gc.setTransform(converter->imageToWidgetTransform(), false);
        gc.setRenderHint(QPainter::SmoothPixmapTransform, true);
        gc.setOpacity(m_opacity);
        gc.drawImage(QRectF(m_rect), m_picture);
        gc.restore();
    }

private:
    QImage m_picture;
    qint64 m_shapeKey {0};
    QColor m_color;
    QRect m_rect;
    qreal m_opacity {1.0};
};

/// A row's color label, undone with the image
class LabelCommand : public KUndo2Command
{
public:
    LabelCommand(KisNodeSP node, int label)
        : KUndo2Command(kundo2_i18n("Color Label"))
        , m_node(node)
        , m_before(node->colorLabelIndex())
        , m_label(label)
    {
    }

    void redo() override { m_node->setColorLabelIndex(m_label); }
    void undo() override { m_node->setColorLabelIndex(m_before); }

private:
    KisNodeSP m_node;
    int m_before;
    int m_label;
};

/// OKLab, to find the label nearest a color as people see it
QVector<qreal> oklab(const QColor &color)
{
    auto linear = [](qreal c) { return c <= 0.04045 ? c / 12.92 : qPow((c + 0.055) / 1.055, 2.4); };
    const qreal r = linear(color.redF()), g = linear(color.greenF()), b = linear(color.blueF());
    const qreal l = std::cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b);
    const qreal m = std::cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b);
    const qreal s = std::cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b);
    return {0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s,
            1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
            0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s};
}

int nearestLabel(const QColor &color)
{
    const QVector<QColor> labels = KisNodeViewColorScheme::instance()->allColorLabels();
    const QVector<qreal> want = oklab(color);
    int best = -1;
    qreal bestDistance = 0;
    for (int i = 0; i < labels.size(); i++) {
        if (labels[i].alpha() == 0) continue;
        const QVector<qreal> have = oklab(labels[i]);
        const qreal distance = qPow(want[0] - have[0], 2) + qPow(want[1] - have[1], 2) + qPow(want[2] - have[2], 2);
        if (best < 0 || distance < bestDistance) {
            best = i;
            bestDistance = distance;
        }
    }
    return qMax(best, 0);
}

}

struct LayerColorDrop::Private
{
    LayerBox *box {nullptr};
    /// The drag is over a row, here, not with the list
    bool mine {false};
    bool forwarding {false};

    /// What the canvas preview shows, worked out on a thread once per layer
    KisNodeWSP shapeOf;
    QImage shape;
    QRect shapeRect;
    QColor color;
    int generation {0};

    NodeView *view() const { return box->m_wdgLayerBox->listLayers; }
    NodeDelegate *delegate() const { return qobject_cast<NodeDelegate *>(view()->itemDelegate()); }
    KisNodeSP nodeAt(const QModelIndex &row) const
    {
        return box->m_filteringModel && row.isValid() ? box->m_filteringModel->nodeFromIndex(row) : KisNodeSP();
    }

    RecolorPreview *preview(bool make) const
    {
        KisCanvas2 *canvas = box->m_canvas;
        if (!canvas) return nullptr;
        KisCanvasDecorationSP decoration = canvas->decoration(previewId);
        if (!decoration && make && canvas->imageView()) {
            decoration = new RecolorPreview(canvas->imageView());
            canvas->addDecoration(decoration);
        }
        return dynamic_cast<RecolorPreview *>(decoration.data());
    }
};

LayerColorDrop::LayerColorDrop(LayerBox *box)
    : QObject(box)
    , d(new Private)
{
    d->box = box;
}

LayerColorDrop::~LayerColorDrop()
{
    delete d;
}

LayerColorDrop::Part LayerColorDrop::partAt(QDropEvent *event, QModelIndex *row) const
{
    NodeView *view = d->view();
    NodeDelegate *delegate = d->delegate();
    const QPoint pos = event->pos();
    const QModelIndex at = static_cast<QAbstractItemView *>(view)->indexAt(pos);
    if (!delegate || !at.isValid()) return Part::None;
    const QModelIndex main = at.sibling(at.row(), NodeView::DEFAULT_COL);
    const QRect rect = view->visualRect(main);
    // near a row's edge the drop goes between rows, where the list makes a fill layer; the band Qt uses
    const int margin = qBound(2, qRound(qreal(rect.height()) / 5.5), 12);
    if (pos.y() < rect.top() + margin || pos.y() > rect.bottom() - margin) return Part::None;
    KisNodeSP node = d->nodeAt(main);
    if (!node) return Part::None;
    *row = main;
    QStyleOptionViewItem option;
    option.rect = rect;
    option.direction = view->layoutDirection();
    if (delegate->thumbnailClickRect(option, main).contains(pos) && KisColorDrop::canRecolor(node)) return Part::Paint;
    return Part::Row;
}

void LayerColorDrop::show(Part part, const QModelIndex &row, const KoColor &color)
{
    const QColor shown = color.toQColor();
    if (NodeDelegate *delegate = d->delegate()) {
        if (part == Part::Paint) {
            delegate->setDropPreview(row, shown);
        } else {
            delegate->setDropPreview(row, QColor(), nearestLabel(shown));
        }
        d->view()->viewport()->update();
    }

    KisNodeSP node = d->nodeAt(row);
    RecolorPreview *preview = d->preview(part == Part::Paint);
    if (!preview) return;
    if (part != Part::Paint || !node) {
        preview->set(QImage(), QRect(), QColor(), 1.0);
        d->box->m_canvas->updateCanvasDecorations();
        return;
    }
    if (d->shapeOf.data() != node.data()) {
        // the layer's shape, made away from the window's thread; until it comes, nothing over the canvas
        d->shapeOf = node;
        d->shape = QImage();
        const int generation = ++d->generation;
        QPointer<LayerColorDrop> self(this);
        QThreadPool::globalInstance()->start([self, node, generation]() {
            QRect rect;
            const QImage shape = KisColorDrop::recolorShape(node, 2048, &rect);
            QMetaObject::invokeMethod(qApp, [self, shape, rect, generation]() {
                if (!self || self->d->generation != generation) return;
                self->d->shape = shape;
                self->d->shapeRect = rect;
                KisNodeSP node = self->d->shapeOf;
                RecolorPreview *preview = self->d->preview(false);
                if (self->d->mine && preview && node) {
                    preview->set(shape, rect, self->d->color, node->opacity() / 255.0);
                    self->d->box->m_canvas->updateCanvasDecorations();
                }
            }, Qt::QueuedConnection);
        });
    }
    d->color = shown;
    preview->set(d->shape, d->shapeRect, shown, node->opacity() / 255.0);
    d->box->m_canvas->updateCanvasDecorations();
}

void LayerColorDrop::clear()
{
    if (NodeDelegate *delegate = d->delegate()) {
        delegate->setDropPreview(QModelIndex());
        d->view()->viewport()->update();
    }
    d->shapeOf = nullptr;
    d->shape = QImage();
    d->generation++;
    if (RecolorPreview *preview = d->preview(false)) {
        preview->set(QImage(), QRect(), QColor(), 1.0);
        d->box->m_canvas->updateCanvasDecorations();
    }
}

void LayerColorDrop::giveToList(QDropEvent *event)
{
    QDragEnterEvent enter(event->pos(), event->possibleActions(), event->mimeData(), event->mouseButtons(), event->keyboardModifiers());
    d->forwarding = true;
    QApplication::sendEvent(d->view()->viewport(), &enter);
    d->forwarding = false;
}

void LayerColorDrop::takeFromList(QDropEvent *)
{
    QDragLeaveEvent leave;
    d->forwarding = true;
    QApplication::sendEvent(d->view()->viewport(), &leave);
    d->forwarding = false;
}

bool LayerColorDrop::filter(QEvent *event)
{
    if (d->forwarding) return false;
    switch (event->type()) {
    case QEvent::DragEnter:
    case QEvent::DragMove: {
        auto *drop = static_cast<QDropEvent *>(event);
        KoColor color;
        if (!KisColorDrop::colorOf(drop->mimeData(), &color)) return false;
        const bool enter = event->type() == QEvent::DragEnter;
        QModelIndex row;
        const Part part = partAt(drop, &row);
        if (part == Part::None) {
            clear();
            if (d->mine) {
                d->mine = false;
                if (!enter) giveToList(drop);
            }
            if (enter) {
                // the list may turn it down where it entered; kept here, the drag goes on reaching the rows
                d->forwarding = true;
                QApplication::sendEvent(d->view()->viewport(), event);
                d->forwarding = false;
                drop->accept();
                return true;
            }
            return false;
        }
        if (!d->mine && !enter) takeFromList(drop);
        d->mine = true;
        show(part, row, color);
        drop->setDropAction(Qt::CopyAction);
        drop->accept();
        return true;
    }
    case QEvent::DragLeave:
        clear();
        if (!d->mine) return false;
        d->mine = false;
        return true;
    case QEvent::Drop: {
        auto *drop = static_cast<QDropEvent *>(event);
        if (!d->mine) {
            clear();
            return false;
        }
        d->mine = false;
        KoColor color;
        QModelIndex row;
        const Part part = KisColorDrop::colorOf(drop->mimeData(), &color) ? partAt(drop, &row) : Part::None;
        clear();
        KisNodeSP node = d->nodeAt(row);
        KisImageSP image = d->box->m_image;
        if (node && image) {
            if (part == Part::Paint) {
                KisColorDrop::recolor(image, node, color);
            } else if (part == Part::Row) {
                const int label = nearestLabel(color.toQColor());
                if (label != node->colorLabelIndex()) image->undoAdapter()->addCommand(new LabelCommand(node, label));
            }
        }
        drop->setDropAction(Qt::CopyAction);
        drop->accept();
        return true;
    }
    default:
        return false;
    }
}
