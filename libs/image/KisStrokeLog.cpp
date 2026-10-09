/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStrokeLog.h"

#include <atomic>

#include <QCoreApplication>
#include <QDataStream>
#include <QHash>
#include <QIODevice>
#include <QMutex>
#include <QMutexLocker>
#include <QPainterPath>
#include <QtMath>

#include <KoColor.h>
#include <kundo2command.h>

#include "kis_clone_layer.h"
#include "kis_default_bounds_base.h"
#include "kis_layer.h"
#include "kis_node.h"
#include "kis_paint_device.h"
#include "kis_paint_device_frames_interface.h"
#include "kis_pixel_selection.h"
#include "kis_random_accessor_ng.h"
#include "kis_selection.h"

namespace {

/// Enough to remember a long painting session on one layer, not so many that the file notices
constexpr int kept = 10000;
constexpr quint32 magic = 0x4b534c32; // "KSL2"
/// How far from a stroke's edge a point still is on it: the rounding of the points, and of the pixel
constexpr qreal slack = 1.5;
/// Paint fainter than this is looked through
constexpr qreal seen = 16.0 / 255.0;
/// Paint at least this strong is what is seen: what is under it is not looked at
constexpr qreal opaque = 0.5;
/// Points in a selection's outline kept with the strokes painted through it
constexpr int clipPoints = 4000;

quint64 nextId()
{
    static std::atomic<quint64> id {1};
    return id++;
}

bool sameFrame(int a, int b)
{
    // a device's content before it had frames is its first frame
    return a == b || (a == -1 && b == 0) || (a == 0 && b == -1);
}

/// In an even number of the outlines: outside (holes are outlines too)
bool inside(const QVector<QPolygon> &outlines, const QPoint &point)
{
    int count = 0;
    Q_FOREACH (const QPolygon &outline, outlines) {
        if (outline.boundingRect().contains(point) && outline.containsPoint(point, Qt::OddEvenFill)) count++;
    }
    return count % 2 == 1;
}

QRect boundsOf(const QVector<QPolygon> &outlines)
{
    QRect rect;
    Q_FOREACH (const QPolygon &outline, outlines) rect |= outline.boundingRect();
    return rect;
}

qreal distanceToSegment(const QPointF &a, const QPointF &b, const QPointF &point, qreal *t)
{
    const QPointF ab = b - a;
    const qreal length2 = ab.x() * ab.x() + ab.y() * ab.y();
    *t = length2 > 0 ? qBound(0.0, QPointF::dotProduct(point - a, ab) / length2, 1.0) : 0.0;
    const QPointF d = a + *t * ab - point;
    return qSqrt(d.x() * d.x() + d.y() * d.y());
}

/// Douglas-Peucker on a closed outline: the points that keep it within \p tolerance
QPolygon simplified(const QPolygon &outline, qreal tolerance)
{
    if (outline.size() < 4) return outline;
    QVector<bool> keep(outline.size(), false);
    keep[0] = true;
    keep[outline.size() - 1] = true;
    QVector<QPair<int, int>> spans {{0, outline.size() - 1}};
    while (!spans.isEmpty()) {
        const QPair<int, int> span = spans.takeLast();
        qreal worst = 0;
        int at = -1;
        for (int i = span.first + 1; i < span.second; i++) {
            qreal t;
            const qreal d = distanceToSegment(outline[span.first], outline[span.second], outline[i], &t);
            if (d > worst) {
                worst = d;
                at = i;
            }
        }
        if (at >= 0 && worst > tolerance) {
            keep[at] = true;
            spans << qMakePair(span.first, at) << qMakePair(at, span.second);
        }
    }
    QPolygon result;
    for (int i = 0; i < outline.size(); i++) {
        if (keep[i]) result << outline[i];
    }
    return result;
}

// ------------------------------------------------------------ the file

void writePoints(QDataStream &out, const QPolygon &points)
{
    out << quint32(points.size());
    QPoint previous;
    for (int i = 0; i < points.size(); i++) {
        const QPoint step = points[i] - previous;
        // small steps in two bytes; the first point, and a far jump, in full
        if (i > 0 && qAbs(step.x()) < 32767 && qAbs(step.y()) < 32767) {
            out << qint16(step.x()) << qint16(step.y());
        } else {
            if (i > 0) out << qint16(-32768);
            out << qint32(points[i].x()) << qint32(points[i].y());
        }
        previous = points[i];
    }
}

bool readPoints(QDataStream &in, QPolygon *points)
{
    quint32 count = 0;
    in >> count;
    if (in.status() != QDataStream::Ok || count > 10000000) return false;
    points->reserve(int(count));
    QPoint at;
    for (quint32 i = 0; i < count && in.status() == QDataStream::Ok; i++) {
        qint16 dx = -32768;
        if (i > 0) in >> dx;
        if (dx == -32768) {
            qint32 x = 0, y = 0;
            in >> x >> y;
            at = QPoint(x, y);
        } else {
            qint16 dy = 0;
            in >> dy;
            at += QPoint(dx, dy);
        }
        *points << at;
    }
    return in.status() == QDataStream::Ok;
}

void writeOutlines(QDataStream &out, const QVector<QPolygon> &outlines)
{
    out << quint32(outlines.size());
    Q_FOREACH (const QPolygon &outline, outlines) writePoints(out, outline);
}

bool readOutlines(QDataStream &in, QVector<QPolygon> *outlines)
{
    quint32 count = 0;
    in >> count;
    if (in.status() != QDataStream::Ok || count > 1000000) return false;
    for (quint32 i = 0; i < count; i++) {
        QPolygon outline;
        if (!readPoints(in, &outline)) return false;
        *outlines << outline;
    }
    return true;
}

// ------------------------------------------------------------ moving with the pixels

/// Outlines moved by \p shift, each selection once however many strokes were painted through it (still shared)
QVector<QPolygon> shifted(const QVector<QPolygon> &outlines, const QPoint &shift, QHash<const void *, QVector<QPolygon>> *done)
{
    if (outlines.isEmpty() || shift.isNull()) return outlines;
    auto it = done->constFind(outlines.constData());
    if (it != done->constEnd()) return *it;
    QVector<QPolygon> result = outlines;
    for (int k = 0; k < result.size(); k++) result[k].translate(shift);
    done->insert(outlines.constData(), result);
    return result;
}

/// How much \p mapper stretches things around \p point: the most it stretches any way
qreal stretchAt(const KisStrokeLog::Mapper &mapper, const QPointF &point, const QPointF &mapped)
{
    constexpr qreal step = 4.0;
    const QPointF dx = (mapper(point + QPointF(step, 0)) - mapped) / step;
    const QPointF dy = (mapper(point + QPointF(0, step)) - mapped) / step;
    // the larger singular value of the jacobian
    const qreal a = dx.x(), b = dy.x(), c = dx.y(), d = dy.y();
    const qreal s1 = a * a + b * b + c * c + d * d;
    const qreal s2 = qSqrt(qMax(0.0, qPow(a * a + b * b - c * c - d * d, 2) + 4 * qPow(a * c + b * d, 2)));
    return qSqrt((s1 + s2) / 2);
}

/// An outline with a point every \p step pixels at least: what a bending move bends along
QPolygonF densifiedOutline(const QPolygon &outline, qreal step)
{
    QPolygonF result;
    for (int i = 0; i < outline.size(); i++) {
        const QPointF a = outline[i];
        const QPointF b = outline[(i + 1) % outline.size()];
        const int n = qMax(1, qCeil(QLineF(a, b).length() / step));
        for (int k = 0; k < n; k++) result << a + (b - a) * (qreal(k) / n);
    }
    return result;
}

/// Outlines moved: bent along with the move, and only as many points kept as that needs
QVector<QPolygon> mapOutlines(const QVector<QPolygon> &outlines, const QPoint &before, const QPoint &after, const KisStrokeLog::Mapper &mapper)
{
    QVector<QPolygon> result;
    result.reserve(outlines.size());
    Q_FOREACH (const QPolygon &outline, outlines) {
        QPolygon mapped;
        Q_FOREACH (const QPointF &point, densifiedOutline(outline, 8.0)) mapped << (mapper(point + before) - after).toPoint();
        result << simplified(mapped, 1.0);
    }
    return result;
}

/**
 * A brush stroke as points every few pixels along it (image coordinates),
 * with the brush's width at each: what is tested against what moves, and
 * moved, point by point (two points far apart do not tell where a stroke
 * between them went, nor bend with a warp).
 */
struct Dense {
    QVector<QPointF> points;
    QVector<qreal> widths;
};

Dense densified(const KisStrokeLog::Stroke &stroke, const QPoint &offset)
{
    Dense dense;
    auto widthAt = [&](int i) { return stroke.size * (stroke.widths.isEmpty() ? 1.0 : quint8(stroke.widths[i]) / 255.0); };
    for (int i = 0; i < stroke.path.size(); i++) {
        const QPointF b = stroke.path[i] + offset;
        if (i > 0) {
            const QPointF a = stroke.path[i - 1] + offset;
            const qreal wa = widthAt(i - 1), wb = widthAt(i);
            const qreal step = qBound(2.0, qMin(wa, wb) / 2, 16.0);
            const int n = qMax(1, qCeil(QLineF(a, b).length() / step));
            for (int k = 1; k < n; k++) {
                const qreal t = qreal(k) / n;
                dense.points << a + (b - a) * t;
                dense.widths << wa + (wb - wa) * t;
            }
        }
        dense.points << b;
        dense.widths << widthAt(i);
    }
    return dense;
}

/// Points [from, to) of a dense stroke as a stroke again, with no more points than it needs
KisStrokeLog::Stroke build(const KisStrokeLog::Stroke &stroke, const QVector<QPointF> &points, const QVector<qreal> &widths,
                           int from, int to, const QPoint &offset)
{
    KisStrokeLog::Stroke result = stroke;
    result.path.clear();
    result.widths.clear();
    const QVector<int> kept = KisStrokeLog::simplifiedPath(points.mid(from, to - from), widths.mid(from, to - from));
    qreal widest = 0;
    Q_FOREACH (int k, kept) widest = qMax(widest, widths[from + k]);
    result.size = float(widest);
    Q_FOREACH (int k, kept) {
        result.path << (points[from + k] - offset).toPoint();
        result.widths.append(char(widest > 0 ? qBound(1, qRound(widths[from + k] / widest * 255), 255) : 255));
    }
    result.updateBounds();
    return result;
}

/// Points [from, to) of a dense stroke moved: where they go, as wide as the move stretches them, cut where it tears
KisStrokeLog::Strokes mapBrush(const KisStrokeLog::Stroke &stroke, const Dense &dense, int from, int to, const QPoint &before,
                               const QPoint &after, const KisStrokeLog::Mapper &mapper, const KisStrokeLog::Tears &tears,
                               QHash<const void *, QVector<QPolygon>> *clips)
{
    QVector<QPointF> points;
    QVector<qreal> widths;
    for (int i = from; i < to; i++) {
        const QPointF mapped = mapper(dense.points[i]);
        points << mapped;
        widths << dense.widths[i] * stretchAt(mapper, dense.points[i], mapped);
    }
    KisStrokeLog::Stroke moved = stroke;
    if (!stroke.clip.isEmpty()) {
        // the selection once for all the strokes painted through it, still shared
        const void *key = stroke.clip.constData();
        if (!clips->contains(key)) clips->insert(key, mapOutlines(stroke.clip, before, after, mapper));
        moved.clip = clips->value(key);
    }

    KisStrokeLog::Strokes result;
    int start = 0;
    for (int i = 1; i <= points.size(); i++) {
        const bool torn = i < points.size() && tears && tears(dense.points[from + i - 1], dense.points[from + i]);
        if (i < points.size() && !torn) continue;
        result << build(moved, points, widths, start, i, after);
        start = i;
    }
    return result;
}

} // namespace

