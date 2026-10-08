/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISBLENDMODEPICKER_H
#define KISBLENDMODEPICKER_H

#include <QFrame>
#include <QSet>

#include <KoID.h>

#include <memory>

#include "kritaui_export.h"

/**
 * Picks a blend mode by what it does instead of by how it is computed.
 *
 * The 27 modes people actually use are grouped by intent (darken, lighten,
 * add contrast, compare, change color); every mode shows a swatch made with
 * its real composite op. Typing searches all of them. Hovering or moving
 * the highlight with the arrow keys emits previewed(), so the caller can
 * show the result on the canvas; Enter or a click picks the mode, Escape
 * leaves the original one.
 */
class KRITAUI_EXPORT KisBlendModePicker : public QFrame
{
    Q_OBJECT
public:
    /**
     * @param available the ids that can be used in the current color space
     */
    KisBlendModePicker(const QString &current, const QSet<QString> &available, QWidget *parent = nullptr);
    ~KisBlendModePicker() override;

    /// The modes worth having at hand, in the order the picker and cycling use
    static QStringList essentialModes();

    void popupBelow(QWidget *anchor);

Q_SIGNALS:
    /// The highlighted mode changed; an empty id when the preview should end
    void previewed(const QString &id);
    void picked(const KoID &op);

public:
    /// Shared with the grid that draws the modes
    struct Private;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    const std::unique_ptr<Private> d;
};

#endif // KISBLENDMODEPICKER_H
