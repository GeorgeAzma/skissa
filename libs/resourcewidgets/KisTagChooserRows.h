/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISTAGCHOOSERROWS_H
#define KISTAGCHOOSERROWS_H

#include <QIdentityProxyModel>
#include <QPainter>
#include <QStyledItemDelegate>

#include <functional>

#include <klocalizedstring.h>

#include <KoIcon.h>
#include <KisTouchMode.h>

// The tag chooser's list: its rows, and the "New tag…" row after them

/// List rows with a delete button on the hovered tag (on every tag in touch mode)
class KisTagItemDelegate : public QStyledItemDelegate
{
public:
    std::function<bool(const QModelIndex &)> deletable;

    using QStyledItemDelegate::QStyledItemDelegate;

    /// A tag's delete button in its list row
    static QRect deleteButtonRect(const QRect &row)
    {
        return QRect(row.right() - 24, row.center().y() - 9, 18, 18);
    }

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
class KisNewTagRow : public QIdentityProxyModel
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


#endif // KISTAGCHOOSERROWS_H
