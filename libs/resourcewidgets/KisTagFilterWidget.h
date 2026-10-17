/*
 *    SPDX-FileCopyrightText: 2011 Srikanth Tiyyagura <srikanth.tulasiram@gmail.com>
 *    SPDX-FileCopyrightText: 2011 José Luis Vergara <pentalis@gmail.com>
 *    SPDX-FileCopyrightText: 2013 Sascha Suelzer <s.suelzer@gmail.com>
 *    SPDX-FileCopyrightText: 2019 Boudewijn Rempt <boud@valdyas.org>
 *
 *    SPDX-License-Identifier: LGPL-2.0-or-later
 */

#ifndef KISTAGFILTERWIDGET_H
#define KISTAGFILTERWIDGET_H

#include <QWidget>
#include <KisTagModel.h>

class KisTagFilterWidget : public QWidget
{
    Q_OBJECT

public:
    explicit KisTagFilterWidget(KisTagModel* model, QWidget* parent);
    ~KisTagFilterWidget() override;

public Q_SLOTS:
    void clear();

Q_SIGNALS:
    void filterTextChanged(const QString &filterText);
protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
private Q_SLOTS:
    void onTextChanged(const QString &lineEditText);
private:
    /// The field while it is used; otherwise only the magnifier
    void setOpen(bool open);

    class Private;
    Private* const d;
};

#endif // KISTAGFILTERWIDGET_H
