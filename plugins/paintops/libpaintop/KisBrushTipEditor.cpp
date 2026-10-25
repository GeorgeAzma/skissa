/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisBrushTipEditor.h"

#include <QHBoxLayout>
#include <QHash>
#include <QLineEdit>
#include <QLocale>
#include <QMouseEvent>
#include <QHelpEvent>
#include <QPainter>
#include <QToolTip>
#include <QPainterPath>
#include <QVBoxLayout>
#include <QVariantAnimation>
#include <QtMath>

#include <klocalizedstring.h>

#include <KisGlobalResourcesInterface.h>
#include <KisModernStyle.h>
#include <KisStudioTips.h>
#include <KisResourceModel.h>
#include <KoColorSpaceRegistry.h>
#include <kis_auto_brush.h>
#include <kis_auto_brush_factory.h>
#include <kis_config.h>
#include <kis_cubic_curve.h>
#include <kis_dab_shape.h>
#include <kis_fixed_paint_device.h>
#include <kis_icon_utils.h>
#include <kis_image_config.h>
#include <kis_paint_information.h>
#include <kis_predefined_brush_factory.h>
#include <kis_signals_blocker.h>
#include <widgets/KisStudioChips.h>
#include <widgets/KisStudioInlineEdit.h>
#include <widgets/KisStudioPicker.h>
#include <widgets/KisStudioValueRow.h>

#include "KisAutoBrushModel.h"
#include "KisPredefinedBrushModel.h"

using namespace KisBrushModel;

namespace {

const QString linkedKey = QStringLiteral("BrushTipSizeLinked");
const QString recentTipsKey = QStringLiteral("StudioRecentBrushTips");
constexpr int handleRadius = 5;
constexpr int grabRadius = 10;
constexpr int stalk = 26;
/// Where the values along the top of the tip's box end
constexpr int chipsBottom = 32;
/// Where the edge's softness and shapes end, under the tip
constexpr int edgeChipsBottom = 26;

/// A tip's axis at \p degrees, as a direction on screen (Krita's angles turn counterclockwise)
QPointF axisVector(qreal degrees)
{
    const qreal radians = qDegreesToRadians(degrees);
    return QPointF(std::cos(radians), -std::sin(radians));
}

qreal angleOf(const QPointF &vector)
{
    qreal degrees = qRadiansToDegrees(std::atan2(-vector.y(), vector.x()));
    while (degrees < 0) degrees += 360.0;
    while (degrees >= 360.0) degrees -= 360.0;
    return degrees;
}

qreal dot(const QPointF &a, const QPointF &b)
{
    return a.x() * b.x() + a.y() * b.y();
}

/// The picture tip a signature names, if it is installed
KisBrushSP tipFor(const KoResourceSignature &signature)
{
    auto source = KisGlobalResourcesInterface::instance()->source<KisBrush>(ResourceType::Brushes);
    return source.bestMatch(signature.md5sum, signature.filename, signature.name);
}

/// \p tip as a mask in \p ink, or as it is when it has colors of its own
QImage tinted(const QImage &tip, const QColor &ink, bool asMask)
{
    QImage image = tip.convertToFormat(QImage::Format_ARGB32);
    if (!asMask) return image;
    for (int y = 0; y < image.height(); y++) {
        QRgb *line = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < image.width(); x++) {
            // dark is paint: a gray tip on white, or a black one on clear
            const int alpha = qAlpha(line[x]) * (255 - qGray(line[x])) / 255;
            line[x] = qRgba(ink.red(), ink.green(), ink.blue(), alpha);
        }
    }
    return image;
}

/// A generated tip drawn \p width by \p height pixels, unturned, in \p ink
QImage autoDab(const AutoBrushData &data, qreal width, qreal height, const QColor &ink)
{
    AutoBrushData shown = data;
    shown.generator.diameter = qMax(2.0, width);
    shown.generator.ratio = qBound(0.01, height / qMax(2.0, width), 1.0);
    CommonData common;
    common.angle = 0;
    KisAutoBrushFactory factory;
    QSharedPointer<KisAutoBrush> brush =
        factory.createBrush(common, shown, KisGlobalResourcesInterface::instance()).resource<KisAutoBrush>();
    if (!brush) return QImage();
    const KisDabShape shape;
    const int w = brush->maskWidth(shape, 0, 0, KisPaintInformation());
    const int h = brush->maskHeight(shape, 0, 0, KisPaintInformation());
    KisPaintInformation info(QPointF(w * 0.5, h * 0.5), 0.5, 0, 0, 0, 0, 0, 0, 0);
    KisFixedPaintDeviceSP device = new KisFixedPaintDevice(KoColorSpaceRegistry::instance()->rgb8());
    device->setRect(QRect(0, 0, w, h));
    device->initialize();
    brush->mask(device, KoColor(Qt::black, device->colorSpace()), shape, info);
    QImage image = device->convertToQImage(nullptr);
    QPainter painter(&image);
    painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
    painter.fillRect(image.rect(), ink);
    return image;
}

/**
 * How far a star tip reaches at \p angle (radians, y down, as in the dab): Krita folds every angle
 * into the first point's slice and reads the ellipse (or rectangle) of half sizes \p a and \p b there.
 */
qreal starReach(qreal angle, qreal a, qreal b, int spikes, bool rectangle)
{
    qreal folded = std::atan2(qAbs(std::sin(angle)), std::cos(angle));
    const qreal slice = M_PI / spikes;
    while (folded > slice) folded -= 2 * slice;
    const qreal c = qAbs(std::cos(folded));
    const qreal s = qAbs(std::sin(folded));
    if (rectangle) {
        return qMin(c > 1e-9 ? a / c : 1e9, s > 1e-9 ? b / s : 1e9);
    }
    return 1.0 / std::sqrt(std::pow(c / qMax(a, 1e-9), 2) + std::pow(s / qMax(b, 1e-9), 2));
}

QPainterPath starOutline(qreal a, qreal b, int spikes, bool rectangle)
{
    QPainterPath path;
    for (int i = 0; i <= 720; i++) {
        const qreal angle = i * 2 * M_PI / 720;
        const qreal r = starReach(angle, a, b, spikes, rectangle);
        const QPointF point(r * std::cos(angle), r * std::sin(angle));
        if (i) path.lineTo(point);
        else path.moveTo(point);
    }
    path.closeSubpath();
    return path;
}

/**
 * A generated tip's edge: Round (Krita's Default), Smooth (its Gaussian) or a curve of one's own (its Soft).
 * Each has a solid middle that reaches some share of the radius, its hardness: Round's fade, how far Smooth
 * stays solid, a curve's flat start.
 */
struct Edge
{
    int type {Default};
    qreal fadeA {1};
    qreal fadeB {1};
    QString curve;

    static Edge of(KisAutoBrushModel *model)
    {
        return Edge{model->type(), model->horizontalFade(), model->verticalFade(), model->curveString()};
    }
};

/// A curve's points from the middle (x 0) to the rim (x 1); Krita reads a curve at the square of the distance
QList<KisCubicCurvePoint> curvePoints(const QString &curve)
{
    QList<KisCubicCurvePoint> points = KisCubicCurve(curve.isEmpty() ? QStringLiteral("0,1;1,0;") : curve).curvePoints();
    if (points.isEmpty()) points << KisCubicCurvePoint(0, 1) << KisCubicCurvePoint(1, 0);
    if (points.first().x() > 0.0) points.prepend(KisCubicCurvePoint(0, points.first().y()));
    if (points.last().x() < 1.0) points.append(KisCubicCurvePoint(1, points.last().y()));
    return points;
}

/// Where a curve's solid middle ends, as a share of the radius: at its second point, when that is as high as the first
qreal curveHold(const QList<KisCubicCurvePoint> &points)
{
    if (points.size() > 2 && points[1].y() >= points[0].y() - 0.002) return std::sqrt(points[1].x());
    return 0.0;
}

/// \p points with their solid middle reaching \p hold of the radius, the fall after it squeezed or spread to fit
QList<KisCubicCurvePoint> heldAt(QList<KisCubicCurvePoint> points, qreal hold)
{
    const qreal was = curveHold(points);
    hold = qBound(0.0, hold, 0.95);
    const bool flat = was > 0.0;
    if (was >= 0.999 || (!flat && hold < 0.005)) return points;
    for (int i = 1; i < points.size(); i++) {
        const qreal r = std::sqrt(points[i].x());
        points[i].setX(std::pow(hold + (r - was) * (1.0 - hold) / (1.0 - was), 2));
    }
    if (!flat) {
        points.insert(1, KisCubicCurvePoint(hold * hold, points[0].y(), true));
    } else if (hold < 0.005) {
        points.removeAt(1);
    }
    return points;
}

/// How solid the edge is from the middle (0) out to the rim (1), as Krita paints a round tip
class EdgeProfile
{
public:
    explicit EdgeProfile(const Edge &edge)
        : m_edge(edge)
    {
        if (edge.type == Soft) m_curve = KisCubicCurve(curvePoints(edge.curve));
        if (edge.type == Gaussian) {
            // as KisGaussCircleMaskGenerator has it: erf around a plateau as wide as the fade
            const qreal fade = qBound(1e-6, 1.0 - (edge.fadeA + edge.fadeB) / 2.0, 1.0 - 1e-6);
            m_factor = M_SQRT2 * 12500.0 / (6761.0 * fade);
            m_center = 2.5 * (6761.0 * fade - 10000.0) / (M_SQRT2 * 6761.0 * fade);
        }
    }

    qreal at(qreal r) const
    {
        r = qBound(0.0, r, 1.0);
        if (m_edge.type == Soft) return qBound(0.0, m_curve.value(r * r), 1.0);
        if (m_edge.type == Gaussian) {
            return qBound(0.0, (std::erf(r * m_factor + m_center) - std::erf(r * m_factor - m_center)) / (2.0 * std::erf(m_center)), 1.0);
        }
        // Default: solid to the fade, then 1 - r² made to meet it
        const qreal fade = m_edge.fadeA;
        if (r <= fade) return 1.0;
        return qBound(0.0, (1.0 - r * r) / qMax(1e-6, 1.0 - fade * fade), 1.0);
    }

