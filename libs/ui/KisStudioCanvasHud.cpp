/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioCanvasHud.h"
#include "KisStudioGlass.h"
#include <QPainterPath>

#include <QAction>
#include <QEvent>
#include <QHBoxLayout>
#include <QPainter>
#include <QPointer>
#include <QProgressBar>
#include <QStatusBar>
#include <QStyleOptionToolButton>
#include <QToolButton>
#include <QtMath>
#include <QWheelEvent>

#include <cmath>

#include <KoCanvasController.h>
#include <kactioncollection.h>
#include <klocalizedstring.h>

#include "KisMainWindow.h"
#include "KisView.h"
#include "canvas/kis_canvas2.h"
#include "canvas/kis_canvas_controller.h"
#include "canvas/kis_coordinates_converter.h"
#include <KisModernStyle.h>

namespace {

QToolButton *hudButton(QWidget *parent)
{
    auto *button = new QToolButton(parent);
    button->setAutoRaise(true);
    button->setFocusPolicy(Qt::NoFocus);
    button->setIconSize(QSize(16, 16));
    button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    return button;
}

/**
 * The zoom, set as a reading: the number semibold, what it is in (the %)
 * smaller and fainter, on the number's baseline. Its digits are all as wide,
 * so the pill keeps its width as the zoom changes.
 */
class ZoomButton : public QToolButton
{
public:
    using QToolButton::QToolButton;

    QSize sizeHint() const override
    {
        const QSize plain = QToolButton::sizeHint();
        const int padding = plain.width() - fontMetrics().horizontalAdvance(text());
        return QSize(qMax(0, padding) + qCeil(textWidth(numberFont(), unitFont())), plain.height());
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        QStyleOptionToolButton option;
        initStyleOption(&option);
        option.text.clear();
        style()->drawComplexControl(QStyle::CC_ToolButton, &option, &painter, this);

        const QFont number = numberFont();
        const QFont unit = unitFont();
        const QFontMetricsF numberMetrics(number);
        const QFontMetricsF unitMetrics(unit);
        // the line centered as a number is: by its digits' height, not the font's
        const qreal baseline = rect().center().y() + 0.5 + numberMetrics.capHeight() / 2;
        qreal x = (width() - textWidth(number, unit)) / 2;
        QColor ink = palette().color(QPalette::ButtonText);
        const QColor faint = KisModernStyle::secondaryInk(ink);
        for (const Run &run : runs()) {
            painter.setFont(run.number ? number : unit);
            painter.setPen(run.number ? ink : faint);
            painter.drawText(QPointF(x, baseline), run.text);
            x += (run.number ? numberMetrics : unitMetrics).horizontalAdvance(run.text) + (run.number ? 0 : 0.5);
        }
    }

private:
    struct Run {
        QString text;
        bool number;
    };

    /// The text in runs of the number (digits, its separators) and the rest
    QList<Run> runs() const
    {
        QList<Run> runs;
        Q_FOREACH (const QChar c, text()) {
            const bool number = c.isDigit() || c == QLatin1Char('.') || c == QLatin1Char(',');
            if (runs.isEmpty() || runs.last().number != number) {
                runs.append({QString(), number});
            }
            runs.last().text += c;
        }
        return runs;
    }

    qreal textWidth(const QFont &number, const QFont &unit) const
    {
        qreal sum = 0;
        for (const Run &run : runs()) {
            sum += QFontMetricsF(run.number ? number : unit).horizontalAdvance(run.text) + (run.number ? 0 : 0.5);
        }
        return sum;
    }

    QFont numberFont() const
    {
        return KisModernStyle::textFont(font(), 1.0, QFont::DemiBold);
    }

    QFont unitFont() const
    {
        return KisModernStyle::textFont(font(), 0.8);
    }
};

} // namespace

struct KisStudioCanvasHud::Private
{
    KisMainWindow *mainWindow {nullptr};
    bool active {false};

    QWidget *progressBox {nullptr};
    QPointer<QWidget> progress;

    QToolButton *mirror {nullptr};
    QToolButton *rotation {nullptr};
    QToolButton *zoom {nullptr};

    QPointer<KisView> view;
    QList<QMetaObject::Connection> viewConnections;

    QAction *action(const char *name) const
    {
        return mainWindow->actionCollection()->action(QLatin1String(name));
    }

    void trigger(const char *name) const
    {
        if (QAction *found = action(name)) {
            found->trigger();
        }
    }
};

KisStudioCanvasHud::KisStudioCanvasHud(KisMainWindow *mainWindow)
    : QWidget(mainWindow->centralWidget())
    , d(new Private)
{
    d->mainWindow = mainWindow;
    setObjectName(QStringLiteral("studioCanvasHud"));
    setAttribute(Qt::WA_TranslucentBackground);

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(6, 3, 6, 3);
    layout->setSpacing(2);

    d->progressBox = new QWidget(this);
    auto *progressLayout = new QHBoxLayout(d->progressBox);
    progressLayout->setContentsMargins(0, 0, 4, 0);
    d->progressBox->hide();
    layout->addWidget(d->progressBox);

    d->mirror = hudButton(this);
    if (QAction *mirror = d->action("mirror_canvas")) {
        d->mirror->setDefaultAction(mirror);
        d->mirror->setToolButtonStyle(Qt::ToolButtonIconOnly);
    }
    layout->addWidget(d->mirror);

    // only there while the canvas is rotated; a click puts it straight again
    d->rotation = hudButton(this);
    d->rotation->setToolTip(i18nc("@info:tooltip", "Rotated: click to straighten"));
    connect(d->rotation, &QToolButton::clicked, this, [this]() { d->trigger("reset_canvas_rotation"); });
    layout->addWidget(d->rotation);

    // no menu: a click flips between fitting the window and the last zoom, the wheel zooms
    d->zoom = new ZoomButton(this);
    d->zoom->setAutoRaise(true);
    d->zoom->setFocusPolicy(Qt::NoFocus);
    d->zoom->setToolButtonStyle(Qt::ToolButtonTextOnly);
    d->zoom->setToolTip(i18nc("@info:tooltip", "Zoom: click to fit or go back, scroll to zoom"));
    d->zoom->installEventFilter(this);
    connect(d->zoom, &QToolButton::clicked, this, [this]() { d->trigger("toggle_zoom_to_fit"); });
    layout->addWidget(d->zoom);

    parentWidget()->installEventFilter(this);
    connect(mainWindow, &KisMainWindow::activeViewChanged, this, &KisStudioCanvasHud::viewChanged);
    hide();
}

