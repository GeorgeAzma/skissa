/*
 * SPDX-FileCopyrightText: 2019 Boudewijn Rempt <boud@valdyas.org>
 * SPDX-FileCopyrightText: 2020 Agata Cacko <cacko.azh@gmail.com>
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#ifndef KISTAGCHOOSERWIDGET_H
#define KISTAGCHOOSERWIDGET_H

#include <QWidget>
#include <QMessageBox>
#include "kritaresourcewidgets_export.h"

#include <KisTag.h>
#include <KisTagModel.h>

/**
 * \brief The KisTagChooserWidget class is responsible for all the logic
 *        that the tags combobox has in various resource choosers.
 *
 * It uses KisTagModel as a model for items in the combobox. Tags are made,
 * renamed and deleted where they are:
 * - "+" next to the box turns it into a field for the new tag's name
 *   (Enter makes it, Escape or clicking elsewhere does not);
 * - in the list, a right click on a tag (or holding a finger or pen on it)
 *   renames it in place;
 * - a tag under the pointer (every tag, while a finger is used) shows a
 *   delete button on its right; for a few seconds after, the "+" is an undo
 *   button that brings it back.
 * Tags Krita makes itself ("All", "All Untagged") cannot be renamed or deleted.
 * All the logic for adding and removing tags is done through KisTagModel.
 */
class KRITARESOURCEWIDGETS_EXPORT KisTagChooserWidget : public QWidget
{
    Q_OBJECT

public:
    explicit KisTagChooserWidget(KisTagModel* model, QString resourceType, QWidget* parent);
    ~KisTagChooserWidget() override;


    /// \brief setCurrentItem sets the tag from the param as the current tag in the combobox
    /// \param tag tag url to be set as current in the combobox
    /// \return true if successful, false if not successful
    void setCurrentItem(const QString &tag);

    /// \brief currentIndex returns the current index in tags combobox
    /// \return the index of the current item in tags combobox
    int currentIndex() const;

    /// \brief currentlySelectedTag returns the current tag from combobox
    /// \see currentIndex
    /// \return the tag that is currently selected in the tag combobox
    KisTagSP currentlySelectedTag();

    /// \brief update icon files on loading and theme change
    void updateIcons();

Q_SIGNALS:

    /// \brief sigTagChosen is emitted when the selected tag in the combobox changes due to user interaction or by other means
    /// \param tag current tag
    void sigTagChosen(const KisTagSP tag);

public Q_SLOTS:

    /// \brief tagChanged slot for the signal from the combobox that the index changed
    /// \param index new index
    ///
    /// When the index in the combobox changes, for example because of user's interaction,
    ///  combobox emits a signal; this method is called when it happens.
    void tagChanged(int index);

    /// \brief addTag creates a tag with this name (asking first when one exists)
    void addTag(const QString &tag);
    void addTag(const QString &tag, KoResourceSP resource);
    void addTag(KisTagSP tag, KoResourceSP resource);

private Q_SLOTS:

    /// \brief cacheSelectedTag slot that stores current tag selection.
    ///
    /// Used to allow restoration of tag even after a model reset. Will store
    /// the tag just before model resets.
    void cacheSelectedTag();

    /// \brief restoreTagFromCache slot designed to restore a selected tag from previously cached selection.
    ///
    /// Companion to `cacheSelectedTag`, this method restore the selection after model reset.
    void restoreTagFromCache();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:


    /// \brief setCurrentIndex sets the current index in the combobox
    /// \param index index is the index of the tag in the combobox
    void setCurrentIndex(int index);

    QMessageBox::ButtonRole overwriteTagDialog(KisTagChooserWidget* parent, bool undelete);

    /// Tags made by Krita ("All", "All Untagged") have no id of their own
    static bool isUserTag(const KisTagSP tag);
    void renameTag(KisTagSP tag, const QString &name);
    /// Not deleted, only marked inactive in the database: the undo button brings it back
    void deleteTag(KisTagSP tag);
    void restoreDeletedTag();
    void startNewTag();
    void startRename(const QModelIndex &index);

private:
    class Private;
    Private* const d;

};

#endif // KISTAGCHOOSERWIDGET_H
