/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisSettingsUndo.h"

#include <QIcon>
#include <QTime>
#include <QVector>

#include <kundo2stack.h>
#include <klocalizedstring.h>

#include "KisMainWindow.h"
#include "KisPart.h"
#include "KisSettingsRegistry.h"
#include "KisViewManager.h"

namespace
{

/// What each undo took back, most recent last; Redo walks it backwards.
enum class Source { Settings, Document };
QVector<Source> &undone()
{
    static QVector<Source> list;
    return list;
}

int documentOnly = 0;

/// Milliseconds since \p stack's latest command ended, or -1 if there is none.
/// Commands only record the time of day, so a negative difference wraps past midnight.
qint64 documentAge(KUndo2Stack *stack)
{
    const KUndo2Command *last = stack->index() > 0 ? stack->command(stack->index() - 1) : nullptr;
    if (!last) return -1;
    const QTime end = last->endTime().isValid() ? last->endTime() : last->time();
    if (!end.isValid()) return -1;
    qint64 age = end.msecsTo(QTime::currentTime());
    if (age < 0) age += 24 * 3600 * 1000;
    return age;
}

void tell(const QString &text)
{
    KisMainWindow *window = KisPart::instance()->currentMainwindow();
    if (window && window->viewManager()) window->viewManager()->showFloatingMessage(text, QIcon(), 1500);
}

bool redoSettings(KisSettingsRegistry *settings)
{
    const QString name = settings->redoName();
    settings->redoChange();
    tell(i18nc("@info:status a setting's change redone", "Redo: %1", name));
    return true;
}

} // namespace

KisSettingsUndo::DocumentOnly::DocumentOnly()
{
    documentOnly++;
}

KisSettingsUndo::DocumentOnly::~DocumentOnly()
{
    documentOnly--;
}

bool KisSettingsUndo::undoInstead(KUndo2Stack *stack)
{
    if (documentOnly > 0) return false;
    KisSettingsRegistry *settings = KisSettingsRegistry::instance();
    const qint64 settingsAge = settings->undoAge();
    const qint64 docAge = documentAge(stack);
    // Undo the settings change if it is newer, or if the document has nothing to undo
    if (settingsAge >= 0 && (docAge < 0 || settingsAge < docAge)) {
        const QString name = settings->undoName();
        if (settings->undoChange()) {
            undone() << Source::Settings;
            tell(i18nc("@info:status a setting's change undone", "Undo: %1", name));
            return true;
        }
    }
    if (docAge >= 0) undone() << Source::Document;
    return false;
}

bool KisSettingsUndo::redoInstead(KUndo2Stack *stack)
{
    if (documentOnly > 0) return false;
    KisSettingsRegistry *settings = KisSettingsRegistry::instance();
    // Redo the most recently undone source that can still be redone; a new change
    // in either history clears that history's redo steps.
    while (!undone().isEmpty()) {
        const Source source = undone().takeLast();
        if (source == Source::Settings && settings->canRedoChange()) return redoSettings(settings);
        if (source == Source::Document && stack->canRedo()) return false;
    }
    if (!stack->canRedo() && settings->canRedoChange()) return redoSettings(settings);
    return false;
}
