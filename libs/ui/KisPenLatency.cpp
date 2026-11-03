/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisPenLatency.h"

#include <QElapsedTimer>
#include <QFile>
#include <QMutex>
#include <QMutexLocker>
#include <QVector>

#include <algorithm>
#include <deque>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace KisPenLatency
{

namespace
{

struct Event {
    qreal strokeTime;
    QPointF at;
    qreal queued;
    qint64 arrived;
    qint64 painted {-1};
    qint64 uploaded {-1};
};

/// What one event's stages took, ms
struct Sample {
    double queued;
    double toPaint;
    double toUpload;
    double toScreen;
    double total;
};

struct State {
    bool enabled {false};
    QString path;
    QElapsedTimer clock;
    QMutex lock;
    std::deque<Event> waiting;
    QVector<Sample> samples;
    int strokes {0};
};

State &state()
{
    static State s;
    static const bool ready = []() {
        s.enabled = qEnvironmentVariableIsSet("KRITA_DEV_PROFILE") && qEnvironmentVariableIsSet("KRITA_DEV_LATENCY");
        if (s.enabled) {
            s.path = qEnvironmentVariable("KRITA_DEV_LATENCY");
            s.clock.start();
        }
        return true;
    }();
    Q_UNUSED(ready);
    return s;
}

double ms(qint64 from, qint64 to)
{
    return (to - from) / 1e6;
}

QString figures(QVector<double> values)
{
    if (values.isEmpty()) return QStringLiteral("-");
    std::sort(values.begin(), values.end());
    auto at = [&values](double share) { return values[std::min(int(values.size()) - 1, int(share * values.size()))]; };
    return QStringLiteral("%1/%2/%3").arg(at(0.5), 0, 'f', 1).arg(at(0.95), 0, 'f', 1).arg(values.last(), 0, 'f', 1);
}

} // namespace

bool enabled()
{
    return state().enabled;
}

void strokeStarted()
{
    State &s = state();
    if (!s.enabled) return;
    QMutexLocker locker(&s.lock);
    s.waiting.clear();
    s.samples.clear();
}

void arrived(qreal strokeTime, const QPointF &at, ulong eventTime)
{
    State &s = state();
    if (!s.enabled) return;
    // Qt stamps Windows' events with their message time; events it made itself (as the dev hooks') have another clock
    qreal queued = 0;
#ifdef Q_OS_WIN
    const DWORD waited = GetTickCount() - DWORD(eventTime);
    if (waited < 1000) queued = waited;
#else
    Q_UNUSED(eventTime);
#endif
    QMutexLocker locker(&s.lock);
    s.waiting.push_back({strokeTime, at, queued, s.clock.nsecsElapsed()});
}

void painted(qreal strokeTime)
{
    State &s = state();
    if (!s.enabled) return;
    QMutexLocker locker(&s.lock);
    const qint64 now = s.clock.nsecsElapsed();
    for (Event &event : s.waiting) {
        if (event.strokeTime > strokeTime) break;
        if (event.painted < 0) event.painted = now;
    }
}

void uploaded(const QRect &imageRect)
{
    State &s = state();
    if (!s.enabled) return;
    QMutexLocker locker(&s.lock);
    const qint64 now = s.clock.nsecsElapsed();
    const QRectF around = QRectF(imageRect).adjusted(-2, -2, 2, 2);
    for (Event &event : s.waiting) {
        if (event.painted < 0) break;
        if (event.uploaded < 0 && around.contains(event.at)) event.uploaded = now;
    }
}

void presented()
{
    State &s = state();
    if (!s.enabled) return;
    QMutexLocker locker(&s.lock);
    const qint64 now = s.clock.nsecsElapsed();
    while (!s.waiting.empty() && s.waiting.front().uploaded >= 0) {
        const Event &event = s.waiting.front();
        s.samples << Sample{event.queued, ms(event.arrived, event.painted), ms(event.painted, event.uploaded), ms(event.uploaded, now),
                            event.queued + ms(event.arrived, now)};
        s.waiting.pop_front();
    }
}

void strokeEnded()
{
    State &s = state();
    if (!s.enabled) return;
    QMutexLocker locker(&s.lock);
    // events that never reached the screen, by the stage they stopped at
    int unpainted = 0, unuploaded = 0, unshown = 0;
    for (const Event &event : s.waiting) {
        if (event.painted < 0) unpainted++;
        else if (event.uploaded < 0) unuploaded++;
        else unshown++;
    }
    if (s.samples.isEmpty() && s.waiting.empty()) return;
    QVector<double> queued, toPaint, toUpload, toScreen, total;
    Q_FOREACH (const Sample &sample, s.samples) {
        queued << sample.queued;
        toPaint << sample.toPaint;
        toUpload << sample.toUpload;
        toScreen << sample.toScreen;
        total << sample.total;
    }
    // median/95th/worst of each stage, then of the whole way
    QString line = QStringLiteral("stroke %1 events %2 queued %3 paint %4 upload %5 screen %6 total %7")
                       .arg(++s.strokes)
                       .arg(s.samples.size())
                       .arg(figures(queued), figures(toPaint), figures(toUpload), figures(toScreen), figures(total));
    if (unpainted + unuploaded + unshown) {
        line += QStringLiteral(" (not shown: %1 unpainted, %2 not uploaded, %3 no frame)").arg(unpainted).arg(unuploaded).arg(unshown);
    }
    line += QLatin1Char('\n');
    QFile file(s.path);
    if (file.open(QIODevice::Append | QIODevice::Text)) file.write(line.toUtf8());
    s.samples.clear();
    s.waiting.clear();
}

} // namespace KisPenLatency
