/*
 *  SPDX-FileCopyrightText: 2007 Cyrille Berger <cberger@cberger.net>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef _KIS_POPUP_BUTTON_H_
#define _KIS_POPUP_BUTTON_H_

#include <QToolButton>

#include <functional>

#include <kritawidgetutils_export.h>

class QFrame;

/**
 * This class is a convenience class for a button that
 * when clicked displays a popup widget.
 */
class KRITAWIDGETUTILS_EXPORT KisPopupButton : public QToolButton
{

    Q_OBJECT

public:

    KisPopupButton(QWidget* parent);
    ~KisPopupButton() override;

    /**
     * Set the popup widget, the KisPopupButton becomes
     * the owner and parent of the widget.
     */
    void setPopupWidget(QWidget* widget);

    /**
     * Set the width of the popup widget.
     * @return new width of the popup widget
     */
    void setPopupWidgetWidth(int w);

    /**
     * @brief adjustPosition
     * adjusts the position of the popup widget based on the position
     * of this button and the size of the widget
     */
    void adjustPosition();

    /**
     * This function allows to show or hide the arrow.
     * @param v set to true to draw the arrow, else set to false
     */
    void setArrowVisible(bool v);

    bool isPopupWidgetVisible();

    /**
     * Called on a popup's frame (the window around the popup widget) each
     * time just before its window is made, while what Qt only takes then
     * (e.g. Qt::WA_TranslucentBackground) can still be set. \p detached is
     * whether it is a dialog rather than a popup, \p holdsMenu whether the
     * popup widget is a menu. The frame's own look (its frame style, no
     * margins) is set again before each call.
     */
    using FrameDecorator = std::function<void(QFrame *frame, bool detached, bool holdsMenu)>;
    static void setFrameDecorator(const FrameDecorator &decorator);

Q_SIGNALS:
    void sigPopupWidgetShown();
    /// Pinned by its pin (kept open as a window of its own), or unpinned
    void sigPinned(bool pinned);

public Q_SLOTS:

    void showPopupWidget();

    void hidePopupWidget();

    void setPopupWidgetVisible(bool visible);

    /**
     * Set whether the popup is detached as a dialog.
     * @param v set to true to cause the popup to be detached
     */
    void setPopupWidgetDetached(bool detach);

protected:
    void paintEvent(QPaintEvent* event) override;

    void paintPopupArrow();

private Q_SLOTS:
    void slotMenuAboutToHide();

private:
    struct Private;
    Private* const m_d;
};

#endif
