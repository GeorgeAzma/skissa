/*
 *    SPDX-FileCopyrightText: 2011 Srikanth Tiyyagura <srikanth.tulasiram@gmail.com>
 *    SPDX-FileCopyrightText: 2011 José Luis Vergara <pentalis@gmail.com>
 *    SPDX-FileCopyrightText: 2013 Sascha Suelzer <s.suelzer@gmail.com>
 *
 *    SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "KisTagFilterWidget.h"

#include <KisModernStyle.h>
#include <kis_icon_utils.h>

#include <QAbstractItemView>
#include <QAction>
#include <QGridLayout>
#include <QKeyEvent>
#include <QLineEdit>
#include <QCompleter>
#include <QToolButton>

#include <klocalizedstring.h>

class KisTagFilterWidget::Private
{
public:
    QLineEdit *tagSearchLineEdit;
    QToolButton *searchButton;
    QCompleter *completer;
};

KisTagFilterWidget::KisTagFilterWidget(KisTagModel* model, QWidget* parent)
    : QWidget(parent)
    , d(new Private())
{
    QGridLayout* filterBarLayout = new QGridLayout(this);

    d->tagSearchLineEdit = new QLineEdit(this);
    d->tagSearchLineEdit->setClearButtonEnabled(true);
    d->tagSearchLineEdit->addAction(KisIconUtils::loadIcon(QStringLiteral("studio_find")), QLineEdit::LeadingPosition);
    d->tagSearchLineEdit->setFrame(false);
    d->tagSearchLineEdit->setProperty(KisModernStyle::quietFieldProperty, true);
    d->tagSearchLineEdit->setTextMargins(2, 3, 2, 3);
    d->tagSearchLineEdit->setPlaceholderText(i18nc("@info:placeholder", "Search"));
    d->tagSearchLineEdit->setEnabled(true);

    d->completer = new QCompleter(model, this);
    d->completer->setCompletionRole(Qt::DisplayRole);
    d->completer->setCaseSensitivity(Qt::CaseInsensitive);
    d->tagSearchLineEdit->setCompleter(d->completer);

    // only the magnifier, at the end of the line, until it is used
    d->searchButton = new QToolButton(this);
    d->searchButton->setAutoRaise(true);
    d->searchButton->setFocusPolicy(Qt::NoFocus);
    d->searchButton->setIcon(KisIconUtils::loadIcon(QStringLiteral("studio_find")));
    d->searchButton->setToolTip(i18nc("@info:tooltip", "Search"));
    d->searchButton->setFixedHeight(d->tagSearchLineEdit->sizeHint().height());
    connect(d->searchButton, &QToolButton::clicked, this, [this]() {
        setOpen(true);
        d->tagSearchLineEdit->setFocus(Qt::MouseFocusReason);
    });
    d->tagSearchLineEdit->installEventFilter(this);

    filterBarLayout->setContentsMargins(0, 0, 0, 0);
    filterBarLayout->setColumnStretch(0, 1);
    filterBarLayout->addWidget(d->tagSearchLineEdit, 0, 0);
    filterBarLayout->addWidget(d->searchButton, 0, 0, Qt::AlignRight | Qt::AlignVCenter);
    setOpen(false);

    connect(d->tagSearchLineEdit, SIGNAL(textChanged(QString)),
            this, SLOT(onTextChanged(QString)));
}

KisTagFilterWidget::~KisTagFilterWidget()
{
    delete d;
}

void KisTagFilterWidget::clear()
{
    d->tagSearchLineEdit->clear();
    if (!d->tagSearchLineEdit->hasFocus()) setOpen(false);
}

void KisTagFilterWidget::setOpen(bool open)
{
    d->tagSearchLineEdit->setVisible(open);
    d->searchButton->setVisible(!open);
}

bool KisTagFilterWidget::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == d->tagSearchLineEdit) {
        if (event->type() == QEvent::KeyPress && static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape) {
            d->tagSearchLineEdit->clear();
            d->tagSearchLineEdit->clearFocus();
            setOpen(false);
            return true;
        }
        // left with nothing typed: back to the magnifier (not while picking from its completions)
        if (event->type() == QEvent::FocusOut && d->tagSearchLineEdit->text().isEmpty()
            && !(d->completer->popup() && d->completer->popup()->isVisible())) {
            setOpen(false);
        }
    }
    return QWidget::eventFilter(watched, event);
}

void KisTagFilterWidget::onTextChanged(const QString& lineEditText)
{
    Q_EMIT filterTextChanged(lineEditText);
}
