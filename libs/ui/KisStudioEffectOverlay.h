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
 * once, and a small panel at the bottom of the canvas. You look at the
 * picture, not at a preview box; ✓ (or Enter) keeps it, ✕ (or Esc) drops
 * it, the eye shows the layer without it.
 *
 * A drag on the canvas sets the effect's first setting (sideways) and its
 * second (up and down), with the value by the pointer, the way most
 * effects are only ever adjusted. Effects with a few number settings open
 * small, their settings a click away; the others open with all of them.
 * "Editable" (remembered) keeps the result as a filter mask instead of
 * changing the layer's pixels. The canvas can still be zoomed and panned.
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
    void setExpanded(bool expanded);
    /// A press, move or release on the canvas while an effect is open; true when it was used
    bool handleCanvasDrag(QEvent *event);

    struct Private;
    const std::unique_ptr<Private> d;
};

#endif // KISSTUDIOEFFECTOVERLAY_H
