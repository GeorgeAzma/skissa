/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioSymmetry.h"

#include <QAction>

#include <KConfigGroup>
#include <KSharedConfig>
#include <KoToolManager.h>
#include <kactioncollection.h>

namespace
{

const QString brushTool = QStringLiteral("KritaShape/KisToolBrush");
const QString multiBrushTool = QStringLiteral("KritaShape/KisToolMultiBrush");

KConfigGroup settings()
{
    return KSharedConfig::openConfig()->group("Symmetry");
}

QAction *mirrorAction(KisKActionCollection *actions, bool across)
{
    return actions ? actions->action(across ? QStringLiteral("hmirror_action") : QStringLiteral("vmirror_action")) : nullptr;
}

void setChecked(QAction *action, bool checked)
{
    if (action && action->isChecked() != checked) action->trigger();
}

} // namespace

KisStudioSymmetry::KisStudioSymmetry() = default;

KisStudioSymmetry *KisStudioSymmetry::instance()
{
    static KisStudioSymmetry *symmetry = new KisStudioSymmetry();
    return symmetry;
}

KisStudioSymmetry::Mode KisStudioSymmetry::mode() const
{
    return Mode(qBound(0, settings().readEntry("Mode", 0), 2));
}

void KisStudioSymmetry::setMode(Mode mode)
{
    settings().writeEntry("Mode", int(mode));
    Q_EMIT changed();
}

int KisStudioSymmetry::copies() const
{
    return qBound(2, settings().readEntry("Copies", 6), maximumCopies);
}

void KisStudioSymmetry::setCopies(int copies)
{
    settings().writeEntry("Copies", qBound(2, copies, maximumCopies));
    Q_EMIT changed();
}

bool KisStudioSymmetry::mirrored() const
{
    return settings().readEntry("Mirrored", false);
}

void KisStudioSymmetry::setMirrored(bool mirrored)
{
    settings().writeEntry("Mirrored", mirrored);
    Q_EMIT changed();
}

qreal KisStudioSymmetry::turn() const
{
    return settings().readEntry("Turn", 0.0);
}

void KisStudioSymmetry::setTurn(qreal turn)
{
    settings().writeEntry("Turn", turn);
    Q_EMIT changed();
}

QSize KisStudioSymmetry::spacing() const
{
    const QSize spacing = settings().readEntry("Spacing", QSize(500, 500));
    return QSize(qBound(0, spacing.width(), 20000), qBound(0, spacing.height(), 20000));
}

void KisStudioSymmetry::setSpacing(const QSize &spacing)
{
    settings().writeEntry("Spacing", spacing);
    Q_EMIT changed();
}

bool KisStudioSymmetry::mirrorAcross() const
{
    return settings().readEntry("MirrorAcross", true);
}

bool KisStudioSymmetry::mirrorDown() const
{
    return settings().readEntry("MirrorDown", false);
}

void KisStudioSymmetry::setMirrorAxes(bool across, bool down)
{
    // never none: an axis the way most pictures are symmetric
    KConfigGroup group = settings();
    group.writeEntry("MirrorAcross", across || !down);
    group.writeEntry("MirrorDown", down);
    Q_EMIT changed();
}

bool KisStudioSymmetry::isOn(KisKActionCollection *actions)
{
    QAction *across = mirrorAction(actions, true);
    QAction *down = mirrorAction(actions, false);
    return (across && across->isChecked()) || (down && down->isChecked()) || KoToolManager::instance()->activeToolId() == multiBrushTool;
}

void KisStudioSymmetry::setOn(KisKActionCollection *actions, bool on)
{
    KisStudioSymmetry *symmetry = instance();
    // mirrored axes turned on or off elsewhere (the toolbar, a shortcut): what is on now is what comes back
    QAction *acrossAction = mirrorAction(actions, true);
    QAction *downAction = mirrorAction(actions, false);
    const bool acrossOn = acrossAction && acrossAction->isChecked();
    const bool downOn = downAction && downAction->isChecked();
    if (!on && (acrossOn || downOn)) symmetry->setMirrorAxes(acrossOn, downOn);

    const bool mirror = on && symmetry->mode() == Mirror;
    setChecked(acrossAction, mirror && symmetry->mirrorAcross());
    setChecked(downAction, mirror && symmetry->mirrorDown());
    // radial and tile are the brush's: the Multibrush tool, and back to the tool it came from
    static QString toolBefore;
    const QString tool = KoToolManager::instance()->activeToolId();
    if (on && !mirror) {
        if (tool != multiBrushTool) {
            toolBefore = tool;
            KoToolManager::instance()->switchToolRequested(multiBrushTool);
        }
    } else if (tool == multiBrushTool) {
        KoToolManager::instance()->switchToolRequested(toolBefore.isEmpty() ? brushTool : toolBefore);
    }
}
