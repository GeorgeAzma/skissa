/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSKISSASHORTCUTS_H
#define KISSKISSASHORTCUTS_H

#include <QHash>
#include <QString>
#include <QStringList>

/**
 * Skissa's whole set of default keys, by action name: everything else starts with none and is found
 * with Find or given a key on its card. Keys sit under the hand that isn't holding the pen; tools are
 * the rail's numbers; Ctrl is for the file and the selection. N, R, V, W and Y are held on the canvas
 * to drag a value, pick a layer or draw a ruler (the input profile), so they have no tap of their own.
 */
inline const QHash<QString, QString> &skissaDefaultShortcuts()
{
    static const QHash<QString, QString> keys{
        {QStringLiteral("studio_rail_1"), QStringLiteral("1")},
        {QStringLiteral("studio_rail_2"), QStringLiteral("2")},
        {QStringLiteral("studio_rail_3"), QStringLiteral("3")},
        {QStringLiteral("studio_rail_4"), QStringLiteral("4")},
        {QStringLiteral("studio_rail_5"), QStringLiteral("5")},
        {QStringLiteral("studio_rail_6"), QStringLiteral("6")},
        {QStringLiteral("studio_rail_7"), QStringLiteral("7")},
        {QStringLiteral("studio_rail_8"), QStringLiteral("8")},
        {QStringLiteral("studio_rail_9"), QStringLiteral("9")},

        {QStringLiteral("erase_action"), QStringLiteral("E")},
        {QStringLiteral("toggle_fg_bg"), QStringLiteral("X")},
        {QStringLiteral("previous_preset"), QStringLiteral("Q")},
        {QStringLiteral("decrease_brush_size"), QStringLiteral("[")},
        {QStringLiteral("increase_brush_size"), QStringLiteral("]")},
        {QStringLiteral("studio_squint"), QStringLiteral("S")},
        {QStringLiteral("toggle_assistant"), QStringLiteral("Ctrl+Shift+L")},

        {QStringLiteral("studio_fit_view"), QStringLiteral("F")},
        {QStringLiteral("mirror_canvas"), QStringLiteral("M")},
        {QStringLiteral("view_show_canvas_only"), QStringLiteral("Tab")},

        {QStringLiteral("isolate_active_layer"), QStringLiteral("`")},
        {QStringLiteral("add_new_paint_layer"), QStringLiteral("A")},
        {QStringLiteral("duplicatelayer"), QStringLiteral("Ctrl+J")},
        {QStringLiteral("merge_layer"), QStringLiteral("Ctrl+E")},
        {QStringLiteral("create_quick_group"), QStringLiteral("Ctrl+G")},
        {QStringLiteral("clear"), QStringLiteral("Del; Backspace")},

        {QStringLiteral("edit_undo"), QStringLiteral("Ctrl+Z")},
        {QStringLiteral("edit_redo"), QStringLiteral("Ctrl+Shift+Z; Ctrl+Y")},
        {QStringLiteral("edit_copy"), QStringLiteral("Ctrl+C")},
        {QStringLiteral("edit_cut"), QStringLiteral("Ctrl+X")},
        {QStringLiteral("edit_paste"), QStringLiteral("Ctrl+V")},
        {QStringLiteral("select_all"), QStringLiteral("Ctrl+A")},
        {QStringLiteral("deselect"), QStringLiteral("Ctrl+D")},
        {QStringLiteral("reselect"), QStringLiteral("Ctrl+Shift+D")},
        {QStringLiteral("invert_selection"), QStringLiteral("Ctrl+Shift+I")},
        {QStringLiteral("KisToolTransform"), QStringLiteral("Ctrl+T")},
        {QStringLiteral("filter_apply_again"), QStringLiteral("Ctrl+F")},
        {QStringLiteral("find_anything"), QStringLiteral("Ctrl+K")},

        {QStringLiteral("file_new"), QStringLiteral("Ctrl+N")},
        {QStringLiteral("file_open"), QStringLiteral("Ctrl+O")},
        {QStringLiteral("file_save"), QStringLiteral("Ctrl+S")},
        {QStringLiteral("file_save_as"), QStringLiteral("Ctrl+Shift+S")},
        {QStringLiteral("file_close"), QStringLiteral("Ctrl+W")},
        {QStringLiteral("file_quit"), QStringLiteral("Ctrl+Q")},
    };
    return keys;
}

/// Action files whose keys work only inside their own tool (moving by arrows, text styles): they keep Krita's
inline bool skissaKeepsKritaKeys(const QString &actionFileName)
{
    static const QStringList kept{QStringLiteral("MoveTool.action"), QStringLiteral("PathTool.action"), QStringLiteral("SvgTextTool.action"),
                                  QStringLiteral("InteractionTool.action"), QStringLiteral("SPenSettings.action")};
    return kept.contains(actionFileName);
}

#endif // KISSKISSASHORTCUTS_H
