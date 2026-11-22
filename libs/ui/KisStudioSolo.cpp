/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "KisStudioSolo.h"

#include <QHash>
#include <QPointer>

#include <functional>

#include <KisDocument.h>
#include <kis_image.h>
#include <kis_layer_utils.h>
#include <kis_node.h>
#include <kis_projection_leaf.h>
#include <kis_simple_stroke_strategy.h>

namespace
{
/// The layers left out of each soloed image's picture
QHash<const KisImage *, QList<KisNodeWSP>> &hiddenOf()
{
    static QHash<const KisImage *, QList<KisNodeWSP>> hidden;
    return hidden;
}

/// What the solo shows, for each soloed image
QHash<const KisImage *, QList<KisNodeWSP>> &shownOf()
{
    static QHash<const KisImage *, QList<KisNodeWSP>> shown;
    return shown;
}

/// Changes what the picture leaves out where no update is running, then draws it again
class SoloStroke : public KisSimpleStrokeStrategy
{
public:
    SoloStroke(KisImageSP image, std::function<void()> change)
        : KisSimpleStrokeStrategy(QLatin1String("studio-solo"), kundo2_noi18n("studio-solo"))
        , m_image(image)
        , m_change(change)
    {
        enableJob(JOB_INIT, true, KisStrokeJobData::SEQUENTIAL, KisStrokeJobData::EXCLUSIVE);
        enableJob(JOB_FINISH, true, KisStrokeJobData::BARRIER);
        setClearsRedoOnStart(false);
        setRequestsOtherStrokesToEnd(false);
    }

    void initStrokeCallback() override
    {
        m_change();
    }

    void finishStrokeCallback() override
    {
        if (KisImageSP image = m_image.toStrongRef()) image->refreshGraphAsync();
    }

private:
    KisImageWSP m_image;
    std::function<void()> m_change;
};

void run(KisImageSP image, std::function<void()> change)
{
    KisStrokeId id = image->startStroke(new SoloStroke(image, change));
    image->endStroke(id);
}

bool within(KisNodeSP node, const KisNodeList &layers)
{
    for (KisNodeSP layer : layers) {
        for (KisNodeSP up = node; up; up = up->parent()) {
            if (up == layer) return true;
        }
    }
    return false;
}

bool holds(KisNodeSP node, const KisNodeList &layers)
{
    for (KisNodeSP layer : layers) {
        for (KisNodeSP up = layer->parent(); up; up = up->parent()) {
            if (up == node) return true;
        }
    }
    return false;
}
} // namespace

namespace KisStudioSolo
{
bool start(KisImageSP image, const KisNodeList &layers)
{
    if (!image || layers.isEmpty()) return false;
    stop(image);
    // left out: everything that is neither one of them, nor inside one, nor a group holding one
    QList<KisNodeWSP> hidden;
    KisLayerUtils::recursiveApplyNodes(image->root(), [&](KisNodeSP node) {
        if (node == image->root() || within(node, layers) || holds(node, layers)) return;
        // inside a node left out already
        if (node->parent() && node->parent() != image->root() && !within(node->parent(), layers) && !holds(node->parent(), layers)) return;
        hidden << KisNodeWSP(node);
    });
    if (hidden.isEmpty()) return false;
    QList<KisNodeWSP> shown;
    for (KisNodeSP layer : layers) shown << KisNodeWSP(layer);
    hiddenOf().insert(image.data(), hidden);
    shownOf().insert(image.data(), shown);
    run(image, [hidden]() {
        for (const KisNodeWSP &node : hidden) {
            if (KisNodeSP strong = node.toStrongRef()) strong->projectionLeaf()->setTemporaryHiddenFromRendering(true);
        }
    });
    return true;
}

void stop(KisImageSP image)
{
    if (!image || !hiddenOf().contains(image.data())) return;
    const QList<KisNodeWSP> hidden = hiddenOf().take(image.data());
    shownOf().remove(image.data());
    run(image, [hidden]() {
        for (const KisNodeWSP &node : hidden) {
            if (KisNodeSP strong = node.toStrongRef()) strong->projectionLeaf()->setTemporaryHiddenFromRendering(false);
        }
    });
}

bool isActive(KisImageSP image)
{
    return image && hiddenOf().contains(image.data());
}

bool shows(KisImageSP image, KisNodeSP node)
{
    if (!image || !node) return false;
    KisNodeList layers;
    for (const KisNodeWSP &layer : shownOf().value(image.data())) {
        if (KisNodeSP strong = layer.toStrongRef()) layers << strong;
    }
    return within(node, layers);
}

void redrawForSaving(KisDocument *copy, KisImageSP original)
{
    if (!copy || !copy->image() || !isActive(original)) return;
    // the copy's layers aren't left out (that isn't copied), but its picture was copied as shown
    copy->image()->refreshGraphAsync();
    copy->image()->waitForDone();
}
} // namespace KisStudioSolo
