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
 * Rings of your own: up to eight of anything Find knows (tools, commands,
 * effects, brushes, panels...) around the pointer, on a key.
 *
 * A ring is a command (its key is given on the Keys page, in Find, on a
 * card): tapped, it opens and stays for a click; held, it is open while the
 * key is, and letting go picks what the pointer went towards. What is on it
 * is changed on it, as on the brush ring: dragged to swap, a right click to
 * replace or remove, an empty slot's "+" to add; a right click in its middle
 * renames it or deletes it.
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

    /// Opens ring \p id at the pointer
    void open(const QString &id);

    static constexpr int slotCount = 8;

private:
    QAction *makeAction(const QString &id);

    struct Private;
    const std::unique_ptr<Private> d;
};

#endif // KISSTUDIORINGS_H
