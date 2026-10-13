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
 * User-defined radial menus bound to keys, each holding up to eight items
 * from Find. Tapping the key leaves the ring open for a click; holding it
 * selects whatever the pointer moved towards on release. Slots can be dragged
 * to swap, right clicked to replace or remove, and added with an empty slot's
 * "+". Right clicking the middle renames or deletes the ring.
 */
class KRITAUI_EXPORT KisStudioRings : public QObject
{
    Q_OBJECT
public:
    explicit KisStudioRings(KisMainWindow *mainWindow);
    ~KisStudioRings() override;

    /// Creates an empty ring and returns its action, ready to be given a key
    QAction *createRing();

    /// The Find keys on ring \p id, clockwise from the top
    QStringList items(const QString &id) const;
    void setItems(const QString &id, const QStringList &items);
    QString name(const QString &id) const;
    void rename(const QString &id, const QString &name);
    void remove(const QString &id);

    /// Opens ring \p id at the pointer, or closes it if it is already open
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
