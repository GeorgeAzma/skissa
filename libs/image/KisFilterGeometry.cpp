/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisFilterGeometry.h"

#include <QHash>
#include <QPolygonF>
#include <QtMath>

#include <cmath>

#include "commands/kis_change_filter_command.h"
#include "filter/kis_filter_configuration.h"
#include "kis_adjustment_layer.h"
#include "kis_filter_mask.h"
#include "kis_image.h"
#include "kis_node_filter_interface.h"
#include "kis_properties_configuration.h"
#include "kis_undo_adapter.h"

namespace KisFilterGeometry
{

namespace
{

/// A length kept under \p key, scaled; whole numbers stay whole and a length there stays at least a pixel
void scaleKey(KisPropertiesConfiguration &config, const Change &change, const QString &key, bool whole = false)
{
    if (!config.hasProperty(key)) return;
    const qreal before = config.getDouble(key);
    const qreal after = change.length(before);
    if (whole) {
        config.setProperty(key, std::max(before > 0 ? 1 : 0, qRound(after)));
    } else {
        config.setProperty(key, after);
    }
}

/// Lengths across and down kept under two keys
void scaleAxes(KisPropertiesConfiguration &config, const Change &change, const QString &across, const QString &down, bool whole = false)
{
    if (!config.hasProperty(across) || !config.hasProperty(down)) return;
    const QSizeF before(config.getDouble(across), config.getDouble(down));
    const QSizeF after = change.axes(before);
    if (whole) {
        config.setProperty(across, std::max(before.width() > 0 ? 1 : 0, qRound(after.width())));
        config.setProperty(down, std::max(before.height() > 0 ? 1 : 0, qRound(after.height())));
    } else {
        config.setProperty(across, after.width());
        config.setProperty(down, after.height());
    }
}

int wholeDegrees(qreal degrees)
{
    const int whole = qRound(degrees) % 360;
    return whole < 0 ? whole + 360 : whole;
}

/// Krita's Lens blur keeps how far its iris reaches, which follows from the iris (KisLensBlurFilter::getKernelHalfSize)
void lensBlur(KisPropertiesConfiguration &config, const Change &change)
{
    const qreal radius = change.length(config.getDouble("irisRadius", 5));
    const qreal rotation = change.direction(config.getDouble("irisRotation", 0));
    config.setProperty("irisRadius", std::max(1, qRound(radius)));
    config.setProperty("irisRotation", wholeDegrees(rotation));

    static const QHash<QString, int> sides = {{QStringLiteral("Triangle"), 3}, {QStringLiteral("Quadrilateral (4)"), 4},
                                              {QStringLiteral("Pentagon (5)"), 5}, {QStringLiteral("Hexagon (6)"), 6},
                                              {QStringLiteral("Heptagon (7)"), 7}, {QStringLiteral("Octagon (8)"), 8}};
    const int count = sides.value(config.getString("irisShape"), 0);
    if (!count) {
        scaleKey(config, change, QStringLiteral("halfWidth"), true);
        scaleKey(config, change, QStringLiteral("halfHeight"), true);
        return;
    }
    QPolygonF iris;
    for (int i = 0; i < count; i++) iris << QPointF(0.5 * std::cos(2 * M_PI * i / count), 0.5 * std::sin(2 * M_PI * i / count));
    QTransform shape;
    shape.rotate(config.getInt("irisRotation"));
    shape.scale(config.getInt("irisRadius") * 2, config.getInt("irisRadius") * 2);
    const QRect bounds = shape.map(iris).boundingRect().toAlignedRect();
    config.setProperty("halfWidth", int(std::ceil(bounds.width() / 2.0)));
    config.setProperty("halfHeight", int(std::ceil(bounds.height() / 2.0)));
}

void motionBlur(KisPropertiesConfiguration &config, const Change &change)
{
    const QPointF motion = change.along(config.getInt("blurLength", 5), config.getInt("blurAngle", 0));
    config.setProperty("blurLength", std::max(1, qRound(motion.x())));
    config.setProperty("blurAngle", wholeDegrees(motion.y()));
}

/// Krita's waves: lengths along each axis
void wave(KisPropertiesConfiguration &config, const Change &change)
{
    const QSizeF along = change.axes(QSizeF(1, 1));
    for (const char *key : {"wavelength", "shift", "amplitude"}) {
        const QString across = QStringLiteral("horizontal") + QLatin1String(key);
        const QString down = QStringLiteral("vertical") + QLatin1String(key);
        if (config.hasProperty(across)) config.setProperty(across, qRound(config.getDouble(across) * along.width()));
        if (config.hasProperty(down)) config.setProperty(down, qRound(config.getDouble(down) * along.height()));
    }
}

struct Registry {
    QMutex mutex;
    QHash<QString, Function> functions;

