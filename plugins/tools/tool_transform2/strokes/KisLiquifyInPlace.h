/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KISLIQUIFYINPLACE_H
#define KISLIQUIFYINPLACE_H

#include <QHash>
#include <QMutex>
#include <QPair>
#include <QRect>
#include <QSharedPointer>

#include <kis_types.h>

class ToolTransformArgs;
class KisPaintDevice;

/**
 * Liquify in a transform stroke: keeps each layer's result, so a move of the
 * brush redraws, writes and recomposes only the area it changed instead of
 * undoing the last update and liquifying the whole layer again.
 */
class KisLiquifyInPlace
{
public:
    /// Whether every node's device holds the last update, so \p args can change it in place
    bool canUpdate(const KisNodeList &nodes, const ToolTransformArgs &args, int levelOfDetail) const;

    /**
     * Liquifies \p src into \p device as KisTransformUtils::transformAndMergeDevice
     * does. \p device holds what it had before the transform, or the last update
     * when \p inPlace. Returns the area that changed.
     */
    QRect transform(const ToolTransformArgs &args, KisPaintDeviceSP src, KisPaintDeviceSP device,
                    int levelOfDetail, bool inPlace);

    /// The devices at this level of detail went back to what they had before the transform
    void undone(int levelOfDetail);
    void undoneAll();

private:
    struct State;
    using Key = QPair<KisPaintDevice *, int>;
    static Key keyOf(KisPaintDevice *device, int levelOfDetail);

    mutable QMutex m_mutex;
    QHash<Key, QSharedPointer<State>> m_states;
};

#endif
