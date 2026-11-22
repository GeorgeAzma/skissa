/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef OVERVIEWLIVE_H
#define OVERVIEWLIVE_H

#include <QObject>
#include <QPointer>
#include <QRect>

#include <kis_signal_compressor.h>

class KisCanvas2;
class OverviewWidget;

/**
 * Keeps the overview's picture up with the image while it changes. Krita's own update waits until the image has
 * been idle for a quarter of a second and then makes the whole picture again; meanwhile this makes only the part
 * that changed, about twenty times a second, on a worker thread.
 */
class OverviewLive : public QObject
{
public:
    explicit OverviewLive(OverviewWidget *widget);

    void setCanvas(KisCanvas2 *canvas);

private:
    void start();

    OverviewWidget *m_widget {nullptr};
    QPointer<KisCanvas2> m_canvas;
    QMetaObject::Connection m_updated;
    KisSignalCompressor m_compressor;
    /// What changed since the last part was made, in image pixels
    QRect m_dirty;
    bool m_busy {false};
};

#endif // OVERVIEWLIVE_H
