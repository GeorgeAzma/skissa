/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisColorDrop.h"

#include <QApplication>
#include <QDrag>
#include <QFutureWatcher>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QTabletEvent>
#include <QTimer>
#include <QToolButton>
#include <new>

#include <QtConcurrent>

#include <KoColor.h>
#include <KoColorSpace.h>
#include <KoColorSpaceRegistry.h>
#include <KoCompositeOpRegistry.h>
#include <KoCanvasController.h>
#include <KoToolManager.h>
#include <KisSwatch.h>
#include <kconfiggroup.h>
#include <klocalizedstring.h>
#include <ksharedconfig.h>

#include "KisView.h"
#include "KisViewManager.h"
#include "canvas/kis_canvas2.h"
#include "canvas/kis_canvas_decoration.h"
#include "floodfill/KisLineArtFill.h"
#include "kis_canvas_resource_provider.h"
#include "kis_coordinates_converter.h"
#include "kis_fill_painter.h"
#include "kis_group_layer.h"
#include "kis_icon.h"
#include "kis_image.h"
#include "kis_paint_layer.h"
#include "kis_sequential_iterator.h"
#include "kis_pixel_selection.h"
#include "kis_resources_snapshot.h"
#include "kis_selection.h"
#include "kis_transaction.h"
#include "processing/fill_processing_visitor.h"
#include <commands/kis_image_layer_add_command.h>
#include <commands_new/kis_processing_command.h>
#include <commands_new/kis_update_command.h>
#include <kis_command_utils.h>
#include <kis_stroke_strategy_undo_command_based.h>

namespace
{

/// The settings a drop shares with the fill tool.
struct Options {
    int threshold {8};
    int closeGap {0};
    bool underLines {true};
    int speckSize {4};