    /// From the middle at the left of \p box to the rim at its right
    QPainterPath half(const QRectF &box) const
    {
        QPainterPath path;
        for (int i = 0; i <= 32; i++) {
            const qreal r = i / 32.0;
            const QPointF point(box.left() + r * box.width(), box.bottom() - at(r) * box.height());
            if (i) path.lineTo(point);
            else path.moveTo(point);
        }
        return path;
    }

private:
    Edge m_edge;
    KisCubicCurve m_curve;
    qreal m_factor {1};
    qreal m_center {0};
};

/// How far a Smooth edge of Krita's \p fade stays solid (within 1% of its middle), as a share of the radius
qreal smoothReach(qreal fade)
{
    const EdgeProfile profile(Edge{Gaussian, fade, fade, QString()});
    qreal low = 0.0;
    qreal high = 1.0;
    for (int i = 0; i < 24; i++) {
        const qreal middle = (low + high) / 2.0;
        if (profile.at(middle) >= 0.99) low = middle;
        else high = middle;
    }
    return low;
}

/// Krita's fade for a Smooth edge solid to \p reach, or as near as a Smooth edge comes
qreal smoothFadeFor(qreal reach)
{
    qreal low = 0.0;
    qreal high = 1.0;
    for (int i = 0; i < 24; i++) {
        const qreal middle = (low + high) / 2.0;
        if (smoothReach(middle) < reach) low = middle;
        else high = middle;
    }
    return (low + high) / 2.0;
}

/// How far the edge stays solid, as a share of the radius: the same measure for every shape
qreal hardnessOf(const Edge &edge)
{
    if (edge.type == Soft) return curveHold(curvePoints(edge.curve));
    if (edge.type == Gaussian) return smoothReach((edge.fadeA + edge.fadeB) / 2.0);
    return edge.fadeA;
}

/// Sets the solid middle to reach \p hold of the radius, from \p press, the edge as it was when the change began
void setHardness(KisAutoBrushModel *model, const Edge &press, qreal hold)
{
    hold = qBound(0.0, hold, 1.0);
    if (press.type == Soft) {
        model->setcurveString(KisCubicCurve(heldAt(curvePoints(press.curve), hold)).toString());
        return;
    }
    if (press.type == Gaussian) {
        // Krita's Gaussian is as soft one way as the other
        const qreal fade = smoothFadeFor(hold);
        model->sethorizontalFade(fade);
        model->setverticalFade(fade);
        return;
    }
    // both ways by as much, so a tip softer one way stays so
    model->sethorizontalFade(hold);
    model->setverticalFade(qBound(0.0, press.fadeB + hold - press.fadeA, 1.0));
}

void makeHard(KisAutoBrushModel *model)
{
    model->settype(Default);
    model->sethorizontalFade(1.0);
    model->setverticalFade(1.0);
}

/// The edge as a curve that looks as it does and is as hard, to be bent
QList<KisCubicCurvePoint> seeded(const Edge &edge)
{
    if (edge.type == Soft) return curvePoints(edge.curve);
    if (edge.type == Default) {
        // Round falls along a straight line in a curve's terms (x = r²)
        const qreal fade = qMin(edge.fadeA, 0.95);
        if (fade < 0.005) return {KisCubicCurvePoint(0, 1), KisCubicCurvePoint(1, 0)};
        return {KisCubicCurvePoint(0, 1), KisCubicCurvePoint(fade * fade, 1, true), KisCubicCurvePoint(1, 0)};
    }
    // solid to where Smooth is, then its fall
    const EdgeProfile profile(edge);
    const qreal reach = qMin(hardnessOf(edge), 0.95);
    QList<KisCubicCurvePoint> points{KisCubicCurvePoint(0, 1)};
    if (reach > 0.01) points << KisCubicCurvePoint(reach * reach, 1);
    for (qreal t : {0.3, 0.6, 0.85, 1.0}) {
        const qreal r = reach + (1.0 - reach) * t;
        points << KisCubicCurvePoint(r * r, profile.at(r));
    }
    return points;
}

/// Sets \p tip as the brush's picture tip, keeping its size, angle and spacing
void usePictureTip(KisPredefinedBrushModel *model, lager::cursor<BrushType> type, KisBrushSP tip)
{
    if (!tip) return;
    PredefinedBrushData data = model->bakedOptionData();
    CommonData loaded;
    KisPredefinedBrushFactory::loadFromBrushResource(loaded, data, tip);
    data.scale = 1.0;
    data.application = KisPredefinedBrushModel::effectiveBrushApplication(data, model->m_supportsHSLBrushTips.get());
    // the tip's own spacing and angle aren't taken: only its picture changes
    model->m_predefinedBrushData.set(data);
    type.set(Predefined);

    QStringList recent = KisConfig(true).readEntry<QStringList>(recentTipsKey, QStringList());
    recent.removeAll(tip->md5Sum());
    recent.prepend(tip->md5Sum());
    KisConfig(false).writeEntry(recentTipsKey, recent.mid(0, 6));
}

/**
 * The dab, large, with its handles and its values. Sizes in the brush's pixels are drawn at a
 * scale that fits the box; it is kept during a drag so the handle stays under the pointer.
 */
class TipCanvas : public QWidget
{
    // a class of its own: a press on it shapes the tip, it doesn't move the window
    Q_OBJECT
public:
    enum Part {
        None,
        AxisA,       ///< along the tip's angle (its width, or its size while in proportion)
        AxisB,       ///< across it (its height)
        Fade,        ///< the softness
        Turn,
        Valley,      ///< a star's notch between two points: how deep they go
        ChipWidth,
        ChipLink,
        ChipHeight,
        ChipAngle,
        ChipPoints,
    };

    struct Chip {
        Part part;
        QRectF rect;
        QString text;
        QString icon;
    };

