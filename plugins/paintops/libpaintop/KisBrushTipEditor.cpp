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
#include <QPointer>
#include <QVBoxLayout>
#include <QVariantAnimation>
#include <QtMath>

#include <klocalizedstring.h>

#include <KisGlobalResourcesInterface.h>
#include <KisModernStyle.h>
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
#include <widgets/KisCurvePresetStrip.h>
#include <widgets/KisStudioChips.h>
#include <widgets/KisStudioInlineEdit.h>
#include <widgets/KisStudioPicker.h>
#include <widgets/KisStudioPopup.h>
#include <widgets/KisStudioValueRow.h>
#include <widgets/kis_curve_widget.h>

#include "KisAutoBrushModel.h"
#include "KisPredefinedBrushModel.h"

using namespace KisBrushModel;

namespace {

const QString linkedKey = QStringLiteral("BrushTipSizeLinked");
const QString fadeLinkedKey = QStringLiteral("BrushTipFadeLinked");
const QString recentTipsKey = QStringLiteral("StudioRecentBrushTips");
constexpr int handleRadius = 5;
constexpr int grabRadius = 10;
constexpr int stalk = 26;

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

/// The edge's falloff from the middle (x = 0) out (x = 1), for drawing it
qreal falloff(int type, const QString &curve, qreal fade, qreal x)
{
    if (type == Gaussian) return std::exp(-x * x * 4.0);
    if (type == Soft && !curve.isEmpty()) return qBound(0.0, KisCubicCurve(curve).value(x), 1.0);
    if (x <= fade) return 1.0;
    return qBound(0.0, 1.0 - (x - fade) / qMax(0.01, 1.0 - fade), 1.0);
}

QPainterPath falloffPath(const QRectF &box, int type, const QString &curve, qreal fade)
{
    QPainterPath path;
    for (int i = 0; i <= 24; i++) {
        const qreal x = i / 24.0;
        const QPointF point(box.left() + x * box.width(), box.bottom() - falloff(type, curve, fade, x) * box.height());
        if (i) path.lineTo(point);
        else path.moveTo(point);
    }
    return path;
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
        AxisA,       ///< along the tip's angle (its width)
        AxisAOpposite,
        AxisB,       ///< across it (its height)
        AxisBOpposite,
        FadeA,
        FadeB,
        Turn,
        Valley,      ///< a star's notch between two points: how deep they go
        ChipWidth,
        ChipLink,
        ChipHeight,
        ChipAngle,
        ChipFadeA,
        ChipFadeLink,
        ChipFadeB,
        ChipEdge,
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
        setFixedHeight(184);
        m_linked = KisConfig(true).readEntry(linkedKey, true);
        m_fadeLinked = KisConfig(true).readEntry(fadeLinkedKey, true);

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

    QPointF center() const
    {
        return QRectF(rect()).center() + QPointF(0, 4);
    }

    /// How far the tip may reach from the middle of the box
    qreal room() const
    {
        return qMax(20.0, qMin(width() / 2.0 - 46, height() / 2.0 - 36));
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
        case AxisAOpposite:
            return c - along() * size.width();
        case AxisB:
            return c + across() * size.height();
        case AxisBOpposite:
            return c - across() * size.height();
        case FadeA:
            return c + along() * size.width() * m_auto->horizontalFade();
        case FadeB:
            return c + across() * size.height() * m_auto->verticalFade();
        case Turn:
            return c + along() * (size.width() + stalk);
        case Valley:
            return c + toScreen(valleyLocal());
        default:
            return c;
        }
    }