// ------------------------------------------------------------ a stroke

void KisStrokeLog::Stroke::updateBounds()
{
    if (isBrush()) {
        const int radius = qCeil(size / 2 + slack) + 1;
        bounds = path.boundingRect().adjusted(-radius, -radius, radius, radius);
    } else {
        bounds = boundsOf(area);
    }
    if (!clip.isEmpty()) {
        bounds &= boundsOf(clip).adjusted(-1, -1, 1, 1);
    }
}

bool KisStrokeLog::Stroke::covers(const QPoint &point) const
{
    if (!bounds.contains(point)) return false;
    if (!clip.isEmpty() && !inside(clip, point)) return false;
    if (!isBrush()) return inside(area, point);
    if (path.isEmpty()) return false;

    auto radiusAt = [this](int i) { return size * (widths.isEmpty() ? 1.0 : quint8(widths[i]) / 255.0) / 2; };
    if (path.size() == 1) {
        qreal t;
        return distanceToSegment(path[0], path[0], point, &t) <= radiusAt(0) + slack;
    }
    for (int i = 1; i < path.size(); i++) {
        const qreal r0 = radiusAt(i - 1), r1 = radiusAt(i);
        // the segment's box first: most are far
        const int reach = qCeil(qMax(r0, r1) + slack);
        if (point.x() < qMin(path[i - 1].x(), path[i].x()) - reach || point.x() > qMax(path[i - 1].x(), path[i].x()) + reach
            || point.y() < qMin(path[i - 1].y(), path[i].y()) - reach || point.y() > qMax(path[i - 1].y(), path[i].y()) + reach) {
            continue;
        }
        qreal t;
        const qreal d = distanceToSegment(path[i - 1], path[i], point, &t);
        if (d <= r0 + (r1 - r0) * t + slack) return true;
    }
    return false;
}

