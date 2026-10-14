/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisLastStrokeReplay.h"

#include <QPointer>
#include <QTimer>

#include <kundo2stack.h>
#include <kis_image.h>

#include "kis_config.h"
#include "tool/kis_tool_freehand_helper.h"

namespace {
const QString enabledKey = QStringLiteral("StudioLastStrokeFollowsBrush");
}

struct KisLastStrokeReplay::Private
{
    QPointer<KisToolFreehandHelper> helper;
    QPointer<KUndo2Stack> stack;
    QMetaObject::Connection stackConnection;
    QString preset;
    /// The brush's settings the stroke was last painted with (or will be)
    QString settings;
    /// The stroke's step in the history, once it is there
    const KUndo2Command *step {nullptr};
    /// Where the stack's index will be once the stroke's step is pushed
    int expectedIndex {-1};
    /// A repaint was started and its step hasn't arrived yet
    bool painting {false};
    /// It was asked to stop, the brush having changed again
    bool cancelling {false};
    /// The image was seen idle once since: a step being pushed has had its turn by the next look
    bool idleSeen {false};
    /// The brush changed since the stroke was last painted
    bool dirty {false};
    int retries {0};
};

KisLastStrokeReplay *KisLastStrokeReplay::instance()
{
    static KisLastStrokeReplay *replay = new KisLastStrokeReplay();
    return replay;
}

KisLastStrokeReplay::KisLastStrokeReplay()
    : d(new Private)
{
}

KisLastStrokeReplay::~KisLastStrokeReplay()
{
}

bool KisLastStrokeReplay::isEnabled()
{
    return KisConfig(true).readEntry(enabledKey, true);
}

void KisLastStrokeReplay::setEnabled(bool on)
{
    KisConfig(false).writeEntry(enabledKey, on);
}

void KisLastStrokeReplay::strokeEnded(KisToolFreehandHelper *helper, KUndo2Stack *stack, const QString &preset, const QString &settings)
{
    d->helper = helper;
    d->preset = preset;
    // the smoothing too: a change of it paints the stroke again as well
    d->settings = settings + (helper ? helper->smoothingKey() : QString());
    d->step = nullptr;
    d->painting = false;
    d->cancelling = false;
    d->dirty = false;
    if (stack != d->stack) {
        disconnect(d->stackConnection);
        d->stack = stack;
        d->stackConnection = connect(stack, &KUndo2Stack::indexChanged, this, [this](int index) {
            if (d->step || !d->stack || index != d->expectedIndex || index < 1) return;
            d->step = d->stack->command(index - 1);
            d->painting = false;
            d->cancelling = false;
            // changed again meanwhile: on to the newest look
            if (d->dirty) replay();
        });
    }
    // the stroke's step arrives once the image has painted it
    d->expectedIndex = stack ? stack->index() + 1 : -1;
}

void KisLastStrokeReplay::brushChanged(const QString &preset, const QString &settings)
{
    // the brush read back as it was (the editor following the preset) changes nothing
    if (!isEnabled() || preset != d->preset || !d->helper) return;
    const QString look = settings + d->helper->smoothingKey();
    if (look == d->settings) return;
    d->settings = look;
    d->dirty = true;
    d->retries = 0;
    // after the change has reached the brush the stroke is painted with
    QTimer::singleShot(0, this, &KisLastStrokeReplay::replay);
}

void KisLastStrokeReplay::replay()
{
    if (!d->dirty || !isEnabled() || !d->helper || !d->stack || !d->helper->hasLastStroke()) return;
    KisImageSP image = d->helper->lastStrokeImage();
    if (!image) return;
    const int index = d->stack->index();

    if (d->painting) {
        // a repaint of an older look is still painting: stop it, it is already out of date
        if (!d->cancelling) {
            d->cancelling = true;
            d->idleSeen = false;
            image->requestStrokeCancellation();
        }
        // idle twice in a row: a stroke that finished instead has pushed its step by then
        if (!image->isIdle() || !d->idleSeen) {
            d->idleSeen = image->isIdle();
            if (++d->retries < 600) QTimer::singleShot(16, this, &KisLastStrokeReplay::replay);
            return;
        }
        // finished after all: its step arrived, and painting goes on from there
        if (index != d->expectedIndex - 1) return;
        // stopped before its step was pushed: the stroke stays taken back, ready to be painted again
        d->cancelling = false;
        d->dirty = false;
        d->helper->replayLastStroke();
        return;
    }

    if (!d->step) {
        // the stroke's own step isn't in the history yet: it is still being painted
        if (d->expectedIndex > 0 && ++d->retries < 600) QTimer::singleShot(16, this, &KisLastStrokeReplay::replay);
        return;
    }
    // only while the stroke is still the newest step, with nothing undone after it
    if (index < 1 || index != d->stack->count() || d->stack->command(index - 1) != d->step) {
        d->step = nullptr;
        d->expectedIndex = -1;
        return;
    }
    if (!image->isIdle()) {
        if (++d->retries < 600) QTimer::singleShot(16, this, &KisLastStrokeReplay::replay);
        return;
    }
    d->step = nullptr;
    d->dirty = false;
    d->expectedIndex = index;
    d->stack->setIndex(index - 1);
    d->helper->replayLastStroke();
    d->painting = true;
}
