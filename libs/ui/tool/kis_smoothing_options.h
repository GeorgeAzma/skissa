/*
 *  SPDX-FileCopyrightText: 2012 Boudewijn Rempt <boud@valdyas.org>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KIS_SMOOTHING_OPTIONS_H
#define KIS_SMOOTHING_OPTIONS_H

#include <qglobal.h>
#include <QObject>
#include <QSharedPointer>
#include <QScopedPointer>
#include <kritaui_export.h>


class KRITAUI_EXPORT KisSmoothingOptions : public QObject
{
    Q_OBJECT
public:
    enum SmoothingType {
        NO_SMOOTHING = 0,
        SIMPLE_SMOOTHING,
        WEIGHTED_SMOOTHING,
        STABILIZER,
        PIXEL_PERFECT
    };

public:

    KisSmoothingOptions(bool useSavedSmoothing = true);
    ~KisSmoothingOptions() override;

    SmoothingType smoothingType() const;
    void setSmoothingType(SmoothingType value);

    qreal smoothnessDistanceMin() const;
    void setSmoothnessDistanceMin(qreal value);

    qreal smoothnessDistanceMax() const;
    void setSmoothnessDistanceMax(qreal value);

    bool smoothnessDistanceKeepAspectRatio() const;
    void setSmoothnessDistanceKeepAspectRatio(bool value);

    qreal tailAggressiveness() const;
    void setTailAggressiveness(qreal value);

    /// Whether pressure is evened out at all; on is about half the strength
    bool smoothPressure() const;
    void setSmoothPressure(bool value);

    /// How much pressure is evened out, 0 (as the pen reports it) to 1
    qreal pressureStrength() const;
    void setPressureStrength(qreal value);
    /// How far a pressure \p ms after the last goes from the evened one toward it, at \p strength
    static qreal pressureFollow(qreal strength, qreal ms);

    bool useScalableDistance() const;
    void setUseScalableDistance(bool value);

    qreal delayDistance() const;
    void setDelayDistance(qreal value);

    void setUseDelayDistance(bool value);
    bool useDelayDistance() const;

    void setFinishStabilizedCurve(bool value);
    bool finishStabilizedCurve() const;

    void setStabilizeSensors(bool value);
    bool stabilizeSensors() const;

Q_SIGNALS:
    void sigSmoothingTypeChanged();

private Q_SLOTS:
    void slotWriteConfig();

private:
    struct Private;
    const QScopedPointer<Private> m_d;
};

typedef QSharedPointer<KisSmoothingOptions> KisSmoothingOptionsSP;

#endif // KIS_SMOOTHING_OPTIONS_H
