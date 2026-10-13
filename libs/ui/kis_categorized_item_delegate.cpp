/*
 *  SPDX-FileCopyrightText: 2009 Cyrille Berger <cberger@cberger.net>
 *  SPDX-FileCopyrightText: 2011 Silvio Heinrich <plassy@web.de>
 *  SPDX-FileCopyrightText: 2014 Mohit Goyal <mohit.bits2011@gmail.com>
 *
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "kis_categorized_item_delegate.h"
#include "kis_categorized_list_model.h"

#include <QPainter>
#include <QStyle>
#include <QStyleOptionMenuItem>
#include <QStyleOptionViewItem>
#include <QApplication>
#include <QTransform>

#include <KoIcon.h>

#include <kis_icon.h>
#include <KisModernStyle.h>
#include "kis_debug.h"

KisCategorizedItemDelegate::KisCategorizedItemDelegate(QObject *parent)
    : QStyledItemDelegate(parent),
      m_minimumItemHeight(0)
{
}

void KisCategorizedItemDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const
{
    painter->resetTransform();

    if(!index.data(__CategorizedListModelBase::IsHeaderRole).toBool()) {
        QStyleOptionViewItem sovi(option);

        if (index.data(__CategorizedListModelBase::isLockableRole).toBool()) {

            const int iconSize = qMax(16, m_minimumItemHeight - 2);
            bool locked = index.data(__CategorizedListModelBase::isLockedRole).toBool();


            QIcon icon = locked ? KisIconUtils::loadIcon(koIconName("layer-locked")) : KisIconUtils::loadIcon(koIconName("layer-unlocked"));

            // to be able to make an icon more transparent. we need to create a new image
            // and use the QPainter to make a more transparent version of it.
            QImage image(iconSize, iconSize, QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::transparent); // needs to happen, otherwise 'junk' pixels are used (looks bad)

            QPainter p(&image);
            p.setCompositionMode(QPainter::CompositionMode_Source);


            // an open lock is faint, clearer while the pointer is on its row
            if (locked) {
                p.setOpacity(1.0);
            } else {
                p.setOpacity(option.state & QStyle::State_MouseOver ? 0.5 : 0.14);
            }

            p.drawPixmap(0, 0, icon.pixmap(iconSize, QIcon::Normal) );
            p.end();

            icon = QIcon( QPixmap::fromImage(image) ); // the new icon is ready



            sovi.decorationPosition = QStyleOptionViewItem::Right;
            sovi.decorationAlignment = Qt::AlignRight;
            sovi.decorationSize = QSize(iconSize, iconSize);
            sovi.features |= QStyleOptionViewItem::HasDecoration;
            sovi.icon = icon;

        }

        // the value under the name, its number after it; a full one only while the pointer is over it
        QVariant fraction = index.data(__CategorizedListModelBase::ValueFractionRole);
        if (index.data(__CategorizedListModelBase::ValueFullRole).toBool() && !(option.state & QStyle::State_MouseOver)) {
            fraction = QVariant();
        }
        if (fraction.isValid()) {
            paintValue(painter, sovi, index, fraction.toReal(), false);
        }
        QStyledItemDelegate::paint(painter, sovi, index);
        painter->setOpacity(1);
        if (fraction.isValid()) {
            paintValue(painter, sovi, index, fraction.toReal(), true);
        }
    }
    else {
        QPalette palette = QApplication::palette();
        if(option.state & QStyle::State_MouseOver)
            painter->fillRect(option.rect, palette.midlight());
        else
            painter->fillRect(option.rect, palette.button());

        painter->setBrush(palette.buttonText());
        painter->drawText(option.rect, index.data().toString(), QTextOption(Qt::AlignVCenter|Qt::AlignHCenter));

        paintTriangle(
            painter,
            option.rect.x(),
            option.rect.y(),
            option.rect.height(),
            !index.data(__CategorizedListModelBase::ExpandCategoryRole).toBool()
        );
    }
    painter->resetTransform();
}

QSize KisCategorizedItemDelegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const
{
    //on first calling this calculates the minimal height of the items
    if (m_minimumItemHeight == 0) {
        for(int i=0; i<index.model()->rowCount(); i++) {
            QSize indexSize = QStyledItemDelegate::sizeHint(option, index.model()->index(i, 0));
            m_minimumItemHeight = qMax(m_minimumItemHeight, indexSize.height());

            /**
             * Make all the items, including the ones having
             * checkboxes look the same.
             */
            QStyle *style = QApplication::style();
            QStyleOptionButton so;
            QSize size = style->sizeFromContents(QStyle::CT_CheckBox, &so, QSize(), 0);

            m_minimumItemHeight = qMax(size.height(), m_minimumItemHeight);
        }
    }

    int width = QStyledItemDelegate::sizeHint(option, index).width();

    if (index.data(__CategorizedListModelBase::isLockableRole).toBool()) {
        width += m_minimumItemHeight;
    }

    return QSize(width, m_minimumItemHeight);
}