    TipCanvas(KisAutoBrushModel *autoModel, KisPredefinedBrushModel *predefinedModel,
              lager::cursor<BrushType> type, QWidget *parent)
        : QWidget(parent)
        , m_auto(autoModel)
        , m_predefined(predefinedModel)
        , m_type(type)
    {
        setMouseTracking(true);
        setAttribute(Qt::WA_Hover);
        setFixedHeight(152);
        m_linked = KisConfig(true).readEntry(linkedKey, true);

        auto changed = [this]() {
            m_dabDirty = true;
            if (!m_dragging) fit(false);
            update();
        };
        for (auto signal : {&KisAutoBrushModel::diameterChanged, &KisAutoBrushModel::ratioChanged,
                            &KisAutoBrushModel::horizontalFadeChanged, &KisAutoBrushModel::verticalFadeChanged,
                            &KisAutoBrushModel::angleChanged, &KisAutoBrushModel::randomnessChanged,
                            &KisAutoBrushModel::densityChanged}) {
            connect(m_auto, signal, this, changed);
        }
        connect(m_auto, &KisAutoBrushModel::spikesChanged, this, changed);
        connect(m_auto, &KisAutoBrushModel::shapeChanged, this, changed);
        connect(m_auto, &KisAutoBrushModel::typeChanged, this, changed);
        connect(m_auto, &KisAutoBrushModel::curveStringChanged, this, changed);
        connect(m_predefined, &KisPredefinedBrushModel::resourceSignatureChanged, this, changed);
        connect(m_predefined, &KisPredefinedBrushModel::brushSizeChanged, this, changed);
        connect(m_predefined, &KisPredefinedBrushModel::angleChanged, this, changed);
        connect(m_predefined, &KisPredefinedBrushModel::applicationChanged, this, changed);
        m_type.watch([this, changed](BrushType) { changed(); });

        m_fitAnimation.setDuration(KisModernStyle::changeDuration);
        m_fitAnimation.setEasingCurve(KisModernStyle::motionCurve());
        connect(&m_fitAnimation, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
            m_scale = value.toReal();
            update();
        });
        fit(false);
    }

    bool isAuto() const
    {
        return m_type.get() != Predefined;
    }

    bool isStar() const
    {
        return isAuto() && m_auto->spikes() > 2;
    }

    /// The notch between the first two points, in the dab's own frame (y down), widget pixels
    QPointF valleyLocal() const
    {
        const QSizeF size = tipSize() * m_scale / 2.0;
        const qreal slice = M_PI / m_auto->spikes();
        const qreal r = starReach(slice, size.width(), size.height(), m_auto->spikes(), m_auto->shape() == Rectangle);
        return QPointF(r * std::cos(slice), r * std::sin(slice));
    }

    /// The dab's own frame turned onto the screen
    QPointF toScreen(const QPointF &local) const
    {
        return QTransform().rotate(-angle()).map(local);
    }

    qreal angle() const
    {
        return isAuto() ? m_auto->angle() : m_predefined->angle();
    }

    /// The tip's size along and across its angle, in the brush's pixels
    QSizeF tipSize() const
    {
        if (isAuto()) return QSizeF(m_auto->diameter(), m_auto->diameter() * m_auto->ratio());
        const QSize base = m_predefined->baseSize();
        const qreal width = m_predefined->brushSize();
        return QSizeF(width, base.width() > 0 ? width * base.height() / base.width() : width);
    }

    /// Under the values along the top
    QPointF center() const
    {
        return QPointF(width() / 2.0, (chipsBottom + height()) / 2.0);
    }

    /// How far the tip may reach from the middle of the box
    qreal room() const
    {
        return qMax(20.0, qMin(width() / 2.0 - 46, (height() - chipsBottom) / 2.0 - 8));
    }

    /// How far the solid middle reaches along the tip and across it, as shares of the half sizes
    QPointF hardness() const
    {
        if (m_auto->type() != Default) {
            const qreal hold = hardnessOf(Edge::of(m_auto));
            return QPointF(hold, hold);
        }
        return QPointF(m_auto->horizontalFade(), m_auto->verticalFade());
    }

    void fit(bool animated)
    {
        const QSizeF size = tipSize();
        const qreal target = room() / qMax(1.0, qMax(size.width(), size.height()) / 2.0);
        if (!animated || qFuzzyIsNull(m_scale)) {
            m_fitAnimation.stop();
            m_scale = target;
            return;
        }
        m_fitAnimation.stop();
        m_fitAnimation.setStartValue(m_scale);
        m_fitAnimation.setEndValue(target);
        m_fitAnimation.start();
    }

    QPointF along() const
    {
        return axisVector(angle());
    }

    QPointF across() const
    {
        return axisVector(angle() + 90.0);
    }

    QPointF partPosition(Part part) const
    {
        const QSizeF size = tipSize() * m_scale / 2.0;
        const QPointF c = center();
        switch (part) {
        case AxisA:
            return c + along() * size.width();
        case AxisB:
            return c + across() * size.height();
        case Fade: {
            const QPointF k = fadeSpot();
            const QPointF hold = hardness();
            return c + along() * size.width() * hold.x() * k.x() + across() * size.height() * hold.y() * k.y();
        }
        case Turn:
            return c + along() * (size.width() + stalk);
        case Valley:
            return c + toScreen(valleyLocal());
        default:
            return c;
        }
    }

    /**
     * Where the softness handle sits on the hard edge, as shares of the half sizes along and across: toward the
     * top right corner, away from the size handle, so each handle does one thing.
     */
    QPointF fadeSpot() const
    {
        if (isStar()) return QPointF(1, 0);
        return m_auto->shape() == Rectangle ? QPointF(1, 1) : QPointF(M_SQRT1_2, M_SQRT1_2);
    }

    /// One handle for each thing: the size, the softness, the angle; the height of its own only while the size chain is open
    QVector<Part> handles() const
    {
        if (!isAuto()) return {AxisA, Turn};
        if (isStar()) return {AxisA, Valley, Fade, Turn};
        if (m_linked) return {AxisA, Fade, Turn};
        return {AxisA, AxisB, Fade, Turn};
    }

    QFont chipFont() const
    {
        return KisModernStyle::textFont(font(), 0.88);
    }

    /// The values, in the corners: each piece can be dragged, the chains clicked
    QVector<Chip> chips() const
    {
        QVector<Chip> chips;
        const QFontMetricsF metrics(chipFont());
        const qreal chipHeight = 24;
        const qreal pad = 8;
        auto row = [&](QPointF at, bool right, bool bottom, const QVector<Chip> &parts) {
            qreal total = 0;
            QVector<qreal> widths;
            Q_FOREACH (const Chip &chip, parts) {
                const qreal w = chip.icon.isEmpty() ? metrics.horizontalAdvance(chip.text) + 2 * pad : 22;
                widths << w;
                total += w;
            }
            qreal x = right ? at.x() - total : at.x();
            const qreal y = bottom ? at.y() - chipHeight : at.y();
            for (int i = 0; i < parts.size(); i++) {
                Chip chip = parts[i];
                chip.rect = QRectF(x, y, widths[i], chipHeight);
                chips << chip;
                x += widths[i];
            }
        };
        const QLocale locale;
        const QSizeF size = tipSize();
        const qreal inset = KisModernStyle::space2;
        const QString px = i18nc("pixels, after a number", " px");
        if (isAuto()) {
            if (qFuzzyCompare(size.width(), size.height()) && m_linked) {
                row(QPointF(inset, inset), false, false, {{ChipWidth, {}, locale.toString(size.width(), 'f', 0) + px, {}},
                                                          {ChipLink, {}, {}, QStringLiteral("studio_link")}});
            } else {
                row(QPointF(inset, inset), false, false, {{ChipWidth, {}, locale.toString(size.width(), 'f', 0), {}},
                                                          {ChipLink, {}, {}, m_linked ? QStringLiteral("studio_link") : QStringLiteral("studio_unlink")},
                                                          {ChipHeight, {}, locale.toString(size.height(), 'f', 0) + px, {}}});
            }
            if (m_auto->spikes() > 2) {
                const QString points = QStringLiteral("✶ ") + QString::number(m_auto->spikes());
                const qreal w = metrics.horizontalAdvance(points) + 2 * pad;
                chips << Chip{ChipPoints, QRectF(width() / 2.0 - w / 2.0, inset, w, chipHeight), points, {}};
            }
        } else {
            row(QPointF(inset, inset), false, false, {{ChipWidth, {}, locale.toString(size.width(), 'f', 0) + px, {}}});
        }
        row(QPointF(width() - inset, inset), true, false, {{ChipAngle, {}, locale.toString(qRound(angle())) + QStringLiteral("°"), {}}});
        return chips;
    }

    QString partTip(Part part) const
    {
        switch (part) {
        case ChipWidth:
            return m_linked ? i18nc("@info:tooltip brush tip", "Size") : i18nc("@info:tooltip brush tip", "Width");
        case ChipHeight:
            return i18nc("@info:tooltip brush tip", "Height");
        case ChipLink:
            return m_linked ? i18nc("@info:tooltip brush tip", "Keep width and height equal") : i18nc("@info:tooltip brush tip", "Stretch width and height separately");
        case ChipAngle:
            return i18nc("@info:tooltip brush tip", "Tip angle");
        case Fade:
            return i18nc("@info:tooltip brush tip", "Soft edge");
        case ChipPoints:
            return i18nc("@info:tooltip brush tip", "Points of the star");
        default:
            return QString();
        }
    }

    bool event(QEvent *event) override
    {
        if (event->type() == QEvent::ToolTip) {
            auto *help = static_cast<QHelpEvent *>(event);
            const QString tip = partTip(partAt(help->pos()));
            if (tip.isEmpty()) {
                QToolTip::hideText();
            } else {
                QToolTip::showText(help->globalPos(), KisStudioTips::html(tip), this);
            }
            return true;
        }
        return QWidget::event(event);
    }

    Part partAt(const QPointF &pos) const
    {
        Q_FOREACH (const Chip &chip, chips()) {
            if (chip.rect.contains(pos)) return chip.part;
        }
        Part best = None;
        qreal nearest = grabRadius;
        Q_FOREACH (Part part, handles()) {
            const qreal distance = QLineF(partPosition(part), pos).length();
            // the softness handle can come to lie on another: it wins when as near
            if (distance < nearest || (distance <= nearest && part == Fade)) {
                nearest = distance;
                best = part;
            }
        }
        return best;
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        const QColor ink = palette().color(QPalette::WindowText);
        const QColor accent = palette().color(QPalette::Highlight);
        const QRectF box = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        QColor well = ink;
        well.setAlphaF(0.045);
        // a generated tip's edge goes on under it, in the same box
        const QRectF wellBox = isAuto() ? box.adjusted(0, 0, 0, 2 * KisModernStyle::cardRadius) : box;
        painter.fillPath(KisModernStyle::squircle(wellBox, KisModernStyle::cardRadius), well);

        const QSizeF size = tipSize() * m_scale;
        if (isAuto()) {
            if (m_dabDirty || m_dabInk != ink || m_dabSize != size.toSize() || !m_dabPicture.isEmpty()) {
                m_dabDirty = false;
                m_dabInk = ink;
                m_dabSize = size.toSize();
                m_dabPicture.clear();
                // a dab grown far past the box by a drag can't be seen whole: it is made at most twice the
                // box and drawn stretched, rather than as a picture of millions of pixels per move
                const qreal shrink = qMin(1.0, 2.0 * qMax(width(), height()) / qMax(1.0, qMax(size.width(), size.height())));
                m_dabStretched = shrink < 1.0;
                m_dab = autoDab(m_auto->bakedOptionData(), size.width() * shrink, size.height() * shrink, ink);
            }
        } else {
            // a picture tip is drawn scaled into its place: made again only for another picture or ink, not for its size
            const QString picture = m_predefined->resourceSignature().md5sum + QString::number(int(m_predefined->application())) + ink.name(QColor::HexArgb);
            if (picture != m_dabPicture) {
                m_dabPicture = picture;
                m_dabDirty = true;
                KisBrushSP tip = tipFor(m_predefined->resourceSignature());
                m_dab = tip ? tinted(tip->brushTipImage(), ink, m_predefined->application() == ALPHAMASK) : QImage();
            }
        }

        painter.save();
        painter.translate(center());
        painter.rotate(-angle());
        const QRectF dabRect(-size.width() / 2, -size.height() / 2, size.width(), size.height());
        if (!m_dab.isNull()) {
            if (isAuto() && !m_dabStretched) painter.drawImage(QPointF(-m_dab.width() / 2.0, -m_dab.height() / 2.0), m_dab);
            else painter.drawImage(dabRect, m_dab);
        }
        painter.setBrush(Qt::NoBrush);
        // every line twice, dark under light, so it shows on a light dab, a dark one and the background
        const QColor dark(0, 0, 0, 150);
        const QColor light(255, 255, 255, 230);
        auto stroke = [&painter, &dark, &light](const QPainterPath &path, bool dashed) {
            QPen under(dark, 3, Qt::SolidLine, Qt::RoundCap);
            under.setCosmetic(true);
            painter.setPen(under);
            painter.drawPath(path);
            QPen over(light, 1, dashed ? Qt::DashLine : Qt::SolidLine);
            over.setCosmetic(true);
            painter.setPen(over);
            painter.drawPath(path);
        };
        auto shapePath = [this](const QRectF &rect) {
            QPainterPath path;
            if (m_auto->shape() == Rectangle) path.addRect(rect);
            else path.addEllipse(rect);
            return path;
        };
        if (isAuto() && m_auto->spikes() <= 2) {
            stroke(shapePath(dabRect), false);
            const QPointF hold = hardness();
            const QRectF hard(-dabRect.width() / 2 * hold.x(), -dabRect.height() / 2 * hold.y(), dabRect.width() * hold.x(), dabRect.height() * hold.y());
            // only where it differs from the edge
            if (hold.x() < 0.995 || hold.y() < 0.995) stroke(shapePath(hard), true);
        } else if (isStar()) {
            const bool rectangle = m_auto->shape() == Rectangle;
            const qreal a = dabRect.width() / 2;
            const qreal b = dabRect.height() / 2;
            stroke(starOutline(a, b, m_auto->spikes(), rectangle), false);
            const QPointF hold = hardness();
            stroke(starOutline(a * hold.x(), b * hold.y(), m_auto->spikes(), rectangle), true);
        } else if (!isAuto()) {
            QPainterPath path;
            path.addRect(dabRect);
            stroke(path, false);
        }
        painter.restore();

        // the turning handle's stalk
        {
            QPainterPath stalk;
            stalk.moveTo(partPosition(AxisA));
            stalk.lineTo(partPosition(Turn));
            stroke(stalk, false);
        }

        Q_FOREACH (Part part, handles()) {
            const QPointF at = partPosition(part);
            const bool hot = part == m_hover || part == m_dragPart;
            const qreal r = handleRadius + (hot ? 1.5 : 0);
            // light with a dark ring, so each shows on anything; softness is the dark one with a light ring
            if (part == Fade) {
                const qreal d = r + 1;
                QPainterPath diamond;
                diamond.moveTo(at + QPointF(0, -d));
                diamond.lineTo(at + QPointF(d, 0));
                diamond.lineTo(at + QPointF(0, d));
                diamond.lineTo(at + QPointF(-d, 0));
                diamond.closeSubpath();
                painter.setPen(QPen(light, 1.5));
                painter.setBrush(hot ? accent : QColor(20, 20, 24));
                painter.drawPath(diamond);
            } else {
                const qreal size = part == Turn ? r + 1 : r;
                painter.setPen(QPen(dark, 1.5));
                painter.setBrush(hot ? accent : QColor(Qt::white));
                painter.drawEllipse(at, size, size);
            }
        }

        painter.setFont(chipFont());
        QVector<Chip> all = chips();
        // a background behind each run of touching chips
        QVector<QRectF> runs;
        Q_FOREACH (const Chip &chip, all) {
            if (!runs.isEmpty() && qFuzzyCompare(runs.last().right(), chip.rect.left()) && qFuzzyCompare(runs.last().top(), chip.rect.top())) {
                runs.last() = runs.last().united(chip.rect);
            } else {
                runs << chip.rect;
            }
        }
        QColor chipGround = ink;
        chipGround.setAlphaF(0.08);
        Q_FOREACH (const QRectF &run, runs) {
            painter.fillPath(KisModernStyle::squircle(run, 6), chipGround);
        }
        Q_FOREACH (const Chip &chip, all) {
            if (chip.part == m_hover && chip.icon.isEmpty()) {
                QColor hot = ink;
                hot.setAlphaF(0.08);
                painter.fillPath(KisModernStyle::squircle(chip.rect, 6), hot);
            }
            if (!chip.icon.isEmpty()) {
                const QIcon icon = KisIconUtils::loadIcon(chip.icon);
                const QRectF iconRect(chip.rect.center() - QPointF(7, 7), QSizeF(14, 14));
                icon.paint(&painter, iconRect.toRect());
            } else if (chip.part != m_editing) {
                painter.setPen(ink);
                painter.drawText(chip.rect, Qt::AlignCenter, chip.text);
            }
        }
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        m_moved = false;
        if (event->button() != Qt::LeftButton) return;
        const QPointF pos = event->pos();
        const Part part = partAt(pos);
        if (part == None) return;
        if (part == ChipLink) {
            m_linked = !m_linked;
            KisConfig(false).writeEntry(linkedKey, m_linked);
            update();
            return;
        }
        m_dragPart = part;
        m_dragging = true;
        m_moved = false;
        m_press = pos;
        m_pressSize = tipSize();
        m_pressAngle = angle();
        m_pressFadeA = m_auto->horizontalFade();
        m_pressFadeB = m_auto->verticalFade();
        m_pressEdge = Edge::of(m_auto);
        m_pressSpikes = m_auto->spikes();
        m_pressAlong = along();
        m_pressAcross = across();
        update();
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        const QPointF pos = event->pos();
        if (!m_dragging) {
            const Part hover = partAt(pos);
            if (hover != m_hover) {
                m_hover = hover;
                switch (hover) {
                case None:
                    unsetCursor();
                    break;
                case ChipLink:
                    setCursor(Qt::PointingHandCursor);
                    break;
                case ChipWidth:
                case ChipHeight:
                case ChipAngle:
                case ChipPoints:
                    setCursor(Qt::SizeHorCursor);
                    break;
                default:
                    setCursor(Qt::CrossCursor);
                }
                update();
            }
            return;
        }
        // the release went elsewhere (the window lost the pointer): the drag ended there
        if (!(event->buttons() & Qt::LeftButton)) {
            m_dragging = false;
            m_dragPart = None;
            fit(true);
            update();
            return;
        }
        if (!m_moved && QLineF(pos, m_press).length() < 3) return;
        m_moved = true;
        const bool flip = event->modifiers() & Qt::ShiftModifier;
        dragTo(pos, flip);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton || !m_dragging) return;
        m_dragging = false;
        m_dragPart = None;
        fit(true);
        update();
    }

    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        const Part part = partAt(event->pos());
        Q_FOREACH (const Chip &chip, chips()) {
            if (chip.part == part && (part == ChipWidth || part == ChipHeight || part == ChipAngle || part == ChipPoints)) {
                type(chip);
                return;
            }
        }
    }

    void leaveEvent(QEvent *) override
    {
        m_hover = None;
        update();
    }

    void resizeEvent(QResizeEvent *) override
    {
        fit(false);
    }

    void contextMenuEvent(QContextMenuEvent *event) override
    {
        // a finger held still at the end of a drag isn't asking for a reset
        if (m_moved) {
            event->accept();
            return;
        }
        // the values' reset: round, upright, hard
        const Part part = partAt(event->pos());
        if (part == ChipAngle || part == Turn) {
            setAngle(0);
        } else if (isAuto() && part == Fade) {
            makeHard(m_auto);
        } else if (isAuto() && (part == ChipHeight || part == AxisB || part == Valley)) {
            m_auto->setratio(1.0);
        }
        event->accept();
    }

