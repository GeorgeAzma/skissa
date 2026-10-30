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

#include "KisMainWindow.h"
#include "KisPart.h"
#include "KisView.h"
#include "KisViewManager.h"
#include "kis_canvas_resource_provider.h"
#include "kis_config.h"
#include "tool/kis_tool_freehand_helper.h"

namespace {
const QString enabledKey = QStringLiteral("StudioLastStrokeFollowsBrush");
}

namespace
{
/// What the canvas adds to the brush as it paints, which the replay takes as it is now: the eraser, alpha lock and mirroring
QString paintMode()
{
    KisMainWindow *window = KisPart::instance()->currentMainwindow();
    KisViewManager *view = window ? window->viewManager() : nullptr;
    KoCanvasResourceProvider *resources = view && view->canvasResourceProvider() ? view->canvasResourceProvider()->resourceManager() : nullptr;
    if (!resources) return QString();
    return QStringList({resources->resource(KoCanvasResource::CurrentEffectiveCompositeOp).toString(),
                        QString::number(resources->resource(KoCanvasResource::GlobalAlphaLock).toBool()),
                        QString::number(resources->resource(KoCanvasResource::MirrorHorizontal).toBool()),
                        QString::number(resources->resource(KoCanvasResource::MirrorVertical).toBool())})
        .join(QLatin1Char(';'));
}
} // namespace

struct KisLastStrokeReplay::Private
{
    QPointer<KisToolFreehandHelper> helper;
    QPointer<KUndo2Stack> stack;
    QMetaObject::Connection stackConnection;
    QString preset;
    /// What the canvas added to the brush as the stroke was painted (see paintMode())
    QString mode;
    /// The brush's settings the stroke was last painted with (or will be)
    QString settings;
    /// The stroke's step in the history, once it is there
    const KUndo2Command *step {nullptr};
    /// The name the stroke's step has: another action's step is never taken for it
    QString stepText;
    /// The newest step while the stroke's is awaited: the stroke's is the next one pushed after it
    const KUndo2Command *before {nullptr};
    /// Where the stack's index will be once the stroke's step is pushed
    int expectedIndex {-1};
    /// A repaint was started and its step hasn't arrived yet
    bool painting {false};
    /// The image was seen idle once since: a step being pushed has had its turn by the next look
    bool idleSeen {false};
    /// The brush changed since the stroke was last painted
    bool dirty {false};
    int retries {0};
    /// The next look, one at a time however often the brush changes while it waits
    QTimer retry;
};

KisLastStrokeReplay *KisLastStrokeReplay::instance()
{
    static KisLastStrokeReplay *replay = new KisLastStrokeReplay();
    return replay;
}

KisLastStrokeReplay::KisLastStrokeReplay()
    : d(new Private)
{
    d->retry.setSingleShot(true);
    connect(&d->retry, &QTimer::timeout, this, &KisLastStrokeReplay::replay);
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
    d->mode = paintMode();
    // the smoothing too: a change of it paints the stroke again as well
    d->settings = settings + (helper ? helper->smoothingKey() : QString());
    d->stepText = helper ? helper->transactionText() : QString();
    d->step = nullptr;
    d->painting = false;
    d->dirty = false;
    if (stack != d->stack) {
        disconnect(d->stackConnection);
        d->stack = stack;
        d->stackConnection = connect(stack, &KUndo2Stack::indexChanged, this, [this](int index) {
            if (!d->stack) return;
            const KUndo2Command *top = index > 0 ? d->stack->command(index - 1) : nullptr;
            if (d->step) {
                // taken back or built on: the step may be deleted, and another could take its address
                if (top != d->step) {
                    d->step = nullptr;
                    d->expectedIndex = -1;
                }
                return;
            }
            // only a step just pushed: nothing to redo after it, and not the one that was newest
            if (d->expectedIndex < 0 || !top || index != d->stack->count() || top == d->before) return;
            if (top->text().toString() != d->stepText) {
                // another action came first: the stroke was taken back before its step arrived
                d->expectedIndex = -1;
                return;
            }
            d->step = top;
            // a full history drops its oldest step instead of growing
            d->expectedIndex = index;
            d->painting = false;
            // changed again meanwhile: on to the newest look, once out of the history's own call
            if (d->dirty) {
                d->retries = 0;
                d->retry.start(0);
            }
        });
    }
    // the stroke's step arrives once the image has painted it
    d->before = stack && stack->index() > 0 ? stack->command(stack->index() - 1) : nullptr;
    d->expectedIndex = stack ? stack->index() + 1 : -1;
}

bool KisLastStrokeReplay::follows(const QString &preset) const
{
    // painted again with the eraser switched on since, a stroke would erase what is under it
    return d->helper && preset == d->preset && isEnabled() && paintMode() == d->mode;
}

void KisLastStrokeReplay::brushChanged(const QString &preset, const QString &settings)
{
    // the brush read back as it was (the editor following the preset) changes nothing
    if (!follows(preset)) return;
    const QString look = settings + d->helper->smoothingKey();
    if (look == d->settings) return;
    // only the smoothing changed: it is dragged, and a stabilized stroke is slow to paint again, so once the drag pauses
    const bool smoothingOnly = d->settings.startsWith(settings);
    d->settings = look;
    d->dirty = true;
    d->retries = 0;
    // after the change has reached the brush the stroke is painted with
    d->retry.start(smoothingOnly ? 150 : 0);
}

void KisLastStrokeReplay::replay()
{
    if (!d->dirty || !isEnabled() || !d->helper || !d->stack || !d->helper->hasLastStroke()) return;
    KisImageSP image = d->helper->lastStrokeImage();
    if (!image) return;
    // only on the picture in view: another document's stroke is left as it is
    KisMainWindow *window = KisPart::instance()->currentMainwindow();
    if (!window || !window->activeView() || window->activeView()->image().data() != image.data()) return;
    const int index = d->stack->index();

    if (d->painting) {
        // a repaint of an older look is still painting: it finishes, and the newest look is painted from its step.
        // Stopped halfway, the stroke would be in no look at all: the step it replaces is gone once it starts.
        if (!image->isIdle() || !d->idleSeen) {
            d->idleSeen = image->isIdle();
            if (++d->retries < 600) d->retry.start(16);
            return;
        }
        // idle twice in a row and its step never arrived: it was taken back (an undo), and stays so
        d->painting = false;
        d->dirty = false;
        d->expectedIndex = -1;
        return;
    }

    if (!d->step) {
        // the stroke's own step isn't in the history yet: it is still being painted
        if (d->expectedIndex > 0 && ++d->retries < 600) d->retry.start(16);
        return;
    }
    // only while the stroke is still the newest step, with nothing undone after it
    if (index < 1 || index != d->stack->count() || d->stack->command(index - 1) != d->step) {
        d->step = nullptr;
        d->expectedIndex = -1;
        return;
    }
    if (!image->isIdle()) {
        if (++d->retries < 600) d->retry.start(16);
        return;
    }
    d->step = nullptr;
    d->dirty = false;
    d->before = index > 1 ? d->stack->command(index - 2) : nullptr;
    d->expectedIndex = index;
    d->stack->setIndex(index - 1);
    if (d->stack->index() != index - 1) {
        // the image refused the undo (busy): the stroke stays as it is until the next look
        d->step = d->stack->command(index - 1);
        d->dirty = true;
        if (++d->retries < 600) d->retry.start(16);
        return;
    }
    if (!d->helper->replayLastStroke()) {
        // couldn't paint it again: it stays as it was
        d->expectedIndex = -1;
        d->stack->setIndex(index);
        return;
    }
    d->painting = true;
    d->idleSeen = false;
}
