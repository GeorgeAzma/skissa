/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIOCURVE_H
#define KISSTUDIOCURVE_H

#include <QPointF>
#include <QString>
#include <QVector>
#include <QWidget>

#include <functional>

#include "kritaui_export.h"

class QPainter;

/// A curve through points from left to right, both coordinates 0 to 1, drawn as a monotone cubic (no overshoot)
struct KRITAUI_EXPORT KisStudioCurve {
    QVector<QPointF> points;

    /// Krita's curve text, "x,y;x,y;"
    QString toString() const;
    static KisStudioCurve fromString(const QString &text, const KisStudioCurve &fallback);
    bool operator==(const KisStudioCurve &other) const;
    bool operator!=(const KisStudioCurve &other) const { return !(*this == other); }

    /**
     * \p count values evenly across 0 to 1. A curve that \p wraps (by hue) meets itself at the ends; one that also
     * \p rises (hue to hue) climbs by a whole turn each turn.
     */
    QVector<float> sampled(int count, bool wraps = false, bool rises = false) const;
};

/**
 * A curve to edit: a press on the line adds a point, a drag moves it (Shift finer), and a point pulled far off the
 * box, double-clicked or right-clicked goes; a right click elsewhere resets the curve. The line at the curve's
 * fallback is drawn faintly; behind it an optional histogram, beside it optional strips of what each axis is.
 */
class KRITAUI_EXPORT KisStudioCurveView : public QWidget
{
    Q_OBJECT
public:
    std::function<void(const KisStudioCurve &)> changed;
    /// The values at a place, as shown while a point is dragged
    std::function<QString(qreal x, qreal y)> describe;
    std::function<QColor(qreal)> xColor;
    std::function<QColor(qreal)> yColor;
    /// Drawn behind the curve, within its box: what the x axis is, as a picture
    std::function<void(QPainter &, const QRectF &)> backdrop;

    explicit KisStudioCurveView(QWidget *parent);

    void setCurve(const KisStudioCurve &curve, const KisStudioCurve &fallback, bool wraps = false, bool rises = false);
    const KisStudioCurve &curve() const { return m_curve; }

    void setHistogram(const QVector<float> &histogram);
    /// Marks a place on the curve (the pixel under the pointer); negative: none
    void setMarker(qreal x);
    qreal valueAt(qreal x) const;
    /// Gives the point nearest \p x (within a little), or a new one on the curve there; its index. The curve is held
    /// \p hold either side, so that what is there changes alone (by default a twelfth around a curve that wraps).
    int pointAt(qreal x, qreal hold = -1);
    /// Moves point \p index to height \p y and reports the change
    void setPointY(int index, qreal y);
    void setShownPoint(int index);

    QSize sizeHint() const override;
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override;

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *) override;

private:
    QRectF boxRect() const;
    QPointF toScreen(const QPointF &value) const;
    QPointF fromScreen(const QPointF &pos) const;
    int pointNear(const QPoint &pos) const;
    bool removable(int index) const;
    int insert(const QPointF &point);
    void tieEnds(int index);
    void finishDrag();
    void commit();

    KisStudioCurve m_curve;
    KisStudioCurve m_fallback;
    bool m_wraps {false};
    bool m_rises {false};
    QVector<float> m_values;
    QVector<float> m_histogram;
    qreal m_marker {-1};
    int m_hovered {-1};
    int m_dragged {-1};
    int m_shown {-1};
    bool m_leaving {false};
    bool m_fine {false};
    QPoint m_grab;
    QPointF m_start;
};

#endif
