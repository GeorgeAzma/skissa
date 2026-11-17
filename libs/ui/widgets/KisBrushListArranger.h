/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISBRUSHLISTARRANGER_H
#define KISBRUSHLISTARRANGER_H

#include <QObject>
#include <QPointer>
#include <QSet>

#include <KisTag.h>

class KisResourceItemChooser;
class KisTagChooserWidget;
class QDropEvent;
class QToolButton;
class QModelIndex;
class QWidget;

/**
 * What the brush list does beyond picking: brushes dragged into an order within their tag (as pictures or strokes),
 * brushes added to the tag shown (all of them listed, a click putting one in or taking it out), and brush files
 * dropped on it or chosen from its menu imported, into the tag shown.
 */
class KisBrushListArranger : public QObject
{
    Q_OBJECT
public:
    KisBrushListArranger(KisResourceItemChooser *chooser, QObject *parent);

    /// While adding: 1 for a brush in the tag, 0 for one that isn't; -1 otherwise
    int markOf(const QModelIndex &index) const;

    bool isAdding() const { return !m_adding.isNull(); }
    void startAdding();
    void stopAdding();
    void importFiles(const QStringList &files);
    void chooseFilesToImport();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    /// The tag shown, if it is one of the user's
    KisTagSP userTag() const;
    /// The tag shown, if its brushes keep an order: the user's, or All
    KisTagSP orderedTag() const;
    void toggle(const QModelIndex &index);
    /// The + as a ✓ and Import beside it while adding
    void showAdding(bool adding);
    /// The row a drop at \p pos goes before; the line showing it
    int dropRowAt(const QPoint &pos) const;
    void showDropLine(const QPoint &pos);
    void hideDropLine();
    /// The brush in file \p moved goes where a drop at \p pos in the list shows
    void moveBrush(const QString &moved, const QPoint &pos);
    void showMenu(const QPoint &globalPos);

    KisResourceItemChooser *m_chooser;
    QPointer<KisTagChooserWidget> m_tags;
    /// The tag brushes are being added to, and the ids of those in it
    KisTagSP m_adding;
    QSet<int> m_members;
    QToolButton *m_import {nullptr};
    QWidget *m_dropLine {nullptr};
    /// The brush pressed: while adding, the click toggles it on release
    int m_pressedRow {-1};
    QPoint m_pressPos;
    /// A brush is being dragged to another place in its tag
    bool m_reordering {false};
};

#endif // KISBRUSHLISTARRANGER_H
