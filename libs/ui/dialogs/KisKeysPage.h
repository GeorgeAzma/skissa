/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISKEYSPAGE_H
#define KISKEYSPAGE_H

#include <QWidget>

#include <memory>

/**
 * The Keys page of the settings: every shortcut and the keys held on the
 * canvas, on a drawn keyboard and in a list. Nothing changes until apply().
 */
class KisKeysPage : public QWidget
{
    Q_OBJECT
public:
    explicit KisKeysPage(QWidget *parent = nullptr);
    ~KisKeysPage() override;

    void apply();
    /// Krita's own keys, all of them (applied with apply())
    void setDefaults();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    struct Private;
    const std::unique_ptr<Private> d;
};

#endif // KISKEYSPAGE_H
