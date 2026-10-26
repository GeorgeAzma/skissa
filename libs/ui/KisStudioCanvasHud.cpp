/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioCanvasHud.h"

#include <QAction>
#include <QApplication>
#include <QContextMenuEvent>
#include <QEvent>
#include <QHBoxLayout>
#include <QStackedWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QProgressBar>
#include <QStatusBar>
#include <QStyleOptionToolButton>
#include <QTimer>
#include <QToolButton>
#include <QWheelEvent>
#include <QtMath>

#include <cmath>
#include <tuple>

#include <KoCanvasController.h>
#include <kactioncollection.h>
#include <kis_config.h>
#include <klocalizedstring.h>

#include "KisMainWindow.h"
#include "KisPart.h"
#include "KisSettingsRegistry.h"
#include "KisStudioBarButtons.h"
#include "KisStudioGlass.h"
#include "KisStudioItemCard.h"
#include "KisStudioLayout.h"
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

const QString spotKey = QStringLiteral("StudioLayoutViewPillSpot");
const QString shownKey = QStringLiteral("StudioViewPill");

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
    /// The view's zoom as shown, 1 at 100%
    qreal zoomLevel {1.0};

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
    /// Buttons added to it
    KisStudioBarButtons *buttons {nullptr};

    /// Where it was dragged to, as shares of the room it has (see KisStudioLayout::spotShare()); bottom right at first
    QPointF spot {1, 1};
    bool pressed {false};
    bool moving {false};
    QPoint pressPos;
    QPoint grab;

    QList<QMetaObject::Connection> viewConnections;

    /// Where its corner can go, as offset and extent: the canvas area less a margin
    QRect room(const QWidget *hud) const
    {
        const QWidget *area = hud->parentWidget();
        const int margin = 10;
        return QRect(margin, margin, area->width() - 2 * margin - hud->width(), area->height() - 2 * margin - hud->height());
    }

    /// What it keeps off: the bars, the tool rail and the groups of panels over the canvas, where they are going
    QVector<QRect> taken(const QWidget *hud) const
    {
        static const QStringList pills = {QStringLiteral("studioToolRail"), QStringLiteral("studioPanels"), QStringLiteral("studioFarPanels"),
                                          QStringLiteral("studioTopPill"), QStringLiteral("studioWindowPill"), QStringLiteral("studioWindowHandle")};
        QVector<QRect> rects;
        for (QObject *child : hud->parentWidget()->children()) {
            auto *widget = qobject_cast<QWidget *>(child);
            if (!widget || widget == hud || widget->isHidden()) continue;
            const QString name = widget->objectName();
            if (!pills.contains(name) && !name.startsWith(QLatin1String("studioPanelGroup"))) continue;
            // a group gliding somewhere is already there
            const QRect target = widget->property("studioTarget").toRect();
            rects << (target.isValid() ? target : widget->geometry());
        }
        return rects;
    }

    /// \p wanted moved the least it takes to be clear of \p taken, its corner within \p room
    static QRect clearOf(const QRect &wanted, const QVector<QRect> &taken, const QRect &room)
    {
        auto clear = [&taken](const QRect &at) {
            for (const QRect &them : taken) {
                if (them.intersects(at)) return false;
            }
            return true;
        };
        if (clear(wanted)) return wanted;
        // beside, above or below any of them, or in a corner two of them make
        const int gap = 6;
        QVector<int> xs{wanted.x()};
        QVector<int> ys{wanted.y()};
        for (const QRect &them : taken) {
            xs << them.right() + 1 + gap << them.left() - gap - wanted.width();
            ys << them.bottom() + 1 + gap << them.top() - gap - wanted.height();
        }
        QRect best;
        for (int x : qAsConst(xs)) {
            for (int y : qAsConst(ys)) {
                if (x < room.left() || x > room.left() + room.width() || y < room.top() || y > room.top() + room.height()) continue;
                const QRect at(QPoint(x, y), wanted.size());
                if (!clear(at)) continue;
                if (best.isNull() || (at.topLeft() - wanted.topLeft()).manhattanLength() < (best.topLeft() - wanted.topLeft()).manhattanLength()) best = at;
            }
        }
        return best.isNull() ? wanted : best;
    }

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

    for (auto [badge, name, tip] : {std::make_tuple(&d->alphaLock, "preserve_alpha", i18nc("@info:tooltip", "Alpha lock: paints only on what's already painted")),
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

    d->buttons = new KisStudioBarButtons(mainWindow, this, KisStudioBarButtons::ViewPill);
    d->buttons->installEventFilter(this);
    layout->addWidget(d->buttons);

    d->mirror = hudButton(this);
    // named because their text changes, and Find tracks a button (its shortcut, its place on the rail) by name
    d->mirror->setObjectName(QStringLiteral("studio_hud_mirror"));
    if (QAction *mirror = d->action("mirror_canvas")) {
        d->mirror->setDefaultAction(mirror);
        d->mirror->setToolButtonStyle(Qt::ToolButtonIconOnly);
    }
    layout->addWidget(d->mirror);

    // shows the image in grayscale, to judge its values
    auto *valueView = new QAction(KisIconUtils::loadIcon(QStringLiteral("studio_value")), i18nc("@action", "Value view"), mainWindow);
    valueView->setObjectName(QStringLiteral("studio_value_view"));
    valueView->setCheckable(true);
    valueView->setToolTip(i18nc("@info:tooltip", "Grayscale: check your values"));
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

    // no menu: a click goes to 100%, and from there to fit the window; the wheel zooms
    d->zoom = new ZoomButton(this);
    d->zoom->setObjectName(QStringLiteral("studio_hud_zoom"));
    d->zoom->setAutoRaise(true);
    d->zoom->setFocusPolicy(Qt::NoFocus);
    d->zoom->setToolButtonStyle(Qt::ToolButtonTextOnly);
    d->zoom->setToolTip(i18nc("@info:tooltip", "Zoom\nClick: 100% ↔ fit\nScroll: zoom\nDrag: move"));
    d->zoom->installEventFilter(this);
    connect(d->zoom, &QToolButton::clicked, this, [this]() {
        // what shows as 100% counts as 100%
        d->trigger(qRound(d->zoomLevel * 100.0) == 100 ? "zoom_to_fit" : "zoom_to_100pct");
    });
    layout->addWidget(d->zoom);

    // a drag on any of its own buttons moves the pill
    for (QToolButton *button : {d->alphaLock.button, d->mirrorX.button, d->mirrorY.button, d->selectionHidden, d->mirror, d->value, d->rotation}) {
        button->installEventFilter(this);
    }
    const QStringList spot = KisConfig(true).readEntry<QString>(spotKey, QString()).split(QLatin1Char(','));
    if (spot.size() == 2) d->spot = QPointF(qBound(0.0, spot[0].toDouble(), 1.0), qBound(0.0, spot[1].toDouble(), 1.0));
    connect(KisSettingsRegistry::instance(), &KisSettingsRegistry::valueChanged, this, [this](const QString &id) {
        if (id == shownKey) viewChanged();
    });

    parentWidget()->installEventFilter(this);
    if (QAction *canvasOnly = d->action("view_show_canvas_only")) {
        connect(canvasOnly, &QAction::toggled, this, &KisStudioCanvasHud::viewChanged);
    }
    connect(mainWindow, &KisMainWindow::activeViewChanged, this, &KisStudioCanvasHud::viewChanged);
    // home (the start page) over the images, or back
    if (auto *pages = qobject_cast<QStackedWidget *>(mainWindow->centralWidget())) {
        connect(pages, &QStackedWidget::currentChanged, this, &KisStudioCanvasHud::viewChanged);
    }
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
    // canvas-only means only the canvas, and home (the start page) shows no canvas
    QAction *canvasOnly = d->action("view_show_canvas_only");
    if (!view || !d->active || (canvasOnly && canvasOnly->isChecked()) || d->mainWindow->isWelcomeScreenShown()
        || !KisConfig(true).readEntry(shownKey, true)) {
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
    KisStudioLayout::keepAbove(this);
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
    d->zoomLevel = zoom;
    // a zoom gesture reports every step, most of them showing the same number
    const QString text = i18nc("zoom level, %1 is the percentage", "%1%", QString::number(zoom * 100.0, 'f', zoom < 0.1 ? 1 : 0));
    if (text == d->zoom->text()) return;
    d->zoom->setText(text);
    reposition();
}

void KisStudioCanvasHud::updateRotation(qreal angle)
{
    qreal normalized = std::fmod(angle, 360.0);
    if (normalized > 180.0) normalized -= 360.0;
    if (normalized < -180.0) normalized += 360.0;
    const bool rotated = qAbs(normalized) > 0.05;
    const QString text = i18nc("canvas rotation, %1 is the angle", "%1°", QString::number(normalized, 'f', 0));
    if (rotated == d->rotation->isVisibleTo(this) && text == d->rotation->text()) return;
    d->rotation->setVisible(rotated);
    d->rotation->setText(text);
    reposition();
}

void KisStudioCanvasHud::reposition()
{
    if (!parentWidget()) {
        return;
    }
    adjustSize();
    // anywhere that is free: off the bars, the tool rail and the panels, the least it takes (while dragged too,
    // so it shows where it will stay)
    const QRect room = d->room(this);
    const QPoint wanted(room.left() + KisStudioLayout::spotOffset(d->spot.x(), room.width()), room.top() + KisStudioLayout::spotOffset(d->spot.y(), room.height()));
    const QRect at = Private::clearOf(QRect(wanted, size()), d->taken(this), room);
    if (at.topLeft() != pos()) move(at.topLeft());
    KisStudioLayout::keepAbove(this);
}

bool KisStudioCanvasHud::drag(QMouseEvent *event, QWidget *from)
{
    const QPoint global = event->globalPos();
    switch (event->type()) {
    case QEvent::MouseButtonPress:
        if (event->button() != Qt::LeftButton) return false;
        d->pressed = true;
        d->moving = false;
        d->pressPos = global;
        d->grab = mapFromGlobal(global);
        return false;
    case QEvent::MouseMove: {
        if (!d->pressed || !(event->buttons() & Qt::LeftButton)) return false;
        // pens wobble, so only a clear move counts as a drag
        if (!d->moving) {
            if ((global - d->pressPos).manhattanLength() <= 2 * QApplication::startDragDistance()) return false;
            d->moving = true;
            raise();
            if (auto *button = qobject_cast<QAbstractButton *>(from)) button->setDown(false);
            setCursor(Qt::ClosedHandCursor);
        }
        // caught by the edges, the corners and the middle as it goes, so it shows where it will stay
        const QRect room = d->room(this);
        const QPoint at = parentWidget()->mapFromGlobal(global) - d->grab;
        d->spot = QPointF(KisStudioLayout::spotShare(at.x() - room.left(), room.width()), KisStudioLayout::spotShare(at.y() - room.top(), room.height()));
        reposition();
        return true;
    }
    case QEvent::MouseButtonRelease:
        if (event->button() != Qt::LeftButton || !d->pressed) return false;
        d->pressed = false;
        if (!d->moving) return false;
        d->moving = false;
        unsetCursor();
        if (auto *button = qobject_cast<QAbstractButton *>(from)) button->setDown(false);
        KisSettingsRegistry::writeLater(spotKey, QString(QString::number(d->spot.x()) + QLatin1Char(',') + QString::number(d->spot.y())));
        return true;
    default:
        return false;
    }
}

void KisStudioCanvasHud::mousePressEvent(QMouseEvent *event)
{
    drag(event, this);
    event->accept();
}

void KisStudioCanvasHud::mouseMoveEvent(QMouseEvent *event)
{
    drag(event, this);
}

void KisStudioCanvasHud::mouseReleaseEvent(QMouseEvent *event)
{
    drag(event, this);
}

void KisStudioCanvasHud::contextMenuEvent(QContextMenuEvent *event)
{
    event->accept();
    d->buttons->showRestoreCard(event->globalPos(), {[](KisStudioItemCard *card) {
        card->addCommand({KisIconUtils::loadIcon(QStringLiteral("studio_hidden")), i18nc("@info:tooltip", "Hide"),
                          []() { KisSettingsRegistry::instance()->setValue(shownKey, false); }});
    }});
}

bool KisStudioCanvasHud::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == parentWidget() && (event->type() == QEvent::Resize || event->type() == QEvent::LayoutRequest)) {
        reposition();
    } else if (watched == d->buttons) {
        // its buttons came or went
        if (event->type() == QEvent::LayoutRequest) QTimer::singleShot(0, this, &KisStudioCanvasHud::reposition);
    } else if (watched != parentWidget()
               && (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseMove || event->type() == QEvent::MouseButtonRelease)
               && drag(static_cast<QMouseEvent *>(event), qobject_cast<QWidget *>(watched))) {
        // only on itself: a press another pill let through to the canvas area isn't its drag
        return true;
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
