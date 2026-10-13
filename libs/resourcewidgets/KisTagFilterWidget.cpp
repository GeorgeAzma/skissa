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

#include <QAction>
#include <QGridLayout>
#include <QLineEdit>
#include <QCompleter>

#include <klocalizedstring.h>

class KisTagFilterWidget::Private
{
public:
    QLineEdit *tagSearchLineEdit;
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

    filterBarLayout->setContentsMargins(0, 0, 0, 0);
    filterBarLayout->setColumnStretch(0, 1);
    filterBarLayout->addWidget(d->tagSearchLineEdit, 0, 0);

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
}

void KisTagFilterWidget::onTextChanged(const QString& lineEditText)
{
    Q_EMIT filterTextChanged(lineEditText);
}
