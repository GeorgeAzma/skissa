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
 *   painted it, at the size it had (Ctrl+click takes the color, as ever):
 *   the stroke seen there, on the topmost layer with paint. Where no brush
 *   painted (nothing, a fill), it says so; Alt+click never paints.
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
    /// Takes the brush seen at \p widgetPos, saying which; false if there is none
    bool pickAt(const QPointF &widgetPos);

    KisMainWindow *m_mainWindow {nullptr};
    /// An Alt+click: the rest of it (moves, release) is not a stroke
    bool m_swallowing {false};
};

#endif // KISSTUDIOBRUSHMEMORY_H
