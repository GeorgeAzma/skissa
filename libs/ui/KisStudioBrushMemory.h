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
 * Brushes taken from what was painted (see KisStrokeLog):
 * - Alt+click on a stroke, with a painting tool, takes the brush that
 *   painted it at the size it had; Alt+click never paints.
 * - Choosing a layer (not adding, deleting or undoing) takes the brush last
 *   painted with on it, unless "Layers remember their brush" is off.
 */
class KRITAUI_EXPORT KisStudioBrushMemory : public QObject
{
    Q_OBJECT
public:
    explicit KisStudioBrushMemory(KisMainWindow *mainWindow);
    ~KisStudioBrushMemory() override;

    /// Whether choosing a layer takes its brush
    static bool layersRememberBrushes();
    static void setLayersRememberBrushes(bool on);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void layerChosen(KisNodeSP node);
    /// Takes the brush seen at \p widgetPos, saying which; false if there is none
    bool pickAt(const QPointF &widgetPos);

    KisMainWindow *m_mainWindow {nullptr};
    /// The rest of an Alt+click (moves, release) is not a stroke
    bool m_swallowing {false};
};

#endif // KISSTUDIOBRUSHMEMORY_H
