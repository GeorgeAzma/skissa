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
 * When the user picks a layer (rather than adding, deleting or undoing one),
 * switches to the brush last painted with on it, as recorded by KisStrokeLog.
 * Can be turned off with the "Layers remember their brush" setting.
 */
class KRITAUI_EXPORT KisStudioBrushMemory : public QObject
{
    Q_OBJECT
public:
    explicit KisStudioBrushMemory(KisMainWindow *mainWindow);

    /// Whether picking a layer switches to its last brush
    static bool layersRememberBrushes();
    static void setLayersRememberBrushes(bool on);

private:
    void layerChosen(KisNodeSP node);

    KisMainWindow *m_mainWindow {nullptr};
};

#endif // KISSTUDIOBRUSHMEMORY_H
