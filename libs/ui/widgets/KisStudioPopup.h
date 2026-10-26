/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIOPOPUP_H
#define KISSTUDIOPOPUP_H

#include <QFrame>
#include <QPointer>

#include "kritaui_export.h"

class KisPopupCloser;

/**
 * A glass panel in a window of its own, shown next to what opened it. A press elsewhere closes it,
 * unless it is pinned; popups opened from it count as inside.
 */
class KRITAUI_EXPORT KisStudioPopup : public QFrame
{
    Q_OBJECT
public:
    explicit KisStudioPopup(QWidget *parent);
    ~KisStudioPopup() override;

    /// Room around the panel for its shadow, in pixels
    int shadowMargin() const;
    /// Sets \p layout's margins so its content sits inside the panel, \p padding from its edge
    void fitLayout(class QLayout *layout, int padding);

    /// Shows the panel beside \p anchor, on its \p edge side if there is room
    void popupBeside(QWidget *anchor, Qt::Edge edge);
    /// Shows the panel with its top left corner at \p globalPos
    void popupAt(const QPoint &globalPos, QWidget *anchor = nullptr);
    /// Keeps the panel on screen after it grew
    void keepOnScreen();
    /// How tall the window may be: the room on its anchor's side within the anchor's window, or the screen's height
    int heightRoom() const;

    void setPinned(bool pinned);

protected:
    /// Called before the panel is placed beside its anchor, so it can fit the room there
    virtual void fitRoom() {}

    void paintEvent(QPaintEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    KisPopupCloser *m_closer {nullptr};
    bool m_pinned {false};
    QPointer<QWidget> m_anchor;
    Qt::Edge m_edge {Qt::BottomEdge};
};

#endif // KISSTUDIOPOPUP_H
