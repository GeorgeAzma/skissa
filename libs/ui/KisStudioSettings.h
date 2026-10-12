/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIOSETTINGS_H
#define KISSTUDIOSETTINGS_H

#include <QVariant>
#include <QWidget>

#include <functional>
#include <memory>

#include "kritaui_export.h"

class KisMainWindow;

/**
 * Settings: a glass panel over the canvas, by the button that opens it.
 *
 * Everything in it is a card (Checkerboard, Autosave, Pressure, ...), in
 * sections by what it is about; a change shows at once, the canvas beside
 * it. Cards can be pinned to Quick (a right click on the Settings button
 * shows them) and put in sections of your own, by dragging them onto the
 * sections or from a card's menu; a card stays in its own section too.
 * Typing finds any card by what it or its settings are called. What
 * differs from Krita's defaults is marked, and listed with Changed; what
 * waits for a restart says so. Undo puts back what was changed since the
 * panel opened.
 *
 * From its menu, settings go to a file and back, and to Krita itself and
 * back (see KisSettingsTransfer), each after a list of what would change.
 */
class KRITAUI_EXPORT KisStudioSettings : public QWidget
{
    Q_OBJECT
public:
    KisStudioSettings(KisMainWindow *mainWindow, QWidget *canvasArea);
    ~KisStudioSettings() override;

    /// Where it goes in the canvas area (by the button that opens it)
    void setPlacement(std::function<QRect(const QSize &wanted)> placement);
    /// Shows it, scrolled to \p cardId (and that card marked for a moment) if given
    void open(const QString &cardId = QString());
    void toggle();
    /// The cards pinned to Quick, in a small window by \p anchor
    void showQuick(QWidget *anchor);

    /// For Find: every card, as one thing to find
    struct CardInfo {
        QString id;
        QString title;
        QString section;
        QString keywords;
        /// The setting it is, when it is one switch (Find shows it, a click flips it)
        QString toggleId;
    };
    static QVector<CardInfo> cardInfos();
    /// A card's switch flipped from elsewhere (Find)
    static bool flip(const QString &settingId);
    /// The Preferences dialog's page with \p cardId's settings ({page, tab}, as options_configure takes it); invalid if none
    static QVariant classicPage(const QString &cardId);

    void reposition();
    /// A short message at its foot, for a few seconds
    void say(const QString &text);

protected:
    bool event(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    struct Private;
    const std::unique_ptr<Private> d;
};

#endif // KISSTUDIOSETTINGS_H
