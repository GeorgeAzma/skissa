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
 * Picks a blend mode from your own list of them.
 *
 * "Your modes" are the ones you keep at hand, in the order you put them in:
 * drag a row to move it, star a mode to add it, unstar or drag it out to
 * take it off. Scrolling over a blend mode box steps through them in that
 * order, then on into all the others; the picker numbers them, so the order
 * is never a guess. The list starts with about thirty common modes, ordered
 * by what they do (darken, lighten, contrast, compare, color).
 *
 * Every mode shows a swatch made with its real composite op. Typing searches
 * all of them; "All modes" shows the others by category. Hovering or moving
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

    /// Your modes, in your order
    static QStringList pinnedModes();
    static void setPinnedModes(const QStringList &ids);
    /// What scrolling steps through: your modes, then all the others as the picker shows them
    static QStringList scrollOrder();

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