private:
    void setAngle(qreal degrees)
    {
        while (degrees < 0) degrees += 360;
        while (degrees >= 360) degrees -= 360;
        if (isAuto()) m_auto->setangle(degrees);
        else m_predefined->setangle(degrees);
    }

    /// Sets the tip to \p dragged pixels along \p axis and \p other across it
    void setExtents(qreal dragged, qreal other, const QPointF &axis)
    {
        const qreal maximum = KisImageConfig(true).maxBrushSize();
        dragged = qBound(1.0, dragged, maximum);
        other = qBound(1.0, other, maximum);
        if (!isAuto()) {
            m_predefined->setbrushSize(dragged);
            return;
        }
        // a tip is kept wider than tall; taller turns it a quarter, its fades with it
        const bool turned = dragged < other;
        const qreal axisAngle = angleOf(axis);
        const bool wasAlong = qAbs(dot(axis, m_pressAlong)) > 0.5;
        const bool alongNow = wasAlong != turned;
        m_auto->setdiameter(qMax(dragged, other));
        m_auto->setratio(qMin(dragged, other) / qMax(dragged, other));
        // the tip is the same turned half way round: stay near the angle it had
        qreal turnedAngle = turned ? axisAngle + 90 : axisAngle;
        const qreal away = std::fmod(turnedAngle - m_pressAngle + 540.0, 360.0) - 180.0;
        if (qAbs(away) > 90.0) turnedAngle += 180.0;
        setAngle(turnedAngle);
        m_auto->sethorizontalFade(alongNow ? m_pressFadeA : m_pressFadeB);
        m_auto->setverticalFade(alongNow ? m_pressFadeB : m_pressFadeA);
    }

    void dragTo(const QPointF &pos, bool flip)
    {
        const QPointF c = center();
        const QPointF from = pos - c;
        const QPointF moved = pos - m_press;
        const bool linked = m_linked != flip;
        const qreal step = moved.x();
        switch (m_dragPart) {
        case AxisA:
        case AxisB: {
            const bool alongAxis = m_dragPart == AxisA;
            const QPointF axis = alongAxis ? m_pressAlong : m_pressAcross;
            const qreal pressExtent = alongAxis ? m_pressSize.width() : m_pressSize.height();
            const qreal otherExtent = alongAxis ? m_pressSize.height() : m_pressSize.width();
            const qreal extent = qMax(1.0, 2.0 * qAbs(dot(from, axis)) / m_scale);
            if (!isAuto() || linked) {
                const qreal factor = extent / qMax(1.0, pressExtent);
                setExtents(extent, otherExtent * factor, axis);
            } else {
                setExtents(extent, otherExtent, axis);
            }
            break;
        }
        case Fade: {
            if (isStar()) {
                const qreal radius = m_pressSize.width() * m_scale / 2.0;
                setHardness(m_auto, m_pressEdge, qAbs(dot(from, m_pressAlong)) / qMax(1.0, radius));
                break;
            }
            // along the handle's own line from the middle to the corner
            const QPointF k = fadeSpot();
            const QPointF ray = (m_pressAlong * m_pressSize.width() * k.x() + m_pressAcross * m_pressSize.height() * k.y()) * m_scale / 2.0;
            const qreal length = qMax(1.0, dot(ray, ray));
            const qreal delta = (dot(from, ray) - dot(m_press - c, ray)) / length;
            setHardness(m_auto, m_pressEdge, hardnessOf(m_pressEdge) + delta);
            break;
        }
        case Turn:
            setAngle(angleOf(from));
            break;
        case Valley: {
            // the notch's reach along its own direction sets how round the star is (Krita's ratio)
            const int spikes = m_auto->spikes();
            const qreal slice = M_PI / spikes;
            const QPointF direction = toScreen(QPointF(std::cos(slice), std::sin(slice)));
            const qreal a = m_pressSize.width() / 2.0;
            const qreal reach = qMax(0.5, dot(from, direction) / m_scale);
            qreal b;
            if (m_auto->shape() == Rectangle) {
                b = reach * std::sin(slice);
            } else {
                const qreal rest = 1.0 / (reach * reach) - std::pow(std::cos(slice) / a, 2);
                b = rest > 1e-12 ? std::sin(slice) / std::sqrt(rest) : a;
            }
            m_auto->setratio(qBound(0.01, b / a, 1.0));
            break;
        }
        case ChipWidth:
        case ChipHeight: {
            // by ratios, as sizes are: 100 px of drag doubles it
            const qreal factor = std::pow(2.0, step / 100.0);
            const bool width = m_dragPart == ChipWidth;
            const qreal dragged = (width ? m_pressSize.width() : m_pressSize.height()) * factor;
            const qreal other = width ? m_pressSize.height() : m_pressSize.width();
            setExtents(dragged, linked || !isAuto() ? other * factor : other, width ? m_pressAlong : m_pressAcross);
            break;
        }
        case ChipAngle:
            setAngle(qRound(m_pressAngle + step / 2.0));
            break;
        case ChipPoints:
            m_auto->setspikes(qBound(3, m_pressSpikes + int(step / 14.0), 20));
            break;
        default:
            break;
        }
    }

    /// Types \p chip's value over it
    void type(const Chip &chip)
    {
        const QSizeF size = tipSize();
        qreal value = 0;
        switch (chip.part) {
        case ChipWidth: value = size.width(); break;
        case ChipHeight: value = size.height(); break;
        case ChipAngle: value = angle(); break;
        case ChipPoints: value = m_auto->spikes(); break;
        default: break;
        }
        const Part part = chip.part;
        m_editing = part;
        // typed where it is written, in the chip's own text
        QLineEdit *editor = KisStudioInlineEdit::open(this, chip.rect.toRect(), QLocale().toString(value, 'f', 0), Qt::AlignHCenter,
                                                      chipFont(), [this, part](const QString &text) {
            bool ok = false;
            const qreal typed = QLocale().toDouble(QString(text).remove(i18nc("percent, after the number", "%")).remove(QLatin1Char('%')).remove(QStringLiteral("°")).trimmed(), &ok);
            if (!ok) return;
            m_pressSize = tipSize();
            m_pressAlong = along();
            m_pressAcross = across();
            m_pressFadeA = m_auto->horizontalFade();
            m_pressFadeB = m_auto->verticalFade();
            switch (part) {
            case ChipWidth:
                setExtents(typed, m_linked || !isAuto() ? m_pressSize.height() * typed / m_pressSize.width() : m_pressSize.height(), m_pressAlong);
                break;
            case ChipHeight:
                setExtents(typed, m_linked ? m_pressSize.width() * typed / m_pressSize.height() : m_pressSize.width(), m_pressAcross);
                break;
            case ChipAngle:
                setAngle(typed);
                break;
            case ChipPoints:
                m_auto->setspikes(qBound(3, qRound(typed), 20));
                break;
            default:
                break;
            }
            fit(true);
        });
        connect(editor, &QObject::destroyed, this, [this]() {
            m_editing = None;
            update();
        });
        update();
    }

    KisAutoBrushModel *m_auto;
    KisPredefinedBrushModel *m_predefined;
    lager::cursor<BrushType> m_type;

    qreal m_scale {0};
    QVariantAnimation m_fitAnimation;
    bool m_linked {true};

    mutable QImage m_dab;
    mutable bool m_dabDirty {true};
    mutable QColor m_dabInk;
    mutable QSize m_dabSize;
    mutable bool m_dabStretched {false};
    /// The picture tip and ink the dab was made for; empty for a round one
    mutable QString m_dabPicture;

    Part m_hover {None};
    /// The chip being typed into, whose text the editor stands in for
    Part m_editing {None};
    Part m_dragPart {None};
    bool m_dragging {false};
    bool m_moved {false};
    QPointF m_press;
    QSizeF m_pressSize;
    qreal m_pressAngle {0};
    qreal m_pressFadeA {1};
    qreal m_pressFadeB {1};
    Edge m_pressEdge;
    int m_pressSpikes {2};
    QPointF m_pressAlong;
    QPointF m_pressAcross;
};

