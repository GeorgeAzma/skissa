/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioSymmetry.h"
#include "KisSettingsRegistry.h"
#include "KisStudioTools.h"

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

/// Written and saved a moment later, so a crash doesn't lose it
template<typename T>
void store(const char *key, const T &value)
{
    settings().writeEntry(key, value);
    KisSettingsRegistry::instance()->scheduleSync();
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
    store("Mode", int(mode));
    Q_EMIT changed();
}

int KisStudioSymmetry::copies() const
{
    return qBound(2, settings().readEntry("Copies", 6), maximumCopies);
}

void KisStudioSymmetry::setCopies(int copies)
{
    store("Copies", qBound(2, copies, maximumCopies));
    Q_EMIT changed();
}

bool KisStudioSymmetry::mirrored() const
{
    return settings().readEntry("Mirrored", false);
}

void KisStudioSymmetry::setMirrored(bool mirrored)
{
    store("Mirrored", mirrored);
    Q_EMIT changed();
}

qreal KisStudioSymmetry::turn() const
{
    return settings().readEntry("Turn", 0.0);
}

void KisStudioSymmetry::setTurn(qreal turn)
{
    store("Turn", turn);
    Q_EMIT changed();
}

QSize KisStudioSymmetry::spacing() const
{
    const QSize spacing = settings().readEntry("Spacing", QSize(500, 500));
    return QSize(qBound(0, spacing.width(), 20000), qBound(0, spacing.height(), 20000));
}

void KisStudioSymmetry::setSpacing(const QSize &spacing)
{
    store("Spacing", spacing);
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
    // Keep at least one axis; left-right mirroring is the most common
    KConfigGroup group = settings();
    group.writeEntry("MirrorAcross", across || !down);
    group.writeEntry("MirrorDown", down);
    KisSettingsRegistry::instance()->scheduleSync();
    Q_EMIT changed();
}

bool KisStudioSymmetry::isOn(KisKActionCollection *actions)
{
    QAction *across = mirrorAction(actions, true);
    QAction *down = mirrorAction(actions, false);
    return (across && across->isChecked()) || (down && down->isChecked()) || KisStudioTools::activeToolId() == multiBrushTool;
}

void KisStudioSymmetry::setOn(KisKActionCollection *actions, bool on)
{
    KisStudioSymmetry *symmetry = instance();
    // Remember axes toggled elsewhere (toolbar, shortcut) so turning symmetry back on restores them
    QAction *acrossAction = mirrorAction(actions, true);
    QAction *downAction = mirrorAction(actions, false);
    const bool acrossOn = acrossAction && acrossAction->isChecked();
    const bool downOn = downAction && downAction->isChecked();
    if (!on && (acrossOn || downOn)) symmetry->setMirrorAxes(acrossOn, downOn);

    const bool mirror = on && symmetry->mode() == Mirror;
    setChecked(acrossAction, mirror && symmetry->mirrorAcross());
    setChecked(downAction, mirror && symmetry->mirrorDown());
    // Radial and tile use the Multibrush tool; switch back to the previous tool when done
    static QString toolBefore;
    const QString tool = KisStudioTools::activeToolId();
    if (on && !mirror) {
        if (tool != multiBrushTool) {
            toolBefore = tool;
            KisStudioTools::switchTo(multiBrushTool);
        }
    } else if (tool == multiBrushTool) {
        KisStudioTools::switchTo(toolBefore.isEmpty() ? brushTool : toolBefore);
    }
}
