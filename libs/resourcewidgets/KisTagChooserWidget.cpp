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
#include <QContextMenuEvent>
#include <QHBoxLayout>
#include <QStyleOption>
#include <KisModernStyle.h>
#include <QIdentityProxyModel>
#include <QSignalBlocker>
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
#include <KisTouchMode.h>

#include <KisTagResourceModel.h>

namespace {

/// A tag's delete button in its list row
QRect deleteButtonRect(const QRect &row)
{
    return QRect(row.right() - 24, row.center().y() - 9, 18, 18);
}

/// List rows with a delete button on the hovered tag (on every tag in touch mode)
class TagItemDelegate : public QStyledItemDelegate
{
public:
    std::function<bool(const QModelIndex &)> deletable;

    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QStyledItemDelegate::paint(painter, option, index);
        const bool pointed = option.state & (QStyle::State_MouseOver | QStyle::State_Selected);
        if ((pointed || KisTouchMode::isActive()) && deletable && deletable(index)) {
            koIcon("edit-delete").paint(painter, deleteButtonRect(option.rect).adjusted(1, 1, -1, -1));
        }
    }
};

/// The tags, and a last "New tag…" row
class NewTagRow : public QIdentityProxyModel
{
public:
    using QIdentityProxyModel::QIdentityProxyModel;

    int newTagRow() const
    {
        return sourceModel() ? sourceModel()->rowCount() : 0;
    }

    /// The added row's index has no internal pointer; the tags' rows carry their source's
    bool isNewTag(const QModelIndex &index) const
    {
        return index.isValid() && index.model() == this && !index.internalPointer() && index.row() == newTagRow();
    }

    int rowCount(const QModelIndex &parent = QModelIndex()) const override
    {
        return parent.isValid() ? 0 : QIdentityProxyModel::rowCount(parent) + 1;
    }

    QModelIndex index(int row, int column, const QModelIndex &parent = QModelIndex()) const override
    {
        if (!parent.isValid() && row == newTagRow() && column == 0) {
            return createIndex(row, column);
        }
        return QIdentityProxyModel::index(row, column, parent);
    }

    QModelIndex parent(const QModelIndex &child) const override
    {
        if (isNewTag(child)) {
            return QModelIndex();
        }
        return QIdentityProxyModel::parent(child);
    }

    QModelIndex mapToSource(const QModelIndex &proxyIndex) const override
    {
        if (isNewTag(proxyIndex)) {
            return QModelIndex();
        }
        return QIdentityProxyModel::mapToSource(proxyIndex);
    }

    QVariant data(const QModelIndex &index, int role) const override
    {
        if (isNewTag(index)) {
            if (role == Qt::DisplayRole) return i18nc("@item:inlistbox", "New tag…");
            return QVariant();
        }
        return QIdentityProxyModel::data(index, role);
    }

    Qt::ItemFlags flags(const QModelIndex &index) const override
    {
        if (isNewTag(index)) {
            return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
        }
        return QIdentityProxyModel::flags(index);
    }
};

} // namespace

class Q_DECL_HIDDEN KisTagChooserWidget::Private
{
public:
    QComboBox *comboBox;
    NewTagRow *rows;
    /// The tag chosen before "New tag…" was picked
    /// The tag chosen before "New tag": followed as tags are added, removed or sorted
    QPersistentModelIndex lastTag;
    QToolButton *undoButton;
    QTimer undoTimeout;
    KisTagSP deletedTag;
    QPointer<QLineEdit> editor;
    /// The list was closed in this turn of the event loop: a change of the tag now was picked in it
    bool listJustClosed {false};
    KisTagModel *model;
    KisTagSP cachedTag;
    QString resourceType;
};

KisTagChooserWidget::KisTagChooserWidget(KisTagModel* model, QString resourceType, QWidget* parent)
    : QWidget(parent)
    , d(new Private)
{
    d->resourceType = resourceType;
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    d->comboBox = new QComboBox(this);
    d->comboBox->setToolTip(i18n("Tag"));
    d->comboBox->setFrame(false);
    d->comboBox->setSizePolicy(QSizePolicy::Policy::Expanding , QSizePolicy::Policy::Fixed);

    // Allow the combo box to not depend on content size.
    // Removing below code will cause the QComboBox when inside a QSplitter to have a width
    // equal to the longest QComboBox item regardless of size policy.
    d->comboBox->setMinimumContentsLength(1);
    d->comboBox->setSizeAdjustPolicy(QComboBox::SizeAdjustPolicy::AdjustToMinimumContentsLengthWithIcon);

    d->comboBox->setInsertPolicy(QComboBox::InsertAlphabetically);
    model->sort(KisAllTagsModel::Name);
    d->rows = new NewTagRow(this);
    d->rows->setSourceModel(model);
    d->comboBox->setModel(d->rows);

    d->model = model;

    // in the list, a delete button on the hovered tag; a right click renames
    auto *delegate = new TagItemDelegate(d->comboBox->view());
    delegate->deletable = [this](const QModelIndex &index) { return isUserTag(tagAt(index)); };
    d->comboBox->view()->setItemDelegate(delegate);
    d->comboBox->view()->setMouseTracking(true);
    d->comboBox->view()->viewport()->installEventFilter(this);

    QHBoxLayout *layout = new QHBoxLayout(this);
    layout->setSpacing(0);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(d->comboBox, 1);

    // never narrower than the tag shown; the search beside takes the rest
    auto fit = [this]() {
        QStyleOptionComboBox option;
        option.initFrom(d->comboBox);
        option.frame = false;
        int text = d->comboBox->fontMetrics().horizontalAdvance(d->comboBox->currentText());
        if (!d->comboBox->itemIcon(d->comboBox->currentIndex()).isNull()) text += d->comboBox->iconSize().width() + 4;
        d->comboBox->setMinimumWidth(qMin(220, d->comboBox->style()->sizeFromContents(QStyle::CT_ComboBox, &option, QSize(text, d->comboBox->height()), d->comboBox).width()));
    };
    connect(d->comboBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, fit);
    QTimer::singleShot(0, this, fit);

    d->undoButton = new QToolButton(this);
    d->undoButton->setAutoRaise(true);
    d->undoButton->hide();
    connect(d->undoButton, &QToolButton::clicked, this, &KisTagChooserWidget::restoreDeletedTag);
    layout->addWidget(d->undoButton);

    d->undoTimeout.setSingleShot(true);
    d->undoTimeout.setInterval(8000);
    connect(&d->undoTimeout, &QTimer::timeout, this, [this]() {
        d->deletedTag.clear();
        d->undoButton->hide();
    });
    updateIcons();

    this->setEnabled(true);

    connect(d->comboBox, SIGNAL(currentIndexChanged(int)),
            this, SLOT(tagChanged(int)));
    // the list closes just before what was picked in it is made current (see tagChanged)
    d->comboBox->view()->window()->installEventFilter(this);

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
    d->undoButton->show();
    d->undoTimeout.start();
}

