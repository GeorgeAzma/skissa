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
 * Effects adjusted on the canvas instead of in a dialog: a small panel over the canvas, the
 * effect shown on the whole layer, and a drag on the canvas setting its first two numbers.
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
    /// Dragged by its title bar
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    void startApplying();
    void createMask(KisFilterConfigurationSP config);
    void finishEditing();
    void reposition();
    /// True when a press, move or release on the canvas was used as a drag
    bool handleCanvasDrag(QEvent *event);

    struct Private;
    const std::unique_ptr<Private> d;
};

#endif // KISSTUDIOEFFECTOVERLAY_H
