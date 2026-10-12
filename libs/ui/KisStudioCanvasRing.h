/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIOCANVASRING_H
#define KISSTUDIOCANVASRING_H

#include <QColor>
#include <QElapsedTimer>
#include <QImage>
#include <QPointer>
#include <QVector>
#include <QWidget>

#include <functional>

#include "kritaui_export.h"

class KisMainWindow;

/**
 * A right press on the canvas with a painting tool: your first eight brushes around the pointer, the layer's colors
 * around them. Flick towards a brush and let go to take it; a click without moving leaves it open to click in.
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

private:
    struct Slot {
        QString brush;
        QString title;
        QImage picture;
    };

    void open(QWidget *canvas, const QPointF &pos);
    void close();
    void loadSlots();
    /// \p change is given all your brushes, the ring's first
    void changeBrushes(const std::function<void(QStringList &brushes)> &change);
    /// A brush chosen in Find for slot \p index (past the last one: one more)
    void pickBrush(int index);
    void slotMenu(int index, const QPoint &global);
    /// Under \p pos: a brush (0..7), an empty slot (200+), a color (100+), the middle (-2), nothing (-1)
    int targetAt(const QPointF &pos) const;
    void choose(int target);
    QPointF slotCenter(int index) const;
    QPointF colorCenter(int index) const;

    KisMainWindow *m_mainWindow {nullptr};
    QVector<Slot> m_slots;
    QVector<QColor> m_colors;
    QColor m_current;
    QString m_currentBrush;
    /// The press that opened it is still down: a release on a target takes it
    bool m_held {false};
    bool m_moved {false};
    /// Where the press that opened it was: off the center near the canvas's edges
    QPointF m_openedAt;
    /// A brush pressed: taken when let go there, or (\p m_dragging) moved to where it is let go
    int m_pressed {-1};
    QPointF m_pressedAt;
    bool m_dragging {false};
    QPointF m_dragAt;
    int m_hover {-1};
    QPointF m_center;
    /// Growing in as it opens
    qreal m_scale {1.0};
    /// The canvas it opened on: gone or another one shown, it closes
    QPointer<QWidget> m_canvas;
    /// The button whose press closed it: its release is eaten too
    Qt::MouseButton m_eatRelease {Qt::NoButton};
    /// Since a click chose something and closed it: a second click of a double click is eaten
    QElapsedTimer m_closedByClick;
};

#endif // KISSTUDIOCANVASRING_H
