/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISLAYERHOVER_H
#define KISLAYERHOVER_H

#include <QImage>
#include <QPointer>
#include <QWidget>

#include <functional>

#include <kis_types.h>

class KisCanvas2;

/// While the key is held: the layer a click would pick, tinted where it has pixels, its name by the pointer
class KisLayerHover : public QWidget
{
public:
    /// \p picks: what a click at a point (widget pixels) would pick
    KisLayerHover(KisCanvas2 *canvas, std::function<KisNodeSP(const QPointF &)> picks);

    /// What a click picks changed (a layer was picked): picked again where the pointer is
    void picksChanged(const QPointF &widgetPos);
    void pointAt(const QPointF &widgetPos);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void paintEvent(QPaintEvent *) override;

private:
    QFont nameFont() const;
    /// Where the name is written, by the pointer
    QRectF pillAt() const;
    QRect pillRect() const;

    std::function<KisNodeSP(const QPointF &)> m_picks;
    QPointer<KisCanvas2> m_canvas;
    bool m_picked {false};
    QPointF m_pickedAt;
    KisNodeSP m_node;
    QRect m_bounds;
    QImage m_tint;
    QPointF m_cursor;
};

#endif // KISLAYERHOVER_H
