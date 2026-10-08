/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioCanvasHud.h"

#include <QAction>
#include <QEvent>
#include <QHBoxLayout>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QProgressBar>
#include <QStatusBar>
#include <QToolButton>

#include <cmath>

#include <KoCanvasController.h>
#include <KoColorModelStandardIds.h>
#include <KoColorProfile.h>
#include <KoColorSpace.h>
#include <KoColorSpaceRegistry.h>
#include <kactioncollection.h>
#include <klocalizedstring.h>

#include "KisMainWindow.h"
#include "KisProofingConfiguration.h"
#include "KisView.h"
#include "canvas/kis_canvas2.h"
#include "canvas/kis_canvas_controller.h"
#include "canvas/kis_coordinates_converter.h"
#include "kis_image.h"

namespace {

/// Half dark, half light: what the value check shows
QIcon valueCheckIcon(const QColor &color)
{
    QIcon icon;
    for (int size : {16, 32}) {
        QPixmap pixmap(size, size);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const qreal pen = size / 12.0;
        const QRectF circle = QRectF(0, 0, size, size).adjusted(pen * 1.5, pen * 1.5, -pen * 1.5, -pen * 1.5);
        painter.setPen(QPen(color, pen));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(circle);
        QPainterPath half;
        half.moveTo(circle.center());
        half.arcTo(circle, 90, 180);
        half.closeSubpath();
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawPath(half);
        painter.end();
        icon.addPixmap(pixmap);
    }
    return icon;
}

QToolButton *hudButton(QWidget *parent)
{
    auto *button = new QToolButton(parent);
    button->setAutoRaise(true);
    button->setFocusPolicy(Qt::NoFocus);
    button->setIconSize(QSize(16, 16));
    button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    return button;
}

} // namespace

struct KisStudioCanvasHud::Private
{
    KisMainWindow *mainWindow {nullptr};
    bool active {false};

    QWidget *progressBox {nullptr};
    QPointer<QWidget> progress;

    QToolButton *valueCheck {nullptr};
    QToolButton *mirror {nullptr};
    QToolButton *rotation {nullptr};
    QToolButton *zoom {nullptr};

    QPointer<KisView> view;
    QList<QMetaObject::Connection> viewConnections;

    /// The view's proofing setup before the value check replaced it
    KisProofingConfiguration savedProofing;
    bool hadSoftProofing {false};

    QAction *action(const char *name) const
    {
        return mainWindow->actionCollection()->action(QLatin1String(name));
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
    layout->setContentsMargins(6, 4, 6, 4);
    layout->setSpacing(2);

    d->progressBox = new QWidget(this);
    auto *progressLayout = new QHBoxLayout(d->progressBox);
    progressLayout->setContentsMargins(0, 0, 4, 0);
    layout->addWidget(d->progressBox);

    d->valueCheck = hudButton(this);
    d->valueCheck->setCheckable(true);
    d->valueCheck->setIcon(valueCheckIcon(palette().color(QPalette::ButtonText)));
    d->valueCheck->setToolTip(i18nc("@info:tooltip", "Value check: show the canvas by lightness only"));
    connect(d->valueCheck, &QToolButton::toggled, this, &KisStudioCanvasHud::setValueCheck);
    layout->addWidget(d->valueCheck);

    d->mirror = hudButton(this);
    if (QAction *mirror = d->action("mirror_canvas")) {
        d->mirror->setDefaultAction(mirror);
        d->mirror->setToolButtonStyle(Qt::ToolButtonIconOnly);
    }
    layout->addWidget(d->mirror);

    // only there while the canvas is rotated; a click puts it straight again
    d->rotation = hudButton(this);
    d->rotation->setToolTip(i18nc("@info:tooltip", "Reset rotation"));
    connect(d->rotation, &QToolButton::clicked, this, [this]() {
        if (QAction *reset = d->action("reset_canvas_rotation")) {
            reset->trigger();
        }
    });
    layout->addWidget(d->rotation);

    d->zoom = hudButton(this);
    d->zoom->setToolTip(i18nc("@info:tooltip", "Zoom"));
    auto *zoomMenu = new QMenu(d->zoom);
    connect(zoomMenu, &QMenu::aboutToShow, this, [this, zoomMenu]() {
        zoomMenu->clear();
        for (const char *name : {"zoom_to_fit", "zoom_to_fit_width", "zoom_to_100pct", "", "view_zoom_in", "view_zoom_out"}) {
            if (!*name) {
                zoomMenu->addSeparator();
            } else if (QAction *action = d->action(name)) {
                zoomMenu->addAction(action);
            }
        }
    });
    connect(d->zoom, &QToolButton::clicked, this, [this, zoomMenu]() {
        zoomMenu->popup(d->zoom->mapToGlobal(QPoint(0, 0)) - QPoint(0, zoomMenu->sizeHint().height() + 4));
    });
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
                d->progressBox->setVisible(bar->isVisible());
            }
        }
    } else {
        if (d->valueCheck->isChecked()) {
            d->valueCheck->setChecked(false);
        }
        if (d->progress && statusBar) {
            d->progressBox->layout()->removeWidget(d->progress);
            statusBar->addWidget(d->progress);
            d->progress->show();
        }
        d->progress = nullptr;
    }
    viewChanged();
}

