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
class QSpinBox;
class KisStudioSelectionAdjust;

/**
 * Shows the active tool's most used options in the top bar: transform modes,
 * selection modes with Grow and Soft for the selection there is, what Move
 * moves, the crop's shape and what it crops, gradient shapes, what the fill
 * tool fills and with what and its threshold, and a shape's fill and outline.
 * The buttons drive the tool's own option widgets, so the two always agree.
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
    /// Adds a button that clicks \p source and follows its checked state
    void mirror(QAbstractButton *source, const QIcon &icon = QIcon());
    void mirrorCombo(QComboBox *combo, const QVector<int> &indexes, const QVector<QIcon> &icons);

    /// Grow and Soft, on the selection there is (selection tools)
    void addSelectionAdjust();
    /// One button showing \p combo's choice as its picture, the others in its menu
    void mirrorComboAsMenu(QComboBox *combo, const QVector<QIcon> &icons, const QString &toolTip);
    /// A scrubber for \p spin, both following each other
    void mirrorSpin(QSpinBox *spin, int kind, const QString &toolTip, int defaultValue);
    /// The crop tool's shape: free or a ratio (the image's own too), held by its ratio lock
    void addCropAspects(QWidget *options);

    QHBoxLayout *m_layout {nullptr};
    KisStudioSelectionAdjust *m_adjust {nullptr};
    bool m_hasOptions {false};
};

#endif // KISSTUDIOTOOLSTRIP_H
