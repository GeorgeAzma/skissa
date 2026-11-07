/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef KISSTUDIOEXPORT_H
#define KISSTUDIOEXPORT_H

#include "widgets/KisStudioPopup.h"

#include <QScopedPointer>

#include "kritaui_export.h"

class KisMainWindow;

/**
 * Export in the Studio: the common formats as pictures of the image as each would save it, part of it at its own
 * size, with the file each makes; a quality for those that lose some. Export asks only where. Other formats go
 * through Krita's export.
 */
class KRITAUI_EXPORT KisStudioExport : public KisStudioPopup
{
    Q_OBJECT
public:
    /// Opens the sheet for \p window's image; false where Krita's own export runs instead (classic layout, no image)
    static bool open(KisMainWindow *window);

    ~KisStudioExport() override;

protected:
    void keyPressEvent(QKeyEvent *event) override;

private:
    explicit KisStudioExport(KisMainWindow *window);
    void refresh();
    void measure(int format);
    void exportNow();

    struct Private;
    QScopedPointer<Private> d;
};

#endif
