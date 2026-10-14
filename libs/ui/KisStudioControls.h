/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIOCONTROLS_H
#define KISSTUDIOCONTROLS_H

#include <QDockWidget>
#include <QStringList>

#include <KoDockFactoryBase.h>

#include <memory>

#include "kritaui_export.h"

class KisMainWindow;
class KisStudioValueRow;

/**
 * Settings pinned outside their own place: a brush's ("brushsetting:<option id>") or Krita's
 * ("setting:<registry id>"), each as a row that changes it where it is pinned (the top bar,
 * the Controls panel). A number drags, a switch and a choice are clicked, a right click resets.
 */
namespace KisStudioControls
{
/// The row for \p key, for the current brush (made again when it changes); null when there is none
KRITAUI_EXPORT KisStudioValueRow *create(KisMainWindow *mainWindow, const QString &key, QWidget *parent);
/// Whether \p key names something that can be pinned
KRITAUI_EXPORT bool isControl(const QString &key);
/// The key that pins Krita's setting \p id
KRITAUI_EXPORT QString settingKey(const QString &id);
/// Whether setting \p id can be pinned (a switch, a number or a choice that applies at once)
KRITAUI_EXPORT bool canPinSetting(const QString &id);
}

/// A panel of the controls the user collected, in their order; dropped in, pulled out
class KRITAUI_EXPORT KisStudioControlsPanel : public QDockWidget
{
    Q_OBJECT
public:
    KisStudioControlsPanel();
    ~KisStudioControlsPanel() override;

    static QString id();
    /// Registers the dock factory; call once before the main window creates its dockers.
    static void registerFactory();

    QStringList items() const;
    void setItems(const QStringList &items);

protected:
    void showEvent(QShowEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    void rebuild();
    void pickSetting();
    int dropIndex(int y) const;

    struct Private;
    const std::unique_ptr<Private> d;
};

class KisStudioControlsPanelFactory : public KoDockFactoryBase
{
public:
    QString id() const override;
    DockPosition defaultDockPosition() const override { return DockRight; }
    QDockWidget *createDockWidget() override;
};

#endif // KISSTUDIOCONTROLS_H