void KisTagChooserWidget::restoreDeletedTag()
{
    KisTagSP tag = d->deletedTag;
    d->undoTimeout.stop();
    d->deletedTag.clear();
    d->undoButton->hide();
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
    // a quiet field over the tag and the search beside it, in the tag's font
    QWidget *line = parentWidget() ? parentWidget() : this;
    auto *editor = new QLineEdit(line);
    editor->setPlaceholderText(i18nc("@info:placeholder", "New tag"));
    editor->setFrame(false);
    editor->setProperty(KisModernStyle::quietFieldProperty, true);
    editor->setFont(d->comboBox->font());
    const QRect here(mapTo(line, QPoint()), size());
    editor->setGeometry(QRect(here.left(), here.top(), line->width() - here.left(), here.height()));
    editor->setProperty("newTag", true);
    editor->installEventFilter(this);
    editor->show();
    editor->setFocus();
    d->editor = editor;
}

KisTagSP KisTagChooserWidget::tagAt(const QModelIndex &rowIndex) const
{
    const QModelIndex index = d->rows->mapToSource(rowIndex);
    return index.isValid() ? d->model->tagForIndex(index) : KisTagSP();
}

void KisTagChooserWidget::startRename(const QModelIndex &index)
{
    KisTagSP tag = tagAt(index);
    if (!isUserTag(tag) || d->editor) {
        return;
    }
    QAbstractItemView *view = d->comboBox->view();
    auto *editor = new QLineEdit(view->viewport());
    // the right click that opened it must not open the field's own menu
    editor->setContextMenuPolicy(Qt::NoContextMenu);
    editor->setFrame(false);
    editor->setProperty(KisModernStyle::quietFieldProperty, true);
    editor->setFont(view->font());
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
    // a tag name being typed: Enter keeps it, Escape or clicking elsewhere drops it
    if (d->editor && watched == d->editor) {
        if (event->type() == QEvent::ShortcutOverride) {
            // the list's popup would take Return and Escape before the field sees them
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

    if (watched == d->comboBox->view()->window() && event->type() == QEvent::Hide) {
        d->listJustClosed = true;
        QTimer::singleShot(0, this, [this]() { d->listJustClosed = false; });
        return false;
    }

    if (watched == d->comboBox->view()->viewport()) {
        QAbstractItemView *view = d->comboBox->view();
        switch (event->type()) {
        case QEvent::ContextMenu: {
            // a right click, or a long press in touch mode: rename in place
            const QModelIndex index = view->indexAt(static_cast<QContextMenuEvent *>(event)->pos());
            if (index.isValid()) startRename(index);
            return true;
        }
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonRelease:
        case QEvent::MouseButtonDblClick: {
            auto *mouse = static_cast<QMouseEvent *>(event);
            const QModelIndex index = view->indexAt(mouse->pos());
            if (!index.isValid()) {
                break;
            }
            if (mouse->button() == Qt::RightButton) {
                // the context menu event that follows renames
                return true;
            }
            KisTagSP tag = tagAt(index);
            if (mouse->button() == Qt::LeftButton && isUserTag(tag)
                && deleteButtonRect(view->visualRect(index)).contains(mouse->pos())) {
                // the list stays open
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
    if (tagIndex == d->rows->newTagRow()) {
        // not a tag: the one chosen before stays; picked from the list, the box becomes the field for the name
        {
            QSignalBlocker blocker(d->comboBox);
            const int last = d->lastTag.isValid() ? d->lastTag.row() : 0;
            d->comboBox->setCurrentIndex(qMin(last, d->rows->newTagRow() - 1));
        }
        if (d->listJustClosed) {
            QTimer::singleShot(0, this, &KisTagChooserWidget::startNewTag);
        }
        return;
    }
    if (tagIndex >= 0) {
        d->lastTag = QPersistentModelIndex(d->comboBox->model()->index(tagIndex, d->comboBox->modelColumn(), d->comboBox->rootModelIndex()));
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
    d->undoButton->setIcon(koIcon("edit-undo"));
}
