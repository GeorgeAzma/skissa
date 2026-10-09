/*
 *    This file is part of the KDE project
 *    SPDX-FileCopyrightText: 2002 Patrick Julien <freak@codepimps.org>
 *    SPDX-FileCopyrightText: 2007 Jan Hambrecht <jaham@gmx.net>
 *    SPDX-FileCopyrightText: 2007 Sven Langkamp <sven.langkamp@gmail.com>
 *    SPDX-FileCopyrightText: 2011 Srikanth Tiyyagura <srikanth.tulasiram@gmail.com>
 *    SPDX-FileCopyrightText: 2011 José Luis Vergara <pentalis@gmail.com>
 *    SPDX-FileCopyrightText: 2013 Sascha Suelzer <s.suelzer@gmail.com>
 *    SPDX-FileCopyrightText: 2020 Agata Cacko <cacko.azh@gmail.com>
 *
 *    SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "KisTagChooserWidget.h"

#include <QAbstractItemView>
#include <QComboBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QToolButton>

#include <functional>

#include <kconfig.h>
#include <kconfiggroup.h>
#include <ksharedconfig.h>
#include <klocalizedstring.h>
#include <kis_assert.h>

#include <KoIcon.h>

#include <KisTagResourceModel.h>

namespace {

/// Where a tag's delete button is, in its row of the list
QRect deleteButtonRect(const QRect &row)
{
    return QRect(row.right() - 24, row.center().y() - 9, 18, 18);
}

/// The list's rows, with a delete button on the tag under the pointer
class TagItemDelegate : public QStyledItemDelegate
{
public:
    std::function<bool(const QModelIndex &)> deletable;

    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QStyledItemDelegate::paint(painter, option, index);
        // the list makes the row under the pointer the current one
        const bool pointed = option.state & (QStyle::State_MouseOver | QStyle::State_Selected);
        if (pointed && deletable && deletable(index)) {
            koIcon("edit-delete").paint(painter, deleteButtonRect(option.rect).adjusted(1, 1, -1, -1));
        }
    }
};

} // namespace

class Q_DECL_HIDDEN KisTagChooserWidget::Private
{
public:
    QComboBox *comboBox;
    QToolButton *addButton;
    QToolButton *undoButton;
    QTimer undoTimeout;
    KisTagSP deletedTag;
    QPointer<QLineEdit> editor;
    KisTagModel *model;
    KisTagSP cachedTag;
    QString resourceType;
};

KisTagChooserWidget::KisTagChooserWidget(KisTagModel* model, QString resourceType, QWidget* parent)
    : QWidget(parent)
    , d(new Private)
{
    d->resourceType = resourceType;

    d->comboBox = new QComboBox(this);
    d->comboBox->setToolTip(i18n("Tag"));
    d->comboBox->setSizePolicy(QSizePolicy::Policy::Expanding , QSizePolicy::Policy::Fixed);

    // Allow the combo box to not depend on content size.
    // Removing below code will cause the QComboBox when inside a QSplitter to have a width
    // equal to the longest QComboBox item regardless of size policy.
    d->comboBox->setMinimumContentsLength(1);
    d->comboBox->setSizeAdjustPolicy(QComboBox::SizeAdjustPolicy::AdjustToMinimumContentsLengthWithIcon);

    d->comboBox->setInsertPolicy(QComboBox::InsertAlphabetically);
    model->sort(KisAllTagsModel::Name);
    d->comboBox->setModel(model);

    d->model = model;

    // the list: a delete button on the tag under the pointer, a right click renames
    auto *delegate = new TagItemDelegate(d->comboBox->view());
    delegate->deletable = [this](const QModelIndex &index) { return isUserTag(d->model->tagForIndex(index)); };
    d->comboBox->view()->setItemDelegate(delegate);
    d->comboBox->view()->setMouseTracking(true);
    d->comboBox->view()->viewport()->installEventFilter(this);

    QHBoxLayout *layout = new QHBoxLayout(this);
    layout->setSpacing(0);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(d->comboBox, 1);

    d->addButton = new QToolButton(this);
    d->addButton->setAutoRaise(true);
    d->addButton->setToolTip(i18nc("@info:tooltip", "New tag"));
    connect(d->addButton, &QToolButton::clicked, this, &KisTagChooserWidget::startNewTag);
    layout->addWidget(d->addButton);

    d->undoButton = new QToolButton(this);
    d->undoButton->setAutoRaise(true);
    d->undoButton->hide();
    connect(d->undoButton, &QToolButton::clicked, this, &KisTagChooserWidget::restoreDeletedTag);
    layout->addWidget(d->undoButton);

    // the undo is offered for a little while, then the "+" is back
    d->undoTimeout.setSingleShot(true);
    d->undoTimeout.setInterval(8000);
    connect(&d->undoTimeout, &QTimer::timeout, this, [this]() {
        d->deletedTag.clear();
        d->undoButton->hide();
        d->addButton->show();
    });
    updateIcons();

    this->setEnabled(true);

    connect(d->comboBox, SIGNAL(currentIndexChanged(int)),
            this, SLOT(tagChanged(int)));

    // Workaround for handling tag selection deselection when model resets.
    // Occurs when model changes under the user e.g. +/- a resource storage.
    connect(d->model, SIGNAL(modelAboutToBeReset()), this, SLOT(cacheSelectedTag()));
    connect(d->model, SIGNAL(modelReset()), this, SLOT(restoreTagFromCache()));
}

KisTagChooserWidget::~KisTagChooserWidget()
{
    delete d;
}

bool KisTagChooserWidget::isUserTag(const KisTagSP tag)
{
    return tag && tag->id() >= 0;
}

void KisTagChooserWidget::deleteTag(KisTagSP tag)
{
    if (!isUserTag(tag)) {
        return;
    }
    const bool wasCurrent = currentlySelectedTag() && currentlySelectedTag()->url() == tag->url();
    if (!d->model->setTagInactive(tag)) {
        return;
    }
    if (wasCurrent) {
        setCurrentIndex(0);
    }
    d->model->sort(KisAllTagsModel::Name);

    d->deletedTag = tag;
    d->undoButton->setToolTip(i18nc("@info:tooltip, %1 is a tag name", "Bring back “%1”", tag->name()));
    d->addButton->hide();
    d->undoButton->show();
    d->undoTimeout.start();
}

void KisTagChooserWidget::restoreDeletedTag()
{
    KisTagSP tag = d->deletedTag;
    d->undoTimeout.stop();
    d->deletedTag.clear();
    d->undoButton->hide();
    d->addButton->show();
    if (tag && d->model->setTagActive(tag)) {
        d->model->sort(KisAllTagsModel::Name);
        setCurrentItem(tag->url());
    }
}

void KisTagChooserWidget::startNewTag()
{
    if (d->editor) {
        return;
    }
    // the box itself becomes the field for the name
    auto *editor = new QLineEdit(this);
    editor->setPlaceholderText(i18nc("@info:placeholder", "New tag"));
    editor->setGeometry(d->comboBox->geometry());
    editor->setProperty("newTag", true);
    editor->installEventFilter(this);
    editor->show();
    editor->setFocus();
    d->editor = editor;
}

void KisTagChooserWidget::startRename(const QModelIndex &index)
{
    KisTagSP tag = d->model->tagForIndex(index);
    if (!isUserTag(tag) || d->editor) {
        return;
    }
    QAbstractItemView *view = d->comboBox->view();
    auto *editor = new QLineEdit(view->viewport());
    // the right click that opened it is not one for the field's own menu
    editor->setContextMenuPolicy(Qt::NoContextMenu);
    editor->setText(tag->name());
    editor->selectAll();
    editor->setGeometry(view->visualRect(index).adjusted(2, 1, -2, -1));
    editor->setProperty("tagUrl", tag->url());
    editor->installEventFilter(this);
    editor->show();
    editor->setFocus();
    d->editor = editor;
}

bool KisTagChooserWidget::eventFilter(QObject *watched, QEvent *event)
{
    // typing a tag's name, new or renamed: Enter keeps it, Escape or clicking elsewhere does not
    if (d->editor && watched == d->editor) {
        if (event->type() == QEvent::ShortcutOverride) {
            // the list's popup takes Return and Escape on their way up, before the field sees them
            const int key = static_cast<QKeyEvent *>(event)->key();
            if (key == Qt::Key_Return || key == Qt::Key_Enter || key == Qt::Key_Escape) {
                event->accept();
                return true;
            }
        } else if (event->type() == QEvent::KeyPress) {
            const int key = static_cast<QKeyEvent *>(event)->key();
            if (key == Qt::Key_Return || key == Qt::Key_Enter || key == Qt::Key_Escape) {
                QLineEdit *editor = d->editor;
                d->editor = nullptr;
                const QString name = editor->text().trimmed();
                const bool isNew = editor->property("newTag").toBool();
                const QString url = editor->property("tagUrl").toString();
                editor->deleteLater();
                if (key != Qt::Key_Escape && !name.isEmpty()) {
                    if (isNew) {
                        addTag(name);
                        setCurrentItem(name);
                    } else if (KisTagSP tag = d->model->tagForUrl(url)) {
                        renameTag(tag, name);
                    }
                }
                return true;
            }
        } else if (event->type() == QEvent::FocusOut) {
            d->editor->deleteLater();
            d->editor = nullptr;
        }
        return false;
    }

    if (watched == d->comboBox->view()->viewport()) {
        QAbstractItemView *view = d->comboBox->view();
        switch (event->type()) {
        case QEvent::ContextMenu:
            // a right click renames, see below
            return true;
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonRelease:
        case QEvent::MouseButtonDblClick: {
            auto *mouse = static_cast<QMouseEvent *>(event);
            const QModelIndex index = view->indexAt(mouse->pos());
            if (!index.isValid()) {
                break;
            }
            if (mouse->button() == Qt::RightButton) {
                if (event->type() == QEvent::MouseButtonPress) startRename(index);
                return true;
            }
            KisTagSP tag = d->model->tagForIndex(index);
            if (mouse->button() == Qt::LeftButton && isUserTag(tag)
                && deleteButtonRect(view->visualRect(index)).contains(mouse->pos())) {
                // the list stays open: the other tags are still there to choose from
                if (event->type() == QEvent::MouseButtonRelease) deleteTag(tag);
                return true;
            }
            break;
        }
        default:
            break;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void KisTagChooserWidget::tagChanged(int tagIndex)
{
    if (tagIndex >= 0) {
        KisTagSP tag = currentlySelectedTag();
        KConfigGroup group =  KSharedConfig::openConfig()->group("SelectedTags");
        group.writeEntry(d->resourceType, currentlySelectedTag()->url());
        d->model->sort(KisAllTagsModel::Name);
        Q_EMIT sigTagChosen(tag);
    } else {
        setCurrentIndex(0);
    }
}

void KisTagChooserWidget::renameTag(KisTagSP tag, const QString& tagName)
{
    bool canRenameTag = isUserTag(tag) && (tagName != tag->name());

    if (tagName == KisAllTagsModel::urlAll() || tagName == KisAllTagsModel::urlAllUntagged()) {
        QMessageBox::information(this, i18nc("Dialog title", "Can't rename the tag"), i18nc("Dialog message", "You can't use this name for your custom tags."), QMessageBox::Ok);
        return;
    }

    const bool wasCurrent = currentlySelectedTag() && currentlySelectedTag()->url() == tag->url();
    bool result = false;

    if (canRenameTag && !tagName.isEmpty()) {
         result = d->model->renameTag(tag, tagName, false);

        if (!result) {
            KisTagSP tagToRemove = d->model->tagForUrl(tagName);

            if (tagToRemove &&
                QMessageBox::question(this, i18nc("Dialog title", "Remove existing tag with that name?"),
                i18nc("Dialog message (the arguments are both somewhat user readable nouns or adjectives (names of the tags), can be treated as nouns since they represent the tags)",
                "A tag with this unique name already exists. In order to continue renaming, the existing tag needs to be removed. Do you want to continue?\n"
                "Tag to be removed: %1\n"
                "Tag's unique name: %2", tagToRemove->name(), tagToRemove->url()), QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Cancel) {
                result = d->model->renameTag(tag, tagName, true);
                KIS_SAFE_ASSERT_RECOVER_RETURN(result);
            }
        }
    }

    if (result) {
        d->model->sort(KisAllTagsModel::Name);
        if (wasCurrent) {
            KisTagSP renamedTag = d->model->tagForUrl(tagName);
            KIS_SAFE_ASSERT_RECOVER_RETURN(renamedTag);
            setCurrentIndex(d->model->indexForTag(renamedTag).row());
        }
    }
}

void KisTagChooserWidget::cacheSelectedTag()
{
    d->cachedTag = currentlySelectedTag();
}

void KisTagChooserWidget::restoreTagFromCache()
{
    if (d->cachedTag) {
        QModelIndex cachedIndex = d->model->indexForTag(d->cachedTag);
        setCurrentIndex(cachedIndex.row());
        d->cachedTag = nullptr;
    }
}

void KisTagChooserWidget::setCurrentIndex(int index)
{
    d->comboBox->setCurrentIndex(index);
}

int KisTagChooserWidget::currentIndex() const
{
    return d->comboBox->currentIndex();
}

void KisTagChooserWidget::setCurrentItem(const QString &tag)
{
    for (int i = 0; i < d->model->rowCount(); i++) {
        QModelIndex index = d->model->index(i, 0);
        QString currentRowTag = d->model->data(index, Qt::UserRole + KisAllTagsModel::Url).toString();
        if (currentRowTag == tag) {
            setCurrentIndex(i);
        }
    }
}

void KisTagChooserWidget::addTag(const QString &tag)
{
    addTag(tag, 0);
}

QMessageBox::ButtonRole KisTagChooserWidget::overwriteTagDialog(KisTagChooserWidget* parent, bool tagIsActive)
{
    QString undeleteOption = !tagIsActive ? i18nc("Option in a dialog to undelete (reactivate) existing tag with its old assigned resources", "Restore previous tag")
                                      : i18nc("Option in a dialog to use existing tag with its old assigned resources", "Use existing tag");
    QMessageBox question = QMessageBox(QMessageBox::Question,
        i18nc("Dialog title", "Overwrite tag?"),
        i18nc("Question to the user in a dialog about creating a tag",
              "A tag with this unique name already exists. Do you want to replace it?")
    );
    question.setParent(parent);
    question.addButton(
        i18nc("Option in a dialog to discard the previously existing tag and creating a new one in its place",
              "Replace (overwrite) tag"),
        QMessageBox::DestructiveRole
    );
    question.addButton(undeleteOption, QMessageBox::AcceptRole);
    question.addButton(QMessageBox::Cancel);
    question.exec();
    return question.buttonRole(question.clickedButton());
}

void KisTagChooserWidget::addTag(const QString &tagName, KoResourceSP resource)
{
    if (tagName == KisAllTagsModel::urlAll() || tagName == KisAllTagsModel::urlAllUntagged()) {
        QMessageBox::information(this, i18nc("Dialog title", "Can't create the tag"), i18nc("Dialog message", "You can't use this name for your custom tags."), QMessageBox::Ok);
        return;
    }

    if (tagName.isEmpty()) return;

    KisTagSP tagForUrl = d->model->tagForUrl(tagName);
    if (!tagForUrl.isNull()) {
        int response = overwriteTagDialog(this, tagForUrl->active());
        if (response == QMessageBox::AcceptRole) { // Undelete
            d->model->setTagActive(tagForUrl);
            if (!resource.isNull()) {
                KisTagResourceModel(d->resourceType).tagResources(tagForUrl, QVector<int>() << resource->resourceId());
            }
            d->model->sort(KisAllTagsModel::Name);
            return;
        } else if (response == QMessageBox::RejectRole) { // Cancel
            return;
        }
    }
    QVector<KoResourceSP> resources = (resource.isNull() ? QVector<KoResourceSP>() : (QVector<KoResourceSP>() << resource));
    d->model->addTag(tagName, true, resources); // this will overwrite the tag
    d->model->sort(KisAllTagsModel::Name);
}

void KisTagChooserWidget::addTag(KisTagSP tag, KoResourceSP resource)
{
    if (tag->name() == KisAllTagsModel::urlAll() || tag->name() == KisAllTagsModel::urlAllUntagged()) {
        QMessageBox::information(this, i18nc("Dialog title", "Can't rename the tag"), i18nc("Dialog message", "You can't use this name for your custom tags."), QMessageBox::Ok);
        return;
    }

    KisTagSP tagForUrl = d->model->tagForUrl(tag->url());
    if (!tagForUrl.isNull()) {
        int response = overwriteTagDialog(this, tagForUrl->active());
        if (response == QMessageBox::AcceptRole) { // Undelete
            d->model->setTagActive(tagForUrl);
            if (!resource.isNull()) {
                KisTagResourceModel(d->resourceType).tagResources(tagForUrl, QVector<int>() << resource->resourceId());
            }
            d->model->sort(KisAllTagsModel::Name);
            return;
        } else if (response == QMessageBox::RejectRole) { // Cancel
            return;
        }
    }
    QVector<KoResourceSP> resources = (resource.isNull() ? QVector<KoResourceSP>() : (QVector<KoResourceSP>() << resource));
    d->model->addTag(tag, true, resources); // this will overwrite the tag
    d->model->sort(KisAllTagsModel::Name);
}

KisTagSP KisTagChooserWidget::currentlySelectedTag()
{
    int row = d->comboBox->currentIndex();
    if (row < 0) {
        return nullptr;
    }

    QModelIndex index = d->model->index(row, 0);
    KisTagSP tag =  d->model->tagForIndex(index);
    return tag;
}

void KisTagChooserWidget::updateIcons()
{
    d->addButton->setIcon(koIcon("list-add"));
    d->undoButton->setIcon(koIcon("edit-undo"));
}