    static Options read()
    {
        const KConfigGroup group = KSharedConfig::openConfig()->group("KritaFill/KisToolFill");
        Options o;
        o.threshold = qBound(1, group.readEntry<int>("thresholdAmount", 8), 100);
        o.closeGap = qBound(0, group.readEntry<int>("closeGapAmount", 0), 32);
        o.underLines = group.readEntry<bool>("fillUnderLines", true);
        o.speckSize = qBound(0, group.readEntry<int>("speckSize", 4), 32);
        return o;
    }
};

QString colorsLayerName()
{
    return i18nc("@item:layer name: flat colors under line art", "Colors");
}

/**
 * The gap to close: the fill tool's, or one sized from the lines near \p point (a small one without lines
 * nearby, for gaps further away). \p share picks which lines: flats size it from the bolder ones, since
 * fine hatching beside them would make every gap look small.
 */
int gapFor(const Options &options, KisPaintDeviceSP reference, const QPoint &point, const QRect &bounds, qreal share = 0.5)
{
    if (options.closeGap > 0) return options.closeGap;
    const int width = KisLineArtFill::lineWidthNear(reference, point, bounds, share);
    return width > 0 ? qBound(4, 2 * width, 12) : 4;
}

/// What a fill from \p point would cover.
KisPixelSelectionSP regionAt(KisPaintDeviceSP reference, const QPoint &point, int threshold, int closeGap, const Options &options,
                             const QRect &bounds)
{
    KisFillPainter painter;
    painter.setFillThreshold(threshold);
    painter.setOpacitySpread(100);
    painter.setCloseGap(closeGap);
    painter.setLineArt(options.underLines, options.speckSize);
    painter.setAntiAlias(false);
    painter.setCareForSelection(false);
    painter.setUseCompositing(true);
    painter.setWidth(bounds.width());
    painter.setHeight(bounds.height());
    return painter.createFloodSelection(new KisPixelSelection(), point.x(), point.y(), reference, nullptr);
}

/// \p region tinted with \p color, at most 1600 pixels across.
QImage tintedOverlay(KisPixelSelectionSP region, const QRect &rect, const QColor &color)
{
    if (rect.isEmpty()) return QImage();
    QImage mask(rect.size(), QImage::Format_Alpha8);
    QVector<quint8> bytes(rect.width() * rect.height());
    region->readBytes(bytes.data(), rect);
    for (int y = 0; y < rect.height(); y++) memcpy(mask.scanLine(y), bytes.constData() + y * rect.width(), rect.width());
    const qreal scale = qMin<qreal>(1.0, 1600.0 / qMax(rect.width(), rect.height()));
    if (scale < 1) mask = mask.scaled((QSizeF(rect.size()) * scale).toSize().expandedTo(QSize(1, 1)), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    QImage overlay(mask.size(), QImage::Format_ARGB32_Premultiplied);
    QColor tint = color;
    tint.setAlphaF(0.72);
    overlay.fill(tint);
    QPainter painter(&overlay);
    painter.setCompositionMode(QPainter::CompositionMode_DestinationIn);
    painter.drawImage(0, 0, mask);
    return overlay;
}

/// The area under the color as the drag goes, and how far it spreads while it is tuned.
class Preview : public KisCanvasDecoration
{
public:
    explicit Preview(QPointer<KisView> view)
        : KisCanvasDecoration(QStringLiteral("colorDropPreview"), view)
    {
        setVisible(true);
    }

    QImage overlay;
    QRect rect;
    bool tuning {false};
    qreal spread {0};
    QPointF pointer;
    QColor color;

protected:
    void drawDecoration(QPainter &gc, const QRectF &, const KisCoordinatesConverter *converter, KisCanvas2 *) override
    {
        if (!overlay.isNull()) {
            gc.save();
            gc.setTransform(converter->imageToWidgetTransform(), false);
            gc.setRenderHint(QPainter::SmoothPixmapTransform, true);
            gc.drawImage(QRectF(rect), overlay);
            gc.restore();
        }
        if (tuning) {
            // a dial by the pointer: how far the fill spreads into similar colors
            gc.save();
            gc.setRenderHint(QPainter::Antialiasing, true);
            const QRectF dial(pointer + QPointF(14, -34), QSizeF(24, 24));
            gc.setPen(QPen(QColor(0, 0, 0, 120), 5));
            gc.drawEllipse(dial);
            gc.setPen(QPen(QColor(255, 255, 255, 90), 3));
            gc.drawEllipse(dial);
            gc.setPen(QPen(color, 3, Qt::SolidLine, Qt::RoundCap));
            gc.drawArc(dial, 90 * 16, -qRound(360 * 16 * spread));
            gc.restore();
        }
    }
};

/// The button by a drop that keeps filling with its color: a fill bucket on a round chip.
class KeepFillingChip : public QToolButton
{
public:
    explicit KeepFillingChip(QWidget *parent)
        : QToolButton(parent)
    {
        setCheckable(true);
        setFixedSize(34, 34);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::NoFocus);
        setIcon(KisIconUtils::loadIcon(QStringLiteral("krita_tool_color_fill")));
        setIconSize(QSize(18, 18));
        setToolTip(i18nc("@info:tooltip", "Keep filling with this color"));
    }

    QColor color;

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QRectF disc = QRectF(rect()).adjusted(1.5, 1.5, -1.5, -1.5);
        QColor base = palette().color(QPalette::Window);
        base.setAlphaF(0.94);
        QColor edge = palette().color(QPalette::WindowText);
        edge.setAlphaF(underMouse() ? 0.45 : 0.22);
        painter.setPen(QPen(isChecked() ? color : edge, isChecked() ? 3 : 1));
        painter.setBrush(base);
        painter.drawEllipse(disc);
        // the color it fills with, as a dot under the bucket
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawEllipse(QRectF(disc.right() - 11, disc.bottom() - 11, 9, 9));
        icon().paint(&painter, QRect(QPoint(8, 7), iconSize()));
    }
};

/// Where a drop paints: an existing node, or a new layer to add first.
struct Target {
    KisNodeSP node;
    KisPaintLayerSP newLayer;
    KisNodeSP parent;
    KisNodeSP above;
};

bool isColorsLayer(KisNodeSP node, bool multiply)
{
    return node && qobject_cast<KisPaintLayer *>(node.data()) && node->name() == colorsLayerName() && node->isEditable()
        && (node->compositeOpId() == COMPOSITE_MULT) == multiply;
}

