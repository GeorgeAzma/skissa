/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "KisReselect.h"

#include <kundo2stack.h>

#include "KisDocument.h"
#include "KisView.h"
#include "kis_canvas2.h"

KisReselect::KisReselect(QObject *parent)
    : QObject(parent)
{
}

void KisReselect::made(KisCanvas2 *canvas, const KUndo2MagicString &step, std::function<void()> again)
{
    KUndo2Stack *stack = canvas && canvas->imageView() && canvas->imageView()->document() ? canvas->imageView()->document()->undoStack()
                                                                                          : nullptr;
    if (stack != m_stack) {
        if (m_stack) disconnect(m_stack, nullptr, this, nullptr);
        m_stack = stack;
        if (stack) connect(stack, &KUndo2Stack::indexChanged, this, &KisReselect::indexChanged);
    }
    m_text = step.toString();
    m_again = again;
    m_step = nullptr;
    m_waiting = stack;
    m_pending = false;
}

void KisReselect::indexChanged()
{
    if (!m_waiting || !m_stack) return;
    const int count = m_stack->count();
    // the step arrives a moment after the selection is made; whatever else lands first is not it
    if (count == 0 || m_stack->index() != count || m_stack->command(count - 1)->text().toString() != m_text) return;
    m_step = m_stack->command(count - 1);
    m_waiting = false;
    if (m_pending) {
        m_pending = false;
        runAgain();
    }
}

void KisReselect::settingChanged()
{
    if (!m_again || !m_stack) return;
    if (m_waiting) {
        m_pending = true;
        return;
    }
    runAgain();
}

void KisReselect::runAgain()
{
    const int index = m_stack ? m_stack->index() : 0;
    // only while the click's selection is the last step done: never take back anything else
    if (!m_step || index == 0 || m_stack->command(index - 1) != m_step) {
        forget();
        return;
    }
    m_step = nullptr;
    m_waiting = true;
    // taken back as the stack does it, not through undo(), which would take back settings changed since first
    m_stack->setIndex(index - 1);
    m_again();
}

void KisReselect::forget()
{
    m_again = {};
    m_step = nullptr;
    m_waiting = false;
    m_pending = false;
}
