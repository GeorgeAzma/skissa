/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIORINGS_H
#define KISSTUDIORINGS_H

#include <QObject>
#include <QStringList>

#include <memory>

#include "kritaui_export.h"

class KisMainWindow;
class QAction;

/**
 * Rings of your own: up to eight of anything Find knows around the pointer,
 * on a key. Tapped, a ring stays open for a click; held, letting go picks
 * what the pointer went towards. Slots are dragged to swap, right clicked to
 * replace or remove, added with an empty slot's "+"; a right click in the
 * middle renames or deletes the ring.
 */
class KRITAUI_EXPORT KisStudioRings : public QObject
{
    Q_OBJECT
public:
    explicit KisStudioRings(KisMainWindow *mainWindow);
    ~KisStudioRings() override;

    /// A new, empty ring: its command, to be given a key
    QAction *createRing();

    /// What is on ring \p id: Find's keys, in order around it from the top
    QStringList items(const QString &id) const;
    void setItems(const QString &id, const QStringList &items);
    QString name(const QString &id) const;
    void rename(const QString &id, const QString &name);
    void remove(const QString &id);

    /// Opens ring \p id at the pointer (or closes it if it is open)
    void open(const QString &id);

    static constexpr int slotCount = 8;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    QAction *makeAction(const QString &id);
    void openRing(const QString &id, int heldKey);

    struct Private;
    const std::unique_ptr<Private> d;
};

#endif // KISSTUDIORINGS_H