/**
 * The layer to fill. When the active layer holds the lines around \p point, the fill goes on a layer of its
 * own: below clear line art, or above opaque line art with Multiply, so the lines stay on top either way.
 */
Target targetFor(KisImageSP image, KisNodeSP active, const QPoint &point)
{
    Target target;
    if (!active) {
        target.newLayer = new KisPaintLayer(image.data(), colorsLayerName(), OPACITY_OPAQUE_U8);
        target.parent = image->root();
        target.above = image->root()->lastChild();
        return target;
    }
    if (!qobject_cast<KisPaintLayer *>(active.data())) {
        // a group, a mask or a layer that cannot be painted: a new layer above it (above a mask's layer)
        KisNodeSP anchor = active;
        while (anchor->parent() && !qobject_cast<KisLayer *>(anchor.data())) anchor = anchor->parent();
        target.newLayer = new KisPaintLayer(image.data(), colorsLayerName(), OPACITY_OPAQUE_U8);
        target.parent = anchor->parent() ? anchor->parent() : image->root();
        target.above = anchor->parent() ? anchor : image->root()->lastChild();
        return target;
    }

    // a layer of colors from earlier drops takes the next ones too
    if (isColorsLayer(active, false) || isColorsLayer(active, true)) {
        target.node = active;
        return target;
    }

    // Lines are the dark ink of the picture near the point; does the active layer hold them?
    const QRect window = QRect(point - QPoint(96, 96), QSize(192, 192)) & image->bounds();
    const QImage picture = image->projection()->convertToQImage(nullptr, window).convertToFormat(QImage::Format_ARGB32);
    const QImage own = active->paintDevice()->convertToQImage(nullptr, window).convertToFormat(QImage::Format_ARGB32);
    int lines = 0, held = 0;
    for (int y = 0; y < picture.height(); y++) {
        const QRgb *row = reinterpret_cast<const QRgb *>(picture.constScanLine(y));
        const QRgb *ownRow = reinterpret_cast<const QRgb *>(own.constScanLine(y));
        for (int x = 0; x < picture.width(); x++) {
            const QRgb p = row[x];
            if (qAlpha(p) * (255 - qGray(p)) < 96 * 255) continue;
            lines++;
            if (qAlpha(ownRow[x]) >= 128) held++;
        }
    }
    if (lines < 24 || held * 5 < lines * 3) {
        target.node = active;
        return target;
    }
    const bool clearPaper = active->paintDevice()->pixel(point).opacityU8() < 128;
    KisNodeSP neighbor = clearPaper ? active->prevSibling() : active->nextSibling();
    if (isColorsLayer(neighbor, !clearPaper)) {
        target.node = neighbor;
        return target;
    }
    target.newLayer = new KisPaintLayer(image.data(), colorsLayerName(), OPACITY_OPAQUE_U8);
    if (!clearPaper) target.newLayer->setCompositeOpId(COMPOSITE_MULT);
    target.parent = active->parent() ? active->parent() : image->root();
    target.above = clearPaper ? active->prevSibling() : active;
    return target;
}

void addLayerJob(KisImageSP image, KisStrokeId stroke, const Target &target)
{
    if (!target.newLayer) return;
    image->addJob(stroke, new KisStrokeStrategyUndoCommandBased::Data(
                              KUndo2CommandSP(new KisImageLayerAddCommand(image, target.newLayer, target.parent, target.above)),
                              false, KisStrokeJobData::SEQUENTIAL, KisStrokeJobData::EXCLUSIVE));
}

} // namespace

struct KisColorDrop::Private {
    KisColorDrop *q {nullptr};
    QPointer<KisView> view;
    KisSharedPtr<Preview> preview;

    KoColor color;
    Options options;
    QPoint pointer;
    QPoint seed {-1, -1};
    int threshold {8};
    /// The area last shown, to skip work while the pointer stays inside it.
    KisPixelSelectionSP region;
    int generation {0};
    bool computing {false};
    bool pending {false};