QRect KisCategorizedItemDelegate::valueRect(const QStyleOptionViewItem &option, const QModelIndex &index)
{
    QStyleOptionViewItem opt(option);
    opt.index = index;
    opt.features |= QStyleOptionViewItem::HasDisplay;
    if (index.flags() & Qt::ItemIsUserCheckable) opt.features |= QStyleOptionViewItem::HasCheckIndicator;
    opt.text = index.data(Qt::DisplayRole).toString();
    const QStyle *style = opt.widget ? opt.widget->style() : QApplication::style();
    // from where the name starts to the row's end (styles may give only the name's width)
    QRect rect = style->subElementRect(QStyle::SE_ItemViewItemText, &opt, opt.widget);
    rect.setRight(option.rect.right() - KisModernStyle::space1);
    // what follows the name: the lock, at the row's end
    if (index.data(__CategorizedListModelBase::isLockableRole).toBool()) {
        rect.setRight(option.rect.right() - option.rect.height() - KisModernStyle::space1);
    }
    return rect.adjusted(0, 2, 0, -2);
}

void KisCategorizedItemDelegate::paintValue(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index, qreal fraction, bool number) const
{
    const QRect rect = valueRect(option, index);
    const bool applies = index.data(__CategorizedListModelBase::ValueAppliesRole).toBool();
    const QColor ink = option.palette.color(QPalette::Text);

    painter->save();
    if (number) {
        painter->setPen(applies ? ink : KisModernStyle::secondaryInk(ink));
        painter->drawText(rect.adjusted(0, 0, -KisModernStyle::space1, 0), Qt::AlignRight | Qt::AlignVCenter,
                          index.data(__CategorizedListModelBase::ValueTextRole).toString());
        painter->restore();
        return;
    }
    painter->setRenderHint(QPainter::Antialiasing, true);
    // the value fills the row as a layer's opacity does its
    QColor track = ink;
    track.setAlphaF(applies ? 0.05 : 0.025);
    painter->setPen(Qt::NoPen);
    painter->setBrush(track);
    const qreal radius = KisModernStyle::cornerRadius;
    painter->drawRoundedRect(QRectF(rect), radius, radius);
    if (applies && fraction > 0) {
        QColor fill = ink;
        fill.setAlphaF(0.16);
        painter->setClipRect(QRectF(rect.left(), rect.top(), rect.width() * fraction, rect.height()));
        painter->setBrush(fill);
        painter->drawRoundedRect(QRectF(rect), radius, radius);
    }
    painter->restore();
}

void KisCategorizedItemDelegate::paintTriangle(QPainter* painter, qint32 x, qint32 y, qint32 size, bool rotate) const
{
    QPolygonF triangle;
    triangle.push_back(QPointF(-0.2,-0.2));
    triangle.push_back(QPointF( 0.2,-0.2));
    triangle.push_back(QPointF( 0.0, 0.2));

    QTransform transform;
    transform.translate(x + size/2, y + size/2);
    transform.scale(size, size);

    if(rotate)
        transform.rotate(-90);

    QPalette palette = QApplication::palette();
    painter->setBrush(palette.buttonText());
    painter->drawPolygon(transform.map(triangle));
}

