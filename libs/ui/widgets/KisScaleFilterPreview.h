/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KISSCALEFILTERPREVIEW_H
#define KISSCALEFILTERPREVIEW_H

#include <QImage>
#include <QTimer>
#include <QVector>
#include <QWidget>

#include <kis_types.h>

#include "kritaui_export.h"

/**
 * The picture scaled each way, as tiles to choose from: its most detailed part at the
 * size it is going to, smooth, as pixels and soft.
 */
class KRITAUI_EXPORT KisScaleFilterPreview : public QWidget
{
    Q_OBJECT
public:
    explicit KisScaleFilterPreview(QWidget *parent = nullptr);

    /// What is scaled, and its bounds
    void setContent(KisPaintDeviceSP content, const QRect &bounds);
    /// The size the whole of it goes to
    void setTargetSize(const QSize &size);
    /// The filter that will be used, outlined
    void setCurrent(const QString &filterId);

    QSize sizeHint() const override;

Q_SIGNALS:
    void picked(const QString &filterId);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    struct Tile {
        QString filterId;
        QString name;
        QImage picture;
    };

    void findDetail();
    void render();
    QRect tileRect(int index) const;
    int tileAt(const QPoint &point) const;

    KisPaintDeviceSP m_content;
    QRect m_bounds;
    QSize m_target;
    QPoint m_detail;
    QString m_current;
    QVector<Tile> m_tiles;
    int m_hovered {-1};
    QTimer m_renderLater;
};

#endif