    // resting a moment, then moving sideways, tunes the threshold
    QTimer rest;
    QPoint restAt;
    bool tuning {false};
    int tuneFrom {0};
    int tuneBase {8};

    QPointer<KeepFillingChip> chip;
    QTimer chipTimeout;
    bool keepFilling {false};
    KoColor keepColor;
    QPoint pressAt;
    bool pressed {false};
    /// Space held on the canvas: its presses pan, as without the chip
    bool spaceHeld {false};

    KisCanvas2 *canvas() const { return view ? view->canvasBase() : nullptr; }
    QWidget *canvasWidget() const { return canvas() ? canvas()->canvasWidget() : nullptr; }

    QPoint imagePoint(const QPoint &canvasPos) const
    {
        return canvas()->coordinatesConverter()->widgetToImage(QPointF(canvasPos)).toPoint();
    }

    void showPreview()
    {
        if (!canvas()) return;
        preview->color = color.toQColor();
        preview->pointer = pointer;
        preview->tuning = tuning;
        preview->spread = threshold / 100.0;
        canvas()->updateCanvasDecorations();
    }

    void clearPreview()
    {
        generation++;
        pending = false;
        rest.stop();
        tuning = false;
        region = nullptr;
        seed = QPoint(-1, -1);
        if (preview) {
            preview->overlay = QImage();
            preview->tuning = false;
        }
        if (canvas()) canvas()->updateCanvasDecorations();
    }

    /// Works out in the background what a fill from the seed covers, and shows it.
    void compute()
    {
        KisImageSP image = view ? view->image() : nullptr;
        if (!image) return;
        if (computing) {
            pending = true;
            return;
        }
        computing = true;
        pending = false;
        const int ticket = ++generation;
        // a snapshot, as the image may change while this runs
        KisPaintDeviceSP reference = new KisPaintDevice(*image->projection());
        const QRect bounds = image->bounds();
        const QPoint at = seed;
        const int t = threshold;
        const Options o = options;
        const QColor tint = color.toQColor();
        struct Result {
            KisPixelSelectionSP region;
            QImage overlay;
            QRect rect;
        };
        auto *watcher = new QFutureWatcher<Result>(q);
        QObject::connect(watcher, &QFutureWatcher<Result>::finished, q, [this, watcher, ticket]() {
            const Result result = watcher->result();
            watcher->deleteLater();
            computing = false;
            if (ticket == generation && preview) {
                region = result.region;
                preview->overlay = result.overlay;
                preview->rect = result.rect;
                showPreview();
            }
            if (pending) compute();
        });
        watcher->setFuture(QtConcurrent::run([reference, at, t, o, bounds, tint]() {
            Result result;
            // out of memory on a huge image: no preview, rather than an exception thrown into the window's thread
            try {
                // the gap too, as finding the width of the lines nearby reads a piece of the image
                result.region = regionAt(reference, at, t, gapFor(o, reference, at, bounds), o, bounds);
                result.rect = result.region->selectedExactRect();
                result.overlay = tintedOverlay(result.region, result.rect, tint);
            } catch (const std::bad_alloc &) {
                result = Result();
            }
            return result;
        }));
    }

    void hideChip()
    {
        keepFilling = false;
        chipTimeout.stop();
        if (QWidget *widget = canvasWidget()) widget->removeEventFilter(q);
        if (chip) chip->deleteLater();
        chip = nullptr;
    }

    void showChip(const QPoint &canvasPos)
    {
        QWidget *widget = canvasWidget();
        if (!widget) return;
        if (!chip) {
            chip = new KeepFillingChip(widget);
            QObject::connect(chip.data(), &QToolButton::toggled, q, [this](bool on) {
                keepFilling = on;
                if (on) {
                    chipTimeout.stop();
                    canvasWidget()->installEventFilter(q);
                } else {
                    hideChip();
                }
            });
        }
        chip->color = color.toQColor();
        keepColor = color;
        const QPoint at = canvasPos + QPoint(18, 18);
        chip->move(qBound(0, at.x(), widget->width() - chip->width()), qBound(0, at.y(), widget->height() - chip->height()));
        chip->show();
        chip->raise();
        chip->update();
        if (!keepFilling) chipTimeout.start();
    }
};