/**
 * The edge, across the tip: how solid it is from rim to rim. Its diamond is where the fade begins (the
 * same softness as the tip's own diamond); a curve's points bend it, and pressing the line anywhere else
 * makes the edge a curve and bends it there. The softness is on its top left, the shapes on its top right.
 */
class EdgeCanvas : public QWidget
{
    // a class of its own: a press on it shapes the edge, it doesn't move the window
    Q_OBJECT
public:
    enum Part {
        None,
        Knee,       ///< where the fade begins
        Point,      ///< a curve's point
        Line,       ///< anywhere else on the edge
        ChipSoft,
        TileRound,
        TileSmooth,
        TileCurve,
    };

    EdgeCanvas(KisAutoBrushModel *model, QWidget *parent)
        : QWidget(parent)
        , m_model(model)
    {
        setMouseTracking(true);
        setAttribute(Qt::WA_Hover);
        setFixedHeight(72);
        for (auto signal : {&KisAutoBrushModel::horizontalFadeChanged, &KisAutoBrushModel::verticalFadeChanged}) {
            connect(m_model, signal, this, QOverload<>::of(&QWidget::update));
        }
        connect(m_model, &KisAutoBrushModel::typeChanged, this, QOverload<>::of(&QWidget::update));
        connect(m_model, &KisAutoBrushModel::curveStringChanged, this, QOverload<>::of(&QWidget::update));
    }

    QRectF graph() const
    {
        return QRectF(rect()).adjusted(KisModernStyle::space3, edgeChipsBottom + KisModernStyle::space1, -KisModernStyle::space3, -KisModernStyle::space2);
    }

    qreal half() const
    {
        return graph().width() / 2.0;
    }

    QPointF spot(qreal r, qreal value) const
    {
        const QRectF box = graph();
        return QPointF(box.center().x() + r * half(), box.bottom() - value * box.height());
    }

    /// How far out \p pos is, either side of the middle, as a share of the radius
    qreal reachAt(const QPointF &pos) const
    {
        return qBound(0.0, qAbs(pos.x() - graph().center().x()) / qMax(1.0, half()), 1.0);
    }

    /// \p pos brought over to the right side, where the handles are
    QPointF mirrored(const QPointF &pos) const
    {
        const qreal middle = graph().center().x();
        return QPointF(middle + qAbs(pos.x() - middle), pos.y());
    }

    int softPercent() const
    {
        return qRound((1.0 - hardnessOf(Edge::of(m_model))) * 100);
    }

    QFont chipFont() const
    {
        return KisModernStyle::textFont(font(), 0.88);
    }

    QRectF chipRect(Part part) const
    {
        const qreal inset = KisModernStyle::space2;
        const qreal top = edgeChipsBottom - 24;
        if (part == ChipSoft) {
            const QString text = QLocale().toString(softPercent()) + i18nc("percent, after the number", "%");
            return QRectF(inset, top, QFontMetricsF(chipFont()).horizontalAdvance(text) + 16, 24);
        }
        const int index = part - TileRound;
        const qreal tile = 34;
        return QRectF(width() - inset - (3 - index) * tile, top, tile, 24);
    }

    /// The curve's points shown as handles: not the one where its solid middle ends, which is the diamond
    QVector<int> pointHandles() const
    {
        QVector<int> handles;
        if (m_model->type() != Soft) return handles;
        const QList<KisCubicCurvePoint> points = curvePoints(m_model->curveString());
        // the diamond stands for the second point, or the first when the edge falls from the middle
        const int knee = curveHold(points) > 0.0 ? 1 : 0;
        for (int i = 0; i < points.size(); i++) {
            if (i != knee) handles << i;
        }
        return handles;
    }

    QPointF pointSpot(const KisCubicCurvePoint &point) const
    {
        return spot(std::sqrt(qMax(0.0, point.x())), point.y());
    }

    Part partAt(const QPointF &pos, int *index = nullptr) const
    {
        for (Part part : {ChipSoft, TileRound, TileSmooth, TileCurve}) {
            if (chipRect(part).contains(pos)) return part;
        }
        const QPointF right = mirrored(pos);
        const Edge edge = Edge::of(m_model);
        const EdgeProfile profile(edge);
        const qreal hold = hardnessOf(edge);
        if (QLineF(spot(hold, profile.at(hold)), right).length() <= grabRadius) return Knee;
        const QList<KisCubicCurvePoint> points = curvePoints(m_model->curveString());
        qreal nearest = grabRadius;
        int best = -1;
        Q_FOREACH (int i, pointHandles()) {
            const qreal distance = QLineF(pointSpot(points[i]), right).length();
            if (distance <= nearest) {
                nearest = distance;
                best = i;
            }
        }
        if (best >= 0) {
            if (index) *index = best;
            return Point;
        }
        const QRectF box = graph();
        if (qAbs(pos.x() - box.center().x()) <= half() + 2) {
            if (qAbs(spot(reachAt(pos), profile.at(reachAt(pos))).y() - pos.y()) <= 8) return Line;
        }
        return None;
    }

    QString partTip(Part part) const
    {
        switch (part) {
        case Knee:
        case ChipSoft:
            return KisStudioTips::tip(i18nc("@info:tooltip brush tip", "Soft edge"), KisStudioTips::valueGestures());
        case Point:
            return KisStudioTips::tip(i18nc("@info:tooltip brush tip edge", "Curve point"),
                                      {i18nc("@info:tooltip a gesture", "Drag: move"), i18nc("@info:tooltip a gesture", "Right-click: remove")});
        case Line:
            return KisStudioTips::tip(i18nc("@info:tooltip brush tip", "Edge, across the tip"),
                                      {i18nc("@info:tooltip a gesture", "Drag: bend"), KisStudioTips::rightClickReset()});
        case TileRound:
            return i18nc("@info:tooltip brush tip edge", "Fades out fastest near the rim");
        case TileSmooth:
            return i18nc("@info:tooltip brush tip edge", "A gentle falloff, like an airbrush");
        case TileCurve:
            return i18nc("@info:tooltip brush tip edge", "Shape the falloff yourself on the line");
        default:
            return QString();
        }
    }

    bool event(QEvent *event) override
    {
        if (event->type() == QEvent::ToolTip) {
            auto *help = static_cast<QHelpEvent *>(event);
            const QString tip = partTip(partAt(help->pos()));
            if (tip.isEmpty()) {
                QToolTip::hideText();
            } else {
                QToolTip::showText(help->globalPos(), KisStudioTips::html(tip), this);
            }
            return true;
        }
        return QWidget::event(event);
    }

