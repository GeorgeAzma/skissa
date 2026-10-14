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
    /// The stroke's step in the history, once it is there
    const KUndo2Command *step {nullptr};
    /// Where the stack's index will be once the stroke's step is pushed
    int expectedIndex {-1};
    QTimer timer;
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
    d->timer.setSingleShot(true);
    // a burst of changes (a drag) paints once it settles a little
    d->timer.setInterval(120);
    connect(&d->timer, &QTimer::timeout, this, &KisLastStrokeReplay::replay);
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

void KisLastStrokeReplay::strokeEnded(KisToolFreehandHelper *helper, KUndo2Stack *stack, const QString &preset)
{
    d->helper = helper;
    d->preset = preset;
    d->step = nullptr;
    if (stack != d->stack) {
        disconnect(d->stackConnection);
        d->stack = stack;
        d->stackConnection = connect(stack, &KUndo2Stack::indexChanged, this, [this](int index) {
            if (!d->step && d->stack && index == d->expectedIndex && index > 0) {
                d->step = d->stack->command(index - 1);
            }
        });
    }
    // the stroke's step arrives once the image has painted it
    d->expectedIndex = stack ? stack->index() + 1 : -1;
}

void KisLastStrokeReplay::brushChanged(const QString &preset)
{
    if (!isEnabled() || preset != d->preset || !d->helper) return;
    d->retries = 0;
    d->timer.start();
}

void KisLastStrokeReplay::replay()
{
    if (!isEnabled() || !d->helper || !d->stack || !d->helper->hasLastStroke()) return;
    if (!d->step) {
        // the stroke's step isn't in the history yet: it is still being painted
        if (d->expectedIndex > 0 && ++d->retries < 40) QTimer::singleShot(50, this, &KisLastStrokeReplay::replay);
        return;
    }
    const int index = d->stack->index();
    // only while the stroke is still the newest step, with nothing undone after it
    if (index < 1 || index != d->stack->count() || d->stack->command(index - 1) != d->step) {
        d->step = nullptr;
        return;
    }
    KisImageSP image = d->helper->lastStrokeImage();
    if (!image) return;
    if (!image->isIdle()) {
        // still painting the previous one
        if (++d->retries < 40) QTimer::singleShot(50, this, &KisLastStrokeReplay::replay);
        return;
    }
    d->step = nullptr;
    d->expectedIndex = index;
    d->stack->setIndex(index - 1);
    d->helper->replayLastStroke();
}
