/*
 *  kis_tool_gradient.cc - part of Krita
 *
 *  SPDX-FileCopyrightText: 2002 Patrick Julien <freak@codepimps.org>
 *  SPDX-FileCopyrightText: 2003 Boudewijn Rempt <boud@valdyas.org>
 *  SPDX-FileCopyrightText: 2004-2007 Adrian Page <adrian@pagenet.plus.com>
 *  SPDX-FileCopyrightText: 2021 L. E. Segovia <amy@amyspark.me>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "kis_tool_gradient.h"

#include <cfloat>

#include <QApplication>
#include <QPainter>
#include <QLabel>
#include <QLayout>
#include <QCheckBox>

#include <kis_transaction.h>
#include <kis_debug.h>
#include <klocalizedstring.h>
#include <kcombobox.h>


#include <KoPointerEvent.h>
#include <KoCanvasBase.h>
#include <KoViewConverter.h>
#include <KoUpdater.h>
#include <KoProgressUpdater.h>

#include <kis_gradient_painter.h>
#include <kis_painter.h>
#include <kis_canvas_resource_provider.h>
#include <kis_layer.h>
#include <kis_selection.h>
#include <kis_paint_layer.h>

#include <canvas/kis_canvas2.h>
#include <KisViewManager.h>
#include <widgets/kis_cmb_composite.h>
#include <kis_slider_spin_box.h>
#include <kis_cursor.h>
#include <kis_config.h>
#include "kis_resources_snapshot.h"
#include "kis_command_utils.h"
#include "kis_processing_applicator.h"
#include "kis_processing_visitor.h"
#include "KisRedoLast.h"
#include "KisStudioCanvasHandles.h"
#include <KoCanvasResourcesIds.h>


KisToolGradient::KisToolGradient(KoCanvasBase * canvas)
        : KisToolPaint(canvas, KisCursor::loadWithSize("tool_gradient_cursor.svg", 32, 32, 6, 6))
{
    setObjectName("tool_gradient");

    m_startPos = QPointF(0, 0);
    m_endPos = QPointF(0, 0);

    m_dither = KisGradientPainter::DitherNone;
    m_ditherSteps = KisGradientPainter::defaultDitherSteps;
    m_reverse = false;
    m_shape = KisGradientPainter::GradientShapeLinear;
    m_repeat = KisGradientPainter::GradientRepeatNone;
    m_antiAliasThreshold = 0.2;

    KisCanvas2 *kritaCanvas = dynamic_cast<KisCanvas2*>(canvas);

    connect(kritaCanvas->viewManager()->canvasResourceProvider(), SIGNAL(sigEffectiveCompositeOpChanged()), SLOT(resetCursorStyle()));
    m_redo = new KisRedoLast(this);
}

KisToolGradient::~KisToolGradient()
{
}

void KisToolGradient::resetCursorStyle()
{
    if (isEraser()) {
        useCursor(KisCursor::loadWithSize("tool_gradient_eraser_cursor.svg", 32, 32, 6, 6));
    } else {
        KisToolPaint::resetCursorStyle();
    }

    overrideCursorIfNotEditable();
}

void KisToolGradient::activate(const QSet<KoShape*> &shapes)
{
    KisToolPaint::activate(shapes);
    m_configGroup =  KSharedConfig::openConfig()->group(toolId());
}

void KisToolGradient::deactivate()
{
    // another tool: the gradient is done
    if (m_handles) m_handles->end();
    m_redo->forget();
    KisToolPaint::deactivate();
}

void KisToolGradient::canvasResourceChanged(int key, const QVariant &value)
{
    KisToolPaint::canvasResourceChanged(key, value);
    // another gradient or other colors picked while the drawn one shows its handles: it takes them
    if (key == KoCanvasResource::CurrentGradient || key == KoCanvasResource::ForegroundColor || key == KoCanvasResource::BackgroundColor) {
        changedWhileShown();
    }
}

void KisToolGradient::paint(QPainter &painter, const KoViewConverter &converter)
{
    if (mode() == KisTool::PAINT_MODE && m_startPos != m_endPos) {
        paintLine(painter);
    }
    KisToolPaint::paint(painter, converter);
}

void KisToolGradient::beginPrimaryAction(KoPointerEvent *event)
{
    if (!nodeEditable()) {
        event->ignore();
        return;
    }

    setMode(KisTool::PAINT_MODE);

    m_startPos = convertToPixelCoordAndSnap(event, QPointF(), false);
    m_endPos = m_startPos;
}

void KisToolGradient::continuePrimaryAction(KoPointerEvent *event)
{
    /**
     * TODO: The gradient tool is still not in strokes, so the end of
     *       its action can call processEvent(), which would result in
     *       nested event handler calls. Please uncomment this line
     *       when the tool is ported to strokes.
     */
    //CHECK_MODE_SANITY_OR_RETURN(KisTool::PAINT_MODE);

    // First ensure the old guideline is deleted
    updateGuideline();

    QPointF pos = convertToPixelCoordAndSnap(event, QPointF(), false);

    if (event->modifiers() == Qt::ShiftModifier) {
        m_endPos = straightLine(pos);
    } else {
        m_endPos = pos;
    }

    updateGuideline();
}

