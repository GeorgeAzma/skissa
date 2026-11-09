/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "KisToolSuperellipse.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QDrag>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtMath>

#include <KisMainWindow.h>
#include <KisModernStyle.h>
#include <KisPart.h>
#include <KisStudioFind.h>
#include <KisStudioItemCard.h>
#include <KisStudioLayout.h>
#include <KoCanvasBase.h>
#include <KoPathShape.h>
#include <KoShapeStroke.h>
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

/// A shape to pick: a click picks it, a drag takes it to the rail or the top bar, a right click opens its card
class ShapeButton : public QToolButton
{
public:
    using QToolButton::QToolButton;

    /// The key it is dragged and pinned as
    std::function<QString()> key;

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) m_pressAt = event->pos();
        QToolButton::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (!(event->buttons() & Qt::LeftButton) || m_pressAt.isNull()
            || (event->pos() - m_pressAt).manhattanLength() < KisModernStyle::dragDistance(event)) {
            QToolButton::mouseMoveEvent(event);
            return;
        }
        m_pressAt = QPoint();
        setDown(false);
        auto *mime = new QMimeData();
        mime->setData(KisStudioShortcuts::itemMimeType(), key().toUtf8());
        // owned by the application, not the button: the panel may close while it is dragged
        auto *drag = new QDrag(qApp);
        drag->setMimeData(mime);
        drag->setPixmap(icon().pixmap(32, 32));
        drag->exec(Qt::CopyAction);
        drag->deleteLater();
    }

    void contextMenuEvent(QContextMenuEvent *event) override
    {
        KisMainWindow *window = KisPart::instance()->currentMainwindow();
        if (window && window->studioFind()) window->studioFind()->showCard(key(), this);
        event->accept();
    }

private:
    QPoint m_pressAt;
};

} // namespace

KisToolSuperellipse::KisToolSuperellipse(KoCanvasBase *canvas)
    : KisToolRectangleBase(canvas, KisToolRectangleBase::PAINT, KisCursor::loadWithSize("tool_ellipse_cursor.svg", 32, 32, 6, 6))
{
    setObjectName("tool_superellipse");
    setSupportOutline(true);
    setIsOpacityPresetMode(true);
    m_look = KisSuperellipse::current();
    connect(KisSuperellipse::Watch::instance(), &KisSuperellipse::Watch::changed, this, [this]() { lookChanged(); });
}

KisToolSuperellipse::~KisToolSuperellipse()
{
}

void KisToolSuperellipse::activate(const QSet<KoShape *> &shapes)
{
    KisToolRectangleBase::activate(shapes);
    m_redoLast.forget();
    m_look = KisSuperellipse::current();
    showLook();
}

void KisToolSuperellipse::deactivate()
{
    m_redoLast.forget();
    KisToolRectangleBase::deactivate();
}

