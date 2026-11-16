/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: LGPL-2.0-or-later
 */
#include "KisResourceOrder.h"

#include <KConfigGroup>
#include <KSharedConfig>

namespace
{

const QChar separator = QLatin1Char('\t');

std::function<void(const QString &, const QStringList &)> &writerFunction()
{
    static std::function<void(const QString &, const QStringList &)> writer;
    return writer;
}

QStringList readList(const QString &key)
{
    return KSharedConfig::openConfig()->group(QString()).readEntry(key, QStringList());
}

void writeList(const QString &key, const QStringList &items)
{
    if (writerFunction()) {
        writerFunction()(key, items);
    } else {
        KConfigGroup group = KSharedConfig::openConfig()->group(QString());
        if (items.isEmpty()) group.deleteEntry(key);
        else group.writeEntry(key, items);
    }
    KisResourceOrder::noteChanged();
}

} // namespace

const QString KisResourceOrder::tagsKey = QStringLiteral("StudioTagOrder");
const QString KisResourceOrder::resourcesKey = QStringLiteral("StudioTagResourceOrder");

KisResourceOrder *KisResourceOrder::instance()
{
    static KisResourceOrder *order = new KisResourceOrder();
    return order;
}

QStringList KisResourceOrder::tags(const QString &type)
{
    const QString prefix = type + separator;
    QStringList urls;
    Q_FOREACH (const QString &item, readList(tagsKey)) {
        if (item.startsWith(prefix)) urls << item.mid(prefix.size());
    }
    return urls;
}

void KisResourceOrder::setTags(const QString &type, const QStringList &urls)
{
    const QString prefix = type + separator;
    QStringList items;
    Q_FOREACH (const QString &item, readList(tagsKey)) {
        if (!item.startsWith(prefix)) items << item;
    }
    Q_FOREACH (const QString &url, urls) items << prefix + url;
    writeList(tagsKey, items);
}

QStringList KisResourceOrder::inTag(const QString &type, const QString &tagUrl)
{
    const QString prefix = type + separator + tagUrl + separator;
    QStringList names;
    Q_FOREACH (const QString &item, readList(resourcesKey)) {
        if (item.startsWith(prefix)) names << item.mid(prefix.size());
    }
    return names;
}

void KisResourceOrder::setInTag(const QString &type, const QString &tagUrl, const QStringList &fileNames)
{
    const QString prefix = type + separator + tagUrl + separator;
    QStringList items;
    Q_FOREACH (const QString &item, readList(resourcesKey)) {
        if (!item.startsWith(prefix)) items << item;
    }
    Q_FOREACH (const QString &name, fileNames) items << prefix + name;
    writeList(resourcesKey, items);
}

void KisResourceOrder::setWriter(const std::function<void(const QString &, const QStringList &)> &writer)
{
    writerFunction() = writer;
}

void KisResourceOrder::noteChanged()
{
    Q_EMIT instance()->changed();
}
