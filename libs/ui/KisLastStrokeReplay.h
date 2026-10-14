/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISLASTSTROKEREPLAY_H
#define KISLASTSTROKEREPLAY_H

#include <QObject>

#include <memory>

#include "kritaui_export.h"

class KisToolFreehandHelper;
class KUndo2Stack;

/**
 * While the brush is edited, the last stroke is painted again with it: taken back and painted as it was
 * drawn, so a change shows on the picture itself. Only the newest step of the history is replaced, only
 * for a stroke of the brush being edited, and nothing happens once anything else was done since.
 */
class KRITAUI_EXPORT KisLastStrokeReplay : public QObject
{
    Q_OBJECT
public:
    static KisLastStrokeReplay *instance();
    ~KisLastStrokeReplay() override;

    /// Whether brush edits repaint the last stroke (a setting, on by default)
    static bool isEnabled();
    static void setEnabled(bool on);

    /// \p helper painted a stroke with brush \p preset; its step goes onto \p stack
    void strokeEnded(KisToolFreehandHelper *helper, KUndo2Stack *stack, const QString &preset);
    /// The brush \p preset changed: its last stroke is painted again shortly
    void brushChanged(const QString &preset);

private:
    KisLastStrokeReplay();
    void replay();

    struct Private;
    const std::unique_ptr<Private> d;
};

#endif // KISLASTSTROKEREPLAY_H
