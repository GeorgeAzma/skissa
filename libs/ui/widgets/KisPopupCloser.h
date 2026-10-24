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
 * Gives a frameless tool window popup behaviour: it closes on a press outside
 * it or when it loses focus. Used because Qt::Popup windows can't be
 * translucent on Windows.
 *
 * A press on \p anchor is swallowed after closing, so the button that opened
 * the popup toggles it rather than reopening it.
 */
class KRITAUI_EXPORT KisPopupCloser : public QObject
{
public:
    KisPopupCloser(QWidget *popup, QWidget *anchor = nullptr);

    /// For every popup and menu: a press on what opened it closes it, and doesn't open it again
    static void installToggle();

    /// Off: the popup stays when the pointer goes elsewhere (pinned)
    void setEnabled(bool enabled) { m_enabled = enabled; }
    /// What opened the popup, if it wasn't known when the closer was made
    void setAnchor(QWidget *anchor) { m_anchor = anchor; }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    bool isInside(QWidget *widget) const;

    QWidget *m_popup;
    QPointer<QWidget> m_anchor;
    bool m_wasActive = false;
    bool m_enabled = true;
};

#endif // KISPOPUPCLOSER_H
