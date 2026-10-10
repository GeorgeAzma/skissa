/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISOPENGLGPUTIMING_H
#define KISOPENGLGPUTIMING_H

#include <cstdio>

#include <QOpenGLContext>
#include <QStringList>
#include <QVector>

/**
 * For development (KRITA_DEV_FRAMELOG set): how long the GPU took for the
 * stages of what is drawn, written to stderr. A query of the time elapsed
 * on the GPU for each stage, one after the other (ARB_timer_query; on
 * OpenGL ES, ANGLE's on Direct3D among them, EXT_disjoint_timer_query,
 * which has no timestamps there), read a frame later, when the GPU has
 * them: never waited for. Nothing where there are none. (Qt's own helper
 * wants a context of OpenGL 3.3, which Krita does not ask for.)
 */
class KisOpenGLGpuTiming
{
public:
    KisOpenGLGpuTiming(const QString &name, const QStringList &stages)
        : m_name(name)
        , m_stages(stages)
    {
    }

    static bool enabled()
    {
        static const bool on = qEnvironmentVariableIsSet("KRITA_DEV_FRAMELOG");
        return on;
    }

    /// Before the first stage; writes what the last frame's took, when the GPU has it
    void begin()
    {
        m_recording = false;
        if (!enabled() || !resolve()) return;
        if (m_pending) {
            int available = 0;
            m_getObjectiv(m_queries.last(), 0x8867 /* GL_QUERY_RESULT_AVAILABLE */, &available);
            if (!available) return;
            QString line = m_name;
            for (int i = 0; i < m_stages.size(); i++) {
                quint64 elapsed = 0;
                m_getObjectui64v(m_queries[i], 0x8866 /* GL_QUERY_RESULT */, &elapsed);
                line += QStringLiteral(" %1 %2").arg(m_stages[i]).arg(elapsed / 1000);
            }
            fprintf(stderr, "%s us\n", qPrintable(line));
            // stderr is buffered when it goes to a file
            fflush(stderr);
            m_pending = false;
        }
        m_begin(timeElapsed, m_queries[0]);
        m_recording = true;
        m_stage = 0;
    }

    /// After each stage
    void mark()
    {
        if (!m_recording) return;
        m_end(timeElapsed);
        if (++m_stage == m_stages.size()) {
            m_recording = false;
            m_pending = true;
            return;
        }
        m_begin(timeElapsed, m_queries[m_stage]);
    }

private:
    static constexpr unsigned timeElapsed = 0x88BF; // GL_TIME_ELAPSED

    bool resolve()
    {
        if (m_failed) return false;
        if (!m_queries.isEmpty()) return true;
        QOpenGLContext *context = QOpenGLContext::currentContext();
        const bool es = context && context->isOpenGLES();
        if (!context || !context->hasExtension(es ? "GL_EXT_disjoint_timer_query" : "GL_ARB_timer_query")) {
            fprintf(stderr, "%s no timer queries here\n", qPrintable(m_name));
            fflush(stderr);
            m_failed = true;
            return false;
        }
        const QByteArray suffix = es ? "EXT" : "";
        auto address = [context, suffix](const char *name) { return context->getProcAddress(QByteArray(name) + suffix); };
        auto genQueries = reinterpret_cast<void(QOPENGLF_APIENTRYP)(int, unsigned *)>(address("glGenQueries"));
        m_begin = reinterpret_cast<void(QOPENGLF_APIENTRYP)(unsigned, unsigned)>(address("glBeginQuery"));
        m_end = reinterpret_cast<void(QOPENGLF_APIENTRYP)(unsigned)>(address("glEndQuery"));
        m_getObjectiv = reinterpret_cast<void(QOPENGLF_APIENTRYP)(unsigned, unsigned, int *)>(address("glGetQueryObjectiv"));
        m_getObjectui64v = reinterpret_cast<void(QOPENGLF_APIENTRYP)(unsigned, unsigned, quint64 *)>(address("glGetQueryObjectui64v"));
        if (!genQueries || !m_begin || !m_end || !m_getObjectiv || !m_getObjectui64v) {
            fprintf(stderr, "%s timer query functions missing\n", qPrintable(m_name));
            fflush(stderr);
            m_failed = true;
            return false;
        }
        m_queries.resize(m_stages.size());
        genQueries(m_queries.size(), m_queries.data());
        return true;
    }

    QString m_name;
    QStringList m_stages;
    QVector<unsigned> m_queries;
    void(QOPENGLF_APIENTRYP m_begin)(unsigned, unsigned) {nullptr};
    void(QOPENGLF_APIENTRYP m_end)(unsigned) {nullptr};
    void(QOPENGLF_APIENTRYP m_getObjectiv)(unsigned, unsigned, int *) {nullptr};
    void(QOPENGLF_APIENTRYP m_getObjectui64v)(unsigned, unsigned, quint64 *) {nullptr};
    bool m_failed {false};
    bool m_pending {false};
    bool m_recording {false};
    int m_stage {0};
};

#endif // KISOPENGLGPUTIMING_H
