/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioCanvasSize.h"

#include <QHBoxLayout>
#include <QPointer>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <KoToolBase.h>
#include <KoToolManager.h>
#include <KisModernStyle.h>
#include <klocalizedstring.h>

#include "KisMainWindow.h"
#include "KisStudioTools.h"
#include "KisViewManager.h"
#include "kis_action.h"
#include "kis_action_manager.h"
#include "kis_canvas2.h"
#include "kis_coordinates_converter.h"
#include "kis_icon_utils.h"
#include "widgets/KisStudioPopup.h"
#include "widgets/KisStudioValueRow.h"

namespace
{
/// The size as numbers beside the handles: typed or dragged, the canvas grows evenly around its middle; the handles
/// and the rows follow each other. Gone when the resize ends, however it ends.
class SizePanel : public QObject
{
    Q_OBJECT
public:
    SizePanel(KoToolBase *tool, KisMainWindow *window, KisCanvas2 *canvas)
        : QObject(window)
        , m_tool(tool)
    {
        m_popup = new KisStudioPopup(window);
        m_popup->setAttribute(Qt::WA_DeleteOnClose);
        // the handles on the canvas are part of it: pressing them doesn't close it
        m_popup->setPinned(true);
        auto *layout = new QVBoxLayout(m_popup);
        m_popup->fitLayout(layout, KisModernStyle::space3);
        layout->setSpacing(KisModernStyle::space2);

        const QSize now(tool->property("cropWidth").toInt(), tool->property("cropHeight").toInt());
        m_shape = qreal(now.height()) / qMax(1, now.width());
        auto rangeOf = [](int side) {
            KisStudioValueRow::Range range;
            // from a sixteenth to four times as large, as Image size: the row's length is spent where sizes are wanted
            range.minimum = qMax(1, side / 16);
            range.maximum = side * 4;
            range.suffix = i18nc("pixels, after a number", " px");
            range.size = true;
            return range;
        };
        m_width = new KisStudioValueRow(i18nc("the canvas's width", "Width"), m_popup);
        m_height = new KisStudioValueRow(i18nc("the canvas's height", "Height"), m_popup);
        m_width->setRange(rangeOf(now.width()));
        m_height->setRange(rangeOf(now.height()));
        for (KisStudioValueRow *row : {m_width, m_height}) row->setDefaultValue(row == m_width ? now.width() : now.height());
        show();

        m_chain = new QToolButton(m_popup);
        m_chain->setCheckable(true);
        m_chain->setAutoRaise(true);
        auto showChain = [this]() {
            m_chain->setIcon(KisIconUtils::loadIcon(m_chain->isChecked() ? QStringLiteral("studio_link") : QStringLiteral("studio_unlink")));
        };
        showChain();
        connect(m_chain, &QToolButton::toggled, m_chain, showChain);
        m_chain->setToolTip(i18nc("@info:tooltip width and height change together", "Keep the shape"));

        auto *sides = new QHBoxLayout;
        auto *rows = new QVBoxLayout;
        rows->setSpacing(KisModernStyle::hairline);
        rows->addWidget(m_width);
        rows->addWidget(m_height);
        sides->addLayout(rows, 1);
        sides->addWidget(m_chain);
        layout->addLayout(sides);

        auto *ends = new QHBoxLayout;
        ends->addStretch(1);
        auto end = [this, ends](const QString &icon, const QString &tip, const char *slot) {
            auto *button = new QToolButton(m_popup);
            button->setAutoRaise(true);
            button->setFocusPolicy(Qt::NoFocus);
            button->setIcon(KisIconUtils::loadIcon(icon));
            button->setIconSize(QSize(20, 20));
            button->setToolTip(tip);
            QPointer<KoToolBase> tool = m_tool;
            connect(button, &QToolButton::clicked, this, [tool, slot]() {
                if (tool) QMetaObject::invokeMethod(tool, slot);
            });
            ends->addWidget(button);
        };
        end(QStringLiteral("studio_cancel"), i18nc("@info:tooltip", "Cancel (Esc)"), "requestStrokeCancellation");
        end(QStringLiteral("studio_done"), i18nc("@info:tooltip", "Done (Enter)"), "requestStrokeEnd");
        layout->addLayout(ends);

        connect(m_width, &KisStudioValueRow::valueChanged, this, [this](qreal value) { typed(qRound(value), m_chain->isChecked() ? qRound(value * m_shape) : -1); });
        connect(m_height, &KisStudioValueRow::valueChanged, this, [this](qreal value) { typed(m_chain->isChecked() ? qRound(value / m_shape) : -1, qRound(value)); });
        connect(tool, SIGNAL(cropWidthChanged(int)), this, SLOT(show()));
        connect(tool, SIGNAL(cropHeightChanged(int)), this, SLOT(show()));
        connect(tool, SIGNAL(canvasResizeEnded()), this, SLOT(finish()));
        // closed by itself (Esc in it): the resize is dropped with it
        connect(m_popup, &QObject::destroyed, this, [this]() {
            if (!m_ended && m_tool) QMetaObject::invokeMethod(m_tool, "requestStrokeCancellation");
            deleteLater();
        });

        // at the bottom, where effects keep their panels, under the picture's left quarter: clear of the handles at
        // its bottom corner and middle, and of the panels at the window's right
        m_popup->adjustSize();
        QWidget *widget = canvas ? canvas->canvasWidget() : nullptr;
        const QRect area = widget ? QRect(widget->mapToGlobal(QPoint()), widget->size()) : window->geometry();
        int middle = area.center().x();
        if (widget) {
            const QRectF picture = canvas->coordinatesConverter()->imageRectInWidgetPixels();
            middle = widget->mapToGlobal(QPoint(qRound(picture.left() + picture.width() / 4), 0)).x();
        }
        m_popup->popupAt(QPoint(middle - m_popup->width() / 2, area.bottom() - m_popup->height() - KisModernStyle::space3));
    }

public Q_SLOTS:
    void show()
    {
        if (!m_tool || m_typing) return;
        m_width->setValue(m_tool->property("cropWidth").toInt());
        m_height->setValue(m_tool->property("cropHeight").toInt());
    }
    void finish()
    {
        m_ended = true;
        if (m_popup) m_popup->close();
    }

private:
    /// A side typed or dragged (below 0: unchanged), the canvas kept centered where it is
    void typed(int width, int height)
    {
        if (!m_tool) return;
        m_typing = true;
        const QRect was(m_tool->property("cropX").toInt(), m_tool->property("cropY").toInt(), m_tool->property("cropWidth").toInt(), m_tool->property("cropHeight").toInt());
        if (width > 0 && width != was.width()) {
            m_tool->setProperty("cropWidth", width);
            m_tool->setProperty("cropX", was.x() - (width - was.width()) / 2);
            m_width->setValue(width);
        }
        if (height > 0 && height != was.height()) {
            m_tool->setProperty("cropHeight", height);
            m_tool->setProperty("cropY", was.y() - (height - was.height()) / 2);
            m_height->setValue(height);
        }
        m_typing = false;
    }

