/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "KisStudioFontPicker.h"

#include <QFontDatabase>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListView>
#include <QPainter>
#include <QSet>
#include <QSortFilterProxyModel>
#include <QStringListModel>
#include <QStyledItemDelegate>
#include <QVBoxLayout>

#include <klocalizedstring.h>

#include <KisModernStyle.h>
#include <KisResourceModel.h>
#include <KisResourceTypes.h>
#include <kis_icon_utils.h>

#include "widgets/KisStudioPicker.h"

namespace
{

/// Each name written in its own font, a little larger than the interface's
class FaceDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &) const override
    {
        return QSize(option.rect.width(), qRound(QFontMetricsF(face(option.font, QString())).height() * 1.6));
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        const QString family = index.data().toString();
        const QPalette &palette = option.palette;
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        const QRectF row = QRectF(option.rect).adjusted(KisModernStyle::hairline, 1, -KisModernStyle::hairline, -1);
        if (option.state & QStyle::State_Selected) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(KisModernStyle::selectedRow(palette));
            painter->drawPath(KisModernStyle::squircle(row, KisModernStyle::cornerRadius));
        } else if (option.state & QStyle::State_MouseOver) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(KisModernStyle::mix(palette.color(QPalette::Window), palette.color(QPalette::Text), 0.08));
            painter->drawPath(KisModernStyle::squircle(row, KisModernStyle::cornerRadius));
        }
        painter->setPen((option.state & QStyle::State_Selected) ? KisModernStyle::selectedRowInk(palette) : palette.color(QPalette::Text));
        const QFont font = face(option.font, family);
        painter->setFont(font);
        const QRectF text = row.adjusted(KisModernStyle::space2, 0, -KisModernStyle::space2, 0);
        painter->drawText(text, Qt::AlignVCenter | Qt::AlignLeft, QFontMetricsF(font).elidedText(family, Qt::ElideRight, text.width()));
        painter->restore();
    }

    static QFont face(const QFont &base, const QString &family)
    {
        QFont font = base;
        if (!family.isEmpty()) font.setFamily(family);
        font.setPointSizeF(base.pointSizeF() * 1.3);
        return font;
    }
};

/// Matches by Find's scores: start of a word first, then letters in order
class Finder : public QSortFilterProxyModel
{
public:
    using QSortFilterProxyModel::QSortFilterProxyModel;

    void setQuery(const QString &query)
    {
        m_query = query.trimmed();
        invalidate();
        sort(m_query.isEmpty() ? -1 : 0);
    }

protected:
    bool filterAcceptsRow(int row, const QModelIndex &parent) const override
    {
        if (m_query.isEmpty()) return true;
        return KisStudioPicker::matchScore(m_query, sourceModel()->index(row, 0, parent).data().toString()) > 0;
    }

    bool lessThan(const QModelIndex &left, const QModelIndex &right) const override
    {
        const int a = KisStudioPicker::matchScore(m_query, left.data().toString());
        const int b = KisStudioPicker::matchScore(m_query, right.data().toString());
        if (a != b) return a > b;
        return left.row() < right.row();
    }

private:
    QString m_query;
};

} // namespace

struct KisStudioFontPicker::Private {
    QLineEdit *search {nullptr};
    QListView *list {nullptr};
    QStringListModel *model {nullptr};
    Finder *finder {nullptr};
    std::function<void(const QString &)> picked;
};

QStringList KisStudioFontPicker::textFamilies()
{
    QStringList families;
    QSet<QString> seen;
    KisResourceModel model(ResourceType::FontFamilies);
    for (int row = 0; row < model.rowCount(); row++) {
        const QString name = model.data(model.index(row, 0), Qt::UserRole + KisAbstractResourceModel::Name).toString();
        if (!name.isEmpty() && !seen.contains(name)) {
            seen.insert(name);
            families << name;
        }
    }
    if (families.isEmpty()) families = QFontDatabase().families();
    families.sort(Qt::CaseInsensitive);
    return families;
}

KisStudioFontPicker::KisStudioFontPicker(const QStringList &families, const QString &current,
                                         std::function<void(const QString &)> picked, QWidget *parent)
    : KisStudioPopup(parent)
    , d(new Private)
{
    setAttribute(Qt::WA_DeleteOnClose);
    d->picked = picked;

    auto *layout = new QVBoxLayout(this);
    fitLayout(layout, KisModernStyle::space2);
    layout->setSpacing(KisModernStyle::space2);

    d->search = new QLineEdit(this);
    d->search->addAction(KisIconUtils::loadIcon(QStringLiteral("studio_find")), QLineEdit::LeadingPosition);
    d->search->setClearButtonEnabled(true);
    d->search->installEventFilter(this);
    layout->addWidget(d->search);

    d->model = new QStringListModel(families, this);
    d->finder = new Finder(this);
    d->finder->setSourceModel(d->model);
    d->list = new QListView(this);
    d->list->setModel(d->finder);
    d->list->setItemDelegate(new FaceDelegate(d->list));
    d->list->setUniformItemSizes(true);
    d->list->setFrameShape(QFrame::NoFrame);
    d->list->setMouseTracking(true);
    d->list->viewport()->setAttribute(Qt::WA_Hover);
    d->list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    d->list->setFocusPolicy(Qt::NoFocus);
    d->list->setAutoFillBackground(false);
    d->list->viewport()->setAutoFillBackground(false);
    layout->addWidget(d->list, 1);
    connect(d->list, &QListView::clicked, this, &KisStudioFontPicker::pick);

    connect(d->search, &QLineEdit::textChanged, this, [this](const QString &text) {
        d->finder->setQuery(text);
        if (d->finder->rowCount() > 0) d->list->setCurrentIndex(d->finder->index(0, 0));
    });

    const int row = families.indexOf(current);
    if (row >= 0) {
        const QModelIndex index = d->finder->mapFromSource(d->model->index(row, 0));
        d->list->setCurrentIndex(index);
        d->list->scrollTo(index, QAbstractItemView::PositionAtCenter);
    }
    // about ten fonts at a time, wide enough for long names
    const int rowHeight = qRound(QFontMetricsF(FaceDelegate::face(font(), QString())).height() * 1.6);
    d->list->setMinimumSize(qMax(280, 28 * fontMetrics().averageCharWidth()), 10 * rowHeight);
    d->search->setFocus();
}

KisStudioFontPicker::~KisStudioFontPicker()
{
}

void KisStudioFontPicker::pick(const QModelIndex &index)
{
    if (!index.isValid()) return;
    const QString family = index.data().toString();
    const std::function<void(const QString &)> picked = d->picked;
    close();
    if (picked) picked(family);
}

bool KisStudioFontPicker::eventFilter(QObject *watched, QEvent *event)
{
    // the list is moved through and picked from while the keys are in the search field
    if (watched == d->search && event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        const int rows = d->finder->rowCount();
        const int row = d->list->currentIndex().isValid() ? d->list->currentIndex().row() : -1;
        if (key->key() == Qt::Key_Down || key->key() == Qt::Key_Up) {
            if (rows > 0) {
                const int next = qBound(0, row + (key->key() == Qt::Key_Down ? 1 : -1), rows - 1);
                d->list->setCurrentIndex(d->finder->index(next, 0));
            }
            return true;
        }
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
            pick(d->list->currentIndex().isValid() ? d->list->currentIndex() : d->finder->index(0, 0));
            return true;
        }
    }
    return KisStudioPopup::eventFilter(watched, event);
}
