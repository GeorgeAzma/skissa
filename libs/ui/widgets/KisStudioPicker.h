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
 * Picks one of many things (blend modes, effects, brushes, panels) from
 * your own numbered list of them, reordered by dragging and starred to add;
 * the recent ones above it, all the others in groups on request, a search
 * over all. Pointing at one emits sigPreview(), a click or Enter picks it,
 * Escape or a click elsewhere leaves things as they were. A Source says
 * what is picked from.
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
        /// The kritarc key of your list
        QString pinnedKey;
        /// Your list until you change it
        std::function<QStringList()> initialPinned;
        std::function<QStringList()> allIds;
        /// The ones not in your list, in groups, in the order shown
        std::function<QVector<Group>(const QStringList &pinned)> otherGroups;
        std::function<QString(const QString &id)> name;
        /// A shorter name for your list, optional
        std::function<QString(const QString &id)> pinnedName;
        /// Other names, for searching
        std::function<QStringList(const QString &id)> aliases;
        std::function<void(QPainter &painter, const QRect &rect, const QString &id)> paintPreview;
        /// Most recent first
        std::function<QStringList()> recent;
        /// Tiles with large previews instead of rows with swatches
        bool tiles {false};
        /// A tile's size, name included (none: the default landscape tile)
        QSize tileSize;
        /// How long the pointer rests on one before it is previewed, in ms
        int previewDelay {0};
        QString yoursTitle;
        QString searchPlaceholder;
        /// "All %1 ..." where %1 is the number of them
        QString allLabel;
    };

    /// Something for an effect or a blend mode to show on, when the canvas is blank
    static QImage samplePicture(const QSize &size);
    /// Whether a picture is one flat color, or transparent: nothing to show an effect on
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
    /// Paints the previews again, e.g. when more of them are ready
    void updatePreviews();

Q_SIGNALS:
    /// The one pointed at; an empty id when the preview should end
    void sigPreview(const QString &id);
    void sigPicked(const QString &id);

public:
    /// Shared with the grid that draws the items
    struct Private;

protected:
    /// Another view in the list's place (e.g. a map), switched to by a link; \p rememberedKey keeps which was shown
    void addAlternateView(QWidget *view, const QString &label, const QString &rememberedKey);
    /// Picks as a click on an item does
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
