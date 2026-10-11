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
 * The Keys page of the settings: every shortcut, and the keys held on the
 * canvas, in one place.
 *
 * On top, the keyboard: what each key does, a layer for each set of
 * modifiers (held, they show theirs), free keys empty, keys two commands
 * want marked, the keys the other hand reaches while drawing tinted. A
 * click on a key shows what has it; a command dragged onto a key gets it.
 *
 * Under it, the commands, grouped by what they are for, found by their
 * names, the words other applications use for them, or a key pressed. Each
 * shortcut is a key cap: a click records another, a right click removes it.
 * The keys held on the canvas (pan, rotate, zoom, the brush size...) are
 * among them, kept apart from those pressed once.
 *
 * "Start from" picks a set to begin with (Krita's, Photoshop's...), keys
 * held on the canvas too. Nothing changes until OK.
 */
class KisKeysPage : public QWidget
{
    Q_OBJECT
public:
    explicit KisKeysPage(QWidget *parent = nullptr);
    ~KisKeysPage() override;

    /// What was changed here, made so (OK)
    void apply();
    /// Krita's own keys, all of them (applied with OK)
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