KisColorDrop::KisColorDrop(KisView *view)
    : QObject(view)
    , d(new Private)
{
    d->q = this;
    d->view = view;
    d->preview = new Preview(view);
    view->canvasBase()->addDecoration(KisCanvasDecorationSP(d->preview.data()));
    d->rest.setSingleShot(true);
    d->rest.setInterval(450);
    connect(&d->rest, &QTimer::timeout, this, [this]() {
        if (!d->region) return;
        d->tuning = true;
        d->tuneFrom = d->pointer.x();
        d->tuneBase = d->threshold;
        d->showPreview();
    });
    d->chipTimeout.setSingleShot(true);
    d->chipTimeout.setInterval(3500);
    connect(&d->chipTimeout, &QTimer::timeout, this, [this]() { d->hideChip(); });
    // picking another tool ends keeping on filling
    if (KoToolManager *tools = view && view->viewManager() ? view->viewManager()->toolManager() : nullptr) {
        connect(tools, &KoToolManager::changedTool, this, [this]() {
            if (d->keepFilling) d->hideChip();
        });
    }
}

KisColorDrop::~KisColorDrop() = default;

bool KisColorDrop::carriesColor(const QMimeData *data)
{
    return data && (data->hasColor() || data->hasFormat(QStringLiteral("krita/x-colorsetentry")));
}

bool KisColorDrop::colorOf(const QMimeData *data, KoColor *color)
{
    if (!data) return false;
    if (data->hasColor()) {
        *color = KoColor(data->colorData().value<QColor>(), KoColorSpaceRegistry::instance()->rgb8());
        return true;
    }
    if (data->hasFormat(QStringLiteral("krita/x-colorsetentry"))) {
        QByteArray bytes = data->data(QStringLiteral("krita/x-colorsetentry"));
        *color = KisSwatch::fromByteArray(bytes).color();
        return true;
    }
    return false;
}

void KisColorDrop::hover(const KoColor &color, const QPoint &canvasPos)
{
    KisImageSP image = d->view ? d->view->image() : nullptr;
    if (!image || !d->canvas()) return;
    const bool newDrag = d->seed == QPoint(-1, -1) && !d->tuning;
    if (newDrag) {
        d->options = Options::read();
        d->threshold = d->options.threshold;
    }
    d->color = color;
    d->pointer = canvasPos;

    if (d->tuning) {
        // sideways: right spreads into more colors, left into fewer
        const int threshold = qBound(1, d->tuneBase + (canvasPos.x() - d->tuneFrom) / 3, 100);
        if (threshold != d->threshold) {
            d->threshold = threshold;
            d->compute();
        }
        d->showPreview();
        return;
    }

    if ((canvasPos - d->restAt).manhattanLength() > 4) {
        d->restAt = canvasPos;
        d->rest.start();
    }

    const QPoint point = d->imagePoint(canvasPos);
    if (!image->bounds().contains(point)) {
        d->clearPreview();
        return;
    }
    // still over the area shown: nothing new to work out
    if (d->region && d->region->pixel(point).opacityU8() > 0) {
        d->showPreview();
        return;
    }
    d->seed = point;
    d->compute();
}

void KisColorDrop::leave()
{
    d->clearPreview();
}

