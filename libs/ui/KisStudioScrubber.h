/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIOSCRUBBER_H
#define KISSTUDIOSCRUBBER_H

#include <QPointer>
#include <QWidget>
#include <QtNumeric>

#include <functional>

class QLineEdit;

/**
 * A number edited by dragging sideways, scrolling, or typing after a double click.
 * Sizes scale exponentially (2 to 4 px feels like 200 to 400 px); other values change linearly.
 */
class KisStudioScrubber : public QWidget
{
    Q_OBJECT
public:
    enum class Kind {
        Size,    ///< in px, shown as a dot that grows with the value
        Opacity, ///< in %, shown as a swatch with matching opacity
    };

    KisStudioScrubber(Kind kind, const QString &toolTip, QWidget *parent = nullptr);

    void setRange(qreal minimum, qreal maximum);
    /// Shows a value without emitting valueCommitted()
    void setValue(qreal value);
    qreal value() const { return m_value; }
    /// Value restored by a right click; NaN disables the reset
    void setDefaultValue(qreal value) { m_default = value; }

    QSize sizeHint() const override;

Q_SIGNALS:
    /// Emitted when a drag ends, on a wheel step or after typing; not during a drag, since applying it can be expensive
    void valueCommitted(qreal value);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    bool event(QEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;

private:
    qreal bounded(qreal value) const;
    QString text() const;
    void commit(qreal value);
    /// Ends a drag, committing the value if it moved
    void endDrag();

    Kind m_kind;
    qreal m_minimum {0.0};
    qreal m_maximum {100.0};
    qreal m_value {0.0};
    qreal m_default {qQNaN()};
    qreal m_pressValue {0.0};
    int m_pressX {0};
    bool m_dragging {false};
    bool m_moved {false};
    /// Shift is held: the drag moves at a quarter of the speed
    bool m_fine {false};
    QPointer<QLineEdit> m_editor;
};

#endif // KISSTUDIOSCRUBBER_H
