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
 * Radial menu opened by a right press on the canvas with a painting tool: the first eight pinned brushes around
 * the pointer, ringed by the colors used on the current layer. Flick towards a brush and release to select it;
 * a click without moving keeps the ring open for clicking.
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
    /// Applies \p change to the pinned brushes (the ring shows the first ones) and saves them
    void changeBrushes(const std::function<void(QStringList &brushes)> &change);
    /// Lets the user pick a brush in Find for slot \p index; an index past the end appends one
    void pickBrush(int index);
    void slotMenu(int index, const QPoint &global);
    /// What is at \p pos: a brush (0..7), a color (100+), an empty slot (200+), the middle (-2) or nothing (-1)
    int targetAt(const QPointF &pos) const;
    void choose(int target);
    QPointF slotCenter(int index) const;
    QPointF colorCenter(int index) const;

    KisMainWindow *m_mainWindow {nullptr};
    QVector<Slot> m_slots;
    QVector<QColor> m_colors;
    QColor m_current;
    QString m_currentBrush;
    /// The press that opened the ring is still down; releasing over a target selects it
    bool m_held {false};
    bool m_moved {false};
    /// Where the opening press was; off-center when the ring was pushed in from the canvas edges
    QPointF m_openedAt;
    /// The pressed brush: selected on release, or moved to another slot if m_dragging
    int m_pressed {-1};
    QPointF m_pressedAt;
    bool m_dragging {false};
    QPointF m_dragAt;
    int m_hover {-1};
    QPointF m_center;
    /// Scale for the opening animation
    qreal m_scale {1.0};
    /// The canvas the ring opened on; the ring closes if it goes away or another canvas is shown
    QPointer<QWidget> m_canvas;
    /// Button whose press closed the ring; its release is swallowed too
    Qt::MouseButton m_eatRelease {Qt::NoButton};
    /// Started when a click selects something, so the second click of a double click can be swallowed
    QElapsedTimer m_closedByClick;
};

#endif // KISSTUDIOCANVASRING_H