KisStudioCanvasHud::~KisStudioCanvasHud() = default;

void KisStudioCanvasHud::setActive(bool active)
{
    if (d->active == active) {
        return;
    }
    d->active = active;

    QStatusBar *statusBar = d->mainWindow->statusBar();
    if (active) {
        // long operations report where the work is seen
        QWidget *progress = statusBar ? statusBar->findChild<QWidget *>(QStringLiteral("ProgressBar")) : nullptr;
        if (progress) {
            statusBar->removeWidget(progress);
            d->progressBox->layout()->addWidget(progress);
            progress->show();
            d->progress = progress;
            // the bar shows and hides itself; the box only takes room while it is there
            if (QProgressBar *bar = progress->findChild<QProgressBar *>()) {
                bar->installEventFilter(this);
                d->progressBox->setVisible(!bar->isHidden());
            }
        }
    } else {
        if (d->progress && statusBar) {
            if (QProgressBar *bar = d->progress->findChild<QProgressBar *>()) {
                bar->removeEventFilter(this);
            }
            d->progressBox->layout()->removeWidget(d->progress);
            statusBar->addWidget(d->progress);
            d->progress->show();
        }
        d->progress = nullptr;
        d->progressBox->hide();
    }
    viewChanged();
}

void KisStudioCanvasHud::viewChanged()
{
    Q_FOREACH (const QMetaObject::Connection &connection, d->viewConnections) {
        disconnect(connection);
    }
    d->viewConnections.clear();

    KisView *view = d->mainWindow->activeView();
    d->view = view;
    if (!view || !d->active) {
        hide();
        return;
    }

    KisCanvasController *controller = view->canvasController();
    if (controller) {
        d->viewConnections << connect(controller->proxyObject, &KoCanvasControllerProxyObject::effectiveZoomChanged,
                                      this, &KisStudioCanvasHud::updateZoom);
        d->viewConnections << connect(controller->proxyObject, &KoCanvasControllerProxyObject::documentRotationChanged,
                                      this, &KisStudioCanvasHud::updateRotation);
    }
    if (KisCanvas2 *canvas = view->canvasBase()) {
        updateZoom(canvas->coordinatesConverter()->effectiveZoom());
        updateRotation(canvas->rotationAngle());
    }

    show();
    raise();
    reposition();
}

void KisStudioCanvasHud::updateZoom(qreal zoom)
{
    d->zoom->setText(i18nc("zoom level, %1 is the percentage", "%1%", QString::number(zoom * 100.0, 'f', zoom < 0.1 ? 1 : 0)));
    reposition();
}

void KisStudioCanvasHud::updateRotation(qreal angle)
{
    qreal normalized = std::fmod(angle, 360.0);
    if (normalized > 180.0) normalized -= 360.0;
    if (normalized < -180.0) normalized += 360.0;
    const bool rotated = qAbs(normalized) > 0.05;
    d->rotation->setVisible(rotated);
    d->rotation->setText(i18nc("canvas rotation, %1 is the angle", "%1°", QString::number(normalized, 'f', 0)));
    reposition();
}

void KisStudioCanvasHud::reposition()
{
    if (!parentWidget()) {
        return;
    }
    adjustSize();
    const int margin = 10;
    // left of the pinned panels over the canvas, if there are
    const int right = parentWidget()->property("studioCanvasRightInset").toInt();
    move(parentWidget()->width() - right - width() - margin, parentWidget()->height() - height() - margin);
    raise();
}

bool KisStudioCanvasHud::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == parentWidget() && (event->type() == QEvent::Resize || event->type() == QEvent::LayoutRequest)) {
        reposition();
    } else if (watched == d->zoom && event->type() == QEvent::Wheel) {
        const int steps = static_cast<QWheelEvent *>(event)->angleDelta().y();
        if (steps) {
            d->trigger(steps > 0 ? "view_zoom_in" : "view_zoom_out");
        }
        return true;
    } else if (qobject_cast<QProgressBar *>(watched)
               && (event->type() == QEvent::ShowToParent || event->type() == QEvent::HideToParent)) {
        // ShowToParent: Show itself only arrives while the box is already visible
        d->progressBox->setVisible(event->type() == QEvent::ShowToParent);
        reposition();
    }
    return QWidget::eventFilter(watched, event);
}

void KisStudioCanvasHud::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QRectF pill = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    const QPainterPath shape = KisStudioGlass::paint(painter, this, pill, pill.height() / 2);
    QColor border = palette().color(QPalette::WindowText);
    border.setAlphaF(0.14);
    KisStudioGlass::paintBorder(painter, this, shape, border);
}
