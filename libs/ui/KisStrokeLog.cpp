/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStrokeLog.h"

#include <QDataStream>
#include <QHash>
#include <QIODevice>
#include <QLineF>
#include <QMutex>
#include <QMutexLocker>
#include <QtMath>

#include <KoColor.h>
#include <KoColorSpace.h>

#include "kis_image.h"
#include "kis_layer_utils.h"
#include "kis_node.h"
#include "kis_paint_device.h"

namespace {

/// Enough to remember a long painting session, not so many that the file notices
constexpr int kept = 20000;
constexpr quint32 magic = 0x4b534c31; // "KSL1"

QHash<const KisImage *, KisStrokeLog *> &logs()
{
    static QHash<const KisImage *, KisStrokeLog *> all;
    return all;
}

/// Saving happens in another thread
QMutex &logsMutex()
{
    static QMutex mutex;
    return mutex;
}

/// How far \p point is from the path
qreal distanceTo(const QPolygon &path, const QPoint &point)
{
    if (path.isEmpty()) return 1e9;
    qreal best = QLineF(path[0], point).length();
    for (int i = 1; i < path.size(); i++) {
        const QPointF a = path[i - 1];
        const QPointF b = path[i];
        const QPointF ab = b - a;
        const qreal length2 = ab.x() * ab.x() + ab.y() * ab.y();
        qreal t = length2 > 0 ? ((point.x() - a.x()) * ab.x() + (point.y() - a.y()) * ab.y()) / length2 : 0;
        t = qBound(0.0, t, 1.0);
        best = qMin(best, QLineF(a + t * ab, point).length());
    }
    return best;
}

} // namespace

KisStrokeLog::KisStrokeLog(QObject *parent)
    : QObject(parent)
{
}

KisStrokeLog *KisStrokeLog::forImage(KisImageWSP image)
{
    KisImage *key = image.data();
    if (!key) {
        return nullptr;
    }
    QMutexLocker locker(&logsMutex());
    auto it = logs().constFind(key);
    if (it != logs().constEnd()) {
        return *it;
    }
    // it goes with the image, in the image's thread
    auto *log = new KisStrokeLog(nullptr);
    log->moveToThread(key->thread());
    logs().insert(key, log);
    QObject::connect(key, &QObject::destroyed, log, [key, log]() {
        {
            QMutexLocker locker(&logsMutex());
            logs().remove(key);
        }
        log->deleteLater();
    });
    return log;
}

KisStrokeLog *KisStrokeLog::existing(const KisImage *image)
{
    QMutexLocker locker(&logsMutex());
    return logs().value(image, nullptr);
}

void KisStrokeLog::copy(KisImageSP from, KisImageSP to)
{
    KisStrokeLog *source = existing(from.data());
    if (!source || source->isEmpty() || !to) {
        return;
    }
    if (KisStrokeLog *target = forImage(to)) {
        target->load(source->save());
    }
}

void KisStrokeLog::add(const Stroke &stroke)
{
    if (stroke.path.isEmpty() || stroke.node.isNull()) {
        return;
    }
    m_strokes.append(stroke);
    if (m_strokes.size() > kept) {
        m_strokes.remove(0, m_strokes.size() - kept + kept / 20);
    }
    Q_EMIT changed();
}

const KisStrokeLog::Stroke *KisStrokeLog::strokeAt(KisImageSP image, const QPoint &imagePos) const
{
    if (!image) {
        return nullptr;
    }
    for (int i = m_strokes.size() - 1; i >= 0; i--) {
        const Stroke &stroke = m_strokes[i];
        if (stroke.erasing || !stroke.bounds.contains(imagePos)) continue;
        if (distanceTo(stroke.path, imagePos) > stroke.size / 2 + 3) continue;
        // still there: a visible layer with paint under the point (not erased or undone since)
        KisNodeSP node = KisLayerUtils::findNodeByUuid(image->root(), stroke.node);
        if (!node || !node->visible(true) || !node->paintDevice()) continue;
        KoColor pixel(node->paintDevice()->colorSpace());
        node->paintDevice()->pixel(imagePos.x(), imagePos.y(), &pixel);
        if (pixel.opacityU8() == 0) continue;
        return &stroke;
    }
    return nullptr;
}

const KisStrokeLog::Stroke *KisStrokeLog::lastOn(const QUuid &node) const
{
    for (int i = m_strokes.size() - 1; i >= 0; i--) {
        if (m_strokes[i].node == node && !m_strokes[i].erasing) return &m_strokes[i];
    }
    return nullptr;
}

