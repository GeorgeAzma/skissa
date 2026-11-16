/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: LGPL-2.0-or-later
 */
#ifndef KISRESOURCEORDER_H
#define KISRESOURCEORDER_H

#include <QObject>
#include <QStringList>

#include <functional>

#include "kritaresources_export.h"

/**
 * The order the user dragged tags and the resources in each tag into. Kept in kritarc, not in the resource
 * database, so the database stays upstream Krita's; tags and resources not in it come after, by name.
 */
class KRITARESOURCES_EXPORT KisResourceOrder : public QObject
{
    Q_OBJECT
public:
    /// kritarc keys: "type\ttag url" in order, and "type\ttag url\tfile name" in order
    static const QString tagsKey;
    static const QString resourcesKey;

    static KisResourceOrder *instance();

    /// The urls of \p type's tags, in the order the user gave them
    static QStringList tags(const QString &type);
    static void setTags(const QString &type, const QStringList &urls);
    /// The file names of the resources in tag \p tagUrl, in the order the user gave them
    static QStringList inTag(const QString &type, const QString &tagUrl);
    static void setInTag(const QString &type, const QString &tagUrl, const QStringList &fileNames);
    /// The file names of \p type's resources left out of the list of all of them; search and tags still find them
    static QStringList putAway(const QString &type);
    static void setPutAway(const QString &type, const QStringList &fileNames);

    /// Where an order is written: the settings' undo history, given by the interface; kritarc by default
    static void setWriter(const std::function<void(const QString &key, const QStringList &items)> &writer);
    /// The order was written (or taken back): sorted lists sort again
    static void noteChanged();

Q_SIGNALS:
    void changed();
};

#endif // KISRESOURCEORDER_H
