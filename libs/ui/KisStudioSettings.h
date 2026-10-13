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
 * The Settings panel: a glass panel of cards over the canvas, grouped in sections
 * and applied live. Cards can be pinned to Quick or added to user sections; settings
 * can be searched, reset, undone, and transferred to a file or to Krita (KisSettingsTransfer).
 */
class KRITAUI_EXPORT KisStudioSettings : public QWidget
{
    Q_OBJECT
public:
    KisStudioSettings(KisMainWindow *mainWindow, QWidget *canvasArea);
    ~KisStudioSettings() override;

    /// Computes the panel's rect in the canvas area (next to the button that opens it)
    void setPlacement(std::function<QRect(const QSize &wanted)> placement);
    /// Shows the panel, scrolled to \p cardId and briefly highlighting it if given
    void open(const QString &cardId = QString());
    /// Shows the cards pinned to Quick in a small popup by \p anchor
    void showQuick(QWidget *anchor);

    /// A searchable entry for Find
    struct CardInfo {
        QString id;
        QString title;
        QString section;
        QString keywords;
        /// The setting of a single-switch entry, which Find shows and can flip
        QString toggleId;
    };
    static QVector<CardInfo> cardInfos();
    /// Flips a toggle setting from outside the panel (Find)
    static bool flip(const QString &settingId);

    void reposition();
    /// Shows a short message at the panel's foot for a few seconds
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
