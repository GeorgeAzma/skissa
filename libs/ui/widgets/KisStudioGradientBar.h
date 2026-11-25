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
 * A gradient as a bar with its stops under it, edited as the gradient strip is: a stop is dragged along it, dragged
 * off (or right clicked) goes, a click on the bar adds one with the color there; a color dropped on a stop becomes
 * it, elsewhere a new stop; a gradient dropped on it becomes its stops; a double click gives a stop the current color. The stop pressed last is the selected one, whose color a panel shows.
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
    /// The color a double click gives a stop; unset, the foreground color
    std::function<QColor()> currentColor;
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
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    QRectF barRect() const;
    QPointF handleAt(int index) const;
    qreal positionOf(int x) const;
    /// The stop under \p pos, the nearest when they overlap; -1: none
    int stopAt(const QPoint &pos) const;
    /// Keeps the stops in order, following stop \p index as the selected one
    void sortKeeping(int index);
    QColor shownAt(qreal at) const;
    void removeStop(int index);

    Stops m_stops;
    QVector<float> m_histogram;
    qreal m_marker {-1};
    int m_selected {0};
    /// The stop under the pointer, -1 for none
    int m_hovered {-1};
    bool m_dragging {false};
    /// The stop follows the pointer only once the press has moved a drag's distance
    bool m_moved {false};
    /// The dragged stop is off the bar: let go, it goes
    bool m_removing {false};
    int m_pressX {0};
    /// How far the pointer was from the stop's middle when it began to move, kept while it moves
    qreal m_grab {0};
};

#endif