    QPointer<KoToolBase> m_tool;
    QPointer<KisStudioPopup> m_popup;
    KisStudioValueRow *m_width {nullptr};
    KisStudioValueRow *m_height {nullptr};
    QToolButton *m_chain {nullptr};
    qreal m_shape {1};
    bool m_typing {false};
    bool m_ended {false};
};
}

namespace KisStudioCanvasSize
{

void start(KisViewManager *view)
{
    KisCanvas2 *canvas = view ? view->canvasBase() : nullptr;
    KoToolManager *tools = view ? KisStudioTools::manager(view->mainWindow()) : nullptr;
    if (!canvas || !canvas->canvasWidget() || !tools) return;
    const QString returnTo = tools->activeToolId();

    // the whole canvas with room around it to pull its edges into: fitted between the bars and panels, two steps back
    for (const char *name : {"zoom_to_fit", "view_zoom_out", "view_zoom_out"}) {
        if (KisAction *action = view->actionManager()->actionByName(QLatin1String(name))) action->trigger();
    }

    tools->switchToolRequested(QStringLiteral("KisToolCrop"));
    // the tool is active once the switch is through
    QPointer<KisCanvas2> alive(canvas);
    QPointer<KoToolManager> manager(tools);
    QTimer::singleShot(0, canvas, [alive, manager, returnTo]() {
        if (!alive || !manager) return;
        if (KoToolBase *tool = manager->toolById(alive, QStringLiteral("KisToolCrop"))) {
            QMetaObject::invokeMethod(tool, "beginCanvasResize", Q_ARG(QString, returnTo));
            if (KisViewManager *view = alive->viewManager()) {
                if (view->mainWindow()) new SizePanel(tool, view->mainWindow(), alive);
            }
        }
    });
}

} // namespace KisStudioCanvasSize

#include "KisStudioCanvasSize.moc"
