/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISFILTERGEOMETRY_H
#define KISFILTERGEOMETRY_H

#include <QMutex>
#include <QPointF>
#include <QRectF>
#include <QSet>
#include <QSizeF>
#include <QString>
#include <QTransform>

#include <functional>

#include "kritaimage_export.h"
#include "kis_types.h"

class KisNode;
class KisPropertiesConfiguration;
class KisUndoAdapter;

/**
 * An effect's settings follow its picture when the image or the layer is scaled, turned, sheared or mirrored: sizes
 * grow and shrink with it, directions turn, places move. A filter mask or filter layer so transformed takes its
 * changed settings in the same undo step, so a 20 px blur on an image halved becomes a 10 px blur and looks the same.
 */
namespace KisFilterGeometry
{

/// What a transform of the image does to a length, a direction and a place
class KRITAIMAGE_EXPORT Change
{
public:
    /// \p frame: the image's bounds before the transform
    Change(const QTransform &transform, const QRectF &frame);

    /// How much lengths that go every way (a radius) grow: the square root of the area's growth
    qreal scale() const;
    qreal length(qreal length) const;
    /// A direction in degrees, from across toward down, as it turns
    qreal direction(qreal degrees) const;
    /// A length along a direction: the length and direction it becomes
    QPointF along(qreal length, qreal degrees) const;
    /// Lengths across and down (an ellipse or a blur stretched one way): what they become across and down
    QSizeF axes(const QSizeF &axes) const;
    /// A place as a share of the image (0 to 1 across and down): where it lands
    QPointF share(const QPointF &share) const;
    /// Whether it mirrors, so whatever turns turns the other way
    bool mirrors() const;

private:
    QTransform m_transform;
    QRectF m_frame;
};

/// Changes \p config of one filter for \p change
using Function = std::function<void(KisPropertiesConfiguration &config, const Change &change)>;

/// How the settings of the filter \p filterId follow a transform
KRITAIMAGE_EXPORT void add(const QString &filterId, const Function &function);

/// The settings of \p config after \p change, or null when nothing in them changes
KRITAIMAGE_EXPORT KisFilterConfigurationSP transformed(KisFilterConfigurationSP config, const Change &change);

/// What a transforming visitor uses: each filter mask and filter layer it visits follows once (a node is visited
/// for each of its frames)
class KRITAIMAGE_EXPORT Follower
{
public:
    /// Only part of each layer is transformed: the settings stay
    void stop();
    void follow(KisNode *node, const QTransform &transform, KisUndoAdapter *undoAdapter);

private:
    QMutex m_mutex;
    QSet<const KisNode *> m_done;
    bool m_stopped {false};
};

} // namespace KisFilterGeometry

#endif
