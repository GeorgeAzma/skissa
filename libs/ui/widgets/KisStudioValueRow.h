/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIOVALUEROW_H
#define KISSTUDIOVALUEROW_H

#include <QWidget>
#include <QtNumeric>

#include "kritaui_export.h"


/**
 * A setting as one row: its name, its value as a bar and a number. Dragging the row changes it
 * (Shift: finer), a double click types it, a right click resets it, a click without a drag says so.
 * A small curve after the value shows that the pen drives it.
 */
class KRITAUI_EXPORT KisStudioValueRow : public QWidget
{
    Q_OBJECT
public:
    struct Range {
        qreal minimum {0.0};
        qreal maximum {1.0};
        int decimals {0};
        QString suffix;
        /// Dragged and drawn by ratios, as sizes are
        bool size {false};
    };

    explicit KisStudioValueRow(const QString &name, QWidget *parent = nullptr);

    QString name() const { return m_name; }
    void setName(const QString &name);
    void setRange(const Range &range);
    /// Shows \p value without emitting valueChanged()
    void setValue(qreal value);
    qreal value() const { return m_value; }
    /// Text shown instead of the number (e.g. "Auto"); empty: the number
    void setValueText(const QString &text);
    /// Restored by a right click; NaN: no reset
    void setDefaultValue(qreal value) { m_default = value; }
    /// The curve the pen drives the value through, as a KisCubicCurve string; empty: no pen
    void setPenCurve(const QString &curve);
    /// Marked as the one being edited elsewhere (its curve open)
    void setSelected(bool selected);
    /// Shorter, for dense sheets
    void setCompact(bool compact);
    /// Quiet: a setting that is off (dragging it still changes it)
    void setInactive(bool inactive);
    /// Without a value bar: a row that is clicked, not dragged
    void setDraggable(bool draggable);
    /// Shows a pin under the pointer (filled while \p pinned), and lets the row be pulled out upwards or downwards
    void setPinnable(bool pinnable, bool pinned);
    /// A cross under the pointer that takes the row away (removeClicked())
    void setRemovable(bool removable);
    /// The pin only under the pointer, even when pinned (where being pinned is plain, as on the bar)
    void setPinOnHoverOnly(bool onHoverOnly) { m_pinOnHoverOnly = onHoverOnly; }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

Q_SIGNALS:
    /// While dragging, after typing and on a reset
    void valueChanged(qreal value);
    /// Pressed and released without dragging
    void clicked();
    void pinClicked();
    void removeClicked();
    /// Its value is about to be typed (a double click): what its first click opened can go
    void typingStarted();
    /// Pulled up or down out of its place, to be dropped elsewhere (the top bar)
    void pulledOut();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    bool event(QEvent *event) override;

private:
    qreal fraction() const;
    QString text() const;
    void change(qreal value);

    QString m_name;
    Range m_range;
    qreal m_value {0.0};
    qreal m_default {qQNaN()};
    QString m_valueText;
    QString m_penCurve;
    bool m_selected {false};
    bool m_compact {false};
    bool m_inactive {false};
    bool m_draggable {true};
    bool m_pinnable {false};
    bool m_pinned {false};
    bool m_pinOnHoverOnly {false};

    QRectF pinRect() const;
    QRectF removeRect() const;
    bool showsPin() const;
    /// Where the name and the value are written, between the row's edges and what is drawn at its end
    QRectF textArea() const;
    bool m_removable {false};
    bool m_editing {false};

    bool m_pressed {false};
    bool m_dragging {false};
    bool m_fine {false};
    int m_pressX {0};
    int m_pressY {0};
    qreal m_pressValue {0.0};
};

#endif // KISSTUDIOVALUEROW_H
