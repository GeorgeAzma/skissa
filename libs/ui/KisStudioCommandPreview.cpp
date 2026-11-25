/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioCommandPreview.h"

#include <QApplication>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QTimer>

#include "KisMainWindow.h"
#include "KisPart.h"
#include "KisView.h"
#include "KisViewManager.h"
#include "canvas/kis_canvas2.h"
#include "canvas/kis_canvas_decoration.h"
#include "canvas/kis_coordinates_converter.h"
#include "kis_canvas_resource_provider.h"
#include "kis_image.h"
#include "kis_paint_device.h"
#include "kis_selection.h"

#include <cmath>

namespace
{
const QString decorationId = QStringLiteral("studioCommandPreview");

enum class Kind { None, Crop, Fill, Clear };

class PreviewDecoration : public KisCanvasDecoration
{
public:
    explicit PreviewDecoration(QPointer<KisView> view)
        : KisCanvasDecoration(decorationId, view)
    {
        setPriority(90);
        setVisible(true);
    }

    void set(Kind kind, const QPainterPath &area, const QColor &color, const QImage &gone = QImage(), const QRectF &goneRect = QRectF())
    {
        m_kind = kind;
        m_area = area;
        m_color = color;
        m_gone = gone;
        m_goneRect = goneRect;
    }

