/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIOFIND_H
#define KISSTUDIOFIND_H

#include <QFrame>

#include <functional>
#include <memory>

#include "kritaui_export.h"

class KisMainWindow;
class QDockWidget;

/**
 * "Find anything": one search field for everything Krita can do.
 *
 * It searches tools, commands, effects, brushes, blend modes, panels, the
 * layers of the current image, workspaces and settings pages, ranks the
 * results by how well they match and by how often and how recently they
 * were used, and runs the chosen one. With an empty query it shows the
 * recently used results, so it doubles as "do that again".
 */
class KRITAUI_EXPORT KisStudioFind : public QFrame
{
    Q_OBJECT
public:
    explicit KisStudioFind(KisMainWindow *mainWindow);
    ~KisStudioFind() override;

    /// How a panel gets shown, e.g. as a studio layout flyout
    void setPanelOpener(std::function<void(QDockWidget *)> opener);

    void open(const QString &text = QString());

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

private:
    void rebuildIndex();
    void updateResults();
    void moveSelection(int delta);
    void runSelected();
    void runRow(int row);
    void adjustGeometry();

    struct Private;
    const std::unique_ptr<Private> d;
};

#endif // KISSTUDIOFIND_H
