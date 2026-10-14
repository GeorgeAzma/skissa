/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISCURVEPRESETSTRIP_H
#define KISCURVEPRESETSTRIP_H

#include <QWidget>

#include "kritaui_export.h"

/**
 * A strip of curve presets, each drawn as a small plot: Linear, Soft and Firm, followed by the user's
 * saved curves. The "+" tile saves the curve being edited; right click renames or deletes a saved one.
 * Saved curves are shared by all brush options.
 */
class KRITAUI_EXPORT KisCurvePresetStrip : public QWidget
{
    Q_OBJECT
public:
    explicit KisCurvePresetStrip(QWidget *parent = nullptr);

    /// The curve being edited: saved by "+" and highlighted if it matches a tile
    void setCurrent(const QString &curve);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override;

Q_SIGNALS:
    void picked(const QString &curve);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    bool event(QEvent *event) override;

private:
    struct Entry {
        QString name;
        QString curve;
        bool saved {false};
    };
    QVector<Entry> entries() const;
    /// The tile at \p pos: an entry's index, the entry count for \"+\", or -1
    int at(const QPoint &pos) const;
    QRect tile(int i) const;
    /// Number of tiles that fit in a row \p width wide
    static int columns(int width);

    QString m_current;
    int m_hovered {-1};
};

#endif // KISCURVEPRESETSTRIP_H
