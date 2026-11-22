/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef KISSTUDIOSOLO_H
#define KISSTUDIOSOLO_H

#include <kis_types.h>

class KisDocument;

/**
 * Several layers shown alone, as Krita's isolate mode shows one: the others are only left out of the picture
 * (KisProjectionLeaf::setTemporaryHiddenFromRendering), so nothing about them changes in the file or the history.
 */
namespace KisStudioSolo
{
/// Shows only \p layers (with the groups they are in and what they hold) in \p image; false if there is nothing to do
bool start(KisImageSP image, const KisNodeList &layers);
/// Shows everything again
void stop(KisImageSP image);
bool isActive(KisImageSP image);
/// Whether \p node is one of the layers shown alone, or inside one
bool shows(KisImageSP image, KisNodeSP node);
/// \p copy, made of \p original for saving: drawn again whole, as the copy of a soloed picture shows only the solo
void redrawForSaving(KisDocument *copy, KisImageSP original);
}

#endif // KISSTUDIOSOLO_H
