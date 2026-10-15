/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIOCHIPS_H
#define KISSTUDIOCHIPS_H

#include <QVector>
#include <QWidget>

#include <functional>

#include "kritaui_export.h"

class QPainter;

/**
 * A row of small choices drawn as chips, wrapping onto more rows when they don't fit.
 * As tabs, they share the row's width equally.
 */
class KRITAUI_EXPORT KisStudioChips : public QWidget
{
    Q_OBJECT
public:
    struct Chip {
        QString text;
        QString toolTip;
        /// Drawn instead of the text (a tip shape, a pattern); on a tile with text, the text is written under it
        std::function<void(QPainter &, const QRectF &)> picture;
        /// Lit, as a choice that is on
        bool on {false};
        /// A dot before the text: in use
        bool dot {false};
    };

    explicit KisStudioChips(QWidget *parent = nullptr);

    void setChips(const QVector<Chip> &chips);
    /// Shares the width equally in one row
    void setTabs(bool tabs);
    /// Square chips of this side holding pictures; 0: sized to their text
    void setTileSize(int side);
    /// Tiles of this height, \p columns to a row sharing its width
    void setTileColumns(int columns, int height);
    /// The chip outlined as the one being edited; -1: none
    void setCurrent(int index);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override;

Q_SIGNALS:
    void clicked(int index);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    bool event(QEvent *event) override;

private:
    QVector<QRectF> layoutFor(int width) const;
    int at(const QPoint &pos) const;
    int chipHeight() const;

    QVector<Chip> m_chips;
    bool m_tabs {false};
    int m_tile {0};
    int m_columns {0};
    int m_current {-1};
    int m_hovered {-1};
};

#endif // KISSTUDIOCHIPS_H
