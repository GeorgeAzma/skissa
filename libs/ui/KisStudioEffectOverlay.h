/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIOEFFECTOVERLAY_H
#define KISSTUDIOEFFECTOVERLAY_H

#include <QWidget>

#include <memory>

#include <kis_types.h>

class KisMainWindow;

/**
 * Effects without a dialog: choosing one shows it on the whole layer at
 * once, and its settings in a small panel at the bottom of the canvas. You
 * look at the picture, not at a preview box; ✓ (or Enter) keeps it, ✕ (or
 * Esc) drops it, and the mask button keeps it editable as a filter mask.
 * The canvas can still be zoomed and panned meanwhile, but not painted on.
 */
class KisStudioEffectOverlay : public QWidget
{
    Q_OBJECT
public:
    explicit KisStudioEffectOverlay(KisMainWindow *mainWindow);
    ~KisStudioEffectOverlay() override;

    /// Starts editing \p filter; false if it can't be (the dialog is used then)
    bool open(KisFilterSP filter, KisFilterConfigurationSP overrideDefaultConfig);
    void accept();
    void reject();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    void startApplying();
    void createMask();
    void finishEditing();
    void reposition();

    struct Private;
    const std::unique_ptr<Private> d;
};

#endif // KISSTUDIOEFFECTOVERLAY_H
