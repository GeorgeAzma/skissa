/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIOGRADIENTBAR_H
#define KISSTUDIOGRADIENTBAR_H

#include <QColor>
#include <QPair>
#include <QVector>
#include <QWidget>

#include <functional>

#include "kritaui_export.h"

/**
 * A gradient as a bar with its stops under it: a stop is dragged along it, a click on the bar adds one with the
 * color there, a right click takes one away. The stop pressed last is the selected one, whose color a panel shows.
 * Above the bar, how much of the picture is at each place along it, if given.
 */
class KRITAUI_EXPORT KisStudioGradientBar : public QWidget
{
    Q_OBJECT
public:
    using Stops = QVector<QPair<qreal, QColor>>;

    explicit KisStudioGradientBar(QWidget *parent = nullptr);

    void setStops(const Stops &stops);
    Stops stops() const { return m_stops; }
    int selected() const { return m_selected; }
    void setSelected(int index);
    /// How much of the picture is at each place along the bar, drawn above it; empty: nothing above
    void setHistogram(const QVector<float> &values);
    /// A tick on the bar at \p at, 0 to 1; below 0, none
    void setMarker(qreal at);

    /// The color between the stops at \p at; unset, they blend in sRGB
    std::function<QColor(const Stops &, qreal)> colorAt;
    /// The stops blended in sRGB
    static QColor blended(const Stops &stops, qreal at);

    QSize sizeHint() const override;

Q_SIGNALS:
    void stopsChanged();
    void selectedChanged();

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *) override;

private:
    QRectF barRect() const;
    QPointF handleAt(int index) const;
    qreal positionOf(int x) const;
    /// The stop under \p pos, the nearest when they overlap; -1: none
    int stopAt(const QPoint &pos) const;
    /// Keeps the stops in order, following stop \p index as the selected one
    void sortKeeping(int index);
    QColor shownAt(qreal at) const;

    Stops m_stops;
    QVector<float> m_histogram;
    qreal m_marker {-1};
    int m_selected {0};
    bool m_dragging {false};
};

#endif