    /**
     * The edge in the shape \p type, solid to \p hold of the radius. A curve is the one the tip keeps, or the
     * edge as it looks.
     */
    Edge reshaped(int type, qreal hold) const
    {
        const Edge edge = Edge::of(m_model);
        Edge shaped{type, hold, hold, QString()};
        if (type == Soft) {
            const QList<KisCubicCurvePoint> points = edge.type == Soft || edge.curve.isEmpty() ? seeded(edge) : curvePoints(edge.curve);
            shaped.curve = KisCubicCurve(heldAt(points, hold)).toString();
        }
        if (type == Gaussian) shaped.fadeA = shaped.fadeB = smoothFadeFor(hold);
        return shaped;
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QColor ink = palette().color(QPalette::WindowText);
        const QColor accent = palette().color(QPalette::Highlight);
        auto faint = [&ink](qreal alpha) {
            QColor color = ink;
            color.setAlphaF(alpha);
            return color;
        };
        // the bottom of the tip's box
        painter.fillPath(KisModernStyle::squircle(QRectF(rect()).adjusted(0.5, -2 * KisModernStyle::cardRadius, -0.5, -0.5), KisModernStyle::cardRadius), faint(0.045));

        // the edge from rim to rim, filled like the cross-section of a dab
        const Edge edge = Edge::of(m_model);
        const EdgeProfile profile(edge);
        const QRectF box = graph();
        QPainterPath line;
        for (int i = -48; i <= 48; i++) {
            const qreal r = i / 48.0;
            const QPointF point = spot(r, profile.at(qAbs(r)));
            if (i > -48) line.lineTo(point);
            else line.moveTo(point);
        }
        QPainterPath area = line;
        area.lineTo(box.bottomRight());
        area.lineTo(box.bottomLeft());
        area.closeSubpath();
        painter.fillPath(area, faint(0.12));
        QPen pen(ink, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(line);

        const QColor dark(0, 0, 0, 150);
        const QColor light(255, 255, 255, 230);
        const QList<KisCubicCurvePoint> points = curvePoints(m_model->curveString());
        const qreal kneeReach = hardnessOf(edge);
        const QPointF knee = spot(kneeReach, profile.at(kneeReach));
        Q_FOREACH (int i, pointHandles()) {
            // under the diamond, which wins the press there anyway
            if (QLineF(pointSpot(points[i]), knee).length() < 2 * handleRadius) continue;
            const bool hot = (m_hover == Point && m_hoverIndex == i) || (m_dragPart == Point && m_dragIndex == i);
            painter.setPen(QPen(dark, 1.5));
            painter.setBrush(hot ? accent : QColor(Qt::white));
            painter.drawEllipse(pointSpot(points[i]), handleRadius - 1 + (hot ? 1.5 : 0), handleRadius - 1 + (hot ? 1.5 : 0));
        }
        {
            const qreal hold = hardnessOf(edge);
            const QPointF at = spot(hold, profile.at(hold));
            const bool hot = m_hover == Knee || m_dragPart == Knee || m_hover == ChipSoft || m_dragPart == ChipSoft;
            const qreal d = handleRadius + 1 + (hot ? 1.5 : 0);
            QPainterPath diamond;
            diamond.moveTo(at + QPointF(0, -d));
            diamond.lineTo(at + QPointF(d, 0));
            diamond.lineTo(at + QPointF(0, d));
            diamond.lineTo(at + QPointF(-d, 0));
            diamond.closeSubpath();
            painter.setPen(QPen(light, 1.5));
            painter.setBrush(hot ? accent : QColor(20, 20, 24));
            painter.drawPath(diamond);
        }

        // the softness and the shapes, each a picture of the edge it gives
        painter.setFont(chipFont());
        const QRectF soft = chipRect(ChipSoft);
        painter.fillPath(KisModernStyle::squircle(soft, 6), faint(m_hover == ChipSoft ? 0.16 : 0.08));
        if (!m_editing) {
            painter.setPen(ink);
            painter.drawText(soft, Qt::AlignCenter, QLocale().toString(softPercent()) + i18nc("percent, after the number", "%"));
        }
        painter.fillPath(KisModernStyle::squircle(chipRect(TileRound).united(chipRect(TileCurve)), 6), faint(0.08));
        painter.setBrush(Qt::NoBrush);
        for (Part part : {TileRound, TileSmooth, TileCurve}) {
            const QRectF tile = chipRect(part);
            const int type = part == TileRound ? Default : part == TileSmooth ? Gaussian : Soft;
            const bool on = edge.type == type;
            if (on || m_hover == part) painter.fillPath(KisModernStyle::squircle(tile, 6), faint(on ? 0.16 : 0.08));
            painter.setPen(QPen(on || m_hover == part ? ink : KisModernStyle::secondaryInk(ink), 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            // soft enough that the shape shows
            painter.drawPath(EdgeProfile(reshaped(type, qMin(hardnessOf(edge), 0.5))).half(tile.adjusted(8, 6, -8, -6)));
        }
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        m_moved = false;
        if (event->button() != Qt::LeftButton) return;
        int index = -1;
        const Part part = partAt(event->pos(), &index);
        const Edge edge = Edge::of(m_model);
        switch (part) {
        case TileRound:
        case TileSmooth: {
            const int type = part == TileRound ? Default : Gaussian;
            if (edge.type == type) return;
            // as hard as it was; a hard tip turning Smooth is made half soft
            const qreal hold = hardnessOf(edge);
            const Edge shaped = reshaped(type, type == Gaussian ? qMin(hold, 0.5) : hold);
            m_model->sethorizontalFade(shaped.fadeA);
            m_model->setverticalFade(shaped.fadeB);
            m_model->settype(type);
            return;
        }
        case TileCurve:
            if (edge.type != Soft) {
                m_model->setcurveString(reshaped(Soft, hardnessOf(edge)).curve);
                m_model->settype(Soft);
            }
            return;
        case None:
            return;
        case Line: {
            // the edge becomes a curve that looks as it did, with a point here to bend it by
            QList<KisCubicCurvePoint> points = seeded(edge);
            const qreal r = reachAt(event->pos());
            const qreal x = r * r;
            int at = 1;
            while (at < points.size() - 1 && points[at].x() < x) at++;
            // close to a point the curve already has, that one is bent
            if (at > 1 && qAbs(std::sqrt(points[at - 1].x()) - r) < 0.03) {
                at--;
            } else if (at == points.size() - 1 || qAbs(std::sqrt(points[at].x()) - r) >= 0.03) {
                points.insert(at, KisCubicCurvePoint(x, KisCubicCurve(points).value(x)));
            }
            m_model->setcurveString(KisCubicCurve(points).toString());
            m_model->settype(Soft);
            m_dragIndex = at;
            m_dragPart = Point;
            break;
        }
        case Point:
            m_dragIndex = index;
            m_dragPart = Point;
            break;
        default:
            m_dragPart = part;
            break;
        }
        m_press = event->pos();
        m_pressEdge = Edge::of(m_model);
        m_pressPoints = curvePoints(m_pressEdge.curve);
        update();
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        const QPointF pos = event->pos();
        if (m_dragPart == None) {
            int index = -1;
            const Part hover = partAt(pos, &index);
            if (hover != m_hover || index != m_hoverIndex) {
                m_hover = hover;
                m_hoverIndex = index;
                switch (hover) {
                case None:
                    unsetCursor();
                    break;
                case TileRound:
                case TileSmooth:
                case TileCurve:
                    setCursor(Qt::PointingHandCursor);
                    break;
                case Knee:
                case ChipSoft:
                    setCursor(Qt::SizeHorCursor);
                    break;
                default:
                    setCursor(Qt::CrossCursor);
                }
                update();
            }
            return;
        }
        // the release went elsewhere (the window lost the pointer): the drag ended there
        if (!(event->buttons() & Qt::LeftButton)) {
            m_dragPart = None;
            m_dragIndex = -1;
            update();
            return;
        }
        if (!m_moved && QLineF(pos, m_press).length() < 3) return;
        m_moved = true;
        switch (m_dragPart) {
        case Knee:
            setHardness(m_model, m_pressEdge, reachAt(pos));
            break;
        case ChipSoft:
            // more soft to the right, as the value reads
            setHardness(m_model, m_pressEdge, hardnessOf(m_pressEdge) - (pos.x() - m_press.x()) / 200.0);
            break;
        case Point: {
            QList<KisCubicCurvePoint> points = m_pressPoints;
            const int i = m_dragIndex;
            if (i < 0 || i >= points.size()) break;
            const QRectF box = graph();
            const bool end = i == 0 || i == points.size() - 1;
            // pulled well off the edge, a point goes
            if (!end && (pos.y() < box.top() - 24 || pos.y() > height() + 24)) {
                points.removeAt(i);
            } else {
                qreal r = i == 0 ? 0.0 : i == points.size() - 1 ? 1.0 : reachAt(pos);
                if (!end) r = qBound(std::sqrt(points[i - 1].x()) + 0.02, r, std::sqrt(points[i + 1].x()) - 0.02);
                const qreal value = qBound(0.0, (box.bottom() - pos.y()) / qMax(1.0, box.height()), 1.0);
                points[i].setPosition(QPointF(r * r, value));
            }
            m_model->setcurveString(KisCubicCurve(points).toString());
            break;
        }
        default:
            break;
        }
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton) return;
        m_dragPart = None;
        m_dragIndex = -1;
        update();
    }

    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        const Part part = partAt(event->pos());
        if (part != Knee && part != ChipSoft) return;
        m_editing = true;
        // typed where it is written, in the chip's own text
        QLineEdit *editor = KisStudioInlineEdit::open(this, chipRect(ChipSoft).toRect(), QLocale().toString(softPercent()), Qt::AlignHCenter,
                                                      chipFont(), [this](const QString &text) {
            bool ok = false;
            const qreal typed = QLocale().toDouble(QString(text).remove(i18nc("percent, after the number", "%")).remove(QLatin1Char('%')).trimmed(), &ok);
            if (ok) setHardness(m_model, Edge::of(m_model), 1.0 - typed / 100.0);
        });
        connect(editor, &QObject::destroyed, this, [this]() {
            m_editing = false;
            update();
        });
        update();
    }

    void contextMenuEvent(QContextMenuEvent *event) override
    {
        event->accept();
        // a finger held still at the end of a drag isn't asking for a reset
        if (m_moved) return;
        int index = -1;
        const Part part = partAt(event->pos(), &index);
        if (part == Point) {
            QList<KisCubicCurvePoint> points = curvePoints(m_model->curveString());
            if (index > 0 && index < points.size() - 1) {
                points.removeAt(index);
                m_model->setcurveString(KisCubicCurve(points).toString());
            }
            return;
        }
        if (part == Knee || part == ChipSoft || part == Line) makeHard(m_model);
    }

    void leaveEvent(QEvent *) override
    {
        m_hover = None;
        m_hoverIndex = -1;
        update();
    }

private:
    KisAutoBrushModel *m_model;
    Part m_hover {None};
    int m_hoverIndex {-1};
    Part m_dragPart {None};
    int m_dragIndex {-1};
    bool m_moved {false};
    bool m_editing {false};
    QPointF m_press;
    Edge m_pressEdge;
    QList<KisCubicCurvePoint> m_pressPoints;
};

/// Shows a page only for generated tips, as the tip's kind changes; watching while the page lives
class AutoShown : public QObject
{
public:
    AutoShown(lager::cursor<BrushType> type, QWidget *page)
        : QObject(page)
        , m_type(type)
    {
        page->setVisible(m_type.get() != Predefined);
        m_type.watch([page](BrushType kind) { page->setVisible(kind != Predefined); });
    }

private:
    lager::cursor<BrushType> m_type;
};

} // namespace

