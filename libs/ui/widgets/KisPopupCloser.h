/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISPOPUPCLOSER_H
#define KISPOPUPCLOSER_H

#include <QObject>
#include <QPointer>

#include "kritaui_export.h"

class QWidget;

/**
 * Closes a frameless tool window the way a popup closes: on a press
 * anywhere outside of it, or when it loses the focus it had. (Qt::Popup
 * can't be translucent on Windows, so the popups here are tool windows.)
 *
 * A press on \p anchor only closes the popup, so that the button that
 * opened it toggles it instead of opening it again.
 */
class KRITAUI_EXPORT KisPopupCloser : public QObject
{
public:
    KisPopupCloser(QWidget *popup, QWidget *anchor = nullptr);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    bool isInside(QWidget *widget) const;

    QWidget *m_popup;
    QPointer<QWidget> m_anchor;
    bool m_wasActive = false;
};

#endif // KISPOPUPCLOSER_H
