/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIOHISTORYSCRUBBER_H
#define KISSTUDIOHISTORYSCRUBBER_H

#include <QPointer>
#include <QVector>
#include <QWidget>

#include <functional>

class QAbstractItemView;

/**
 * The undo history as a line to drag along: each step a place on it, the steps undone fainter, snapshots as dots
 * where they were kept. Dragging undoes and redoes as it goes and keeps the steps after, until something new is done.
 * It drives \p steps, the history's list (a row a step, the first the image as opened), so the history stays Krita's.
 */
class KisStudioHistoryScrubber : public QWidget
{
public:
    struct Mark {
        int step; ///< -1: kept from a history that is gone, shown before the line
        QString name;
    };

    explicit KisStudioHistoryScrubber(QAbstractItemView *steps, QWidget *parent = nullptr);

    /// The snapshots, at the step each was kept; \p open is called on a click (\p menu false) or a right click
    void setMarks(const QVector<Mark> &marks, const std::function<void(int mark, bool menu)> &open);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    bool event(QEvent *event) override;

private:
    int stepCount() const;
    int currentStep() const;
    void goTo(int step);
    void openLater(int mark, bool menu);
    qreal lineLeft() const;
    qreal xOf(int step) const;
    QPointF markAtPoint(int mark) const;
    int stepAt(qreal x) const;
    int markAt(const QPoint &pos) const;

    QPointer<QAbstractItemView> m_steps;
    QVector<Mark> m_marks;
    std::function<void(int, bool)> m_open;
    bool m_dragging {false};
    int m_pressedMark {-1};
};

#endif // KISSTUDIOHISTORYSCRUBBER_H
