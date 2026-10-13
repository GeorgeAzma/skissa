/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISCURVEPRESETSTRIP_H
#define KISCURVEPRESETSTRIP_H

#include <QWidget>

#include <kritapaintop_export.h>

/**
 * Curves to pick at a click, each drawn as it is: Linear, Soft and Firm, then the curves you saved
 * (+ saves the one being edited; a right click renames or deletes one). The saved ones are the same
 * in every brush setting.
 */
class PAINTOP_EXPORT KisCurvePresetStrip : public QWidget
{
    Q_OBJECT
public:
    explicit KisCurvePresetStrip(QWidget *parent = nullptr);

    /// The curve being edited: what + saves, and the tile shown as chosen
    void setCurrent(const QString &curve);

    QSize sizeHint() const override;

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
    /// The tile at \p pos: an entry's index, the + (entries' count) or -1
    int at(const QPoint &pos) const;
    QRect tile(int i) const;

    QString m_current;
    int m_hovered {-1};
};

#endif // KISCURVEPRESETSTRIP_H
