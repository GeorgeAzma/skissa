/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIOTOOLRAIL_H
#define KISSTUDIOTOOLRAIL_H

#include <QWidget>

#include <memory>

class KisMainWindow;
class KoToolFactoryBase;

/**
 * The tools of the studio layout, grouped by what you want to do instead of
 * by how they are implemented: Paint, Blend and Erase (all the freehand brush,
 * each remembering its own preset), Select, Move, Fill, Shape, Text and Guides.
 *
 * Every slot remembers the variant that was used last, e.g. the lasso for
 * Select. Clicking the active slot again, holding it or right-clicking it
 * shows its variants. Tools that are better reached by a modifier or a
 * gesture (zoom, pan, color sampler) have no slot; they are still found with
 * Find Anything and keep their shortcuts.
 */
class KisStudioToolRail : public QWidget
{
    Q_OBJECT
public:
    explicit KisStudioToolRail(KisMainWindow *mainWindow, QWidget *parent = nullptr);
    ~KisStudioToolRail() override;

    /// Widgets below the tools, e.g. the brush size and opacity sliders
    void addBottomWidget(QWidget *widget);

    /// "Rectangular Selection Tool" -> "Rectangular Selection"
    static QString toolName(KoToolFactoryBase *factory);

private:
    void activateSlot(int slot);
    void activateTool(int slot, const QString &toolId);
    void showVariants(int slot);
    void updateActiveSlot();
    void rememberPreset();
    void loadPresetForSlot(int slot);

    struct Private;
    const std::unique_ptr<Private> d;
};

#endif // KISSTUDIOTOOLRAIL_H