struct KisBrushTipQuickPage::Private
{
    KisAutoBrushModel *autoModel {nullptr};
    KisPredefinedBrushModel *predefinedModel {nullptr};
    lager::cursor<BrushType> type;
    TipCanvas *canvas {nullptr};
    EdgeCanvas *edge {nullptr};
    KisStudioChips *shapes {nullptr};
    /// The picture tips the shapes row shows, after round, square and star
    QStringList tipIds;
    /// Their pictures, small and in the ink they were made in, and names: by md5
    QHash<QString, QImage> tipPictures;
    QHash<QString, QString> tipNames;
    QColor tipInk;

    std::optional<lager::cursor<PrecisionData>> precision;
    bool withRest {true};
    /// Anti-aliasing, auto neutral point: those that apply
    KisStudioChips *switches {nullptr};
    QVector<std::function<void()>> flips;
    KisStudioValueRow *precisionRow {nullptr};
    /// How a picture tip in color is used, and its adjustments
    KisStudioChips *application {nullptr};
    KisStudioValueRow *brightness {nullptr};
    KisStudioValueRow *contrast {nullptr};
    KisStudioValueRow *neutral {nullptr};
};

KisBrushTipQuickPage::KisBrushTipQuickPage(KisAutoBrushModel *autoModel,
                                           KisPredefinedBrushModel *predefinedModel,
                                           lager::cursor<BrushType> type,
                                           std::optional<lager::cursor<PrecisionData>> precision,
                                           QWidget *parent,
                                           bool withRest)
    : QWidget(parent)
    , d(new Private)
{
    d->autoModel = autoModel;
    d->predefinedModel = predefinedModel;
    d->type = type;
    d->precision = precision;
    d->withRest = withRest;

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(KisModernStyle::space2);

    // the tip and, for a generated one, its edge under it: one box
    auto *card = new QWidget(this);
    auto *cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(0, 0, 0, 0);
    cardLayout->setSpacing(0);
    d->canvas = new TipCanvas(autoModel, predefinedModel, type, card);
    cardLayout->addWidget(d->canvas);
    d->edge = new EdgeCanvas(autoModel, card);
    cardLayout->addWidget(d->edge);
    new AutoShown(type, d->edge);
    layout->addWidget(card);

    d->shapes = new KisStudioChips(this);
    d->shapes->setTileSize(36);
    layout->addWidget(d->shapes);
    connect(d->shapes, &KisStudioChips::clicked, this, [this](int index) {
        KisAutoBrushModel *model = d->autoModel;
        if (index == 0 || index == 1) {
            model->setshape(index == 0 ? Circle : Rectangle);
            model->setspikes(2);
            d->type.set(Auto);
        } else if (index == 2) {
            if (model->spikes() <= 2) model->setspikes(5);
            // round, its points wouldn't show: Krita's star is the ellipse folded into them
            if (model->ratio() > 0.9) model->setratio(0.35);
            d->type.set(Auto);
        } else if (index - 3 < d->tipIds.size()) {
            auto source = KisGlobalResourcesInterface::instance()->source<KisBrush>(ResourceType::Brushes);
            const KisBrushSP match = source.bestMatch(d->tipIds[index - 3], QString(), QString());
            if (!!match) usePictureTip(d->predefinedModel, d->type, match);
        } else {
            // all picture tips
            auto *model = new KisResourceModel(ResourceType::Brushes, this);
            QStringList ids;
            QHash<QString, QString> names;
            QHash<QString, QImage> thumbnails;
            for (int row = 0; row < model->rowCount(); row++) {
                const QModelIndex at = model->index(row, 0);
                const QString md5 = model->data(at, Qt::UserRole + KisAbstractResourceModel::MD5).toString();
                if (md5.isEmpty() || ids.contains(md5)) continue;
                ids << md5;
                names.insert(md5, model->data(at, Qt::UserRole + KisAbstractResourceModel::Name).toString());
                thumbnails.insert(md5, model->data(at, Qt::UserRole + KisAbstractResourceModel::Thumbnail).value<QImage>());
            }
            delete model;
            KisStudioPicker::Source source;
            source.pinnedKey = QStringLiteral("StudioPinnedBrushTips");
            source.initialPinned = []() { return QStringList(); };
            source.allIds = [ids]() { return ids; };
            source.otherGroups = [ids](const QStringList &pinned) {
                KisStudioPicker::Group group{i18nc("brush tip group", "Tips"), {}};
                Q_FOREACH (const QString &id, ids) {
                    if (!pinned.contains(id)) group.ids << id;
                }
                return QVector<KisStudioPicker::Group>{group};
            };
            source.name = [names](const QString &id) { return names.value(id); };
            source.paintPreview = [thumbnails](QPainter &painter, const QRect &rect, const QString &id) {
                const QImage image = thumbnails.value(id);
                if (image.isNull()) return;
                const QSize fitted = image.size().scaled(rect.size(), Qt::KeepAspectRatio);
                QRect target(QPoint(0, 0), fitted);
                target.moveCenter(rect.center());
                painter.fillRect(rect, Qt::white);
                painter.drawImage(target, image);
            };
            source.recent = []() { return KisConfig(true).readEntry<QStringList>(recentTipsKey, QStringList()); };
            source.tiles = true;
            source.tileSize = QSize(84, 96);
            source.yoursTitle = i18nc("brush tip group", "Your tips");
            source.searchPlaceholder = i18nc("@info:placeholder", "Search %1 tips", ids.size());
            const QString current = d->type.get() == Predefined ? d->predefinedModel->resourceSignature().md5sum : QString();
            auto *picker = new KisStudioPicker(source, current, {}, window());
            connect(picker, &KisStudioPicker::sigPicked, this, [this](const QString &md5) {
                auto tips = KisGlobalResourcesInterface::instance()->source<KisBrush>(ResourceType::Brushes);
                const KisBrushSP match = tips.bestMatch(md5, QString(), QString());
                if (!!match) usePictureTip(d->predefinedModel, d->type, match);
                updateShapes();
            });
            picker->popupBelow(d->shapes);
        }
        updateShapes();
    });

    d->switches = new KisStudioChips(this);
    layout->addWidget(d->switches);
    connect(d->switches, &KisStudioChips::clicked, this, [this](int index) {
        if (index >= 0 && index < d->flips.size()) d->flips[index]();
        updateRest();
    });

    KisStudioValueRow::Range levels;
    levels.minimum = 1;
    levels.maximum = 5;
    d->precisionRow = new KisStudioValueRow(i18nc("brush tip precision", "Precision"), this);
    d->precisionRow->setCompact(true);
    d->precisionRow->setRange(levels);
    d->precisionRow->setDefaultValue(5);
    d->precisionRow->setToolTip(i18nc("@info:tooltip", "1 fast → 5 exact"));
    layout->addWidget(d->precisionRow);
    connect(d->precisionRow, &KisStudioValueRow::valueChanged, this, [this](qreal value) {
        if (!d->precision) return;
        // a level chosen by hand is the level, not the size's
        PrecisionData data = d->precision->get();
        data.precisionLevel = qBound(1, qRound(value), 5);
        data.useAutoPrecision = false;
        d->precision->set(data);
    });
    connect(d->precisionRow, &KisStudioValueRow::automaticToggled, this, [this](bool on) {
        if (!d->precision) return;
        PrecisionData data = d->precision->get();
        data.useAutoPrecision = on;
        d->precision->set(data);
    });

    d->application = new KisStudioChips(this);
    layout->addWidget(d->application);
    connect(d->application, &KisStudioChips::clicked, this, [this](int index) {
        d->predefinedModel->setapplication(index);
        updateRest();
    });
    auto adjustment = [this, layout](const QString &name, const QString &tip, qreal minimum, qreal maximum, qreal standard) {
        auto *row = new KisStudioValueRow(name, this);
        row->setCompact(true);
        KisStudioValueRow::Range range;
        range.minimum = minimum;
        range.maximum = maximum;
        row->setRange(range);
        row->setDefaultValue(standard);
        row->setToolTip(tip);
        layout->addWidget(row);
        return row;
    };
    d->brightness = adjustment(i18nc("picture brush tip adjustment", "Brightness"), i18nc("@info:tooltip", "-100 darker → 100 lighter"), -100, 100, 0);
    d->contrast = adjustment(i18nc("picture brush tip adjustment", "Contrast"), i18nc("@info:tooltip", "-100 flat → 100 strong"), -100, 100, 0);
    d->neutral = adjustment(i18nc("picture brush tip adjustment: the lightness that leaves the color as it is", "Neutral point"),
                            i18nc("@info:tooltip", "The tip's lightness that leaves the color as it is"), 0, 255, 127);
    connect(d->brightness, &KisStudioValueRow::valueChanged, this, [this](qreal value) { d->predefinedModel->setbrightnessAdjustment(qRound(value)); });
    connect(d->contrast, &KisStudioValueRow::valueChanged, this, [this](qreal value) { d->predefinedModel->setcontrastAdjustment(qRound(value)); });
    connect(d->neutral, &KisStudioValueRow::valueChanged, this, [this](qreal value) { d->predefinedModel->setadjustmentMidPoint(qRound(value)); });

    connect(autoModel, &KisAutoBrushModel::shapeChanged, this, &KisBrushTipQuickPage::updateShapes);
    connect(autoModel, &KisAutoBrushModel::spikesChanged, this, &KisBrushTipQuickPage::updateShapes);
    connect(predefinedModel, &KisPredefinedBrushModel::resourceSignatureChanged, this, &KisBrushTipQuickPage::updateShapes);
    connect(autoModel, &KisAutoBrushModel::antialiasEdgesChanged, this, &KisBrushTipQuickPage::updateRest);
    connect(predefinedModel, &KisPredefinedBrushModel::applicationSwitchStateChanged, this, &KisBrushTipQuickPage::updateRest);
    connect(predefinedModel, &KisPredefinedBrushModel::brightnessAdjustmentChanged, this, &KisBrushTipQuickPage::updateRest);
    connect(predefinedModel, &KisPredefinedBrushModel::contrastAdjustmentChanged, this, &KisBrushTipQuickPage::updateRest);
    connect(predefinedModel, &KisPredefinedBrushModel::adjustmentMidPointChanged, this, &KisBrushTipQuickPage::updateRest);
    connect(predefinedModel, &KisPredefinedBrushModel::autoAdjustMidPointChanged, this, &KisBrushTipQuickPage::updateRest);
    if (d->precision) d->precision->watch([this](const PrecisionData &) { updateRest(); });
    updateShapes();
}

