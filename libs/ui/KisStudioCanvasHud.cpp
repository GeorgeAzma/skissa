/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioCanvasHud.h"

#include <QAction>
#include <QEvent>
#include <QHBoxLayout>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QProgressBar>
#include <QStatusBar>
#include <QStyleOptionToolButton>
#include <QToolButton>
#include <QWheelEvent>
#include <QtMath>

#include <cmath>
#include <tuple>

#include <KoCanvasController.h>
#include <kactioncollection.h>
#include <klocalizedstring.h>

#include "KisMainWindow.h"
#include "KisPart.h"
#include "KisStudioGlass.h"
#include "KisStudioItemCard.h" // KisStudioShortcuts
#include "KisView.h"
#include "KisViewManager.h"
#include "canvas/kis_canvas2.h"
#include "canvas/kis_canvas_controller.h"
#include "canvas/kis_coordinates_converter.h"
#include "kis_selection_manager.h"
#include "opengl/KisOpenGLUpdateInfoBuilder.h"
#include <KisModernStyle.h>
#include <kis_icon_utils.h>

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

/// Shows the zoom as a semibold number with a smaller, fainter % on the same baseline
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
        // center on the digits' height rather than the font's
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

    /// Splits the text into number runs (digits and separators) and the rest
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

    /// A mode that changes what a stroke does; shown while on, and a click turns it off
    struct Badge {
        QToolButton *button {nullptr};
        QPointer<QAction> action;
    };
    Badge alphaLock;
    Badge mirrorX;
    Badge mirrorY;
    QToolButton *selectionHidden {nullptr};

    QToolButton *mirror {nullptr};
    QToolButton *value {nullptr};
    QToolButton *rotation {nullptr};
    QToolButton *zoom {nullptr};

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
    layout->setContentsMargins(KisModernStyle::space2, KisModernStyle::space1, KisModernStyle::space2, KisModernStyle::space1);
    layout->setSpacing(KisModernStyle::hairline);

    d->progressBox = new QWidget(this);
    auto *progressLayout = new QHBoxLayout(d->progressBox);
    progressLayout->setContentsMargins(0, 0, KisModernStyle::space1, 0);
    d->progressBox->hide();
    layout->addWidget(d->progressBox);

    for (auto [badge, name, tip] : {std::make_tuple(&d->alphaLock, "preserve_alpha", i18nc("@info:tooltip", "Alpha lock: paint only over existing paint")),
                                    std::make_tuple(&d->mirrorX, "hmirror_action", i18nc("@info:tooltip", "Mirrored painting, across")),
                                    std::make_tuple(&d->mirrorY, "vmirror_action", i18nc("@info:tooltip", "Mirrored painting, up and down"))}) {
        badge->action = d->action(name);
        badge->button = hudButton(this);
        badge->button->setToolButtonStyle(Qt::ToolButtonIconOnly);
        badge->button->setCheckable(true);
        badge->button->setChecked(true);
        badge->button->setToolTip(tip);
        badge->button->hide();
        if (QAction *action = badge->action) {
            badge->button->setIcon(action->icon());
            connect(badge->button, &QToolButton::clicked, action, [action]() { if (action->isChecked()) action->trigger(); });
            connect(action, &QAction::toggled, this, &KisStudioCanvasHud::updateBadges);
        }
        layout->addWidget(badge->button);
    }
    d->selectionHidden = hudButton(this);
    d->selectionHidden->setToolButtonStyle(Qt::ToolButtonIconOnly);
    d->selectionHidden->setCheckable(true);
    d->selectionHidden->setChecked(true);
    d->selectionHidden->setToolTip(i18nc("@info:tooltip", "Selection outline hidden"));
    d->selectionHidden->hide();
    if (QAction *show = d->action("toggle_display_selection")) d->selectionHidden->setIcon(show->icon());
    connect(d->selectionHidden, &QToolButton::clicked, this, [this]() { d->trigger("toggle_display_selection"); });
    layout->addWidget(d->selectionHidden);
    if (KisSelectionManager *selections = mainWindow->viewManager() ? mainWindow->viewManager()->selectionManager() : nullptr) {
        connect(selections, &KisSelectionManager::currentSelectionChanged, this, &KisStudioCanvasHud::updateBadges);
        connect(selections, &KisSelectionManager::displaySelectionChanged, this, &KisStudioCanvasHud::updateBadges);
    }

    d->mirror = hudButton(this);
    // named because their text changes, and Find tracks a button (its shortcut, its place on the rail) by name
    d->mirror->setObjectName(QStringLiteral("studio_hud_mirror"));
    if (QAction *mirror = d->action("mirror_canvas")) {
        d->mirror->setDefaultAction(mirror);
        d->mirror->setToolButtonStyle(Qt::ToolButtonIconOnly);
    }
    layout->addWidget(d->mirror);

    // shows the image in grayscale, to judge its values
    auto *valueView = new QAction(KisIconUtils::loadIcon(QStringLiteral("studio_value")), i18nc("@action", "Value View"), mainWindow);
    valueView->setObjectName(QStringLiteral("studio_value_view"));
    valueView->setCheckable(true);
    valueView->setToolTip(i18nc("@info:tooltip", "Grayscale: see lights and darks"));
    valueView->setShortcutContext(Qt::WindowShortcut);
    KisStudioShortcuts::restore(valueView);
    mainWindow->actionCollection()->addAction(valueView->objectName(), valueView);
    connect(valueView, &QAction::toggled, this, [](bool on) {
        KisOpenGLUpdateInfoBuilder::setValueView(on);
        // value view applies to every window, so keep all their buttons in sync
        Q_FOREACH (QPointer<KisMainWindow> window, KisPart::instance()->mainWindows()) {
            QAction *other = window ? window->actionCollection()->action(QStringLiteral("studio_value_view")) : nullptr;
            if (other) other->setChecked(on);
        }
        Q_FOREACH (QPointer<KisView> view, KisPart::instance()->views()) {
            KisCanvas2 *canvas = view ? view->canvasBase() : nullptr;
            if (canvas) canvas->updateCanvas();
        }
    });
    d->value = hudButton(this);
    d->value->setDefaultAction(valueView);
    d->value->setToolButtonStyle(Qt::ToolButtonIconOnly);
    layout->addWidget(d->value);

    // shown only while the canvas is rotated; a click resets the rotation
    d->rotation = hudButton(this);
    d->rotation->setObjectName(QStringLiteral("studio_hud_rotation"));
    d->rotation->setToolTip(i18nc("@info:tooltip", "Straighten"));
    connect(d->rotation, &QToolButton::clicked, this, [this]() { d->trigger("reset_canvas_rotation"); });
    layout->addWidget(d->rotation);

    // no menu: a click toggles between fit-to-window and the last zoom, the wheel zooms
    d->zoom = new ZoomButton(this);
    d->zoom->setObjectName(QStringLiteral("studio_hud_zoom"));
    d->zoom->setAutoRaise(true);
    d->zoom->setFocusPolicy(Qt::NoFocus);
    d->zoom->setToolButtonStyle(Qt::ToolButtonTextOnly);
    d->zoom->setToolTip(i18nc("@info:tooltip", "Zoom\nClick: fit ↔ last zoom\nScroll: zoom"));
    d->zoom->installEventFilter(this);
    connect(d->zoom, &QToolButton::clicked, this, [this]() { d->trigger("toggle_zoom_to_fit"); });
    layout->addWidget(d->zoom);

    parentWidget()->installEventFilter(this);
    if (QAction *canvasOnly = d->action("view_show_canvas_only")) {
        connect(canvasOnly, &QAction::toggled, this, &KisStudioCanvasHud::viewChanged);
    }
    connect(mainWindow, &KisMainWindow::activeViewChanged, this, &KisStudioCanvasHud::viewChanged);
    // when the last document closes no view becomes active; only the view manager reports it
    if (KisViewManager *viewManager = mainWindow->viewManager()) {
        connect(viewManager, &KisViewManager::viewChanged, this, &KisStudioCanvasHud::viewChanged);
    }
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
        QWidget *progress = statusBar ? statusBar->findChild<QWidget *>(QStringLiteral("ProgressBar")) : nullptr;
        if (progress) {
            statusBar->removeWidget(progress);
            d->progressBox->layout()->addWidget(progress);
            progress->show();
            d->progress = progress;
            // the bar shows and hides itself; the box only takes space while the bar is visible
            if (QProgressBar *bar = progress->findChild<QProgressBar *>()) {
                bar->installEventFilter(this);
                d->progressBox->setVisible(!bar->isHidden());
            }
        }
    } else {
        // the value view button leaves with the Studio layout, so value view is turned off
        if (d->value && d->value->defaultAction()) d->value->defaultAction()->setChecked(false);
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

    // while the last document closes the window's active view is still set, but the view manager has none
    KisViewManager *viewManager = d->mainWindow->viewManager();
    KisView *view = viewManager && viewManager->canvasBase() ? d->mainWindow->activeView() : nullptr;
    // canvas-only means only the canvas
    QAction *canvasOnly = d->action("view_show_canvas_only");
    if (!view || !d->active || (canvasOnly && canvasOnly->isChecked())) {
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

    updateBadges();
    show();
    raise();
    reposition();
}

void KisStudioCanvasHud::updateBadges()
{
    bool changed = false;
    auto showIf = [&changed](QToolButton *button, bool on) {
        if (button->isVisibleTo(button->parentWidget()) == on) return;
        button->setVisible(on);
        changed = true;
    };
    for (const Private::Badge *badge : {&d->alphaLock, &d->mirrorX, &d->mirrorY}) {
        showIf(badge->button, badge->action && badge->action->isChecked());
    }
    KisSelectionManager *selections = d->mainWindow->viewManager() ? d->mainWindow->viewManager()->selectionManager() : nullptr;
    showIf(d->selectionHidden, selections && selections->haveAnySelectionWithPixels() && !selections->displaySelection());
    if (changed && isVisible()) reposition();
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
    // stay left of any panels pinned over the canvas
    const int right = parentWidget()->property("studioCanvasRightInset").toInt();
    move(parentWidget()->width() - right - width() - margin, parentWidget()->height() - height() - margin);
    raise();
}

bool KisStudioCanvasHud::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == parentWidget() && (event->type() == QEvent::Resize || event->type() == QEvent::LayoutRequest)) {
        reposition();
    } else if (watched == d->zoom && event->type() == QEvent::Wheel) {
        // one zoom step per wheel notch, however fine the wheel
        const int steps = KisModernStyle::wheelSteps(static_cast<QWheelEvent *>(event), watched);
        for (int i = 0; i < qAbs(steps); i++) {
            d->trigger(steps > 0 ? "view_zoom_in" : "view_zoom_out");
        }
        return true;
    } else if (qobject_cast<QProgressBar *>(watched)
               && (event->type() == QEvent::ShowToParent || event->type() == QEvent::HideToParent)) {
        // ShowToParent, because Show only arrives while the box is already visible
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
