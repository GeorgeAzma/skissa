/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KIS_STUDIO_CANVAS_HANDLES_H
#define KIS_STUDIO_CANVAS_HANDLES_H

#include <QColor>
#include <QObject>
#include <QPointer>
#include <QPointF>
#include <QTransform>
#include <QVector>
#include <functional>

#include "kritaui_export.h"

class KisCanvas2;
class QPainter;

/**
 * Handles on the picture for what was just done (a gradient's ends, a pattern's place, a selection's edge), there until
 * something else is done: a press away from them, another tool's step, Enter or Esc. A press on a handle drags it, under
 * any tool, before the tool sees it. One set at a time: starting another ends the one before.
 */
class KRITAUI_EXPORT KisStudioCanvasHandles : public QObject
{
public:
    struct Handle {
        /// In image pixels
        QPointF at;
        /// Its fill; invalid: the handle is a small plain dot (moving a whole thing)
        QColor fill;
        /// Drawn with a soft edge: it softens something
        bool soft {false};
    };

    /// A new set of handles on \p canvas, ending the one before
    static KisStudioCanvasHandles *start(KisCanvas2 *canvas);
    /// Ends the set shown, if any
    static void endAll();

    void setHandles(const QVector<Handle> &handles);
    QVector<Handle> handles() const { return m_handles; }
    /// Ends this set: its handles go, and \p ended runs
    void end();

    /// A handle dragged to \p at (image pixels), Shift held or not
    std::function<void(int index, const QPointF &at, Qt::KeyboardModifiers modifiers)> moved;
    /// The drag let go
    std::function<void(int index)> released;
    /// Drawn under the handles, in widget coordinates: lines between them, an outline
    std::function<void(QPainter &painter, const QTransform &imageToWidget)> drawUnder;
    /// Checked when the history changes: false ends the set (its step was taken back or built on)
    std::function<bool()> alive;
    /// The set ended
    std::function<void()> ended;

    KisCanvas2 *canvas() const;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    explicit KisStudioCanvasHandles(KisCanvas2 *canvas);
    ~KisStudioCanvasHandles() override;
    void repaint();
    int handleAt(const QPointF &widgetPos) const;

    struct Private;
    Private *const d;
    QVector<Handle> m_handles;
};

#endif
