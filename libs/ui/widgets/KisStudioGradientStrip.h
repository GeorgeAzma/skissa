/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIOGRADIENTSTRIP_H
#define KISSTUDIOGRADIENTSTRIP_H

#include <QWidget>
#include <functional>

#include <KoAbstractGradient.h>
#include <KoStopGradient.h>

#include "kritaui_export.h"

/**
 * A gradient as a strip of paint, edited where it is: a click on the strip adds a color there, a stop dragged
 * along moves, dragged off goes; a color dropped on a stop becomes it, elsewhere a new stop; a double click gives
 * a stop the current color.
 */
class KRITAUI_EXPORT KisStudioGradientStrip : public QWidget
{
public:
    explicit KisStudioGradientStrip(QWidget *parent = nullptr);

    /// Shows \p gradient, its variable colors as they are now
    void setGradient(KoAbstractGradientSP gradient);
    /// The gradient as edited, a new one each time
    KoStopGradientSP gradient() const;

    /// Each edit, as it is made
    std::function<void(KoStopGradientSP)> changed;
    /// The color a double click gives a stop
    std::function<KoColor()> currentColor;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    QRectF stripRect() const;
    qreal xOf(qreal position) const;
    qreal positionAt(qreal x) const;
    int stopAt(const QPointF &pos) const;
    KoColor colorAt(qreal position) const;
    void sortStops();
    void emitChanged();

    QList<KoGradientStop> m_stops;
    int m_dragged {-1};
    /// The stop under the pointer, -1 for none
    int m_hovered {-1};
    bool m_removing {false};
    bool m_moved {false};
};

#endif