// ------------------------------------------------------------ the log

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
            const int count = m_strokes.size() - kept + kept / 20;
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
        // they are the newest, as the undo of what came after is done
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

bool KisStrokeLog::strokeAt(const QPoint &devicePos, int frame, Stroke *result, bool *covered) const
{
    if (covered) *covered = false;
    QReadLocker locker(&m_lock);
    for (int i = m_strokes.size() - 1; i >= 0; i--) {
        const Stroke &stroke = m_strokes[i];
        if (!sameFrame(stroke.frame, frame) || stroke.erasing || !stroke.covers(devicePos)) continue;
        if (!stroke.isBrush()) {
            if (covered) *covered = true;
            return false;
        }
        if (result) *result = stroke;
        return true;
    }
    return false;
}

bool KisStrokeLog::lastBrush(Stroke *result) const
{
    QReadLocker locker(&m_lock);
    for (int i = m_strokes.size() - 1; i >= 0; i--) {
        if (m_strokes[i].isBrush() && !m_strokes[i].erasing) {
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
        if (!stroke.isBrush() || stroke.erasing || have.contains(stroke.color)) continue;
        have << stroke.color;
        colors << QColor::fromRgba(stroke.color);
    }
    return colors;
}

int KisStrokeLog::currentFrame(KisPaintDeviceSP device)
{
    return device && device->framesInterface() ? device->framesInterface()->currentFrameId() : -1;
}

QByteArray KisStrokeLog::save(const std::function<int(int)> &frameToTime) const
{
    const Strokes strokes = this->strokes();
    if (strokes.isEmpty()) {
        return QByteArray();
    }
    QStringList brushes;
    QHash<QString, int> brushIndex;
    QVector<QVector<QPolygon>> clips;
    QHash<const void *, int> clipIndex;
    Q_FOREACH (const Stroke &stroke, strokes) {
        if (stroke.isBrush() && !brushIndex.contains(stroke.brush)) {
            brushIndex.insert(stroke.brush, brushes.size());
            brushes << stroke.brush;
        }
        if (!stroke.clip.isEmpty() && !clipIndex.contains(stroke.clip.constData())) {
            clipIndex.insert(stroke.clip.constData(), clips.size());
            clips << stroke.clip;
        }
    }

    QByteArray data;
    QDataStream out(&data, QIODevice::WriteOnly);
    out.setVersion(QDataStream::Qt_5_12);
    out << magic << brushes << quint32(clips.size());
    Q_FOREACH (const QVector<QPolygon> &clip, clips) writeOutlines(out, clip);

    // the time of each one's frame; -1: no frames; -2: on a frame no longer there, not kept
    QVector<int> times;
    int count = 0;
    Q_FOREACH (const Stroke &stroke, strokes) {
        times << frameToTime(stroke.frame);
        if (times.last() != -2) count++;
    }
    out << quint32(count);
    for (int i = 0; i < strokes.size(); i++) {
        const Stroke &stroke = strokes[i];
        const int time = times[i];
        if (time == -2) continue;
        out << qint32(stroke.isBrush() ? brushIndex.value(stroke.brush) : -1) << stroke.brushSize << stroke.size << quint32(stroke.color)
            << quint8(stroke.erasing ? 1 : 0) << qint32(time)
            << qint32(stroke.clip.isEmpty() ? -1 : clipIndex.value(stroke.clip.constData()));
        if (stroke.isBrush()) {
            writePoints(out, stroke.path);
            out << stroke.widths;
        } else {
            writeOutlines(out, stroke.area);
        }
    }
    return qCompress(data, 9);
}

void KisStrokeLog::load(const QByteArray &compressed, const std::function<int(int)> &timeToFrame)
{
    const QByteArray data = qUncompress(compressed);
    QDataStream in(data);
    in.setVersion(QDataStream::Qt_5_12);
    quint32 head = 0;
    in >> head;
    if (head != magic) {
        return;
    }
    QStringList brushes;
    quint32 clipCount = 0;
    in >> brushes >> clipCount;
    if (in.status() != QDataStream::Ok || clipCount > 1000000) {
        return;
    }
    QVector<QVector<QPolygon>> clips;
    for (quint32 i = 0; i < clipCount; i++) {
        QVector<QPolygon> clip;
        if (!readOutlines(in, &clip)) return;
        clips << clip;
    }

    Strokes strokes;
    quint32 count = 0;
    in >> count;
    for (quint32 i = 0; i < count && in.status() == QDataStream::Ok; i++) {
        qint32 brush = -1, time = -1, clip = -1;
        quint32 color = 0;
        quint8 flags = 0;
        Stroke stroke;
        in >> brush >> stroke.brushSize >> stroke.size >> color >> flags >> time >> clip;
        stroke.id = nextId();
        stroke.brush = brushes.value(brush);
        stroke.color = color;
        stroke.erasing = flags & 1;
        stroke.frame = time < 0 ? -1 : timeToFrame(time);
        stroke.clip = clips.value(clip);
        if (brush >= 0) {
            if (!readPoints(in, &stroke.path)) break;
            in >> stroke.widths;
        } else if (!readOutlines(in, &stroke.area)) {
            break;
        }
        if (in.status() != QDataStream::Ok) break;
        if (!stroke.widths.isEmpty() && stroke.widths.size() != stroke.path.size()) stroke.widths.clear();
        // on a frame no longer there: nothing to find it on
        if (time >= 0 && stroke.frame < 0) continue;
        stroke.updateBounds();
        strokes << stroke;
    }
    set(strokes);
}

// ------------------------------------------------------------ the undo commands

class KisStrokeLogAddCommand : public KUndo2Command
{
public:
    KisStrokeLogAddCommand(KisNodeSP node, KisStrokeLog::Strokes strokes)
        : m_node(node)
    {
        // into the device's coordinates, on its current frame
        KisPaintDeviceSP device = node->paintDevice();
        const QPoint offset = device ? QPoint(device->x(), device->y()) : QPoint();
        const int frame = KisStrokeLog::currentFrame(device);
        QHash<const void *, QVector<QPolygon>> clips;
        for (int i = 0; i < strokes.size(); i++) {
            KisStrokeLog::Stroke &stroke = strokes[i];
            stroke.id = nextId();
            stroke.frame = frame;
            stroke.path.translate(-offset);
            for (int k = 0; k < stroke.area.size(); k++) stroke.area[k].translate(-offset);
            stroke.clip = shifted(stroke.clip, -offset, &clips);
            stroke.updateBounds();
            m_ids << stroke.id;
        }
        m_strokes = strokes;
    }

    void redo() override
    {
        m_dropped = m_node->strokeLog().append(m_strokes);
    }

    void undo() override
    {
        m_node->strokeLog().remove(m_ids);
        // the oldest ones let go to make room: back again
        m_node->strokeLog().restore(m_dropped);
        m_dropped.clear();
    }

private:
    KisNodeSP m_node;
    KisStrokeLog::Strokes m_strokes;
    KisStrokeLog::Strokes m_dropped;
    QVector<quint64> m_ids;
};

class KisStrokeLogSetCommand : public KUndo2Command
{
public:
    using Make = std::function<KisStrokeLog::Strokes(const KisStrokeLog::Strokes &)>;

    KisStrokeLogSetCommand(KisNodeSP node, Make make)
        : m_node(node)
        , m_make(make)
    {
    }

    void redo() override
    {
        // worked out when it happens: what the strokes are by then, and the frame shown then
        if (m_make) {
            m_before = m_node->strokeLog().strokes();
            m_after = m_make(m_before);
            m_make = Make();
        }
        m_node->strokeLog().set(m_after);
    }

    void undo() override
    {
        m_node->strokeLog().set(m_before);
    }

private:
    KisNodeSP m_node;
    Make m_make;
    KisStrokeLog::Strokes m_before;
    KisStrokeLog::Strokes m_after;
};

QVector<int> KisStrokeLog::simplifiedPath(const QVector<QPointF> &points, const QVector<qreal> &widths)
{
    QVector<int> result;
    if (points.size() < 3) {
        for (int i = 0; i < points.size(); i++) result << i;
        return result;
    }
    // Douglas-Peucker on where the brush went and how wide it was together
    QVector<bool> keep(points.size(), false);
    keep.first() = true;
    keep.last() = true;
    QVector<QPair<int, int>> spans {{0, points.size() - 1}};
    while (!spans.isEmpty()) {
        const QPair<int, int> span = spans.takeLast();
        const QPointF a = points[span.first], b = points[span.second];
        const qreal wa = widths[span.first], wb = widths[span.second];
        qreal worst = 0;
        int at = -1;
        for (int i = span.first + 1; i < span.second; i++) {
            qreal t;
            const qreal off = distanceToSegment(a, b, points[i], &t);
            const qreal widthOff = qAbs(widths[i] - (wa + (wb - wa) * t)) / 2;
            const qreal error = qMax(off, widthOff) / qMax(0.5, 0.1 * widths[i]);
            if (error > worst) {
                worst = error;
                at = i;
            }
        }
        if (at >= 0 && worst > 1.0) {
            keep[at] = true;
            spans << qMakePair(span.first, at) << qMakePair(at, span.second);
        }
    }
    for (int i = 0; i < points.size(); i++) {
        if (keep[i]) result << i;
    }
    return result;
}

KUndo2Command *KisStrokeLog::addCommand(KisNodeSP node, Strokes strokes)
{
    if (!node || strokes.isEmpty()) {
        return nullptr;
    }
    return new KisStrokeLogAddCommand(node, strokes);
}

KUndo2Command *KisStrokeLog::mapCommand(KisNodeSP node, const Move &move)
{
    if (!node || !move.map || node->strokeLog().isEmpty()) {
        return nullptr;
    }
    return new KisStrokeLogSetCommand(node, [node, move](const Strokes &before) {
        KisPaintDeviceSP device = node->paintDevice();
        // from the device as it was to the device as it is (moving the pixels may have moved it)
        const QPoint offsetAfter = device ? QPoint(device->x(), device->y()) : QPoint();
        const QPoint offsetBefore = move.offsetBefore.value_or(offsetAfter);
        const int frame = currentFrame(device);
        const Mapper &mapper = move.map;
        const Selected &selected = move.selected;
        QHash<const void *, QVector<QPolygon>> clips;
        Strokes stay;
        Strokes moved;
        Q_FOREACH (const Stroke &stroke, before) {
            if (!sameFrame(stroke.frame, frame)) {
                stay << stroke;
                continue;
            }
            // nowhere near what moves
            if (selected && !move.selectedBounds.isEmpty() && !stroke.bounds.translated(offsetBefore).intersects(move.selectedBounds)) {
                stay << stroke;
                continue;
            }
            if (!stroke.isBrush()) {
                // paint no brush made moves as a whole, if most of it is in what moves
                int in = 0, all = 0;
                Q_FOREACH (const QPolygon &outline, stroke.area) {
                    Q_FOREACH (const QPointF &point, densifiedOutline(outline, 8.0)) {
                        all++;
                        if (!selected || selected(QPoint(qFloor(point.x()), qFloor(point.y())) + offsetBefore)) in++;
                    }
                }
                if (in * 2 <= all) {
                    stay << stroke;
                    continue;
                }
                Stroke mapped = stroke;
                mapped.area = mapOutlines(stroke.area, offsetBefore, offsetAfter, mapper);
                if (!stroke.clip.isEmpty()) mapped.clip = mapOutlines(stroke.clip, offsetBefore, offsetAfter, mapper);
                mapped.updateBounds();
                moved << mapped;
                continue;
            }
            const Dense dense = densified(stroke, offsetBefore);
            const int count = dense.points.size();
            if (!selected) {
                moved += mapBrush(stroke, dense, 0, count, offsetBefore, offsetAfter, mapper, move.tears, &clips);
                continue;
            }
            // only the parts in what moves: the rest stays where it is
            QVector<bool> in(count);
            bool any = false;
            for (int i = 0; i < count; i++) {
                in[i] = selected(QPoint(qFloor(dense.points[i].x()), qFloor(dense.points[i].y())));
                any = any || in[i];
            }
            if (!any) {
                stay << stroke;
                continue;
            }
            int start = 0;
            for (int i = 1; i <= count; i++) {
                if (i < count && in[i] == in[start]) continue;
                if (in[start]) {
                    moved += mapBrush(stroke, dense, start, i, offsetBefore, offsetAfter, mapper, move.tears, &clips);
                } else {
                    stay << build(stroke, dense.points, dense.widths, start, i, offsetAfter);
                }
                start = i;
            }
        }
        // what moved is over what it was moved onto
        return stay + moved;
    });
}

KisStrokeLog::Move KisStrokeLog::moveIn(KisSelectionSP selection, Mapper map)
{
    Move move;
    move.map = map;
    if (!selection) {
        return move;
    }
    // asked point by point along every stroke near it: one accessor for all of them
    KisPixelSelectionSP pixels = selection->projection();
    QSharedPointer<KisRandomConstAccessorSP> accessor(new KisRandomConstAccessorSP(pixels->createRandomConstAccessorNG()));
    move.selected = [pixels, accessor](const QPoint &point) {
        (*accessor)->moveTo(point.x(), point.y());
        return *(*accessor)->rawDataConst() > MAX_SELECTED / 2;
    };
    move.selectedBounds = selection->selectedExactRect();
    return move;
}

KUndo2Command *KisStrokeLog::mergeCommand(KisNodeSP target, KisNodeList sources)
{
    if (!target) {
        return nullptr;
    }
    return new KisStrokeLogSetCommand(target, [target, sources](const Strokes &) {
        Strokes merged;
        KisPaintDeviceSP targetDevice = target->paintDevice();
        // frames are made anew in a merge: the strokes on them cannot be told apart
        if (!targetDevice || targetDevice->framesInterface()) {
            return merged;
        }
        const QPoint targetOffset(targetDevice->x(), targetDevice->y());

        // the visible layers in them, from the bottom up: what is merged
        std::function<void(KisNodeSP)> collect = [&](KisNodeSP node) {
            if (!node->visible() || node->inherits("KisMask")) return;
            if (node->childCount() > 0 && !node->paintDevice()) {
                for (KisNodeSP child = node->firstChild(); child; child = child->nextSibling()) collect(child);
                return;
            }
            KisPaintDeviceSP device = node->paintDevice();
            if (!device || device->framesInterface()) return;
            const QPoint shift = QPoint(device->x(), device->y()) - targetOffset;
            QHash<const void *, QVector<QPolygon>> clips;
            Q_FOREACH (Stroke stroke, node->strokeLog().strokes()) {
                stroke.path.translate(shift);
                for (int k = 0; k < stroke.area.size(); k++) stroke.area[k].translate(shift);
                stroke.clip = shifted(stroke.clip, shift, &clips);
                stroke.frame = -1;
                stroke.updateBounds();
                merged << stroke;
            }
        };
        Q_FOREACH (KisNodeSP source, sources) collect(source);
        if (merged.size() > kept) merged.remove(0, merged.size() - kept);
        return merged;
    });
}

QVector<QPolygon> KisStrokeLog::outlineOf(KisSelectionSP selection)
{
    if (!selection) {
        return QVector<QPolygon>();
    }
    // the same selection for many strokes: worked out once, and kept once
    static QMutex mutex;
    static KisSelectionWSP lastSelection;
    static int lastSequence = -1;
    static QRect lastRect;
    static QVector<QPolygon> lastOutline;

    KisPixelSelectionSP pixels = selection->projection();
    const int sequence = pixels->sequenceNumber();
    const QRect rect = selection->selectedExactRect();
    QMutexLocker locker(&mutex);
    if (lastSelection.isValid() && lastSelection.data() == selection.data() && sequence == lastSequence && rect == lastRect) {
        return lastOutline;
    }

    QVector<QPolygon> outline;
    if (selection->outlineCacheValid()) {
        Q_FOREACH (const QPolygonF &polygon, selection->outlineCache().toSubpathPolygons()) outline << polygon.toPolygon();
    } else {
        outline = pixels->outline();
    }
    // a pixel off is nothing; more points than are needed, taken away
    qreal tolerance = 1.0;
    QVector<QPolygon> result;
    while (true) {
        result.clear();
        int points = 0;
        Q_FOREACH (const QPolygon &polygon, outline) {
            const QPolygon simple = simplified(polygon, tolerance);
            if (simple.size() >= 3) {
                result << simple;
                points += simple.size();
            }
        }
        if (points <= clipPoints) break;
        if (tolerance >= 16) {
            // too fine to keep: where it is, roughly
            result = {QPolygon(rect)};
            break;
        }
        tolerance *= 2;
    }
    lastSelection = selection;
    lastSequence = sequence;
    lastRect = rect;
    lastOutline = result;
    return result;
}

// ------------------------------------------------------------ what is seen

namespace {

enum class Found { Nothing, Stroke, Covered };

Found pickIn(KisNodeSP parent, const QPoint &imagePos, qreal opacity, KisStrokeLog::Pick *result)
{
    for (KisNodeSP node = parent->lastChild(); node; node = node->prevSibling()) {
        if (!node->visible() || node->inherits("KisMask") || node->inherits("KisAdjustmentLayer")) continue;
        const qreal nodeOpacity = opacity * node->opacity() / 255.0;
        if (node->inherits("KisGroupLayer")) {
            const Found found = pickIn(node, imagePos, nodeOpacity, result);
            if (found != Found::Nothing) return found;
            continue;
        }
        KisPaintDeviceSP projection = node->projection();
        if (!projection) continue;
        KoColor pixel(projection->colorSpace());
        projection->pixel(imagePos.x(), imagePos.y(), &pixel);
        const qreal alpha = pixel.opacityF() * nodeOpacity;
        if (alpha < seen) continue;

        // a clone shows its source's paint, moved
        KisNodeSP owner = node;
        QPoint ownerPos = imagePos;
        if (KisCloneLayer *clone = qobject_cast<KisCloneLayer *>(node.data())) {
            owner = clone->copyFrom();
            ownerPos -= QPoint(clone->x(), clone->y());
        }
        KisPaintDeviceSP device = owner ? owner->paintDevice() : KisPaintDeviceSP();
        if (device) {
            const QPoint devicePos = ownerPos - QPoint(device->x(), device->y());
            // with the canvas wrapped around, a stroke may be a whole canvas (or more) away
            QVector<QPoint> places {devicePos};
            KisDefaultBoundsBaseSP bounds = device->defaultBounds();
            if (bounds && bounds->wrapAroundMode()) {
                const QRect wrap = bounds->bounds();
                const bool across = bounds->wrapAroundModeAxis() != WRAPAROUND_VERTICAL;
                const bool down = bounds->wrapAroundModeAxis() != WRAPAROUND_HORIZONTAL;
                for (int i = across ? -3 : 0; i <= (across ? 3 : 0); i++) {
                    for (int j = down ? -3 : 0; j <= (down ? 3 : 0); j++) {
                        if (i || j) places << devicePos + QPoint(i * wrap.width(), j * wrap.height());
                    }
                }
            }
            const int frame = KisStrokeLog::currentFrame(device);
            Q_FOREACH (const QPoint &place, places) {
                bool covered = false;
                if (owner->strokeLog().strokeAt(place, frame, &result->stroke, &covered)) {
                    result->node = owner;
                    return Found::Stroke;
                }
                if (covered) return Found::Covered;
            }
        }
        // paint no stroke is known for, that hides what is under it
        if (alpha >= opaque) return Found::Covered;
    }
    return Found::Nothing;
}

} // namespace

bool KisStrokeLog::pick(KisNodeSP root, const QPoint &imagePos, Pick *result)
{
    if (!root || !result) {
        return false;
    }
    return pickIn(root, imagePos, 1.0, result) == Found::Stroke;
}

KisStrokeLogNotifier *KisStrokeLog::notifier()
{
    static KisStrokeLogNotifier *notifier = []() {
        auto *object = new KisStrokeLogNotifier();
        // it lives in the main thread, whatever thread first asked for it
        if (QCoreApplication::instance()) object->moveToThread(QCoreApplication::instance()->thread());
        return object;
    }();
    return notifier;
}