void KisStudioCanvasHud::viewChanged()
{
    Q_FOREACH (const QMetaObject::Connection &connection, d->viewConnections) {
        disconnect(connection);
    }
    d->viewConnections.clear();

    if (d->view && d->valueCheck->isChecked()) {
        d->valueCheck->setChecked(false);
    }

    KisView *view = d->mainWindow->activeView();
    d->view = view;
    if (!view || !d->active) {
        hide();
        return;
    }

    KisCanvasController *controller = view->canvasController();
    if (controller) {
        // the canvas is panned by gesture, wheel and Space; scroll bars are only chrome
        controller->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        controller->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        d->viewConnections << connect(controller->proxyObject, &KoCanvasControllerProxyObject::effectiveZoomChanged,
                                      this, &KisStudioCanvasHud::updateZoom);
        d->viewConnections << connect(controller->proxyObject, &KoCanvasControllerProxyObject::documentRotationChanged,
                                      this, &KisStudioCanvasHud::updateRotation);
    }
    if (KisCanvas2 *canvas = view->canvasBase()) {
        updateZoom(canvas->coordinatesConverter()->effectiveZoom());
        updateRotation(canvas->rotationAngle());
        // soft proofing, and so the value check, needs integer channels
        d->valueCheck->setEnabled(view->image() && view->image()->colorSpace()->colorDepthId().id().contains(QLatin1Char('U')));
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

void KisStudioCanvasHud::setValueCheck(bool enabled)
{
    KisView *view = d->view;
    QAction *softProof = d->action("softProof");
    if (!view || !view->canvasBase() || !softProof) {
        return;
    }
    KisCanvas2 *canvas = view->canvasBase();
    KisProofingConfigurationSP config = canvas->proofingConfiguration();
    if (!config) {
        return;
    }

    if (enabled) {
        // a soft proof into gray: lightness as the color management computes it,
        // without touching the image or its own proofing setup
        const KoColorSpace *gray = KoColorSpaceRegistry::instance()->colorSpace(
            GrayAColorModelID.id(), Integer8BitsColorDepthID.id(), nullptr);
        if (!gray || !gray->profile()) {
            return;
        }
        d->savedProofing = *config;
        d->hadSoftProofing = softProof->isChecked();
        config->proofingModel = GrayAColorModelID.id();
        config->proofingDepth = Integer8BitsColorDepthID.id();
        config->proofingProfile = gray->profile()->name();
        config->displayFlags.setFlag(KoColorConversionTransformation::GamutCheck, false);
        if (softProof->isChecked()) {
            canvas->slotSoftProofing();
        } else {
            softProof->setChecked(true);
        }
    } else {
        *config = d->savedProofing;
        if (d->hadSoftProofing) {
            canvas->slotSoftProofing();
        } else {
            softProof->setChecked(false);
        }
    }
}

void KisStudioCanvasHud::reposition()
{
    if (!parentWidget()) {
        return;
    }
    adjustSize();
    const int margin = 10;
    move(parentWidget()->width() - width() - margin, parentWidget()->height() - height() - margin);
    raise();
}

bool KisStudioCanvasHud::eventFilter(QObject *watched, QEvent *event)
{
    if (qobject_cast<QProgressBar *>(watched) && (event->type() == QEvent::Show || event->type() == QEvent::Hide)) {
        d->progressBox->setVisible(event->type() == QEvent::Show);
        reposition();
    }
    if (watched == parentWidget() && (event->type() == QEvent::Resize || event->type() == QEvent::LayoutRequest)) {
        reposition();
    }
    return QWidget::eventFilter(watched, event);
}

void KisStudioCanvasHud::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    QColor background = palette().color(QPalette::Window);
    background.setAlphaF(0.88);
    QColor border = palette().color(QPalette::WindowText);
    border.setAlphaF(0.14);
    painter.setPen(QPen(border, 1));
    painter.setBrush(background);
    const QRectF pill = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    painter.drawRoundedRect(pill, pill.height() / 2, pill.height() / 2);
}