    Registry()
    {
        auto length = [](const QString &key, bool whole = false) {
            return [key, whole](KisPropertiesConfiguration &config, const Change &change) { scaleKey(config, change, key, whole); };
        };
        auto axes = [](const QString &across, const QString &down, bool whole = false) {
            return [across, down, whole](KisPropertiesConfiguration &config, const Change &change) {
                scaleAxes(config, change, across, down, whole);
            };
        };
        // Krita's filters with sizes, by their settings
        functions.insert(QStringLiteral("gaussian blur"), axes(QStringLiteral("horizRadius"), QStringLiteral("vertRadius")));
        functions.insert(QStringLiteral("unsharp"), length(QStringLiteral("halfSize")));
        functions.insert(QStringLiteral("gaussianhighpass"), length(QStringLiteral("blurAmount")));
        functions.insert(QStringLiteral("motion blur"), motionBlur);
        functions.insert(QStringLiteral("lens blur"), lensBlur);
        functions.insert(QStringLiteral("blur"), [](KisPropertiesConfiguration &config, const Change &change) {
            scaleKey(config, change, QStringLiteral("halfWidth"), true);
            scaleKey(config, change, QStringLiteral("halfHeight"), true);
        });
        functions.insert(QStringLiteral("pixelize"), axes(QStringLiteral("pixelWidth"), QStringLiteral("pixelHeight"), true));
        functions.insert(QStringLiteral("oilpaint"), length(QStringLiteral("brushSize"), true));
        functions.insert(QStringLiteral("propagatecolors"), length(QStringLiteral("expansionAmount")));
        functions.insert(QStringLiteral("gaussiannoisereducer"), length(QStringLiteral("windowsize"), true));
        functions.insert(QStringLiteral("randompick"), length(QStringLiteral("windowsize")));
        functions.insert(QStringLiteral("roundcorners"), length(QStringLiteral("radius"), true));
        functions.insert(QStringLiteral("raindrops"), length(QStringLiteral("dropsize"), true));
        functions.insert(QStringLiteral("wave"), wave);
        functions.insert(QStringLiteral("edge detection"), axes(QStringLiteral("horizRadius"), QStringLiteral("vertRadius")));
        functions.insert(QStringLiteral("height to normal"), axes(QStringLiteral("horizRadius"), QStringLiteral("vertRadius")));
    }
};

Registry &registry()
{
    static Registry instance;
    return instance;
}

} // namespace

Change::Change(const QTransform &transform, const QRectF &frame)
    : m_transform(transform)
    , m_frame(frame)
{
}

qreal Change::scale() const
{
    // rounded, so a turn alone keeps lengths exactly
    const qreal growth = std::sqrt(std::abs(m_transform.m11() * m_transform.m22() - m_transform.m12() * m_transform.m21()));
    return std::round(growth * 1e9) / 1e9;
}

qreal Change::length(qreal length) const
{
    return length * scale();
}

qreal Change::direction(qreal degrees) const
{
    return along(1, degrees).y();
}

QPointF Change::along(qreal length, qreal degrees) const
{
    const qreal radians = qDegreesToRadians(degrees);
    const QPointF way(std::cos(radians) * length, std::sin(radians) * length);
    const QPointF turned = m_transform.map(way) - m_transform.map(QPointF());
    const qreal reach = std::hypot(turned.x(), turned.y());
    if (reach < 1e-9) return QPointF(0, degrees);
    // to a millionth of a degree, so whole angles stay whole
    return QPointF(reach, std::round(qRadiansToDegrees(std::atan2(turned.y(), turned.x())) * 1e6) / 1e6);
}

QSizeF Change::axes(const QSizeF &axes) const
{
    // the ellipse with these half axes, transformed, measured across and down
    const qreal w2 = axes.width() * axes.width(), h2 = axes.height() * axes.height();
    const qreal across = w2 * m_transform.m11() * m_transform.m11() + h2 * m_transform.m21() * m_transform.m21();
    const qreal down = w2 * m_transform.m12() * m_transform.m12() + h2 * m_transform.m22() * m_transform.m22();
    return QSizeF(std::sqrt(across), std::sqrt(down));
}

QPointF Change::share(const QPointF &share) const
{
    if (m_frame.isEmpty()) return share;
    const QPointF at = m_frame.topLeft() + QPointF(share.x() * m_frame.width(), share.y() * m_frame.height());
    const QPointF to = m_transform.map(at);
    // when the image itself is transformed its frame lands at the corner, and places are shares of the new frame;
    // a layer transformed inside the image keeps the frame
    const QRectF landed = m_transform.mapRect(m_frame);
    const QRectF frame = std::abs(landed.left()) < 1 && std::abs(landed.top()) < 1 ? landed : m_frame;
    if (frame.isEmpty()) return share;
    return QPointF((to.x() - frame.left()) / frame.width(), (to.y() - frame.top()) / frame.height());
}

bool Change::mirrors() const
{
    return m_transform.m11() * m_transform.m22() - m_transform.m12() * m_transform.m21() < 0;
}

void add(const QString &filterId, const Function &function)
{
    Registry &r = registry();
    QMutexLocker lock(&r.mutex);
    r.functions.insert(filterId, function);
}

KisFilterConfigurationSP transformed(KisFilterConfigurationSP config, const Change &change)
{
    if (!config) return {};
    Function function;
    {
        Registry &r = registry();
        QMutexLocker lock(&r.mutex);
        function = r.functions.value(config->name());
    }
    if (!function) return {};
    KisFilterConfigurationSP result = config->clone();
    function(*result, change);
    return result->compareTo(config.data()) ? KisFilterConfigurationSP() : result;
}

void Follower::stop()
{
    m_stopped = true;
}

void Follower::follow(KisNode *node, const QTransform &transform, KisUndoAdapter *undoAdapter)
{
    if (m_stopped || !undoAdapter || transform.isIdentity()) return;
    if (!dynamic_cast<KisFilterMask *>(node) && !dynamic_cast<KisAdjustmentLayer *>(node)) return;
    KisNodeFilterInterface *filtered = dynamic_cast<KisNodeFilterInterface *>(node);
    KisImageSP image = node->image().toStrongRef();
    if (!filtered || !image) return;
    {
        QMutexLocker lock(&m_mutex);
        if (m_done.contains(node)) return;
        m_done.insert(node);
    }
    KisFilterConfigurationSP before = filtered->filter();
    KisFilterConfigurationSP after = transformed(before, Change(transform, image->bounds()));
    if (after) undoAdapter->addCommand(new KisChangeFilterCmd(node, before, after));
}

} // namespace KisFilterGeometry