bool KisColorDrop::drop(const KoColor &color, const QPoint &canvasPos)
{
    KisImageSP image = d->view ? d->view->image() : nullptr;
    KisViewManager *viewManager = d->view ? d->view->viewManager() : nullptr;
    if (!image || !viewManager || !d->canvas()) return false;

    const bool tuned = d->tuning;
    const QPoint point = tuned ? d->seed : d->imagePoint(canvasPos);
    if (!tuned) {
        d->options = Options::read();
        d->threshold = d->options.threshold;
    }
    const int threshold = d->threshold;
    d->clearPreview();
    if (!image->bounds().contains(point)) return false;

    KisNodeSP active = viewManager->activeNode();
    if (active && !active->isEditable()) {
        d->view->showFloatingMessage(i18nc("@info:status", "Locked"), KisIconUtils::loadIcon(QStringLiteral("object-locked")));
        return false;
    }

    KisSelectionSP selection = viewManager->selection();
    const bool inSelection = selection && selection->projection()->pixel(point).opacityU8() > 0;
    const Target target = inSelection && active && qobject_cast<KisPaintLayer *>(active.data())
        ? Target{active, nullptr, nullptr, nullptr}
        : targetFor(image, active, point);
    KisNodeSP node = target.newLayer ? KisNodeSP(target.newLayer) : target.node;

    KisStrokeStrategyUndoCommandBased *strategy =
        new KisStrokeStrategyUndoCommandBased(kundo2_i18n("Fill"), false, image.data());
    strategy->setSupportsWrapAroundMode(true);
    KisStrokeId stroke = image->startStroke(strategy);
    KIS_SAFE_ASSERT_RECOVER_RETURN_VALUE(stroke, false);
    addLayerJob(image, stroke, target);

    KisResourcesSnapshotSP resources =
        new KisResourcesSnapshot(image, node, viewManager->canvasResourceProvider()->resourceManager());
    resources->setFGColorOverride(color.convertedTo(image->colorSpace()));
    // a new layer is empty: with Preserve Alpha the fill would show nowhere
    if (target.newLayer) resources->setGlobalAlphaLock(false);

    FillProcessingVisitor *visitor = new FillProcessingVisitor(image->projection(), resources->activeSelection(), resources);
    visitor->setSeedPoint(point);
    visitor->setSelectionOnly(inSelection);
    visitor->setUseFastMode(false);
    visitor->setUseCustomBlendingOptions(true);
    visitor->setCustomOpacity(1.0);
    visitor->setCustomCompositeOp(COMPOSITE_OVER);
    visitor->setRegionFillingMode(KisFillPainter::RegionFillingMode_FloodFill);
    visitor->setFillThreshold(threshold);
    visitor->setOpacitySpread(100);
    visitor->setCloseGap(gapFor(d->options, image->projection(), point, image->bounds()));
    visitor->setUseSelectionAsBoundary(true);
    visitor->setAntiAlias(true);
    visitor->setLineArt(d->options.underLines, d->options.speckSize);
    QSharedPointer<QRect> dirty(new QRect);
    visitor->setOutDirtyRect(dirty);
    image->addJob(stroke, new KisStrokeStrategyUndoCommandBased::Data(KUndo2CommandSP(new KisProcessingCommand(visitor, node)), false,
                                                                     KisStrokeJobData::SEQUENTIAL, KisStrokeJobData::EXCLUSIVE));
    image->addJob(stroke, new KisStrokeStrategyUndoCommandBased::Data(KUndo2CommandSP(new KisUpdateCommand(node, dirty, image.data())), false,
                                                                     KisStrokeJobData::SEQUENTIAL, KisStrokeJobData::EXCLUSIVE));
    image->endStroke(stroke);

    d->color = color;
    d->showChip(canvasPos);
    return true;
}

void KisColorDrop::startDrag(QWidget *source, const QColor &color)
{
    auto *data = new QMimeData;
    data->setColorData(color);
    // a swatch may be rebuilt while the drag runs (the color it set changes the panel): the drag isn't its child
    QDrag *drag = new QDrag(qApp);
    drag->setMimeData(data);
    const qreal ratio = source->devicePixelRatioF();
    QPixmap pixmap(QSize(30, 30) * ratio);
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(Qt::transparent);
    {
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing, true);
        // a dark and a light ring, so it shows over any color
        painter.setPen(QPen(QColor(0, 0, 0, 110), 4));
        painter.setBrush(color);
        painter.drawEllipse(QRectF(3, 3, 24, 24));
        painter.setPen(QPen(QColor(255, 255, 255, 220), 1.5));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(QRectF(3, 3, 24, 24));
    }
    drag->setPixmap(pixmap);
    drag->setHotSpot(QPoint(15, 15));
    drag->exec(Qt::CopyAction);
    drag->deleteLater();
}

