/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIOBRUSHMEMORY_H
#define KISSTUDIOBRUSHMEMORY_H

#include <QObject>
#include <QPointer>

#include <kis_types.h>

#include "kritaui_export.h"

class KisMainWindow;

/**
 * Brushes taken from what was painted (see KisStrokeLog):
 *
 * - Alt+click on a stroke, with a painting tool, takes the brush that
 *   painted it, at the size it had (Ctrl+click takes the color, as ever).
 *   Where nothing was painted, Alt+click does what it did before.
 * - Choosing a layer (a click in the layers panel, or R+click on the
 *   canvas) takes the brush last painted with on it, if one was: a layer
 *   of ink lines brings back the ink brush. Never on its own: adding,
 *   deleting or undoing changes no brush; a new layer keeps the brush in
 *   hand. A message says which brush it took. The layers panel's menu
 *   turns it off ("Layers remember their brush").
 */
class KRITAUI_EXPORT KisStudioBrushMemory : public QObject
{
    Q_OBJECT
public:
    explicit KisStudioBrushMemory(KisMainWindow *mainWindow);
    ~KisStudioBrushMemory() override;

    /// Whether choosing a layer takes its brush (the setting)
    static bool layersRememberBrushes();
    static void setLayersRememberBrushes(bool on);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void layerChosen(KisNodeSP node);
    bool pickAt(const QPointF &widgetPos);

    KisMainWindow *m_mainWindow {nullptr};
    /// A press that took a brush: the rest of it (moves, release) is not a stroke
    bool m_swallowing {false};
};

#endif // KISSTUDIOBRUSHMEMORY_H
