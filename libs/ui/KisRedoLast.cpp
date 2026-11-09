/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "KisRedoLast.h"

#include <kundo2stack.h>

#include "KisDocument.h"
#include "KisView.h"
#include "kis_canvas2.h"

KisRedoLast::KisRedoLast(QObject *parent)
    : QObject(parent)
{
}

void KisRedoLast::made(KisCanvas2 *canvas, const KUndo2MagicString &step, std::function<void()> again)
{
    made(canvas && canvas->imageView() && canvas->imageView()->document() ? canvas->imageView()->document()->undoStack() : nullptr,
         step, again);
}

void KisRedoLast::made(KUndo2Stack *stack, const KUndo2MagicString &step, std::function<void()> again)
{
    if (stack != m_stack) {
        if (m_stack) disconnect(m_stack, nullptr, this, nullptr);
        m_stack = stack;
        if (stack) connect(stack, &KUndo2Stack::indexChanged, this, &KisRedoLast::indexChanged);
    }
    m_text = step.toString();
    m_again = again;
    m_step = nullptr;
    m_waiting = stack;
    m_pending = false;
}

void KisRedoLast::indexChanged()
{
    if (!m_waiting || !m_stack) return;
    const int count = m_stack->count();
    // the step arrives a moment after the click; whatever else lands first is not it
    if (count == 0 || m_stack->index() != count || m_stack->command(count - 1)->text().toString() != m_text) return;
    m_step = m_stack->command(count - 1);
    m_waiting = false;
    if (m_pending) {
        m_pending = false;
        runAgain();
    }
}

void KisRedoLast::settingChanged()
{
    if (!m_again || !m_stack) return;
    if (m_waiting) {
        m_pending = true;
        return;
    }
    runAgain();
}

bool KisRedoLast::isNewest() const
{
    const int index = m_stack ? m_stack->index() : 0;
    return m_step && index > 0 && m_stack->command(index - 1) == m_step;
}

void KisRedoLast::runAgain()
{
    // only while the click's step is the last one done: never take back anything else
    if (!isNewest()) {
        forget();
        return;
    }
    m_step = nullptr;
    m_waiting = true;
    // taken back as the stack does it, not through undo(), which would take back settings changed since first
    m_stack->setIndex(m_stack->index() - 1);
    // a copy: doing it again may note a new click, replacing what runs
    const std::function<void()> again = m_again;
    again();
}

bool KisRedoLast::takeBack()
{
    if (!isNewest()) return false;
    m_stack->setIndex(m_stack->index() - 1);
    forget();
    return true;
}

void KisRedoLast::forget()
{
    m_again = {};
    m_step = nullptr;
    m_waiting = false;
    m_pending = false;
}
