/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIOSELECTIONADJUST_H
#define KISSTUDIOSELECTIONADJUST_H

#include <QObject>
#include <QTimer>

#include <kis_types.h>

#include <atomic>
#include <memory>

class KisProcessingApplicator;

/**
 * The selection there is, grown (or shrunk) and softened as a value is dragged: each change is applied to the
 * selection as it was when the drag began, and the whole drag is one undo step.
 */
class KisStudioSelectionAdjust : public QObject
{
public:
    explicit KisStudioSelectionAdjust(QObject *parent = nullptr);
    ~KisStudioSelectionAdjust() override;

    /// From the current view's selection, if it has one
    void begin();
    /// \p grow px (below 0: shrink), then \p soft px of feather; applied soon, so a fast drag doesn't queue every step
    void set(int grow, int soft);
    /// Applies what was set last and ends the step
    void end();
    bool isActive() const { return m_applicator; }
    /// Whether the image ended the drag before it was let go (closed, saved, undone); asked once, at the release
    bool takeInterrupted()
    {
        const bool was = m_interrupted;
        m_interrupted = false;
        return was;
    }

    /// What the drag has made of the selection so far
    struct Live;

private:
    void apply();

    KisImageWSP m_image;
    KisSelectionSP m_selection;
    std::shared_ptr<Live> m_live;
    KisProcessingApplicator *m_applicator {nullptr};
    int m_grow {0};
    int m_soft {0};
    bool m_pending {false};
    /// The amounts the selection was last made with, so letting go doesn't make it again
    int m_madeGrow {0};
    int m_madeSoft {0};
    bool m_ending {false};
    bool m_interrupted {false};
    /// A value is being made; the next waits for it
    std::shared_ptr<std::atomic<bool>> m_busy {std::make_shared<std::atomic<bool>>(false)};
    QTimer m_timer;
    /// To the image's requests to end or cancel strokes, which wait for this one too
    QMetaObject::Connection m_endRequest;
    QMetaObject::Connection m_cancelRequest;
};

#endif // KISSTUDIOSELECTIONADJUST_H