KisBrushTipQuickPage::~KisBrushTipQuickPage()
{
}

void KisBrushTipQuickPage::updateShapes()
{
    const bool isAuto = d->type.get() != Predefined;
    const int spikes = d->autoModel->spikes();
    const int shape = d->autoModel->shape();
    const QColor ink = palette().color(QPalette::WindowText);

    QVector<KisStudioChips::Chip> chips;
    KisStudioChips::Chip round;
    round.toolTip = i18nc("brush tip shape", "Round");
    round.on = isAuto && spikes <= 2 && shape == Circle;
    round.picture = [ink](QPainter &painter, const QRectF &box) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(ink);
        painter.drawEllipse(box.center(), 7, 7);
    };
    chips << round;
    KisStudioChips::Chip square;
    square.toolTip = i18nc("brush tip shape", "Square");
    square.on = isAuto && spikes <= 2 && shape == Rectangle;
    square.picture = [ink](QPainter &painter, const QRectF &box) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(ink);
        painter.drawRect(QRectF(box.center() - QPointF(6.5, 6.5), QSizeF(13, 13)));
    };
    chips << square;
    KisStudioChips::Chip star;
    star.toolTip = i18nc("brush tip shape", "Star");
    star.on = isAuto && spikes > 2;
    star.picture = [ink](QPainter &painter, const QRectF &box) {
        QPainterPath path;
        for (int i = 0; i < 10; i++) {
            const qreal r = i % 2 ? 3.4 : 8.0;
            const qreal a = M_PI / 2 + i * M_PI / 5;
            const QPointF point = box.center() + QPointF(r * std::cos(a), -r * std::sin(a));
            if (i) path.lineTo(point);
            else path.moveTo(point);
        }
        path.closeSubpath();
        painter.setPen(Qt::NoPen);
        painter.setBrush(ink);
        painter.drawPath(path);
    };
    chips << star;

    // the current picture tip and the ones used last
    QStringList ids;
    if (!isAuto) ids << d->predefinedModel->resourceSignature().md5sum;
    Q_FOREACH (const QString &md5, KisConfig(true).readEntry<QStringList>(recentTipsKey, QStringList())) {
        if (!ids.contains(md5)) ids << md5;
    }
    d->tipIds.clear();
    if (ink != d->tipInk) {
        d->tipPictures.clear();
        d->tipInk = ink;
    }
    auto source = KisGlobalResourcesInterface::instance()->source<KisBrush>(ResourceType::Brushes);
    Q_FOREACH (const QString &md5, ids) {
        if (d->tipIds.size() >= 4) break;
        // looked up and tinted once: this runs with each step of the star's points dragged
        if (!d->tipPictures.contains(md5)) {
            const KisBrushSP match = source.bestMatch(md5, QString(), QString());
            if (!match) continue;
            const enumBrushType kind = match->brushType();
            const QImage small = match->brushTipImage().scaled(64, 64, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            d->tipPictures.insert(md5, tinted(small, ink, kind == MASK || kind == PIPE_MASK));
            d->tipNames.insert(md5, match->name());
        }
        d->tipIds << md5;
        KisStudioChips::Chip tip;
        tip.toolTip = d->tipNames.value(md5);
        tip.on = !isAuto && md5 == d->predefinedModel->resourceSignature().md5sum;
        const QImage image = d->tipPictures.value(md5);
        tip.picture = [image](QPainter &painter, const QRectF &box) {
            const QSizeF fitted = QSizeF(image.size()).scaled(box.size(), Qt::KeepAspectRatio);
            QRectF target(QPointF(0, 0), fitted);
            target.moveCenter(box.center());
            painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
            painter.drawImage(target, image);
        };
        chips << tip;
    }
    KisStudioChips::Chip more;
    more.toolTip = i18nc("@info:tooltip", "All tips");
    more.picture = [ink](QPainter &painter, const QRectF &box) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(KisModernStyle::secondaryInk(ink));
        for (int i = -1; i <= 1; i++) {
            painter.drawEllipse(box.center() + QPointF(i * 6, 0), 1.6, 1.6);
        }
    };
    chips << more;
    d->shapes->setChips(chips);
    updateRest();
}

void KisBrushTipQuickPage::updateRest()
{
    if (!d->withRest) {
        for (QWidget *widget : std::initializer_list<QWidget *>{d->switches, d->precisionRow, d->application, d->brightness, d->contrast, d->neutral}) {
            widget->hide();
        }
        return;
    }
    const bool isAuto = d->type.get() == Auto;
    const bool isPicture = d->type.get() == Predefined;
    const ComboBoxState uses = d->predefinedModel->applicationSwitchState();
    const bool lightness = isPicture && uses.currentIndex == LIGHTNESSMAP;

    QVector<KisStudioChips::Chip> switches;
    d->flips.clear();
    auto add = [&](const QString &text, const QString &tip, bool on, std::function<void()> flip) {
        KisStudioChips::Chip chip;
        chip.text = text;
        chip.toolTip = tip;
        chip.on = on;
        switches << chip;
        d->flips << flip;
    };
    if (isAuto) {
        add(i18nc("brush tip switch", "Anti-aliasing"), i18nc("@info:tooltip", "Smooth hard edges"), d->autoModel->antialiasEdges(),
            [this]() { d->autoModel->setantialiasEdges(!d->autoModel->antialiasEdges()); });
    }
    if (lightness) {
        add(i18nc("brush tip switch", "Auto neutral point"), i18nc("@info:tooltip", "The tip's own middle lightness"), d->predefinedModel->autoAdjustMidPoint(),
            [this]() { d->predefinedModel->setautoAdjustMidPoint(!d->predefinedModel->autoAdjustMidPoint()); });
    }
    d->switches->setSwitches(true);
    d->switches->setChips(switches);
    d->switches->setVisible(!switches.isEmpty());

    // auto spacing is on the Spacing row; auto precision on this one, the level quiet while the size chooses it
    d->precisionRow->setVisible(bool(d->precision));
    if (d->precision) {
        KisSignalsBlocker blocker(d->precisionRow);
        const PrecisionData precision = d->precision->get();
        d->precisionRow->setValue(precision.precisionLevel);
        d->precisionRow->setAutomatic(true, precision.useAutoPrecision);
        d->precisionRow->setInactive(precision.useAutoPrecision);
    }

    // a picture tip in color: as a mask, as it is, or lighting the brush color
    QVector<KisStudioChips::Chip> applications;
    if (isPicture && uses.enabled) {
        const QStringList names = {i18nc("picture brush tip used as", "Mask"), i18nc("picture brush tip used as", "Its colors"),
                                   i18nc("picture brush tip used as", "Lightness"), i18nc("picture brush tip used as", "Gradient")};
        const QStringList tips = {i18nc("@info:tooltip", "In the brush color"), i18nc("@info:tooltip", "Painted as it is"),
                                  i18nc("@info:tooltip", "The brush color, lit by the tip"), i18nc("@info:tooltip", "The tip's lights in the gradient's colors")};
        for (int i = 0; i < uses.items.size() && i < names.size(); i++) {
            KisStudioChips::Chip chip;
            chip.text = names[i];
            chip.toolTip = tips[i];
            chip.on = i == uses.currentIndex;
            applications << chip;
        }
    }
    d->application->setChips(applications);
    d->application->setVisible(!applications.isEmpty());

    const bool adjusted = isPicture && d->predefinedModel->adjustmentsEnabled();
    d->brightness->setVisible(adjusted);
    d->contrast->setVisible(adjusted);
    d->neutral->setVisible(lightness && !d->predefinedModel->autoAdjustMidPoint());
    KisSignalsBlocker blocker(d->brightness, d->contrast, d->neutral);
    d->brightness->setValue(d->predefinedModel->brightnessAdjustment());
    d->contrast->setValue(d->predefinedModel->contrastAdjustment());
    d->neutral->setValue(d->predefinedModel->adjustmentMidPoint());
}

QWidget *KisBrushTipQuickPage::createGrainPage(KisAutoBrushModel *autoModel, lager::cursor<BrushType> type, QWidget *parent)
{
    auto *page = new QWidget(parent);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(KisModernStyle::space1);

    // the tip's own grain: a speckle in the dab, and dabs with holes
    auto *noise = new KisStudioValueRow(i18nc("brush tip grain: random speckle in each dab", "Noise"), page);
    auto *coverage = new KisStudioValueRow(i18nc("brush tip grain: how much of the dab is painted", "Coverage"), page);
    KisStudioValueRow::Range percent;
    percent.minimum = 0;
    percent.maximum = 100;
    percent.suffix = i18nc("percent, after the number", "%");
    noise->setRange(percent);
    // none of the dab painted is no brush at all
    KisStudioValueRow::Range painted = percent;
    painted.minimum = 1;
    coverage->setRange(painted);
    noise->setDefaultValue(0);
    coverage->setDefaultValue(100);
    noise->setToolTip(i18nc("@info:tooltip", "0% smooth → 100% grainy"));
    coverage->setToolTip(i18nc("@info:tooltip", "100% solid → 10% a few dots"));
    layout->addWidget(noise);
    layout->addWidget(coverage);

    auto show = [noise, coverage, autoModel]() {
        noise->setValue(autoModel->randomness());
        coverage->setValue(autoModel->density());
    };
    show();
    QObject::connect(autoModel, &KisAutoBrushModel::randomnessChanged, noise, show);
    QObject::connect(autoModel, &KisAutoBrushModel::densityChanged, coverage, show);
    QObject::connect(noise, &KisStudioValueRow::valueChanged, autoModel, [autoModel](qreal value) { autoModel->setrandomness(value); });
    QObject::connect(coverage, &KisStudioValueRow::valueChanged, autoModel, [autoModel](qreal value) { autoModel->setdensity(value); });

    // a picture tip has no grain of its own, whichever kind of tip is picked
    new AutoShown(type, page);
    return page;
}

#include "KisBrushTipEditor.moc"