QVector<QColor> KisStrokeLog::colorsOn(const QUuid &node, int count) const
{
    QVector<QColor> colors;
    for (int i = m_strokes.size() - 1; i >= 0 && colors.size() < count; i--) {
        const Stroke &stroke = m_strokes[i];
        if (stroke.node != node || stroke.erasing) continue;
        bool have = false;
        Q_FOREACH (const QColor &color, colors) {
            if (color.rgba() == stroke.color.rgba()) {
                have = true;
                break;
            }
        }
        if (!have) colors << stroke.color;
    }
    return colors;
}

/**
 * Small: the layers and the brushes once each, every stroke as indexes
 * into them, and its points as the first one and then steps from one to
 * the next; compressed.
 */
QByteArray KisStrokeLog::save() const
{
    if (m_strokes.isEmpty()) {
        return QByteArray();
    }
    QVector<QUuid> nodes;
    QStringList brushes;
    QHash<QUuid, int> nodeIndex;
    QHash<QString, int> brushIndex;
    Q_FOREACH (const Stroke &stroke, m_strokes) {
        if (!nodeIndex.contains(stroke.node)) {
            nodeIndex.insert(stroke.node, nodes.size());
            nodes << stroke.node;
        }
        if (!brushIndex.contains(stroke.brush)) {
            brushIndex.insert(stroke.brush, brushes.size());
            brushes << stroke.brush;
        }
    }

    QByteArray data;
    QDataStream out(&data, QIODevice::WriteOnly);
    out.setVersion(QDataStream::Qt_5_12);
    out << magic << quint32(nodes.size());
    Q_FOREACH (const QUuid &node, nodes) out << node;
    out << brushes << quint32(m_strokes.size());
    Q_FOREACH (const Stroke &stroke, m_strokes) {
        out << quint32(nodeIndex.value(stroke.node)) << quint32(brushIndex.value(stroke.brush)) << float(stroke.size)
            << quint32(stroke.color.rgba()) << quint8(stroke.erasing ? 1 : 0) << quint16(stroke.path.size());
        QPoint previous;
        for (int i = 0; i < stroke.path.size(); i++) {
            if (i == 0) {
                out << qint32(stroke.path[0].x()) << qint32(stroke.path[0].y());
            } else {
                const QPoint step = stroke.path[i] - previous;
                out << qint16(qBound(-32767, step.x(), 32767)) << qint16(qBound(-32767, step.y(), 32767));
            }
            previous = stroke.path[i];
        }
    }
    return qCompress(data, 9);
}

void KisStrokeLog::load(const QByteArray &compressed)
{
    m_strokes.clear();
    const QByteArray data = qUncompress(compressed);
    QDataStream in(data);
    in.setVersion(QDataStream::Qt_5_12);
    quint32 head = 0;
    quint32 nodeCount = 0;
    in >> head >> nodeCount;
    if (head != magic || in.status() != QDataStream::Ok || nodeCount > 1000000) {
        return;
    }
    QVector<QUuid> nodes;
    nodes.resize(int(nodeCount));
    for (quint32 i = 0; i < nodeCount; i++) in >> nodes[int(i)];
    QStringList brushes;
    quint32 count = 0;
    in >> brushes >> count;
    for (quint32 i = 0; i < count && in.status() == QDataStream::Ok; i++) {
        quint32 node = 0, brush = 0, rgba = 0;
        float size = 0;
        quint8 flags = 0;
        quint16 points = 0;
        in >> node >> brush >> size >> rgba >> flags >> points;
        Stroke stroke;
        stroke.node = nodes.value(int(node));
        stroke.brush = brushes.value(int(brush));
        stroke.size = size;
        stroke.color = QColor::fromRgba(rgba);
        stroke.erasing = flags & 1;
        QPoint at;
        for (int p = 0; p < points; p++) {
            if (p == 0) {
                qint32 x = 0, y = 0;
                in >> x >> y;
                at = QPoint(x, y);
            } else {
                qint16 dx = 0, dy = 0;
                in >> dx >> dy;
                at += QPoint(dx, dy);
            }
            stroke.path << at;
        }
        const int radius = qCeil(stroke.size / 2) + 2;
        stroke.bounds = stroke.path.boundingRect().adjusted(-radius, -radius, radius, radius);
        if (in.status() == QDataStream::Ok) m_strokes << stroke;
    }
    Q_EMIT changed();
}
