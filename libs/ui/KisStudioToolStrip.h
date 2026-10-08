/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIOTOOLSTRIP_H
#define KISSTUDIOTOOLSTRIP_H

#include <QIcon>
#include <QPointer>
#include <QWidget>

class QAbstractButton;
class QHBoxLayout;
class QComboBox;
class QToolButton;

/**
 * The few options of the active tool that change from one use to the next,
 * in the top bar next to the tool chip: transform modes (warp and liquify
 * included, which were two clicks deep), selection add/subtract/intersect,
 * gradient shapes, what fill fills and with what.
 *
 * The buttons drive the tool's own option widgets instead of the tool, so
 * both always agree and the tool keeps its settings handling; the full
 * options stay one click away under the chip.
 */
class KisStudioToolStrip : public QWidget
{
    Q_OBJECT
public:
    explicit KisStudioToolStrip(QWidget *parent = nullptr);

    /// Rebuilds the strip from the option widgets the active tool just provided
    void setOptionWidgets(const QList<QPointer<QWidget>> &widgets);
    /// Whether the active tool has anything for the strip; a toolbar decides visibility by its action
    bool hasOptions() const { return m_hasOptions; }

private:
    void clear();
    void addGap();
    QToolButton *addButton(const QIcon &icon, const QString &toolTip);
    /// A button of ours that presses \p source and follows its checked state
    void mirror(QAbstractButton *source, const QIcon &icon = QIcon());
    void mirrorCombo(QComboBox *combo, const QVector<int> &indexes, const QVector<QIcon> &icons);

    QHBoxLayout *m_layout {nullptr};
    bool m_hasOptions {false};
};

#endif // KISSTUDIOTOOLSTRIP_H