void KisToolGradient::endPrimaryAction(KoPointerEvent *event)
{
    Q_UNUSED(event);
    CHECK_MODE_SANITY_OR_RETURN(KisTool::PAINT_MODE);
    setMode(KisTool::HOVER_MODE);

    if (!currentNode())
        return;

    if (m_startPos == m_endPos) {
        return;
    }

    paintGradientStep();
    showHandles();

    updateGuideline();
}

void KisToolGradient::paintGradientStep()
{
    KisImageSP image = this->image();
    if (!image || !currentNode()) return;

    KisResourcesSnapshotSP resources =
        new KisResourcesSnapshot(image, currentNode(), this->canvas()->resourceManager());
    m_shownGradient = resources->currentGradient();

    if (resources->currentNode()->paintDevice()) {
        KUndo2MagicString actionName = kundo2_i18n("Gradient");
        m_redo->made(dynamic_cast<KisCanvas2 *>(canvas()), actionName, [this]() { paintGradientStep(); });
        KisProcessingApplicator applicator(image, resources->currentNode(),
                                           KisProcessingApplicator::NONE,
                                           KisImageSignalVector(),
                                           actionName);

        applicator.applyCommand(
            new KisCommandUtils::LambdaCommand(
                [resources, startPos = m_startPos, endPos = m_endPos,
                 shape = m_shape, repeat = m_repeat, reverse = m_reverse,
                 antiAliasThreshold = m_antiAliasThreshold, dither = m_dither,
                 ditherSteps = m_ditherSteps] () mutable {

                    KisNodeSP node = resources->currentNode();
                    KisPaintDeviceSP device = node->paintDevice();
                    KisProcessingVisitor::ProgressHelper helper(node);
                    const QRect bounds = device->defaultBounds()->bounds();

                    KisGradientPainter painter(device, resources->activeSelection());
                    resources->setupPainter(&painter);
                    painter.setProgress(helper.updater());

                    painter.beginTransaction();

                    painter.setGradientShape(shape);
                    painter.paintGradient(startPos, endPos,
                                          repeat, antiAliasThreshold,
                                          reverse, 0, 0,
                                          bounds.width(), bounds.height(),
                                          dither, ditherSteps);

                    return painter.endAndTakeTransaction();
                }));
        applicator.end();
    }
}

void KisToolGradient::showHandles()
{
    KisStudioCanvasHandles *handles = KisStudioCanvasHandles::start(dynamic_cast<KisCanvas2 *>(canvas()));
    if (!handles) return;
    m_handles = handles;
    // the ends, each in its color, and the middle that moves both
    handles->moved = [this](int index, const QPointF &at, Qt::KeyboardModifiers modifiers) {
        const bool straight = modifiers & Qt::ShiftModifier;
        if (index == 0) {
            m_startPos = straight ? straightLine(at, m_endPos) : at;
        } else if (index == 1) {
            m_endPos = straight ? straightLine(at, m_startPos) : at;
        } else {
            const QPointF shift = at - (m_startPos + m_endPos) / 2;
            m_startPos += shift;
            m_endPos += shift;
        }
        updateHandles();
        m_redo->settingChanged();
    };
    handles->drawUnder = [this](QPainter &painter, const QTransform &toWidget) {
        const QLineF line(toWidget.map(m_startPos), toWidget.map(m_endPos));
        painter.setPen(QPen(QColor(0, 0, 0, 110), 3));
        painter.drawLine(line);
        painter.setPen(QPen(QColor(255, 255, 255, 220), 1.25));
        painter.drawLine(line);
    };
    KisRedoLast *redo = m_redo;
    handles->alive = [redo]() { return redo->isLive(); };
    updateHandles();
}

