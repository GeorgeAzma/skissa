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
 * The active tool's most changed options in the top bar: transform modes,
 * selection modes, gradient shapes, what fill fills and with what. The
 * buttons drive the tool's own option widgets, so both always agree.
 */
class KisStudioToolStrip : public QWidget
{
    Q_OBJECT
public:
    explicit KisStudioToolStrip(QWidget *parent = nullptr);

    /// Rebuilds the strip from the option widgets the active tool just provided
    void setOptionWidgets(const QList<QPointer<QWidget>> &widgets);
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
