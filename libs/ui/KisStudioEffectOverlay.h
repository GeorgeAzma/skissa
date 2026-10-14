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
 * Adjusts an effect on the canvas instead of in a dialog: a small panel floats over the canvas,
 * the effect is previewed on the whole layer, and dragging on the canvas sets its first two numbers.
 */
class KisStudioEffectOverlay : public QWidget
{
    Q_OBJECT
public:
    explicit KisStudioEffectOverlay(KisMainWindow *mainWindow);
    ~KisStudioEffectOverlay() override;

    /// Starts editing \p filter; returns false if that isn't possible, so that the dialog can be used instead.
    bool open(KisFilterSP filter, KisFilterConfigurationSP overrideDefaultConfig);
    void accept();
    void reject();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    /// The panel is moved by dragging its title bar
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    void startApplying();
    /// The reset to defaults shows while the settings differ from them
    void showReset();
    void createMask(KisFilterConfigurationSP config);
    void finishEditing();
    void reposition();
    /// Returns true if a press, move or release on the canvas was consumed as a drag
    bool handleCanvasDrag(QEvent *event);

    struct Private;
    const std::unique_ptr<Private> d;
};

#endif // KISSTUDIOEFFECTOVERLAY_H
