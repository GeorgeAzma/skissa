/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef KISTOOLSUPERELLIPSE_H
#define KISTOOLSUPERELLIPSE_H

#include <QPointer>

#include <KisRedoLast.h>
#include <KisSuperellipse.h>
#include <KoIcon.h>
#include <kis_tool_rectangle_base.h>

class KisStudioValueRow;
class QAbstractButton;

/**
 * The shape tool: a box dragged on the image (Shift square, Alt from its middle) becomes one superellipse
 * (KisSuperellipse), its look picked from presets or set by Roundness and Curve. Changing the look right after
 * drawing reshapes what was just drawn, as one step.
 */
class KisToolSuperellipse : public KisToolRectangleBase
{
    Q_OBJECT
public:
    explicit KisToolSuperellipse(KoCanvasBase *canvas);
    ~KisToolSuperellipse() override;

    bool supportsPaintingAssistants() const override { return true; }
    QList<QPointer<QWidget>> createOptionWidgets() override;

public Q_SLOTS:
    void activate(const QSet<KoShape *> &shapes) override;
    void deactivate() override;

protected:
    void finishRect(const QRectF &rect, qreal roundCornersX, qreal roundCornersY) override;
    void paintRectangle(QPainter &gc, const QRectF &imageRect) override;
    bool showRoundCornersGUI() const override { return false; }

private:
    void draw(const QRectF &rect, qreal angle);
    void setLook(const KisSuperellipse::Look &look);
    void showLook();

    KisSuperellipse::Look m_look;
    KisRedoLast m_redoLast;
    QPointer<QWidget> m_options;
    QPointer<KisStudioValueRow> m_roundness;
    QPointer<KisStudioValueRow> m_curve;
    QVector<QPointer<QAbstractButton>> m_presetButtons;
};

class KisToolSuperellipseFactory : public KisToolPaintFactoryBase
{
public:
    KisToolSuperellipseFactory();
    ~KisToolSuperellipseFactory() override {}

    KoToolBase *createTool(KoCanvasBase *canvas) override { return new KisToolSuperellipse(canvas); }
};

#endif