QList<QPointer<QWidget>> KisToolSuperellipse::createOptionWidgets()
{
    QList<QPointer<QWidget>> widgets = KisToolShape::createOptionWidgets();
    auto *panel = new QWidget();
    panel->setObjectName(QStringLiteral("superellipseOptions"));
    panel->setWindowTitle(i18nc("tool options: the shape drawn", "Shape"));
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(KisModernStyle::space2, KisModernStyle::space2, KisModernStyle::space2, KisModernStyle::space2);
    layout->setSpacing(KisModernStyle::space1);

    // the presets, two rows of six
    auto *grid = new QGridLayout();
    grid->setSpacing(KisModernStyle::space1);
    const QColor ink = panel->palette().color(QPalette::ButtonText);
    m_presetButtons.clear();
    const QVector<KisSuperellipse::Preset> presets = KisSuperellipse::presets();
    for (int i = 0; i < presets.size(); i++) {
        const KisSuperellipse::Look look = presets[i].look;
        auto *button = new ShapeButton(panel);
        button->key = [look]() { return KisSuperellipse::keyOf(look); };
        button->setIcon(KisSuperellipse::iconOf(look, ink));
        button->setIconSize(QSize(24, 24));
        button->setToolTip(presets[i].name);
        button->setCheckable(true);
        button->setAutoRaise(true);
        button->setFocusPolicy(Qt::NoFocus);
        connect(button, &QToolButton::clicked, this, [this, look]() { setLook(look); });
        grid->addWidget(button, i / KisSuperellipse::presetsPerRow, i % KisSuperellipse::presetsPerRow);
        m_presetButtons << button;
    }
    grid->setColumnStretch(KisSuperellipse::presetsPerRow, 1);
    layout->addLayout(grid);

    auto add = [&](const QString &name, KisStudioValueRow::Range range, qreal reset, std::function<void(KisSuperellipse::Look &, qreal)> write) {
        auto *row = new KisStudioValueRow(name, panel);
        row->setRange(range);
        row->setClickSetsValue(true);
        row->setDefaultValue(reset);
        connect(row, &KisStudioValueRow::valueChanged, this, [this, write](qreal value) {
            KisSuperellipse::Look look = m_look;
            write(look, value);
            setLook(look);
        });
        layout->addWidget(row);
        return row;
    };
    KisStudioValueRow::Range count;
    count.minimum = KisSuperellipse::leastSides;
    count.maximum = KisSuperellipse::mostSides;
    m_sides = add(i18nc("shape tool: how many sides the shape has", "Sides"), count, 4, [](KisSuperellipse::Look &look, qreal v) { look.sides = qRound(v); });
    KisStudioValueRow::Range share;
    share.maximum = 100;
    share.suffix = QStringLiteral("%");
    m_roundness = add(i18nc("shape tool: how far the corners reach", "Roundness"), share, 0,
                      [](KisSuperellipse::Look &look, qreal v) { look.roundness = v / 100; });
    m_roundness->setToolTip(i18nc("@info:tooltip", "All the way: no straight side left"));
    KisStudioValueRow::Range exponent;
    exponent.minimum = KisSuperellipse::leastCurve;
    exponent.maximum = KisSuperellipse::mostCurve;
    exponent.decimals = 2;
    // by ratios: as much changes between 1 and 2 as between 2 and 4
    exponent.size = true;
    m_curve = add(i18nc("shape tool: the corners' curve", "Curve"), exponent, 2, [](KisSuperellipse::Look &look, qreal v) { look.curve = v; });
    m_curve->setToolTip(i18nc("@info:tooltip the shape tool's corner exponent", "2 is round"));
    KisStudioValueRow::Range turn;
    turn.minimum = -180;
    turn.maximum = 180;
    turn.suffix = QStringLiteral("°");
    turn.angle = true;
    m_angle = add(i18nc("shape tool: how the shape is turned in its box", "Angle"), turn, 0, [](KisSuperellipse::Look &look, qreal v) { look.angle = v; });

    // the look as set, to pin: its card, or dragged where it goes
    auto *pinRow = new QHBoxLayout();
    pinRow->addStretch(1);
    auto *pin = new ShapeButton(panel);
    pin->key = [this]() { return KisSuperellipse::keyOf(m_look); };
    pin->setText(i18nc("shape tool: pin the shape as it is set", "Pin"));
    pin->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    pin->setIconSize(QSize(24, 24));
    pin->setAutoRaise(true);
    pin->setFocusPolicy(Qt::NoFocus);
    pin->setToolTip(i18nc("@info:tooltip", "Pin this shape to the tools"));
    connect(pin, &QToolButton::clicked, this, [this, pin]() {
        KisMainWindow *window = KisPart::instance()->currentMainwindow();
        if (window && window->studioFind()) window->studioFind()->showCard(KisSuperellipse::keyOf(m_look), pin);
    });
    pinRow->addWidget(pin);
    layout->addLayout(pinRow);
    m_pin = pin;

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
    if (look == m_look) return;
    m_look = look;
    KisSuperellipse::use(look);
    m_look = KisSuperellipse::current();
    showLook();
    // the shape just drawn takes the new look
    m_redoLast.settingChanged();
}

void KisToolSuperellipse::lookChanged()
{
    const KisSuperellipse::Look look = KisSuperellipse::current();
    if (look == m_look) return;
    m_look = look;
    showLook();
    m_redoLast.settingChanged();
}

void KisToolSuperellipse::showLook()
{
    if (m_sides) m_sides->setValue(m_look.sides);
    if (m_roundness) m_roundness->setValue(m_look.roundness * 100);
    if (m_curve) m_curve->setValue(m_look.curve);
    if (m_angle) m_angle->setValue(m_look.angle);
    if (m_pin) {
        m_pin->setIcon(KisSuperellipse::iconOf(m_look, m_pin->palette().color(QPalette::ButtonText)));
    }
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