void KisToolGradient::updateHandles()
{
    if (!m_handles) return;
    QColor first(Qt::black), last(Qt::white);
    if (m_shownGradient) {
        KoColor color;
        m_shownGradient->colorAt(color, m_reverse ? 1.0 : 0.0);
        first = color.toQColor();
        m_shownGradient->colorAt(color, m_reverse ? 0.0 : 1.0);
        last = color.toQColor();
    }
    first.setAlpha(255);
    last.setAlpha(255);
    m_handles->setHandles({{m_startPos, first}, {m_endPos, last}, {(m_startPos + m_endPos) / 2, QColor()}});
}

void KisToolGradient::changedWhileShown()
{
    if (!m_handles) return;
    m_redo->settingChanged();
    // the colors the step is drawn again with
    KisResourcesSnapshotSP resources = image() && currentNode() ? new KisResourcesSnapshot(image(), currentNode(), canvas()->resourceManager()) : nullptr;
    if (resources) m_shownGradient = resources->currentGradient();
    updateHandles();
}

QPointF KisToolGradient::straightLine(QPointF point, QPointF from)
{
    const QPointF comparison = point - from;
    return fabs(comparison.x()) > fabs(comparison.y()) ? QPointF(point.x(), from.y()) : QPointF(from.x(), point.y());
}

QPointF KisToolGradient::straightLine(QPointF point)
{
    QPointF comparison = point - m_startPos;
    QPointF result;

    if (fabs(comparison.x()) > fabs(comparison.y())) {
        result.setX(point.x());
        result.setY(m_startPos.y());
    } else {
        result.setX(m_startPos.x());
        result.setY(point.y());
    }

    return result;
}

void KisToolGradient::paintLine(QPainter& gc)
{
    QPointF viewStartPos = pixelToView(m_startPos);
    QPointF viewStartEnd = pixelToView(m_endPos);

    if (canvas()) {
        QPainterPath path;
        path.moveTo(viewStartPos);
        path.lineTo(viewStartEnd);
        paintToolOutline(&gc, path);
    }
}

void KisToolGradient::updateGuideline()
{
    if (canvas()) {
        QRectF bound(m_startPos, m_endPos);
        canvas()->updateCanvas(convertToPt(bound.normalized().adjusted(-3, -3, 3, 3)));
    }
}