bool KisColorDrop::canRecolor(KisNodeSP node)
{
    // pixels of its own to recolor: a fill or filter layer's device, or a mask's, is only where it applies
    return node && qobject_cast<KisPaintLayer *>(node.data()) && node->isEditable(false);
}

void KisColorDrop::recolor(KisImageSP image, KisNodeSP node, const KoColor &color)
{
    if (!image || !canRecolor(node)) return;
    KisStrokeStrategyUndoCommandBased *strategy =
        new KisStrokeStrategyUndoCommandBased(kundo2_i18n("Recolor Layer"), false, image.data());
    KisStrokeId stroke = image->startStroke(strategy);
    KIS_SAFE_ASSERT_RECOVER_RETURN(stroke);
    image->addJob(stroke, new KisStrokeStrategyUndoCommandBased::Data(
                              KUndo2CommandSP(new KisCommandUtils::LambdaCommand([node, color]() mutable -> KUndo2Command * {
                                  KisPaintDeviceSP device = node->paintDevice();
                                  const QRect rect = device->exactBounds();
                                  if (rect.isEmpty()) return nullptr;
                                  KisTransaction transaction(device);
                                  const KoColorSpace *colorSpace = device->colorSpace();
                                  const KoColor paint = color.convertedTo(colorSpace);
                                  const int pixelSize = colorSpace->pixelSize();
                                  // the new color in every pixel, each keeping its own transparency
                                  KisSequentialIterator it(device, rect);
                                  while (it.nextPixel()) {
                                      quint8 *pixel = it.rawData();
                                      const qreal opacity = colorSpace->opacityF(pixel);
                                      if (opacity <= 0) continue;
                                      memcpy(pixel, paint.data(), pixelSize);
                                      colorSpace->setOpacity(pixel, opacity, 1);
                                  }
                                  node->setDirty(rect);
                                  return transaction.endAndTake();
                              })),
                              false, KisStrokeJobData::SEQUENTIAL, KisStrokeJobData::EXCLUSIVE));
    image->endStroke(stroke);
}

void KisColorDrop::autoFlats(KisViewManager *viewManager)
{
    KisImageSP image = viewManager ? viewManager->image() : nullptr;
    if (!image) return;
    KisNodeSP active = viewManager->activeNode();
    const bool fromLayer = active && qobject_cast<KisPaintLayer *>(active.data()) && !active->paintDevice()->exactBounds().isEmpty();
    // its working buffers take about twenty bytes a pixel
    if (qint64(image->bounds().width()) * image->bounds().height() > 60'000'000) {
        viewManager->showFloatingMessage(i18nc("@info:status", "Too large for flats"), KisIconUtils::loadIcon(QStringLiteral("krita_tool_color_fill")), 2000);
        return;
    }
    KisPaintDeviceSP lines = new KisPaintDevice(fromLayer ? *active->paintDevice() : *image->projection());
    const QRect bounds = image->bounds();
    const Options options = Options::read();
    const KoColorSpace *colorSpace = image->colorSpace();
    viewManager->showFloatingMessage(i18nc("@info:status", "Flats…"), KisIconUtils::loadIcon(QStringLiteral("krita_tool_color_fill")), 2000);

    KisImageWSP weakImage = image;
    QPointer<KisViewManager> manager = viewManager;
    KisNodeWSP weakActive = active;
    auto *watcher = new QFutureWatcher<KisPaintDeviceSP>(viewManager);
    QObject::connect(watcher, &QFutureWatcher<KisPaintDeviceSP>::finished, viewManager, [watcher, weakImage, manager, weakActive, fromLayer]() {
        KisPaintDeviceSP flats = watcher->result();
        watcher->deleteLater();
        KisImageSP image = weakImage;
        if (!image || !manager || !flats) return;
        KisNodeSP active = weakActive;
        Target target;
        target.newLayer = new KisPaintLayer(image.data(), i18nc("@item:layer name: a flat color for every area of the line art", "Flats"),
                                            OPACITY_OPAQUE_U8, flats);
        if (fromLayer && active && active->parent()) {
            // under clear line art; over opaque line art, multiplied, so the lines show through
            const bool opaque = active->paintDevice()->exactBounds().contains(image->bounds())
                && active->paintDevice()->pixel(image->bounds().center()).opacityU8() == OPACITY_OPAQUE_U8;
            if (opaque) target.newLayer->setCompositeOpId(COMPOSITE_MULT);
            target.parent = active->parent();
            target.above = opaque ? active : active->prevSibling();
        } else {
            target.parent = image->root();
            target.above = image->root()->lastChild();
        }
        KisStrokeStrategyUndoCommandBased *strategy =
            new KisStrokeStrategyUndoCommandBased(kundo2_i18n("Auto Flats"), false, image.data());
        KisStrokeId stroke = image->startStroke(strategy);
        KIS_SAFE_ASSERT_RECOVER_RETURN(stroke);
        addLayerJob(image, stroke, target);
        image->endStroke(stroke);
    });
    watcher->setFuture(QtConcurrent::run([lines, bounds, options, colorSpace]() -> KisPaintDeviceSP {
        // an exception thrown here would be thrown again in the window's event loop, and end the app
        try {
            return KisLineArtFill::flats(lines, bounds, gapFor(options, lines, bounds.center(), bounds, 0.75), options.speckSize, colorSpace);
        } catch (const std::bad_alloc &) {
            return nullptr;
        }
    }));
}