    QVector<Part> handles() const
    {
        if (!isAuto()) return {AxisA, AxisAOpposite, Turn};
        if (isStar()) return {AxisA, Valley, FadeA, Turn};
        return {AxisA, AxisAOpposite, AxisB, AxisBOpposite, FadeA, FadeB, Turn};
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
                const qreal w = chip.part == ChipEdge ? 52 : chip.icon.isEmpty() ? metrics.horizontalAdvance(chip.text) + 2 * pad : 22;
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
            const int softA = qRound((1.0 - m_auto->horizontalFade()) * 100);
            const int softB = qRound((1.0 - m_auto->verticalFade()) * 100);
            if (softA == softB && m_fadeLinked) {
                row(QPointF(inset, height() - inset), false, true, {{ChipFadeA, {}, QLocale().toString(softA) + i18nc("percent, after the number", "%"), {}},
                                                                    {ChipFadeLink, {}, {}, QStringLiteral("studio_link")}});
            } else {
                row(QPointF(inset, height() - inset), false, true, {{ChipFadeA, {}, QLocale().toString(softA) + i18nc("percent, after the number", "%"), {}},
                                                                    {ChipFadeLink, {}, {}, m_fadeLinked ? QStringLiteral("studio_link") : QStringLiteral("studio_unlink")},
                                                                    {ChipFadeB, {}, QLocale().toString(softB) + i18nc("percent, after the number", "%"), {}}});
            }
            row(QPointF(width() - inset, height() - inset), true, true, {{ChipEdge, {}, {}, {}}});
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
            return m_linked ? i18nc("@info:tooltip brush tip", "Linked: width = height") : i18nc("@info:tooltip brush tip", "Unlinked: stretch freely");
        case ChipAngle:
            return i18nc("@info:tooltip brush tip", "Tip angle");
        case ChipFadeA:
            return m_fadeLinked ? i18nc("@info:tooltip brush tip", "Soft edge: 0% crisp → 100% all fade") : i18nc("@info:tooltip brush tip", "Soft edge, across");
        case ChipFadeB:
            return i18nc("@info:tooltip brush tip", "Soft edge, down");
        case ChipFadeLink:
            return m_fadeLinked ? i18nc("@info:tooltip brush tip", "Linked: the same softness all round")
                                : i18nc("@info:tooltip brush tip", "Unlinked: softer one way");
        case ChipEdge:
            return i18nc("@info:tooltip brush tip", "Edge falloff: hard, curve or gaussian");
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
                QToolTip::showText(help->globalPos(), tip, this);
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
            // the fade handles lie inside the size handles: they win when as near
            if (distance < nearest || (distance <= nearest && (part == FadeA || part == FadeB))) {
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
        painter.fillPath(KisModernStyle::squircle(box, KisModernStyle::cardRadius), well);

        const QSizeF size = tipSize() * m_scale;
        if (isAuto()) {
            if (m_dabDirty || m_dabInk != ink || m_dabSize != size.toSize() || !m_dabPicture.isEmpty()) {
                m_dabDirty = false;
                m_dabInk = ink;
                m_dabSize = size.toSize();
                m_dabPicture.clear();
                m_dab = autoDab(m_auto->bakedOptionData(), size.width(), size.height(), ink);
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
            if (isAuto()) painter.drawImage(QPointF(-m_dab.width() / 2.0, -m_dab.height() / 2.0), m_dab);
            else painter.drawImage(dabRect, m_dab);
        }
        painter.setBrush(Qt::NoBrush);
        QPen outline(accent, 1);
        outline.setCosmetic(true);
        painter.setPen(outline);
        if (isAuto() && m_auto->spikes() <= 2) {
            if (m_auto->shape() == Rectangle) painter.drawRect(dabRect);
            else painter.drawEllipse(dabRect);
            QPen dashed(accent, 1, Qt::DashLine);
            dashed.setCosmetic(true);
            painter.setPen(dashed);
            const QRectF hard(-dabRect.width() / 2 * m_auto->horizontalFade(), -dabRect.height() / 2 * m_auto->verticalFade(),
                              dabRect.width() * m_auto->horizontalFade(), dabRect.height() * m_auto->verticalFade());
            if (m_auto->shape() == Rectangle) painter.drawRect(hard);
            else painter.drawEllipse(hard);
        } else if (isStar()) {
            const bool rectangle = m_auto->shape() == Rectangle;
            const qreal a = dabRect.width() / 2;
            const qreal b = dabRect.height() / 2;
            painter.drawPath(starOutline(a, b, m_auto->spikes(), rectangle));
            QPen dashed(accent, 1, Qt::DashLine);
            dashed.setCosmetic(true);
            painter.setPen(dashed);
            painter.drawPath(starOutline(a * m_auto->horizontalFade(), b * m_auto->verticalFade(), m_auto->spikes(), rectangle));
        } else if (!isAuto()) {
            painter.drawRect(dabRect);
        }
        painter.restore();

        // the turning handle's stalk
        painter.setPen(QPen(accent, 1));
        painter.drawLine(partPosition(AxisA), partPosition(Turn));

        Q_FOREACH (Part part, handles()) {
            const QPointF at = partPosition(part);
            const bool hot = part == m_hover || part == m_dragPart;
            const qreal r = handleRadius + (hot ? 1.5 : 0);
            if (part == FadeA || part == FadeB) {
                QPainterPath diamond;
                diamond.moveTo(at + QPointF(0, -r));
                diamond.lineTo(at + QPointF(r, 0));
                diamond.lineTo(at + QPointF(0, r));
                diamond.lineTo(at + QPointF(-r, 0));
                diamond.closeSubpath();
                painter.setPen(Qt::NoPen);
                painter.setBrush(accent);
                painter.drawPath(diamond);
            } else if (part == Turn) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(accent);
                painter.drawEllipse(at, r + 1, r + 1);
            } else {
                QColor fill = palette().color(QPalette::Window);
                fill.setAlpha(255);
                painter.setPen(QPen(accent, 2));
                painter.setBrush(fill);
                painter.drawEllipse(at, r, r);
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
            } else if (chip.part == ChipEdge) {
                painter.setPen(QPen(ink, 1.5));
                painter.setBrush(Qt::NoBrush);
                const QRectF graph = chip.rect.adjusted(8, 6, -8, -6);
                painter.drawPath(falloffPath(graph, m_auto->type(), m_auto->curveString(), m_auto->horizontalFade()));
            } else if (chip.part != m_editing) {
                painter.setPen(ink);
                painter.drawText(chip.rect, Qt::AlignCenter, chip.text);
            }
        }
    }

    void mousePressEvent(QMouseEvent *event) override
    {
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
        if (part == ChipFadeLink) {
            m_fadeLinked = !m_fadeLinked;
            KisConfig(false).writeEntry(fadeLinkedKey, m_fadeLinked);
            update();
            return;
        }
        if (part == ChipEdge) {
            showEdge();
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
                case ChipFadeLink:
                case ChipEdge:
                    setCursor(Qt::PointingHandCursor);
                    break;
                case ChipWidth:
                case ChipHeight:
                case ChipAngle:
                case ChipFadeA:
                case ChipFadeB:
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
            if (chip.part == part && (part == ChipWidth || part == ChipHeight || part == ChipAngle || part == ChipFadeA
                                      || part == ChipFadeB || part == ChipPoints)) {
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
        // the values' reset: round, upright, hard
        const Part part = partAt(event->pos());
        if (part == ChipAngle || part == Turn) {
            setAngle(0);
        } else if (isAuto() && (part == ChipFadeA || part == ChipFadeB || part == FadeA || part == FadeB)) {
            m_auto->sethorizontalFade(1.0);
            m_auto->setverticalFade(1.0);
        } else if (isAuto() && (part == ChipHeight || part == AxisB || part == AxisBOpposite || part == Valley)) {
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
        const bool fadeLinked = m_fadeLinked != flip;
        const qreal step = moved.x();
        switch (m_dragPart) {
        case AxisA:
        case AxisAOpposite:
        case AxisB:
        case AxisBOpposite: {
            const bool alongAxis = m_dragPart == AxisA || m_dragPart == AxisAOpposite;
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
        case FadeA:
        case FadeB: {
            const bool alongAxis = m_dragPart == FadeA;
            const QPointF axis = alongAxis ? m_pressAlong : m_pressAcross;
            const qreal radius = (alongAxis ? m_pressSize.width() : m_pressSize.height()) * m_scale / 2.0;
            const qreal fade = qBound(0.0, qAbs(dot(from, axis)) / qMax(1.0, radius), 1.0);
            const qreal pressFade = alongAxis ? m_pressFadeA : m_pressFadeB;
            const qreal otherPress = alongAxis ? m_pressFadeB : m_pressFadeA;
            const qreal other = fadeLinked ? qBound(0.0, otherPress + (fade - pressFade), 1.0) : otherPress;
            m_auto->sethorizontalFade(alongAxis ? fade : other);
            m_auto->setverticalFade(alongAxis ? other : fade);
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
        case ChipFadeA:
        case ChipFadeB: {
            const bool a = m_dragPart == ChipFadeA;
            const qreal delta = -step / 200.0;
            const qreal fade = qBound(0.0, (a ? m_pressFadeA : m_pressFadeB) + delta, 1.0);
            const qreal other = fadeLinked ? qBound(0.0, (a ? m_pressFadeB : m_pressFadeA) + delta, 1.0) : (a ? m_pressFadeB : m_pressFadeA);
            m_auto->sethorizontalFade(a ? fade : other);
            m_auto->setverticalFade(a ? other : fade);
            break;
        }
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
        case ChipFadeA: value = (1.0 - m_auto->horizontalFade()) * 100; break;
        case ChipFadeB: value = (1.0 - m_auto->verticalFade()) * 100; break;
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
            case ChipFadeA:
                m_auto->sethorizontalFade(qBound(0.0, 1.0 - typed / 100.0, 1.0));
                if (m_fadeLinked) m_auto->setverticalFade(qBound(0.0, 1.0 - typed / 100.0, 1.0));
                break;
            case ChipFadeB:
                m_auto->setverticalFade(qBound(0.0, 1.0 - typed / 100.0, 1.0));
                if (m_fadeLinked) m_auto->sethorizontalFade(qBound(0.0, 1.0 - typed / 100.0, 1.0));
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

    void showEdge();

    KisAutoBrushModel *m_auto;
    KisPredefinedBrushModel *m_predefined;
    lager::cursor<BrushType> m_type;

    qreal m_scale {0};
    QVariantAnimation m_fitAnimation;
    bool m_linked {true};
    bool m_fadeLinked {true};

    mutable QImage m_dab;
    mutable bool m_dabDirty {true};
    mutable QColor m_dabInk;
    mutable QSize m_dabSize;
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
    int m_pressSpikes {2};
    QPointF m_pressAlong;
    QPointF m_pressAcross;
    QPointer<KisStudioPopup> m_edge;
};

/// The edge's falloff: hard, soft along a curve, or Gaussian
class EdgePopup : public KisStudioPopup
{
public:
    EdgePopup(KisAutoBrushModel *model, QWidget *parent)
        : KisStudioPopup(parent)
        , m_model(model)
    {
        auto *layout = new QVBoxLayout(this);
        fitLayout(layout, KisModernStyle::space3);
        layout->setSpacing(KisModernStyle::space2);

        m_kinds = new KisStudioChips(this);
        m_kinds->setTabs(true);
        m_kinds->setTileSize(44);
        layout->addWidget(m_kinds);
        connect(m_kinds, &KisStudioChips::clicked, this, [this](int index) {
            m_model->settype(index);
            refresh();
        });

        m_curve = new KisCurveWidget(this);
        m_curve->setFixedHeight(130);
        layout->addWidget(m_curve);
        connect(m_curve, &KisCurveWidget::modified, this, [this]() {
            m_model->setcurveString(m_curve->curve().toString());
            if (m_model->type() != Soft) m_model->settype(Soft);
            m_saved->setCurrent(m_curve->curve().toString());
            refreshKinds();
        });

        m_saved = new KisCurvePresetStrip(this);
        layout->addWidget(m_saved);
        connect(m_saved, &KisCurvePresetStrip::picked, this, [this](const QString &curve) {
            m_model->setcurveString(curve);
            m_model->settype(Soft);
            refresh();
        });

        setFixedWidth(280 + 2 * shadowMargin());
        connect(m_model, &KisAutoBrushModel::curveStringChanged, this, [this]() { refresh(); });
        refresh();
    }

private:
    QString curveOrDefault() const
    {
        // a soft tip without a curve of its own falls off in a straight line
        return m_model->curveString().isEmpty() ? QStringLiteral("0,1;1,0;") : m_model->curveString();
    }

    void refreshKinds()
    {
        QVector<KisStudioChips::Chip> chips;
        const QStringList names = {i18nc("@info:tooltip brush tip edge", "Hard: solid, softened only by the edge %"),
                                   i18nc("@info:tooltip brush tip edge", "Curve: your own falloff, drawn below"),
                                   i18nc("@info:tooltip brush tip edge", "Gaussian: soft, natural falloff, like an airbrush")};
        for (int kind = 0; kind < 3; kind++) {
            KisStudioChips::Chip chip;
            chip.toolTip = names[kind];
            chip.on = m_model->type() == kind;
            const QString curve = curveOrDefault();
            const qreal fade = m_model->horizontalFade();
            const QColor ink = palette().color(QPalette::WindowText);
            chip.picture = [kind, curve, fade, ink](QPainter &painter, const QRectF &box) {
                painter.setPen(QPen(ink, 1.6));
                painter.setBrush(Qt::NoBrush);
                painter.drawPath(falloffPath(box.adjusted(4, 6, -4, -6), kind, curve, kind == Default ? qMax(0.3, fade) : fade));
            };
            chips << chip;
        }
        m_kinds->setChips(chips);
    }

    void refresh()
    {
        refreshKinds();
        const QString curve = curveOrDefault();
        if (m_curve->curve().toString() != curve) {
            KisSignalsBlocker blocker(m_curve);
            m_curve->setCurve(KisCubicCurve(curve));
        }
        m_saved->setCurrent(curve);
    }

    KisAutoBrushModel *m_model;
    KisStudioChips *m_kinds {nullptr};
    KisCurveWidget *m_curve {nullptr};
    KisCurvePresetStrip *m_saved {nullptr};
};

void TipCanvas::showEdge()
{
    if (m_edge) {
        m_edge->close();
        return;
    }
    auto *edge = new EdgePopup(m_auto, this);
    m_edge = edge;
    edge->popupBeside(this, Qt::RightEdge);
}

} // namespace

struct KisBrushTipQuickPage::Private
{
    KisAutoBrushModel *autoModel {nullptr};
    KisPredefinedBrushModel *predefinedModel {nullptr};
    lager::cursor<BrushType> type;
    TipCanvas *canvas {nullptr};
    KisStudioChips *shapes {nullptr};
    /// The picture tips the shapes row shows, after round, square and star
    QStringList tipIds;
    /// Their pictures, small and in the ink they were made in, and names: by md5
    QHash<QString, QImage> tipPictures;
    QHash<QString, QString> tipNames;
    QColor tipInk;
};

KisBrushTipQuickPage::KisBrushTipQuickPage(KisAutoBrushModel *autoModel,
                                           KisPredefinedBrushModel *predefinedModel,
                                           lager::cursor<BrushType> type,
                                           QWidget *parent)
    : QWidget(parent)
    , d(new Private)
{
    d->autoModel = autoModel;
    d->predefinedModel = predefinedModel;
    d->type = type;

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(KisModernStyle::space2);

    d->canvas = new TipCanvas(autoModel, predefinedModel, type, this);
    layout->addWidget(d->canvas);

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
            source.allLabel = i18nc("@action:button %1 is a number", "All %1 tips", QStringLiteral("%1"));
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

    connect(autoModel, &KisAutoBrushModel::shapeChanged, this, &KisBrushTipQuickPage::updateShapes);
    connect(autoModel, &KisAutoBrushModel::spikesChanged, this, &KisBrushTipQuickPage::updateShapes);
    connect(predefinedModel, &KisPredefinedBrushModel::resourceSignatureChanged, this, &KisBrushTipQuickPage::updateShapes);
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
}

namespace {
/// Shows the grain page for the tips that have a grain, as the tip's kind changes; watching while the page lives
class GrainShown : public QObject
{
public:
    GrainShown(lager::cursor<BrushType> type, QWidget *page)
        : QObject(page)
        , m_type(type)
    {
        page->setVisible(m_type.get() != Predefined);
        m_type.watch([page](BrushType kind) { page->setVisible(kind != Predefined); });
    }

private:
    lager::cursor<BrushType> m_type;
};
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
    coverage->setRange(percent);
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
    new GrainShown(type, page);
    return page;
}

#include "KisBrushTipEditor.moc"
