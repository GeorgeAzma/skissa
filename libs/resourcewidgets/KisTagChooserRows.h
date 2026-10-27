/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISTAGCHOOSERROWS_H
#define KISTAGCHOOSERROWS_H

#include <QAbstractItemView>
#include <QCursor>
#include <QIdentityProxyModel>
#include <QPainter>
#include <QPainterPath>
#include <QStyledItemDelegate>

#include <functional>

#include <klocalizedstring.h>

#include <KisModernStyle.h>
#include <KisTouchMode.h>

// The tag chooser's list: its rows, and the "New tag…" row after them

/**
 * List rows drawn as the Studio's lists are: a soft squircle under the pointer, the tag in use in bold, a delete
 * button on the hovered tag (on every tag in touch mode) and the last row, "New tag…", quieter with a plus
 */
class KisTagItemDelegate : public QStyledItemDelegate
{
public:
    std::function<bool(const QModelIndex &)> deletable;
    /// The tag shown, which the list marks
    std::function<bool(const QModelIndex &)> current;
    /// The "New tag…" row
    std::function<bool(const QModelIndex &)> adding;

    using QStyledItemDelegate::QStyledItemDelegate;

    /// A tag's delete button in its list row
    static QRect deleteButtonRect(const QRect &row)
    {
        return QRect(row.right() - 30, row.center().y() - 12, 24, 24);
    }

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        return QStyledItemDelegate::sizeHint(option, index).expandedTo(QSize(0, 30));
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        const QColor ink = option.palette.color(QPalette::Text);
        auto faded = [&ink](qreal alpha) {
            QColor c = ink;
            c.setAlphaF(alpha);
            return c;
        };
        const QRectF row = QRectF(option.rect).adjusted(4, 1, -4, -1);
        const bool pointed = option.state & (QStyle::State_MouseOver | QStyle::State_Selected);
        if (pointed) painter->fillPath(KisModernStyle::squircle(row, KisModernStyle::cornerRadius + 1), faded(0.09));

        const bool add = adding && adding(index);
        const bool shown = current && current(index);
        QFont font = option.font;
        if (shown) font.setWeight(QFont::DemiBold);
        painter->setFont(font);
        QRect text = option.rect.adjusted(12, 0, -36, 0);
        if (add) {
            // a plus before it, drawn
            const QPointF plus(text.left() + 5, option.rect.center().y() + 0.5);
            painter->setPen(QPen(faded(0.6), 1.3, Qt::SolidLine, Qt::RoundCap));
            painter->drawLine(plus - QPointF(4, 0), plus + QPointF(4, 0));
            painter->drawLine(plus - QPointF(0, 4), plus + QPointF(0, 4));
            text.setLeft(text.left() + 16);
        }
        painter->setPen(add ? KisModernStyle::secondaryInk(ink) : ink);
        painter->drawText(text, Qt::AlignVCenter | Qt::AlignLeft,
                          QFontMetrics(font).elidedText(index.data(Qt::DisplayRole).toString(), Qt::ElideRight, text.width()));

        if ((pointed || KisTouchMode::isActive()) && deletable && deletable(index)) {
            const QRectF button = deleteButtonRect(option.rect);
            const QAbstractItemView *view = qobject_cast<const QAbstractItemView *>(option.widget);
            const bool onButton = view && button.contains(view->viewport()->mapFromGlobal(QCursor::pos()));
            if (onButton) painter->fillPath(KisModernStyle::squircle(button, KisModernStyle::cornerRadius), faded(0.12));
            // a bin in the ink, clear on any theme
            painter->setPen(QPen(faded(onButton ? 0.95 : 0.6), 1.3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            painter->setBrush(Qt::NoBrush);
            const QPointF c = button.center();
            painter->drawLine(c + QPointF(-5, -3.5), c + QPointF(5, -3.5));
            painter->drawLine(c + QPointF(-1.5, -5.5), c + QPointF(1.5, -5.5));
            QPainterPath bin;
            bin.moveTo(c + QPointF(-3.8, -3.5));
            bin.lineTo(c + QPointF(-3.1, 5));
            bin.lineTo(c + QPointF(3.1, 5));
            bin.lineTo(c + QPointF(3.8, -3.5));
            painter->drawPath(bin);
        }
        painter->restore();
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
