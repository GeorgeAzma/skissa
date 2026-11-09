/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "KisToolSuperellipse.h"

#include <QHBoxLayout>
#include <QPainter>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtMath>

#include <KSharedConfig>
#include <KoCanvasBase.h>
#include <KoPathShape.h>
#include <KoShapeStroke.h>
#include <KisModernStyle.h>
#include <KisSettingsRegistry.h>
#include <KisStudioLayout.h>
#include <canvas/kis_canvas2.h>
#include <kis_figure_painting_tool_helper.h>
#include <kis_resources_snapshot.h>
#include <klocalizedstring.h>
#include <widgets/KisStudioValueRow.h>

namespace
{

const KUndo2MagicString stepName()
{
    return kundo2_i18n("Draw Shape");
}

/// The look as a little drawing for its button, in \p ink
QIcon lookIcon(const KisSuperellipse::Look &look, const QColor &ink)
{
    QIcon icon;
    for (qreal ratio : {1.0, 1.5, 2.0}) {
        QPixmap pixmap(QSize(24, 24) * ratio);
        pixmap.setDevicePixelRatio(ratio);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(QPen(ink, 1.75, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.drawPath(KisSuperellipse::pathOf(QRectF(4, 4, 16, 16), look));
        painter.end();
        icon.addPixmap(pixmap);
    }
    return icon;
}

} // namespace

KisToolSuperellipse::KisToolSuperellipse(KoCanvasBase *canvas)
    : KisToolRectangleBase(canvas, KisToolRectangleBase::PAINT, KisCursor::loadWithSize("tool_ellipse_cursor.svg", 32, 32, 6, 6))
{
    setObjectName("tool_superellipse");
    setSupportOutline(true);
    setIsOpacityPresetMode(true);
    const KConfigGroup config = KSharedConfig::openConfig()->group(QStringLiteral("KritaShape/KisToolSuperellipse"));
    const KisSuperellipse::Look rounded = KisSuperellipse::presets()[1].look;
    m_look.roundness = qBound(0.0, config.readEntry("skissaRoundness", rounded.roundness), 1.0);
    m_look.curve = qBound(KisSuperellipse::leastCurve, config.readEntry("skissaCurve", rounded.curve), KisSuperellipse::mostCurve);
}

KisToolSuperellipse::~KisToolSuperellipse()
{
}

void KisToolSuperellipse::activate(const QSet<KoShape *> &shapes)
{
    KisToolRectangleBase::activate(shapes);
    m_redoLast.forget();
}

void KisToolSuperellipse::deactivate()
{
    m_redoLast.forget();
    KisToolRectangleBase::deactivate();
}

QList<QPointer<QWidget>> KisToolSuperellipse::createOptionWidgets()
{
    // Krita's fill and outline, shown as the top bar's pictures in the Studio, then the shape itself
    QList<QPointer<QWidget>> widgets = KisToolShape::createOptionWidgets();
    auto *panel = new QWidget();
    panel->setObjectName(QStringLiteral("superellipseOptions"));
    panel->setWindowTitle(i18nc("tool options: the shape drawn", "Shape"));
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(KisModernStyle::space2, KisModernStyle::space2, KisModernStyle::space2, KisModernStyle::space2);
    layout->setSpacing(KisModernStyle::space1);

    auto *row = new QHBoxLayout();
    row->setSpacing(KisModernStyle::space1);
    const QColor ink = panel->palette().color(QPalette::ButtonText);
    m_presetButtons.clear();
    Q_FOREACH (const KisSuperellipse::Preset &preset, KisSuperellipse::presets()) {
        auto *button = new QToolButton(panel);
        button->setObjectName(QStringLiteral("shape_") + preset.id);
        button->setIcon(lookIcon(preset.look, ink));
        button->setIconSize(QSize(24, 24));
        button->setToolTip(preset.name);
        button->setCheckable(true);
        button->setAutoRaise(true);
        button->setFocusPolicy(Qt::NoFocus);
        // the top bar shows it too
        button->setProperty("studioBar", true);
        const KisSuperellipse::Look look = preset.look;
        connect(button, &QToolButton::clicked, this, [this, look]() { setLook(look); });
        row->addWidget(button);
        m_presetButtons << button;
    }
    row->addStretch(1);
    layout->addLayout(row);

    KisStudioValueRow::Range share;
    share.maximum = 100;
    share.suffix = QStringLiteral("%");
    m_roundness = new KisStudioValueRow(i18nc("shape tool: how far the corners reach", "Roundness"), panel);
    m_roundness->setRange(share);
    m_roundness->setClickSetsValue(true);
    m_roundness->setDefaultValue(KisSuperellipse::presets()[1].look.roundness * 100);
    m_roundness->setToolTip(i18nc("@info:tooltip", "All the way: an ellipse"));
    connect(m_roundness, &KisStudioValueRow::valueChanged, this, [this](qreal value) {
        KisSuperellipse::Look look = m_look;
        look.roundness = value / 100;
        setLook(look);
    });
    layout->addWidget(m_roundness);

    KisStudioValueRow::Range exponent;
    exponent.minimum = KisSuperellipse::leastCurve;
    exponent.maximum = KisSuperellipse::mostCurve;
    exponent.decimals = 2;
    // by ratios: as much changes between 1 and 2 as between 2 and 4
    exponent.size = true;
    m_curve = new KisStudioValueRow(i18nc("shape tool: the corners' curve", "Curve"), panel);
    m_curve->setRange(exponent);
    m_curve->setClickSetsValue(true);
    m_curve->setDefaultValue(2);
    m_curve->setToolTip(i18nc("@info:tooltip the shape tool's corner exponent", "2 is round"));
    connect(m_curve, &KisStudioValueRow::valueChanged, this, [this](qreal value) {
        KisSuperellipse::Look look = m_look;
        look.curve = value;
        setLook(look);
    });
    layout->addWidget(m_curve);

    m_options = panel;
    showLook();
    // in the Studio, Krita's fill and outline are the top bar's: kept, out of sight, for it to mirror
    if (m_shapeOptionsWidget && KisStudioLayout::isEnabledInConfig()) {
        widgets.removeAll(QPointer<QWidget>(m_shapeOptionsWidget));
        m_shapeOptionsWidget->setParent(panel);
        m_shapeOptionsWidget->hide();
    }
    widgets.prepend(panel);
    return widgets;
}

void KisToolSuperellipse::setLook(const KisSuperellipse::Look &look)
{
    m_look.roundness = qBound(0.0, look.roundness, 1.0);
    m_look.curve = qBound(KisSuperellipse::leastCurve, look.curve, KisSuperellipse::mostCurve);
    KConfigGroup config = KSharedConfig::openConfig()->group(QStringLiteral("KritaShape/KisToolSuperellipse"));
    config.writeEntry("skissaRoundness", m_look.roundness);
    config.writeEntry("skissaCurve", m_look.curve);
    KisSettingsRegistry::instance()->scheduleSync();
    showLook();
    // the shape just drawn takes the new look
    m_redoLast.settingChanged();
}

void KisToolSuperellipse::showLook()
{
    if (m_roundness) m_roundness->setValue(m_look.roundness * 100);
    if (m_curve) m_curve->setValue(m_look.curve);
    const QVector<KisSuperellipse::Preset> presets = KisSuperellipse::presets();
    for (int i = 0; i < m_presetButtons.size() && i < presets.size(); i++) {
        if (m_presetButtons[i]) m_presetButtons[i]->setChecked(presets[i].look == m_look);
    }
}

void KisToolSuperellipse::paintRectangle(QPainter &gc, const QRectF &imageRect)
{
    KIS_ASSERT_RECOVER_RETURN(canvas());
    QPainterPath shape = KisSuperellipse::pathOf(imageRect, m_look);
    getRotatedPath(shape, imageRect.center(), getRotationAngle());
    QPainterPath path = pixelToView(shape);
    path.addPath(drawX(pixelToView(m_dragStart)));
    path.addPath(drawX(pixelToView(m_dragCenter)));
    paintToolOutline(&gc, path);
}

void KisToolSuperellipse::finishRect(const QRectF &rect, qreal roundCornersX, qreal roundCornersY)
{
    Q_UNUSED(roundCornersX);
    Q_UNUSED(roundCornersY);
    if (rect.isEmpty()) return;
    draw(rect, getRotationAngle());
}

void KisToolSuperellipse::draw(const QRectF &rect, qreal angle)
{
    const KisToolShape::ShapeAddInfo info = shouldAddShape(currentNode());
    if (!info.shouldAddShape) {
        KisFigurePaintingToolHelper helper(stepName(), image(), currentNode(), canvas()->resourceManager(), strokeStyle(), fillStyle(), fillTransform());
        QPainterPath path = KisSuperellipse::pathOf(rect, m_look);
        getRotatedPath(path, rect.center(), angle);
        helper.paintPainterPath(path);
        if (auto *kritaCanvas = dynamic_cast<KisCanvas2 *>(canvas())) {
            m_redoLast.made(kritaCanvas, stepName(), [this, rect, angle]() { draw(rect, angle); });
        }
        return;
    }
    KisResourcesSnapshot resources(image(), currentNode(), canvas()->resourceManager());
    KoPathShape *shape = KoPathShape::createShapeFromPainterPath(KisSuperellipse::pathOf(convertToPt(rect), m_look));
    shape->rotate(qRadiansToDegrees(angle));
    KoShapeStrokeSP border(new KoShapeStroke(currentStrokeWidth(), resources.currentFgColor().toQColor()));
    shape->setStroke(border);
    info.markAsSelectionShapeIfNeeded(shape);
    addShape(shape);
}

KisToolSuperellipseFactory::KisToolSuperellipseFactory()
    : KisToolPaintFactoryBase("KritaShape/KisToolSuperellipse")
{
    setToolTip(i18nc("@info:tooltip a tool", "Shape"));
    setSection(ToolBoxSection::Shape);
    setActivationShapeId(KRITA_TOOL_ACTIVATION_ID);
    setIconName(koIconNameCStr("studio_superellipse"));
    setPriority(2);
}
