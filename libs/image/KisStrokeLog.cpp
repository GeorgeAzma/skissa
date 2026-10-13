/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStrokeLog.h"

#include <atomic>

#include <QCoreApplication>
#include <QDataStream>
#include <QIODevice>
#include <QStringList>

#include <kundo2command.h>

#include "kis_node.h"

namespace {

/// Enough for the colors a layer is painted with, and its last brush
constexpr int kept = 512;
constexpr quint32 magic = 0x4b534c33; // "KSL3"

quint64 nextId()
{
    static std::atomic<quint64> id {1};
    return id++;
}

} // namespace

KisStrokeLog::KisStrokeLog()
{
}

KisStrokeLog::KisStrokeLog(const KisStrokeLog &rhs)
{
    QReadLocker locker(&rhs.m_lock);
    m_strokes = rhs.m_strokes;
}

KisStrokeLog &KisStrokeLog::operator=(const KisStrokeLog &rhs)
{
    if (this != &rhs) set(rhs.strokes());
    return *this;
}

KisStrokeLog::Strokes KisStrokeLog::strokes() const
{
    QReadLocker locker(&m_lock);
    return m_strokes;
}

bool KisStrokeLog::isEmpty() const
{
    QReadLocker locker(&m_lock);
    return m_strokes.isEmpty();
}

KisStrokeLog::Strokes KisStrokeLog::append(const Strokes &strokes)
{
    Strokes dropped;
    {
        QWriteLocker locker(&m_lock);
        m_strokes += strokes;
        if (m_strokes.size() > kept) {
            const int count = m_strokes.size() - kept;
            dropped = m_strokes.mid(0, count);
            m_strokes.remove(0, count);
        }
    }
    notifier()->notify();
    return dropped;
}

void KisStrokeLog::restore(const Strokes &dropped)
{
    if (dropped.isEmpty()) {
        return;
    }
    {
        QWriteLocker locker(&m_lock);
        m_strokes = dropped + m_strokes;
    }
    notifier()->notify();
}

void KisStrokeLog::remove(const QVector<quint64> &ids)
{
    {
        QWriteLocker locker(&m_lock);
        for (int i = m_strokes.size() - 1; i >= 0; i--) {
            if (ids.contains(m_strokes[i].id)) m_strokes.remove(i);
        }
    }
    notifier()->notify();
}

void KisStrokeLog::set(const Strokes &strokes)
{
    {
        QWriteLocker locker(&m_lock);
        m_strokes = strokes;
    }
    notifier()->notify();
}

bool KisStrokeLog::lastBrush(Stroke *result) const
{
    QReadLocker locker(&m_lock);
    for (int i = m_strokes.size() - 1; i >= 0; i--) {
        if (!m_strokes[i].erasing) {
            if (result) *result = m_strokes[i];
            return true;
        }
    }
    return false;
}

QVector<QColor> KisStrokeLog::colors(int count) const
{
    QVector<QColor> colors;
    QVector<QRgb> have;
    QReadLocker locker(&m_lock);
    for (int i = m_strokes.size() - 1; i >= 0 && colors.size() < count; i--) {
        const Stroke &stroke = m_strokes[i];
        if (stroke.erasing || have.contains(stroke.color)) continue;
        have << stroke.color;
        colors << QColor::fromRgba(stroke.color);
    }
    return colors;
}

QByteArray KisStrokeLog::save() const
{
    const Strokes strokes = this->strokes();
    if (strokes.isEmpty()) {
        return QByteArray();
    }
    // each brush's name once
    QStringList brushes;
    Q_FOREACH (const Stroke &stroke, strokes) {
        if (!brushes.contains(stroke.brush)) brushes << stroke.brush;
    }
    QByteArray data;
    QDataStream out(&data, QIODevice::WriteOnly);
    out.setVersion(QDataStream::Qt_5_12);
    out << magic << brushes << quint32(strokes.size());
    Q_FOREACH (const Stroke &stroke, strokes) {
        out << qint32(brushes.indexOf(stroke.brush)) << stroke.brushSize << quint32(stroke.color) << quint8(stroke.erasing ? 1 : 0);
    }
    return qCompress(data, 9);
}

void KisStrokeLog::load(const QByteArray &compressed)
{
    const QByteArray data = qUncompress(compressed);
    QDataStream in(data);
    in.setVersion(QDataStream::Qt_5_12);
    quint32 head = 0;
    in >> head;
    // an older log (where strokes went) is let go
    if (head != magic) {
        return;
    }
    // read name by name: a count no file this size could hold is not believed (a damaged or made-up file)
    QStringList brushes;
    quint32 brushCount = 0;
    in >> brushCount;
    if (brushCount > quint32(data.size())) return;
    for (quint32 i = 0; i < brushCount && in.status() == QDataStream::Ok; i++) {
        QString brush;
        in >> brush;
        brushes << brush;
    }
    quint32 count = 0;
    in >> count;
    if (in.status() != QDataStream::Ok || count > quint32(data.size())) return;
    Strokes strokes;
    for (quint32 i = 0; i < count && in.status() == QDataStream::Ok; i++) {
        qint32 brush = -1;
        quint32 color = 0;
        quint8 flags = 0;
        Stroke stroke;
        in >> brush >> stroke.brushSize >> color >> flags;
        if (in.status() != QDataStream::Ok) break;
        stroke.id = nextId();
        stroke.brush = brushes.value(brush);
        stroke.color = color;
        stroke.erasing = flags & 1;
        if (!stroke.brush.isEmpty()) strokes << stroke;
    }
    set(strokes.mid(qMax(0, strokes.size() - kept)));
}

class KisStrokeLogAddCommand : public KUndo2Command
{
public:
    KisStrokeLogAddCommand(KisNodeSP node, KisStrokeLog::Strokes strokes)
        : m_node(node)
        , m_strokes(strokes)
    {
        for (KisStrokeLog::Stroke &stroke : m_strokes) {
            stroke.id = nextId();
            m_ids << stroke.id;
        }
    }

    void redo() override
    {
        m_dropped = m_node->strokeLog().append(m_strokes);
    }

    void undo() override
    {
        m_node->strokeLog().remove(m_ids);
        // the oldest ones dropped to make room come back
        m_node->strokeLog().restore(m_dropped);
        m_dropped.clear();
    }

private:
    KisNodeSP m_node;
    KisStrokeLog::Strokes m_strokes;
    KisStrokeLog::Strokes m_dropped;
    QVector<quint64> m_ids;
};

KUndo2Command *KisStrokeLog::addCommand(KisNodeSP node, const Strokes &strokes)
{
    if (!node || strokes.isEmpty()) {
        return nullptr;
    }
    return new KisStrokeLogAddCommand(node, strokes);
}

KisStrokeLogNotifier *KisStrokeLog::notifier()
{
    static KisStrokeLogNotifier *notifier = []() {
        auto *object = new KisStrokeLogNotifier();
        // in the main thread, whichever thread first asked for it
        if (QCoreApplication::instance()) object->moveToThread(QCoreApplication::instance()->thread());
        return object;
    }();
    return notifier;
}
