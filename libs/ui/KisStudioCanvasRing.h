/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIOCANVASRING_H
#define KISSTUDIOCANVASRING_H

#include <QColor>
#include <QImage>
#include <QPointer>
#include <QVector>
#include <QWidget>

#include "kritaui_export.h"

class KisMainWindow;

/**
 * The canvas's right click (or a pen's side button) with a painting tool:
 * a ring around the pointer with your first eight brushes, each where it
 * always is (the first at the top, then clockwise), the colors painted on
 * the layer around them, and the current color in the middle.
 *
 * Press, move towards a brush and let go: it is taken, without looking,
 * once the places are known. A click without moving leaves the ring open
 * to choose from with a click; Esc or a click elsewhere closes it; the
 * middle opens all the brushes.
 */
class KRITAUI_EXPORT KisStudioCanvasRing : public QWidget
{
    Q_OBJECT
public:
    explicit KisStudioCanvasRing(KisMainWindow *mainWindow);
    ~KisStudioCanvasRing() override;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    struct Slot {
        QString brush;
        QString title;
        QImage picture;
    };

    void open(QWidget *canvas, const QPointF &pos);
    void close();
    /// What is under \p pos (ring coordinates): a brush (0..7), a color (100+), the middle (-2), nothing (-1)
    int targetAt(const QPointF &pos) const;
    void choose(int target);
    QPointF slotCenter(int index) const;
    QPointF colorCenter(int index) const;

    KisMainWindow *m_mainWindow {nullptr};
    QVector<Slot> m_slots;
    QVector<QColor> m_colors;
    QColor m_current;
    QString m_currentBrush;
    /// Held: the press that opened it is still down, a release on a target takes it
    bool m_held {false};
    bool m_moved {false};
    int m_hover {-1};
    QPointF m_center;
};

#endif // KISSTUDIOCANVASRING_H
