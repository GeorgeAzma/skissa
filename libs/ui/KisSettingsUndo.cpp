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

/// Who was undone, latest last: the order Redo goes back through
enum class Source { Settings, Document };
QVector<Source> &undone()
{
    static QVector<Source> list;
    return list;
}

/// How long ago \p stack's latest step ended, in ms (a step is timed by the time of day only)
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

} // namespace

bool KisSettingsUndo::undoInstead(KUndo2Stack *stack)
{
    KisSettingsRegistry *settings = KisSettingsRegistry::instance();
    const qint64 settingsAge = settings->undoAge();
    const qint64 docAge = documentAge(stack);
    // the settings' change is the later one, or the document has nothing to undo
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
    KisSettingsRegistry *settings = KisSettingsRegistry::instance();
    // what was taken back last, of what can still be done again (a new change ends a redo of its own kind)
    while (!undone().isEmpty()) {
        const Source source = undone().takeLast();
        if (source == Source::Settings && settings->canRedoChange()) {
            const QString name = settings->redoName();
            settings->redoChange();
            tell(i18nc("@info:status a setting's change redone", "Redo: %1", name));
            return true;
        }
        if (source == Source::Document && stack->canRedo()) return false;
    }
    if (!stack->canRedo() && settings->canRedoChange()) {
        const QString name = settings->redoName();
        settings->redoChange();
        tell(i18nc("@info:status a setting's change redone", "Redo: %1", name));
        return true;
    }
    return false;
}
