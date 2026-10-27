/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisColorDrop.h"

#include <QApplication>
#include <QCheckBox>
#include <QDrag>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
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

#include "KisDocument.h"
#include "KisModernStyle.h"
#include "KisSettingsRegistry.h"
#include "KisStudioGlass.h"
#include "KisView.h"
#include "KisViewManager.h"
#include "kis_slider_spin_box.h"
#include <kundo2stack.h>
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

    /// Kept for the next drops and the fill tool, which share them
    void write() const
    {
        KConfigGroup group = KSharedConfig::openConfig()->group("KritaFill/KisToolFill");
        group.writeEntry("thresholdAmount", threshold);
        group.writeEntry("closeGapAmount", closeGap);
        group.writeEntry("fillUnderLines", underLines);
        KisSettingsRegistry::instance()->scheduleSync();
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

protected:
    void drawDecoration(QPainter &gc, const QRectF &, const KisCoordinatesConverter *converter, KisCanvas2 *) override
    {
        if (overlay.isNull()) return;
        gc.save();
        gc.setTransform(converter->imageToWidgetTransform(), false);
        gc.setRenderHint(QPainter::SmoothPixmapTransform, true);
        gc.drawImage(QRectF(rect), overlay);
        gc.restore();
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

/**
 * By a drop: how it filled, changed in place (the fill is made again while it is the newest step), and the
 * bucket that keeps filling with its color. It goes a moment after the pointer leaves it.
 */
class DropPanel : public QWidget
{
public:
    explicit DropPanel(QWidget *canvas)
        : QWidget(canvas)
    {
        setAttribute(Qt::WA_Hover);
        auto *row = new QHBoxLayout(this);
        row->setContentsMargins(KisModernStyle::space2, KisModernStyle::space2, KisModernStyle::space3, KisModernStyle::space2);
        row->setSpacing(KisModernStyle::space2);
        bucket = new KeepFillingChip(this);
        row->addWidget(bucket, 0, Qt::AlignTop);
        auto *grid = new QGridLayout();
        grid->setContentsMargins(0, 0, 0, 0);
        grid->setHorizontalSpacing(KisModernStyle::space2);
        grid->setVerticalSpacing(0);
        auto value = [this](int low, int high) {
            auto *slider = new KisSliderSpinBox(this);
            slider->setRange(low, high);
            slider->setFrame(false);
            slider->setProperty(KisModernStyle::quietFieldProperty, true);
            slider->setButtonSymbols(QAbstractSpinBox::NoButtons);
            slider->setFixedWidth(120);
            slider->setMinimumHeight(26);
            slider->setBlockUpdateSignalOnDrag(true);
            slider->setKeyboardTracking(false);
            return slider;
        };
        gap = value(0, 32);
        gap->setSpecialValueText(i18nc("color drop gap: sized from the lines", "Auto"));
        gap->setSuffix(i18nc("unit", " px"));
        gap->setProperty("kisDefaultValue", 0);
        auto *gapName = new QLabel(i18nc("color drop: openings in the lines it closes", "Gap"), this);
        gapName->setToolTip(i18nc("@info:tooltip", "Openings in the lines this wide still hold the fill; Auto sizes it from the lines"));
        gap->setToolTip(gapName->toolTip());
        spread = value(1, 100);
        spread->setProperty("kisDefaultValue", 8);
        auto *spreadName = new QLabel(i18nc("color drop: how far into similar colors it fills", "Spread"), this);
        spreadName->setToolTip(i18nc("@info:tooltip", "Higher fills on into similar colors"));
        spread->setToolTip(spreadName->toolTip());
        underLines = new QCheckBox(i18nc("color drop: the fill reaches under the lines", "Under lines"), this);
        underLines->setToolTip(i18nc("@info:tooltip", "The fill reaches under the lines' soft edges to their middle, leaving no pale rim"));
        grid->addWidget(gapName, 0, 0);
        grid->addWidget(gap, 0, 1);
        grid->addWidget(spreadName, 1, 0);
        grid->addWidget(spread, 1, 1);
        grid->addWidget(underLines, 2, 0, 1, 2);
        row->addLayout(grid);
        for (QWidget *widget : std::initializer_list<QWidget *>{gapName, spreadName, gap, spread, underLines}) {
            KisStudioGlass::setSeeThrough(widget, true);
        }
        adjustSize();
    }

    KeepFillingChip *bucket {nullptr};
    KisSliderSpinBox *gap {nullptr};
    KisSliderSpinBox *spread {nullptr};
    QCheckBox *underLines {nullptr};
    std::function<void(bool)> hovered;

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QPainterPath shape = KisStudioGlass::paint(painter, this, QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), KisModernStyle::panelRadius);
        QColor border = palette().color(QPalette::WindowText);
        border.setAlphaF(0.14);
        KisStudioGlass::paintBorder(painter, this, shape, border);
    }

    bool event(QEvent *event) override
    {
        if (event->type() == QEvent::HoverEnter && hovered) hovered(true);
        if (event->type() == QEvent::HoverLeave && hovered) hovered(false);
        // presses on the panel stay on it, not on the canvas under it
        if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonRelease) {
            QWidget::event(event);
            event->accept();
            return true;
        }
        return QWidget::event(event);
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

    // Lines are the dark ink of the picture around the point; does the active layer hold them? Far enough around
    // that the middle of a large shape still sees its outline
    const QRect window = QRect(point - QPoint(768, 768), QSize(1536, 1536)) & image->bounds();
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

    QPointer<DropPanel> panel;
    QTimer panelTimeout;
    bool keepFilling {false};
    KoColor keepColor;

    // the last drop, made again as its panel changes it while it is still the newest step
    QPoint lastSeed {-1, -1};
    KoColor lastColor;
    QPointer<KUndo2Stack> stack;
    QMetaObject::Connection stackConnection;
    const KUndo2Command *step {nullptr};
    int expectedIndex {-1};
    QTimer refill;
    int refillTries {0};
    /// The layer active when the last fill was made
    KisNodeWSP lastActive;
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
        if (canvas()) canvas()->updateCanvasDecorations();
    }

    void clearPreview()
    {
        generation++;
        pending = false;
        region = nullptr;
        seed = QPoint(-1, -1);
        if (preview) preview->overlay = QImage();
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

    void hidePanel()
    {
        keepFilling = false;
        panelTimeout.stop();
        refill.stop();
        if (QWidget *widget = canvasWidget()) widget->removeEventFilter(q);
        if (panel) panel->deleteLater();
        panel = nullptr;
    }

    /// The panel's values changed: kept for the next drops, and the last drop is made again with them
    void changed()
    {
        if (!panel) return;
        options.closeGap = panel->gap->value();
        options.threshold = panel->spread->value();
        options.underLines = panel->underLines->isChecked();
        options.write();
        threshold = options.threshold;
        refillTries = 0;
        refill.start(0);
    }

    void showPanel(const QPoint &canvasPos)
    {
        QWidget *widget = canvasWidget();
        if (!widget) return;
        if (!panel) {
            panel = new DropPanel(widget);
            QObject::connect(panel->bucket, &QToolButton::toggled, q, [this](bool on) {
                keepFilling = on;
                if (on) {
                    panelTimeout.stop();
                    canvasWidget()->installEventFilter(q);
                } else {
                    canvasWidget()->removeEventFilter(q);
                    panelTimeout.start();
                }
            });
            panel->hovered = [this](bool in) {
                if (in) panelTimeout.stop();
                else if (!keepFilling) panelTimeout.start();
            };
            QObject::connect(panel->gap, QOverload<int>::of(&KisSliderSpinBox::valueChanged), q, [this]() { changed(); });
            QObject::connect(panel->spread, QOverload<int>::of(&KisSliderSpinBox::valueChanged), q, [this]() { changed(); });
            QObject::connect(panel->underLines, &QCheckBox::toggled, q, [this]() { changed(); });
            // beside the drop, clear of the pointer
            const QPoint at = canvasPos + QPoint(22, 22);
            panel->move(qBound(0, at.x(), widget->width() - panel->width()), qBound(0, at.y(), widget->height() - panel->height()));
        }
        {
            const QSignalBlocker gapBlocker(panel->gap), spreadBlocker(panel->spread), linesBlocker(panel->underLines);
            panel->gap->setValue(options.closeGap);
            panel->spread->setValue(threshold);
            panel->underLines->setChecked(options.underLines);
        }
        panel->bucket->color = color.toQColor();
        keepColor = color;
        panel->show();
        panel->raise();
        panel->bucket->update();
        if (!keepFilling && !panel->underMouse()) panelTimeout.start();
    }

    /// Notes the history step the next fill makes, to take it back if its panel changes it
    void expectStep()
    {
        KisDocument *document = view ? view->document() : nullptr;
        KUndo2Stack *undo = document ? document->undoStack() : nullptr;
        step = nullptr;
        if (undo != stack) {
            QObject::disconnect(stackConnection);
            stack = undo;
            if (undo) {
                stackConnection = QObject::connect(undo, &KUndo2Stack::indexChanged, q, [this](int index) {
                    if (step || !stack || index != expectedIndex || index < 1) return;
                    step = stack->command(index - 1);
                });
            }
        }
        expectedIndex = stack ? stack->index() + 1 : -1;
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
    d->panelTimeout.setSingleShot(true);
    d->panelTimeout.setInterval(6000);
    connect(&d->panelTimeout, &QTimer::timeout, this, [this]() {
        // a number being typed into it keeps it
        if (d->panel && d->panel->isAncestorOf(QApplication::focusWidget())) {
            d->panelTimeout.start();
            return;
        }
        d->hidePanel();
    });
    d->refill.setSingleShot(true);
    connect(&d->refill, &QTimer::timeout, this, [this]() { refillLast(); });
    // picking another tool ends keeping on filling
    if (KoToolManager *tools = view && view->viewManager() ? view->viewManager()->toolManager() : nullptr) {
        connect(tools, &KoToolManager::changedTool, this, [this]() {
            if (d->keepFilling) d->hidePanel();
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
    if (d->seed == QPoint(-1, -1)) {
        d->options = Options::read();
        d->threshold = d->options.threshold;
    }
    d->color = color;
    d->pointer = canvasPos;

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
    if (!d->canvas()) return false;
    const QPoint point = d->imagePoint(canvasPos);
    d->options = Options::read();
    d->threshold = d->options.threshold;
    d->clearPreview();
    if (!fillAt(color, point)) return false;
    d->color = color;
    d->showPanel(canvasPos);
    return true;
}

void KisColorDrop::refillLast()
{
    KisImageSP image = d->view ? d->view->image() : nullptr;
    if (!image || !d->stack || d->lastSeed == QPoint(-1, -1)) return;
    if (!d->step) {
        // the fill's step isn't in the history yet: it is still being made
        if (d->expectedIndex > 0 && ++d->refillTries < 300) d->refill.start(16);
        return;
    }
    const int index = d->stack->index();
    // only while the fill is still the newest step, with nothing undone after it
    if (index < 1 || index != d->stack->count() || d->stack->command(index - 1) != d->step) {
        d->step = nullptr;
        return;
    }
    if (!image->isIdle()) {
        if (++d->refillTries < 300) d->refill.start(16);
        return;
    }
    d->step = nullptr;
    d->stack->setIndex(index - 1);
    fillAt(d->lastColor, d->lastSeed);
}

bool KisColorDrop::fillAt(const KoColor &color, const QPoint &point)
{
    KisImageSP image = d->view ? d->view->image() : nullptr;
    KisViewManager *viewManager = d->view ? d->view->viewManager() : nullptr;
    if (!image || !viewManager) return false;
    const int threshold = d->threshold;
    if (!image->bounds().contains(point)) return false;

    KisNodeSP active = viewManager->activeNode();
    // a fill made again: the layer it made was just taken back with it, and may still be the active one
    if (active && !active->parent()) active = d->lastActive;
    if (active && !active->isEditable()) {
        d->view->showFloatingMessage(i18nc("@info:status", "Locked"), KisIconUtils::loadIcon(QStringLiteral("object-locked")));
        return false;
    }
    d->lastActive = active;

    KisSelectionSP selection = viewManager->selection();
    const bool inSelection = selection && selection->projection()->pixel(point).opacityU8() > 0;
    const Target target = inSelection && active && qobject_cast<KisPaintLayer *>(active.data())
        ? Target{active, nullptr, nullptr, nullptr}
        : targetFor(image, active, point);
    KisNodeSP node = target.newLayer ? KisNodeSP(target.newLayer) : target.node;

    d->lastSeed = point;
    d->lastColor = color;
    d->expectStep();
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
            d->hidePanel();
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
            // the panel stays where it is: what it changes is this newest fill
            d->options = Options::read();
            d->threshold = d->options.threshold;
            fillAt(d->keepColor, d->imagePoint(d->pressAt));
        }
        return true;
    case QEvent::KeyPress:
        if (static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape) {
            d->hidePanel();
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
