/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIOBRUSHSLIDER_H
#define KISSTUDIOBRUSHSLIDER_H

#include <QWidget>

#include <functional>

/**
 * A slim vertical slider for the edge of the canvas, e.g. brush size and
 * opacity in the studio layout. It fills from the bottom and shows its value
 * underneath. Like the brush sliders in the toolbar it only commits the value
 * when a drag is finished, since applying it to a preset can be expensive.
 */
class KisStudioBrushSlider : public QWidget
{
    Q_OBJECT
public:
    explicit KisStudioBrushSlider(const QString &title, QWidget *parent = nullptr);

    /**
     * @param exponent the same as KisDoubleSliderSpinBox::setExponentRatio(),
     *        values greater than 1 give more room to small values
     */
    void setRange(qreal minimum, qreal maximum, qreal exponent = 1.0);

    /**
     * Equal distances along the slider give equal ratios of the value, the
     * way sizes are perceived: 2 to 4 px feels like 200 to 400 px.
     */
    void setLogarithmic(bool logarithmic);

    /// Sets the shown value without emitting valueCommitted()
    void setValue(qreal value);
    qreal value() const;

    void setValueFormatter(std::function<QString(qreal)> formatter);

    enum class Preview {
        None,
        Dot,     ///< a dot as large as the brush, up to what fits
        Opacity, ///< a disc as opaque as the value, over a checkerboard
    };
    /// What is shown under the track instead of the number, which appears while hovered or dragged
    void setPreview(Preview preview);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

Q_SIGNALS:
    void valueCommitted(qreal value);

protected:
    bool event(QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

private:
    QRectF trackRect() const;
    qreal position() const;
    qreal valueAtPosition(qreal position) const;
    qreal valueAtY(qreal y) const;
    void updateToolTip();

    QString m_title;
    qreal m_minimum {0.0};
    qreal m_maximum {100.0};
    qreal m_exponent {1.0};
    bool m_logarithmic {false};
    Preview m_preview {Preview::None};
    qreal m_value {0.0};
    bool m_dragging {false};
    std::function<QString(qreal)> m_formatter;
};

#endif // KISSTUDIOBRUSHSLIDER_H