    Kind kind() const { return m_kind; }

protected:
    void drawDecoration(QPainter &gc, const QRectF &, const KisCoordinatesConverter *converter, KisCanvas2 *) override
    {
        if (m_kind == Kind::None || m_area.isEmpty()) return;
        gc.save();
        gc.setRenderHint(QPainter::Antialiasing, true);
        const QPainterPath area = converter->imageToWidgetTransform().map(m_area);
        if (m_kind == Kind::Crop) {
            // what would be cut away goes dark; the edge stays readable on any picture
            QPainterPath everything;
            everything.addRect(QRectF(-1e6, -1e6, 2e6, 2e6));
            gc.fillPath(everything - area, QColor(0, 0, 0, 150));
            gc.setPen(QPen(QColor(255, 255, 255, 200), 1));
            gc.drawPath(area);
        } else if (m_kind == Kind::Clear) {
            // where the layer's paint would go, the see-through checkers show instead
            gc.setClipPath(area);
            gc.setTransform(converter->imageToWidgetTransform(), true);
            gc.setRenderHint(QPainter::SmoothPixmapTransform, true);
            gc.drawImage(m_goneRect, m_gone);
        } else {
            gc.fillPath(area, m_color);
        }
        gc.restore();
    }

private:
    Kind m_kind {Kind::None};
    QPainterPath m_area;
    QColor m_color;
    QImage m_gone;
    QRectF m_goneRect;
};

/// The layer's paint in \p rect as checkers: squares about 8 screen px at \p zoom, as see-through is drawn
QImage checkersOf(KisPaintDeviceSP device, const QRect &rect, qreal zoom)
{
    const qreal shrink = qMin<qreal>(1, std::sqrt(4e6 / qMax<qreal>(1, qreal(rect.width()) * rect.height())));
    const QSize size(qMax(1, qRound(rect.width() * shrink)), qMax(1, qRound(rect.height() * shrink)));
    QImage paint = shrink < 1 ? device->createThumbnailUncached(size.width(), size.height(), rect)
                              : device->convertToQImage(nullptr, rect.x(), rect.y(), rect.width(), rect.height());
    if (paint.isNull()) return QImage();
    paint = paint.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    QImage checkers(paint.size(), QImage::Format_ARGB32_Premultiplied);
    const int square = qMax(1, qRound(8 / qMax<qreal>(1e-3, zoom) * shrink));
    QPainter painter(&checkers);
    for (int y = 0; y < checkers.height(); y += square) {
        for (int x = 0; x < checkers.width(); x += square) {
            painter.fillRect(x, y, square, square, ((x / square + y / square) % 2) ? QColor(204, 204, 204) : QColor(255, 255, 255));
        }
    }
    painter.setCompositionMode(QPainter::CompositionMode_DestinationIn);
    painter.drawImage(0, 0, paint);
    return checkers;
}

Kind kindOf(const QString &id)
{
    if (id == QLatin1String("resizeimagetoselection")) return Kind::Crop;
    if (id == QLatin1String("clear")) return Kind::Clear;
    if (id.startsWith(QLatin1String("fill_selection_foreground_color")) || id.startsWith(QLatin1String("fill_selection_background_color"))) {
        return Kind::Fill;
    }
    return Kind::None;
}

/// The command a menu entry runs: the entry itself, or the action a short entry stands in for
QString commandOf(QAction *entry)
{
    if (!entry) return QString();
    if (QObject *target = entry->property("kisShortcutTarget").value<QObject *>()) return target->objectName();
    return entry->objectName();
}

void show(const QString &id)
{
    KisMainWindow *window = KisPart::instance()->currentMainwindow();
    KisView *view = window ? window->activeView() : nullptr;
    KisCanvas2 *canvas = view ? view->canvasBase() : nullptr;
    if (!canvas || !view->image()) return;

    Kind kind = kindOf(id);
    KisSelectionSP selection = view->viewManager()->selection();
    QPainterPath area;
    QColor color;
    QImage gone;
    QRectF goneRect;
    if (kind == Kind::Clear) {
        KisPaintDeviceSP device = view->viewManager()->activeDevice();
        QRect rect = device ? device->exactBounds() : QRect();
        if (selection) rect &= selection->selectedExactRect();
        if (rect.isEmpty()) {
            kind = Kind::None;
        } else {
            if (selection && selection->outlineCacheValid()) area = selection->outlineCache();
            else area.addRect(rect);
            gone = checkersOf(device, rect, canvas->coordinatesConverter()->effectiveZoom());
            goneRect = rect;
            if (gone.isNull()) kind = Kind::None;
        }
    } else if (kind == Kind::Crop) {
        const QRect rect = selection ? selection->selectedExactRect() : QRect();
        if (rect.isEmpty()) kind = Kind::None;
        else area.addRect(rect);
    } else if (kind == Kind::Fill) {
        if (selection && selection->outlineCacheValid()) area = selection->outlineCache();
        else area.addRect(selection ? selection->selectedExactRect() : view->image()->bounds());
        KisCanvasResourceProvider *resources = view->viewManager()->canvasResourceProvider();
        color = (id.startsWith(QLatin1String("fill_selection_background")) ? resources->bgColor() : resources->fgColor()).toQColor();
    }

    KisCanvasDecorationSP existing = canvas->decoration(decorationId);
    auto *decoration = dynamic_cast<PreviewDecoration *>(existing.data());
    if (!decoration) {
        if (kind == Kind::None) return;
        decoration = new PreviewDecoration(view);
        canvas->addDecoration(decoration);
    }
    if (kind == Kind::None && decoration->kind() == Kind::None) return;
    decoration->set(kind, area, color, gone, goneRect);
    canvas->updateCanvas();
}

class MenuWatcher : public QObject
{
public:
    using QObject::QObject;

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        auto *menu = qobject_cast<QMenu *>(watched);
        if (!menu) return false;
        switch (event->type()) {
        case QEvent::MouseMove:
        case QEvent::KeyRelease:
        case QEvent::Leave:
        case QEvent::Hide: {
            // once the menu has settled which entry is lit
            QPointer<QMenu> guarded = menu;
            QTimer::singleShot(0, this, [guarded]() {
                show(guarded && guarded->isVisible() ? commandOf(guarded->activeAction()) : QString());
            });
            break;
        }
        default:
            break;
        }
        return false;
    }
};
}

void KisStudioCommandPreview::install()
{
    static QPointer<MenuWatcher> watcher;
    if (watcher) return;
    watcher = new MenuWatcher(qApp);
    qApp->installEventFilter(watcher);
}
