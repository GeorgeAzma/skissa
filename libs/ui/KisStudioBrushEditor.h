/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIOBRUSHEDITOR_H
#define KISSTUDIOBRUSHEDITOR_H

#include "widgets/KisStudioPopup.h"

#include <QList>
#include <QPointer>

#include <functional>
#include <memory>

class KisMainWindow;
class KisPaintOpOption;
class KisPaintOpSettingsWidget;
class KisStudioPenPopup;
class KisStudioValueRow;

/**
 * The quick brush editor: a test stroke, the brush's parts as tabs (Tip, Mask, Texture, Color with the blend mode
 * and the mixing, Stroke), each with its pictures and the settings in use as rows. Dragging a row changes it, clicking
 * it shows how the pen drives it. The brush sheet (KisStudioBrushSheet) is the same parts side by side, every setting
 * shown; both build their parts' rows with optionRow() and blendRow().
 */
class KRITAUI_EXPORT KisStudioBrushEditor : public KisStudioPopup
{
    Q_OBJECT
public:
    /// Opens the editor beside \p anchor (on its \p edge side), or closes it when it is open
    static void toggle(KisMainWindow *mainWindow, QWidget *anchor, Qt::Edge edge);
    /// Whether the editor is open in \p mainWindow
    static bool isOpen(KisMainWindow *mainWindow);

    /// The current brush's settings in \p mainWindow
    static KisPaintOpSettingsWidget *brushSettings(KisMainWindow *mainWindow);
    /// The current brush's setting called \p id (KisPaintOpOption::id()), if it has one
    static KisPaintOpOption *brushOption(KisMainWindow *mainWindow, const QString &id);
    /// The key that pins \p option to the top bar
    static QString pinKey(KisPaintOpOption *option);
    /// A part of a brush as its tab and column are named (Tip, Mask, Texture, Color, Filter, Stroke)
    static QString partName(int part);
    /// The part that is how the line is laid down (smoothing, shapes): not one of the brush's own categories
    static constexpr int strokePart = 1000;
    /// Whether the brush has a blend mode, which the Color part shows with its mixing
    static bool blends(const QList<KisPaintOpOption *> &options);
    /// What a brush setting does, as a short tooltip; empty when its name says it
    static QString optionTip(KisPaintOpOption *option);
    /// Keeps \p row showing \p option's value and changing it
    static void bindRow(KisStudioValueRow *row, KisPaintOpOption *option, KisPaintOpSettingsWidget *settings);
    /**
     * A row for \p option's value in \p host, an editor: pinned to the top bar by its pin or pulled onto it, and its pen
     * settings in \p pen, placed by \p place, on a click. \p removable: an effect the brush can go without, taken off
     * by its cross or pulled off the editor.
     */
    static KisStudioValueRow *optionRow(KisMainWindow *mainWindow, KisPaintOpOption *option, KisPaintOpSettingsWidget *settings,
                                        QWidget *host, QPointer<KisStudioPenPopup> &pen,
                                        const std::function<void(KisStudioPenPopup *, KisStudioValueRow *)> &place, bool removable,
                                        QWidget *parent);
    /// The brush's blend mode, as the top bar has it, with a picture of it and the eraser mode; null without a view
    static QWidget *blendRow(KisMainWindow *mainWindow, QWidget *parent);

    ~KisStudioBrushEditor() override;

protected:
    void showEvent(QShowEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

private:
    explicit KisStudioBrushEditor(KisMainWindow *mainWindow);
    void rebuild();
    void showPart(int part);
    void fitPage();
    void fitRoom() override { fitPage(); }
    /// The brush or its smoothing changed: the last stroke is painted again with it
    void followLastStroke();
    void updateHeader();

    struct Private;
    const std::unique_ptr<Private> d;
};

#endif // KISSTUDIOBRUSHEDITOR_H
