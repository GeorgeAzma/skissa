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
 * A popup for picking one of many items (blend modes, effects, brushes,
 * panels). It shows the recent items, the user's own numbered list (reordered
 * by dragging, extended with the star), the groups they made, then all other
 * items in groups, and a search over everything. Hovering an item emits sigPreview(); a click or
 * Enter picks it, and Escape or a click outside leaves things as they were.
 * A Source describes the items.
 */
class KRITAUI_EXPORT KisStudioPicker : public QFrame
{
    Q_OBJECT
public:
    struct Group {
        QString title;
        QStringList ids;
        /// What names a group the picker makes (a category) whatever its title; empty for the user's own
        QString key;
        bool operator==(const Group &other) const { return title == other.title && ids == other.ids && key == other.key; }
    };

    struct Source {
        /// The kritarc key that stores the user's list
        QString pinnedKey;
        /// The user's list until they change it
        std::function<QStringList()> initialPinned;
        /// The kritarc key that stores the groups the user made, shown after their list; empty for none
        QString groupsKey;
        /// The user's groups until they change them
        std::function<QVector<Group>()> initialGroups;
        /// Items added since groups may have been saved: each goes once into the group of its initial one
        QStringList addedLater;
        std::function<QStringList()> allIds;
        /// The items not in the user's list, grouped, in display order
        std::function<QVector<Group>(const QStringList &pinned)> otherGroups;
        std::function<QString(const QString &id)> name;
        /// Optional shorter name used in the user's list
        std::function<QString(const QString &id)> pinnedName;
        /// Alternative names to search by
        std::function<QStringList(const QString &id)> aliases;
        std::function<void(QPainter &painter, const QRect &rect, const QString &id)> paintPreview;
        /// Most recent first
        std::function<QStringList()> recent;
        /// Tiles with large previews instead of rows with swatches
        bool tiles {false};
        /// Tile size including the name; invalid for the default landscape tile
        QSize tileSize;
        /// How long the pointer rests on an item before it is previewed, in ms
        int previewDelay {0};
        QString yoursTitle;
        QString searchPlaceholder;
    };

    /// A picture to preview effects or blend modes on when the canvas is blank
    static QImage samplePicture(const QSize &size);
    /// How well \p query matches \p text: a prefix beats a word start beats the middle; 0 means no match
    static int matchScore(const QString &query, const QString &text);
    /// Whether \p image is one flat color or transparent, so useless for showing an effect
    static bool isFlat(const QImage &image);

    static QStringList pinnedIds(const Source &source);
    static void setPinnedIds(const Source &source, const QStringList &ids);
    static QVector<Group> userGroups(const Source &source);
    static void setUserGroups(const Source &source, const QVector<Group> &groups);
    /// The user's list and groups followed by all other items, in the order the picker shows them
    static QStringList scrollOrder(const Source &source);

    /// @param available the ids that can be picked now; empty means all
    KisStudioPicker(const Source &source, const QString &current, const QSet<QString> &available, QWidget *parent);
    ~KisStudioPicker() override;

    void popupBelow(QWidget *anchor);
    void popupNear(const QPoint &globalPos);
    /// Repaints the previews, e.g. when more of them are ready
    void updatePreviews();

Q_SIGNALS:
    /// The hovered item; an empty id ends the preview
    void sigPreview(const QString &id);
    void sigPicked(const QString &id);

public:
    /// Public so that the grid drawing the items can use it
    struct Private;

protected:
    /// Adds a view that can replace the list (e.g. a map), toggled by a link; \p rememberedKey stores which one shows
    void addAlternateView(QWidget *view, const QString &label, const QString &rememberedKey);
    /// Picks \p id as if its item were clicked
    void pickId(const QString &id);
    const Source &pickerSource() const;

    bool eventFilter(QObject *watched, QEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    void popupAt(QPoint topLeft, QWidget *anchor);
    void keepOnScreen();

    const std::unique_ptr<Private> d;
};

#endif // KISSTUDIOPICKER_H
