/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIOPICKER_H
#define KISSTUDIOPICKER_H

#include <QFrame>
#include <QImage>
#include <QSet>
#include <QSize>
#include <QVector>

#include <functional>
#include <memory>

#include "kritaui_export.h"

class QPainter;

/**
 * Picks one of many things (blend modes, effects) from your own list of
 * them, with everything else a click away.
 *
 * "Yours" are the ones you keep at hand, in the order you put them in, and
 * numbered: drag one to move it, star one to add it, unstar or drag it out
 * to take it off. Above them, the recently used ones that are not yours yet;
 * below, on request, all the others in groups. Typing searches all of them,
 * other names for them included. Pointing at one emits sigPreview() (after
 * a moment, for the slow kinds), so the caller can show it; a click or
 * Enter picks it, Escape or clicking elsewhere leaves things as they were.
 *
 * What is picked from is described by a Source; KisBlendModePicker and
 * KisStudioEffectsPicker are the two kinds.
 */
class KRITAUI_EXPORT KisStudioPicker : public QFrame
{
    Q_OBJECT
public:
    struct Group {
        QString title;
        QStringList ids;
    };

    struct Source {
        /// Where your list is kept (kritarc)
        QString pinnedKey;
        /// Your list until you change it
        std::function<QStringList()> initialPinned;
        /// Everything there is, to search and to tell valid ids
        std::function<QStringList()> allIds;
        /// The ones that are not yours, in groups, in the order they are shown
        std::function<QVector<Group>(const QStringList &pinned)> otherGroups;
        std::function<QString(const QString &id)> name;
        /// A shorter name in your list, if there is one
        std::function<QString(const QString &id)> pinnedName;
        /// What else it is called, for searching
        std::function<QStringList(const QString &id)> aliases;
        /// Its picture: a swatch in a row, a thumbnail in a tile
        std::function<void(QPainter &painter, const QRect &rect, const QString &id)> paintPreview;
        /// Recently used ones, most recent first
        std::function<QStringList()> recent;
        /// Tiles with large previews instead of rows with swatches
        bool tiles {false};
        /// How long the pointer rests on one before it is previewed, in ms
        int previewDelay {0};
        QString yoursTitle;
        QString searchPlaceholder;
        /// "All %1 ..." where %1 is the number of them
        QString allLabel;
    };

    /// A picture with a bit of everything an effect or blend mode can change, for a blank canvas
    static QImage samplePicture(const QSize &size);
    /// Whether a picture is one flat color (or empty): an effect or a blend mode would show nothing on it
    static bool isFlat(const QImage &image);

    static QStringList pinnedIds(const Source &source);
    static void setPinnedIds(const Source &source, const QStringList &ids);
    /// Yours, then all the others in the order the picker shows them
    static QStringList scrollOrder(const Source &source);

    /// @param available the ids that can be picked now; empty: all of them
    KisStudioPicker(const Source &source, const QString &current, const QSet<QString> &available, QWidget *parent);
    ~KisStudioPicker() override;

    void popupBelow(QWidget *anchor);
    void popupNear(const QPoint &globalPos);
    /// Paints the previews again, e.g. when one more of them is ready
    void updatePreviews();

Q_SIGNALS:
    /// The one pointed at; an empty id when the preview should end
    void sigPreview(const QString &id);
    void sigPicked(const QString &id);

public:
    /// Shared with the grid that draws the items
    struct Private;

protected:
    /**
     * Another way to see them (e.g. a map), instead of the list: a link in
     * the footer switches; \p rememberedKey keeps which one was last shown.
     */
    void addAlternateView(QWidget *view, const QString &label, const QString &rememberedKey);
    /// Picks as a click on an item does: says so, and closes
    void pickId(const QString &id);

    bool eventFilter(QObject *watched, QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    void popupAt(QPoint topLeft, QWidget *anchor);

    const std::unique_ptr<Private> d;
};

#endif // KISSTUDIOPICKER_H
