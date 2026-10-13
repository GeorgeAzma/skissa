/*
 *  SPDX-FileCopyrightText: 2020 Agata Cacko <cacko.azh@gmail.com>>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisRecoverNamedAutosaveDialog.h"
#include "ui_KisRecoverNamedAutosaveDialog.h"

#include <QDate>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLocale>
#include <QThread>
#include <QToolButton>

#include <KisModernStyle.h>

#include "kis_debug.h"
#include "kis_image.h"
#include "kis_composite_progress_proxy.h"

#include <KisFileIconCreator.h>


KisRecoverNamedAutosaveDialog::KisRecoverNamedAutosaveDialog(QWidget *parent, QString mainFile, QString autosaveFile)
    : QDialog(parent),
      ui(new Ui::KisRecoverNamedAutosaveDialog)
{

    ui->setupUi(this);

    connect(ui->btOk, SIGNAL(clicked()), this, SLOT(slotOkRequested()));
    connect(ui->btCancel, SIGNAL(clicked()), this, SLOT(slotCancelRequested()));

    ui->lblExplanation->setText(i18nc("Recover an autosave for an already existing file: explanation in the recovery dialog",
                                      "An autosave for this file exists. How do you want to proceed?\n"
                                      "Warning: if you discard the autosave now, it will be removed."));

    KisFileIconCreator creator;
    QIcon mainFileIcon, autosaveFileIcon;

    QSize size = ui->rbOpenAutosave->iconSize();
    size = size*4;
    bool success = creator.createFileIcon(mainFile, mainFileIcon, devicePixelRatioF(), size);
    if (success) {
        ui->rbDiscardAutosave->setIcon(mainFileIcon);
        ui->rbDiscardAutosave->setIconSize(size);
    }
    success = creator.createFileIcon(autosaveFile, autosaveFileIcon, devicePixelRatioF(), size);
    if (success) {
        ui->rbOpenAutosave->setIcon(autosaveFileIcon);
        ui->rbOpenAutosave->setIconSize(size);
    }

    ui->rbOpenAutosave->setChecked(true); // it should be selected by default

    // the two versions side by side, as pictures with their times: a click opens that one, Esc keeps neither
    setWindowTitle(QFileInfo(mainFile).fileName());
    for (QWidget *old : {static_cast<QWidget *>(ui->lblExplanation), static_cast<QWidget *>(ui->rbOpenAutosave),
                         static_cast<QWidget *>(ui->rbDiscardAutosave), static_cast<QWidget *>(ui->btOk), static_cast<QWidget *>(ui->btCancel)}) {
        old->hide();
    }
    auto *choices = new QHBoxLayout();
    choices->setSpacing(KisModernStyle::space3);
    const QSize picture(240, 180);
    auto choice = [&](const QString &file, const QString &what, QRadioButton *radio) {
        auto *button = new QToolButton(this);
        button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        button->setAutoRaise(true);
        button->setIconSize(picture);
        QIcon icon;
        if (KisFileIconCreator().createFileIcon(file, icon, devicePixelRatioF(), picture)) button->setIcon(icon);
        const QDateTime saved = QFileInfo(file).lastModified();
        const QString when = saved.date() == QDate::currentDate() ? QLocale().toString(saved.time(), QLocale::ShortFormat)
                                                                 : QLocale().toString(saved, QLocale::ShortFormat);
        button->setText(what + QLatin1Char('\n') + when);
        connect(button, &QToolButton::clicked, this, [this, radio]() {
            radio->setChecked(true);
            slotOkRequested();
        });
        choices->addWidget(button);
        return button;
    };
    choice(autosaveFile, i18nc("recovering an image: its autosave", "Autosave"), ui->rbOpenAutosave)->setFocus();
    QToolButton *main = choice(mainFile, i18nc("recovering an image: the file as last saved", "Saved file"), ui->rbDiscardAutosave);
    main->setToolTip(i18nc("@info:tooltip", "The autosave is deleted"));
    static_cast<QBoxLayout *>(layout())->insertLayout(0, choices);
}

void KisRecoverNamedAutosaveDialog::reject()
{
    // a rejected dialog's result would be 0, which is OpenAutosave
    done(Cancel);
}

KisRecoverNamedAutosaveDialog::~KisRecoverNamedAutosaveDialog()
{
    delete ui;
}

void KisRecoverNamedAutosaveDialog::slotOkRequested()
{
    close();
    setResult(ui->rbOpenAutosave->isChecked() ? OpenAutosave : OpenMainFile);
}

void KisRecoverNamedAutosaveDialog::slotCancelRequested()
{
    close();
    setResult(Cancel);
}