bool KisColorDrop::eventFilter(QObject *watched, QEvent *event)
{
    // Keeping on filling: a tap on the canvas fills, a drag does nothing, Esc or a right click stops.
    if (!d->keepFilling || watched != d->canvasWidget()) return false;
    // a pen's tap arrives as a tablet event and then again as a mouse event made from it: one fill
    if (auto *mouse = dynamic_cast<QMouseEvent *>(event); mouse && mouse->source() != Qt::MouseEventNotSynthesized) return true;
    if (dynamic_cast<QTabletEvent *>(event)) event->accept();
    auto position = [event]() -> QPoint {
        if (auto *tablet = dynamic_cast<QTabletEvent *>(event)) return tablet->pos();
        return static_cast<QMouseEvent *>(event)->pos();
    };
    switch (event->type()) {
    case QEvent::MouseButtonPress:
    case QEvent::TabletPress: {
        const bool right = event->type() == QEvent::MouseButtonPress
            ? static_cast<QMouseEvent *>(event)->button() == Qt::RightButton
            : static_cast<QTabletEvent *>(event)->button() == Qt::RightButton;
        if (right) {
            d->hideChip();
            return true;
        }
        // only a plain tap fills: middle button, modifiers and Space keep panning, zooming and picking
        const Qt::MouseButton button = event->type() == QEvent::MouseButtonPress ? static_cast<QMouseEvent *>(event)->button()
                                                                                 : static_cast<QTabletEvent *>(event)->button();
        if (button != Qt::LeftButton || static_cast<QInputEvent *>(event)->modifiers() != Qt::NoModifier || d->spaceHeld) {
            return false;
        }
        d->pressed = true;
        d->pressAt = position();
        return true;
    }
    case QEvent::MouseMove:
    case QEvent::TabletMove:
        return d->pressed;
    case QEvent::MouseButtonRelease:
    case QEvent::TabletRelease:
        if (!d->pressed) return false;
        d->pressed = false;
        if ((position() - d->pressAt).manhattanLength() <= QApplication::startDragDistance()) {
            drop(d->keepColor, d->pressAt);
        }
        return true;
    case QEvent::KeyPress:
        if (static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape) {
            d->hideChip();
            return true;
        }
        if (static_cast<QKeyEvent *>(event)->key() == Qt::Key_Space) d->spaceHeld = true;
        return false;
    case QEvent::KeyRelease:
        if (static_cast<QKeyEvent *>(event)->key() == Qt::Key_Space && !static_cast<QKeyEvent *>(event)->isAutoRepeat()) d->spaceHeld = false;
        return false;
    case QEvent::FocusOut:
        d->spaceHeld = false;
        return false;
    default:
        return false;
    }
}
