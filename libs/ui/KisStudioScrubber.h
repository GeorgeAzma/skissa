/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIOSCRUBBER_H
#define KISSTUDIOSCRUBBER_H

#include <QWidget>

#include <functional>

class QLineEdit;

/**
 * A value you change by dragging it sideways, like the scrubbable numbers of
 * design tools: a picture of the value and the number, no label. The wheel
 * steps it, a double click types it. Sizes change by ratios (2 to 4 px feels
 * like 200 to 400 px), other values linearly.
 */
class KisStudioScrubber : public QWidget
{
    Q_OBJECT
public:
    enum class Kind {
        Size,    ///< a dot that grows with the value, in px
        Opacity, ///< a tile as opaque as the value, in %
    };

    KisStudioScrubber(Kind kind, const QString &toolTip, QWidget *parent = nullptr);

    void setRange(qreal minimum, qreal maximum);
    /// Shows a value without emitting valueCommitted()
    void setValue(qreal value);
    qreal value() const { return m_value; }

    QSize sizeHint() const override;

Q_SIGNALS:
    /// After a drag ends, a wheel step or typing: applying it can be expensive
    void valueCommitted(qreal value);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    bool event(QEvent *event) override;

private:
    qreal bounded(qreal value) const;
    QString text() const;
    void commit(qreal value);

    Kind m_kind;
    qreal m_minimum {0.0};
    qreal m_maximum {100.0};
    qreal m_value {0.0};
    qreal m_pressValue {0.0};
    int m_pressX {0};
    bool m_dragging {false};
    bool m_moved {false};
    /// Shift held: a quarter as fast, from where it was when Shift changed
    bool m_fine {false};
    QLineEdit *m_editor {nullptr};
};

#endif // KISSTUDIOSCRUBBER_H
