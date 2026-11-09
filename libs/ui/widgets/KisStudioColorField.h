/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef KISSTUDIOCOLORFIELD_H
#define KISSTUDIOCOLORFIELD_H

#include <QScopedPointer>
#include <QWidget>

#include <functional>

#include "kritaui_export.h"

/**
 * A color as a swatch. A click opens the color panel's picker beside it, with the color written out (typed or
 * pasted in any format the color panel reads), the paint colors to take, an eyedropper that takes one from the image,
 * and its opacity where it may be see-through. Right-click gives back its default.
 */
class KRITAUI_EXPORT KisStudioColorField : public QWidget
{
    Q_OBJECT
public:
    /// What the eyedropper takes at a pixel of the image; false where there is nothing to take
    using Sampler = std::function<bool(const QPoint &pixel, QColor *color)>;

    explicit KisStudioColorField(QWidget *parent = nullptr);
    ~KisStudioColorField() override;

    void setColor(const QColor &color);
    QColor color() const;
    void setDefaultColor(const QColor &color);
    /// Whether the color may be see-through: the picker then has its opacity
    void setAlphaEnabled(bool enabled);
    /// The pixels the eyedropper takes (by default the image as it shows)
    void setSampler(Sampler sampler);
    /// Opens the picker beside it, as a click does
    void openPicker();

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

Q_SIGNALS:
    /// A color chosen (not one set)
    void colorChanged(const QColor &color);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;

private:
    friend class KisStudioColorFieldPopup;
    friend class KisStudioColorDropper;
    void choose(const QColor &color);
    void startPicking();
    /// The color shown while the eyedropper is over the image; invalid: the color itself
    void showWhilePicking(const QColor &color);

    struct Private;
    QScopedPointer<Private> d;
};

#endif
