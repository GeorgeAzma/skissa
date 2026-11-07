/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef KISSTUDIOFONTPICKER_H
#define KISSTUDIOFONTPICKER_H

#include "widgets/KisStudioPopup.h"

#include <QScopedPointer>

#include <functional>

#include "kritaui_export.h"

/**
 * Fonts as their names written in themselves: what a font looks like is why it is picked. Typing finds one;
 * a click or Enter picks it.
 */
class KRITAUI_EXPORT KisStudioFontPicker : public KisStudioPopup
{
    Q_OBJECT
public:
    KisStudioFontPicker(const QStringList &families, const QString &current, std::function<void(const QString &)> picked,
                        QWidget *parent);
    ~KisStudioFontPicker() override;

    /// The font families Krita's text knows, sorted
    static QStringList textFamilies();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void pick(const QModelIndex &index);

    struct Private;
    QScopedPointer<Private> d;
};

#endif
