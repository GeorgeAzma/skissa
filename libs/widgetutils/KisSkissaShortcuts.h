/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSKISSASHORTCUTS_H
#define KISSKISSASHORTCUTS_H

#include <QHash>
#include <QString>

/**
 * Default keys Skissa has otherwise than Krita's action files, by action name; empty: none.
 * The tool rail's slots take T and F (the slot's last tool), and keys for single shape and
 * selection tools are left for people to choose.
 */
inline const QHash<QString, QString> &skissaDefaultShortcuts()
{
    static const QHash<QString, QString> changed{
        {QStringLiteral("KritaTransform/KisToolMove"), QString()},
        {QStringLiteral("KritaFill/KisToolFill"), QString()},
        {QStringLiteral("KisToolSelectElliptical"), QString()},
        {QStringLiteral("KisToolSelectRectangular"), QString()},
        {QStringLiteral("KritaShape/KisToolEllipse"), QString()},
        {QStringLiteral("KritaShape/KisToolRectangle"), QString()},
    };
    return changed;
}

#endif // KISSKISSASHORTCUTS_H
