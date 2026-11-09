/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIOGLIDE_H
#define KISSTUDIOGLIDE_H

#include <QElapsedTimer>
#include <QObject>
#include <QPoint>
#include <QSize>
#include <QVariantAnimation>

#include <optional>

#include "kritaui_export.h"

class QWidget;

/**
 * Moves an overlay to its place the way the panel groups move: at once when nothing would be seen moving (hidden,
 * just shown, its parent resized) and where a drag's pointer takes it, gliding the rest, so a jump to a catch or out
 * of another's way is seen. Its widget's "studioTarget" property is where it is going, for what keeps off it.
 */
class KRITAUI_EXPORT KisStudioGlide : public QObject
{
public:
    explicit KisStudioGlide(QWidget *widget);

    /// Goes to \p target; while dragged, \p byPointer is where the pointer alone would put it
    void moveTo(const QPoint &target, const std::optional<QPoint> &byPointer = std::nullopt);

    /// Where it is going, or is when it isn't gliding
    QPoint target() const;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    QPoint offsetNow() const;
    void markTarget();

    QWidget *m_widget;
    QVariantAnimation m_glide;
    QPoint m_target;
    bool m_placed {false};
    /// The part of the place the pointer didn't make (a catch), at the last move
    QPoint m_extra;
    QSize m_areaSize;
    QElapsedTimer m_shown;
};

#endif