QWidget* KisToolGradient::createOptionWidget()
{
    QWidget *widget = KisToolPaint::createOptionWidget();
    Q_CHECK_PTR(widget);
    widget->setObjectName(toolId() + " option widget");


    // Make sure to create the connections last after everything is set up. The initialized values
    // won't be loaded from the configuration file if you add the widget before the connection
    m_lbShape = new QLabel(i18n("Shape:"), widget);
    m_cmbShape = new KComboBox(widget);
    m_cmbShape->setObjectName("shape_combo");
    m_cmbShape->addItem(i18nc("the gradient will be drawn linearly", "Linear"));
    m_cmbShape->addItem(i18nc("the gradient will be drawn bilinearly", "Bi-Linear"));
    m_cmbShape->addItem(i18nc("the gradient will be drawn radially", "Radial"));
    m_cmbShape->addItem(i18nc("the gradient will be drawn in a square around a centre", "Square"));
    m_cmbShape->addItem(i18nc("the gradient will be drawn as an asymmetric cone", "Conical"));
    m_cmbShape->addItem(i18nc("the gradient will be drawn as a symmetric cone", "Conical Symmetric"));
    m_cmbShape->addItem(i18nc("the gradient will be drawn as a spiral", "Spiral"));
    m_cmbShape->addItem(i18nc("the gradient will be drawn as a reverse spiral", "Reverse Spiral"));
    m_cmbShape->addItem(i18nc("the gradient will be drawn in a selection outline", "Shaped"));
    addOptionWidgetOption(m_cmbShape, m_lbShape);
    connect(m_cmbShape, SIGNAL(currentIndexChanged(int)), this, SLOT(slotSetShape(int)));

    m_lbRepeat = new QLabel(i18n("Repeat:"), widget);
    m_cmbRepeat = new KComboBox(widget);
    m_cmbRepeat->setObjectName("repeat_combo");
    m_cmbRepeat->addItem(i18nc("The gradient will not repeat", "None"));
    m_cmbRepeat->addItem(i18nc("The gradient will repeat forwards", "Forwards"));
    m_cmbRepeat->addItem(i18nc("The gradient will repeat alternatingly", "Alternating"));
    addOptionWidgetOption(m_cmbRepeat, m_lbRepeat);
    connect(m_cmbRepeat, SIGNAL(currentIndexChanged(int)), this, SLOT(slotSetRepeat(int)));

    m_lbDither = new QLabel(i18n("Dither:"), widget);
    m_cmbDither = new KComboBox(widget);
    m_cmbDither->setObjectName("dither");
    m_cmbDither->addItem(i18nc("The gradient will not be dithered", "None"));
    m_cmbDither->addItem(i18nc("The gradient will be dithered with Blue Noise", "Blue Noise"));
    m_cmbDither->addItem(i18nc("The gradient will be dithered with Bayer Matrix 2x2", "Bayer Matrix 2x2"));
    m_cmbDither->addItem(i18nc("The gradient will be dithered with Bayer Matrix 4x4", "Bayer Matrix 4x4"));
    m_cmbDither->addItem(i18nc("The gradient will be dithered with Bayer Matrix 8x8", "Bayer Matrix 8x8"));
    addOptionWidgetOption(m_cmbDither, m_lbDither);
    connect(m_cmbDither, SIGNAL(currentIndexChanged(int)), this, SLOT(slotSetDither(int)));

    m_lbDitherSteps = new QLabel(i18n("Dither steps:"), widget);
    m_sbDitherSteps = new KisIntParseSpinBox(widget);
    m_sbDitherSteps->setObjectName("dither_steps");
    m_sbDitherSteps->setRange(2, 10000);
    addOptionWidgetOption(m_sbDitherSteps, m_lbDitherSteps);
    connect(m_sbDitherSteps, SIGNAL(valueChanged(int)), this, SLOT(slotSetDitherSteps(int)));

    m_lbAntiAliasThreshold = new QLabel(i18n("Anti-alias threshold:"), widget);
    m_slAntiAliasThreshold = new KisDoubleSliderSpinBox(widget);
    m_slAntiAliasThreshold->setObjectName("threshold_slider");
    m_slAntiAliasThreshold->setRange(0, 1, 3);
    m_slAntiAliasThreshold->setSingleStep(0.001);
    addOptionWidgetOption(m_slAntiAliasThreshold, m_lbAntiAliasThreshold);
    connect(m_slAntiAliasThreshold, SIGNAL(valueChanged(qreal)), this, SLOT(slotSetAntiAliasThreshold(qreal)));

    m_ckReverse = new QCheckBox(i18nc("the gradient will be drawn with the color order reversed", "Reverse"), widget);
    m_ckReverse->setObjectName("reverse_check");
    connect(m_ckReverse, SIGNAL(toggled(bool)), this, SLOT(slotSetReverse(bool)));
    addOptionWidgetOption(m_ckReverse);


    // load configuration settings into widget (updating UI will update internal variables from signals/slots)
    slotSetDither((int)m_configGroup.readEntry("dither", 0));
    m_sbDitherSteps->setValue((int)m_configGroup.readEntry("ditherSteps", KisGradientPainter::defaultDitherSteps));
    m_ckReverse->setChecked((bool)m_configGroup.readEntry("reverse", false));
    m_cmbShape->setCurrentIndex((int)m_configGroup.readEntry("shape", 0));
    m_cmbRepeat->setCurrentIndex((int)m_configGroup.readEntry("repeat", 0));
    m_slAntiAliasThreshold->setValue((qreal)m_configGroup.readEntry("antialiasThreshold", 0.0));

    return widget;
}

void KisToolGradient::slotSetShape(int shape)
{
    m_shape = static_cast<KisGradientPainter::enumGradientShape>(shape);
    m_configGroup.writeEntry("shape", shape);
    changedWhileShown();
}

void KisToolGradient::slotSetRepeat(int repeat)
{
    m_repeat = static_cast<KisGradientPainter::enumGradientRepeat>(repeat);
    m_configGroup.writeEntry("repeat", repeat);
    changedWhileShown();
}

void KisToolGradient::slotSetReverse(bool state)
{
    m_reverse = state;
    m_configGroup.writeEntry("reverse", state);
    changedWhileShown();
}

void KisToolGradient::slotSetDither(int dither)
{
    if (m_cmbDither->currentIndex() != dither) {
        m_cmbDither->setCurrentIndex(dither);
    }

    m_dither = static_cast<KisGradientPainter::enumDither>(dither);
    m_configGroup.writeEntry("dither", dither);

    showControl(m_sbDitherSteps, KisGradientPainter::isBayerDither(m_dither));
}

void KisToolGradient::slotSetDitherSteps(int value)
{
    m_ditherSteps = value;
    m_configGroup.writeEntry("ditherSteps", value);
    changedWhileShown();
}

void KisToolGradient::slotSetAntiAliasThreshold(qreal value)
{
    m_antiAliasThreshold = value;
    m_configGroup.writeEntry("antialiasThreshold", value);
    changedWhileShown();
}


