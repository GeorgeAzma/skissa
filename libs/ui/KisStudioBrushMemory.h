/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIOBRUSHMEMORY_H
#define KISSTUDIOBRUSHMEMORY_H

#include <QObject>

#include <kis_types.h>

#include "kritaui_export.h"

class KisMainWindow;

/**
 * Choosing a layer (not adding, deleting or undoing) takes the brush last
 * painted with on it (see KisStrokeLog), unless "Layers remember their brush"
 * is off.
 */
class KRITAUI_EXPORT KisStudioBrushMemory : public QObject
{
    Q_OBJECT
public:
    explicit KisStudioBrushMemory(KisMainWindow *mainWindow);

    /// Whether choosing a layer takes its brush
    static bool layersRememberBrushes();
    static void setLayersRememberBrushes(bool on);

private:
    void layerChosen(KisNodeSP node);

    KisMainWindow *m_mainWindow {nullptr};
};

#endif // KISSTUDIOBRUSHMEMORY_H
