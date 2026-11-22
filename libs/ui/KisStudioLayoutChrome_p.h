/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIOLAYOUTCHROME_P_H
#define KISSTUDIOLAYOUTCHROME_P_H

#include "KisStudioLayoutParts_p.h"

// The panel stack, the rails' and top bar's pills, the window handle and the line between lanes; private to
// KisStudioLayout*.cpp

namespace KisStudioLayoutParts
{
/**
 * The pinned panels of a column, stacked top to bottom. Each is as tall as its
 * contents, or as its handle set it; when they don't all fit, the ones that
 * scroll give up room, those sized by hand last.
 */
class PanelStack : public QWidget
{
public:
    /// Height of the handle under each panel; taller for fingers
    static int gap() { return KisTouchMode::isActive() ? 12 : 6; }
    /// A scrolling panel keeps at least this much of its contents
    static constexpr int minimumContents = 56;
    /// The height a scrolling panel is first given is its contents', within these: a short list has room to
    /// grow into, and a long one (measured perhaps before it was filtered) doesn't take the whole column
    static constexpr int settledMinimum = 160;
    static constexpr int settledMaximum = 360;

    /// A panel's height, set by its handle or when it was first laid out; 0: not set yet
    std::function<int(const QDockWidget *)> userHeight;
    /// A panel that scrolls was laid out for the first time and fitted to its contents: its height from now on
    std::function<void(QDockWidget *dock, int height)> settled;
    /// Its handle moved: \p height for \p dock, 0 to fit it again; \p done once let go
    std::function<void(QDockWidget *dock, int height, bool done)> resizedByUser;
    /// How tall the stack would like to be has changed
    std::function<void()> wantsHeight;
    /// The handle under \p above was tapped: the panels below it go apart
    std::function<void(QDockWidget *above)> separated;
    /// One of its panels was shown or hidden
    std::function<void()> shownChanged;

    explicit PanelStack(QWidget *parent)
        : QWidget(parent)
    {
        m_timer.setSingleShot(true);
        m_timer.setInterval(0);
        QObject::connect(&m_timer, &QTimer::timeout, this, [this]() { relayout(true); });
        QObject::connect(KisTouchMode::instance(), &KisTouchMode::changed, this, [this]() { later(); });
    }

    QList<QDockWidget *> panels() const
    {
        QList<QDockWidget *> docks;
        for (const QPointer<QDockWidget> &dock : m_panels) {
            if (dock && dock->parentWidget() == this) docks << dock;
        }
        return docks;
    }

    /// Takes \p docks in this order, each keeping its visibility
    void setPanels(const QList<QDockWidget *> &docks)
    {
        m_panels.clear();
        for (QDockWidget *dock : docks) {
            const bool shown = !dock->isHidden();
            KisStudioGlass::setSeeThrough(dock, true);
            keepUpright(dock, true);
            if (dock->parentWidget() != this) dock->setParent(this);
            dock->setVisible(shown);
            dock->removeEventFilter(m_watcher);
            watch(dock);
            m_panels << dock;
        }
        later();
    }

    /// The height all its shown panels want, with their handles
    int wantedHeight()
    {
        int total = 0;
        int shown = 0;
        for (QDockWidget *dock : panels()) {
            if (dock->isHidden()) continue;
            total += measure(dock).want;
            shown++;
        }
        // a handle under the last one too, so it can be made taller
        return total + shown * gap();
    }

    QSize sizeHint() const override { return QSize(width(), m_wanted); }

    /// Lays the panels out again soon, once their contents have settled
    void later()
    {
        m_passes = 0;
        m_timer.start();
    }

    /// Lays the panels out in the stack's height; \p ask: first see whether the stack wants another height
    void relayout(bool ask)
    {
        if (ask) {
            const int wanted = wantedHeight();
            if (wanted != m_wanted) {
                m_wanted = wanted;
                updateGeometry();
                // the column resizes the stack, which lays the panels out
                if (wantsHeight) wantsHeight();
            }
        }
        arrange();
    }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QWidget::resizeEvent(event);
        arrange();
        // a new width rewraps grids of tiles
        if (event->oldSize().width() != width()) later();
    }

private:
    struct Measure {
        int want {0};
        int minimum {0};
        bool byHand {false};
    };

    /// \p dock's main scroll area (mainScrollArea()), kept: a drag measures every panel at each move. Looked for
    /// again once it is gone, hidden or out of the panel, or the panel holds other contents
    QAbstractScrollArea *scrollAreaOf(QDockWidget *dock)
    {
        FoundArea &found = m_scrollAreas[dock];
        QAbstractScrollArea *area = found.area;
        if (area && found.contents == dock->widget() && dock->isAncestorOf(area) && area->isVisibleTo(dock)) return area;
        found.contents = dock->widget();
        found.area = mainScrollArea(dock);
        return found.area;
    }

    Measure measure(QDockWidget *dock)
    {
        Measure m;
        QWidget *title = dock->titleBarWidget();
        // none on the only panel of a group, whose grip moves and folds it instead (see PanelGrip::setAlone())
        const int titleHeight = title && !title->isHidden() ? qMax(title->height(), title->minimumHeight()) : 0;
        if (dock->widget() && dock->widget()->property("studioFolded").toBool()) {
            m.want = m.minimum = titleHeight;
            return m;
        }
        int natural;
        QAbstractScrollArea *area = scrollAreaOf(dock);
        int byHand = userHeight ? userHeight(dock) : 0;
        // with its height set, only whether it has a little to show counts: a long list (layers, the history
        // after every stroke) isn't walked row by row
        const int contents = area ? contentsHeight(area, byHand > 0 ? minimumContents : std::numeric_limits<int>::max()) : -1;
        if (area && contents >= 0 && dock->height() <= titleHeight) {
            // not laid out yet: measured again once it is
            m_remeasure = true;
        }
        if (area && contents >= 0 && dock->height() > titleHeight) {
            // what isn't the scroll area, from the size hints (the current sizes are off when squeezed), and its frame
            const int chrome = dock->sizeHint().height() - area->sizeHint().height() + area->height() - area->viewport()->height();
            natural = chrome + qMax(contents, minimumContents);
            m.minimum = chrome + qMin(qMax(contents, 1), minimumContents);
        } else {
            natural = dock->hasHeightForWidth() ? dock->heightForWidth(width()) : dock->sizeHint().height();
            m.minimum = dock->minimumSizeHint().height();
        }
        // a panel that scrolls keeps the height it first fitted, so what grows in it (layers, brushes) scrolls
        // instead of pushing the panels below it down; its handle sets another, a right click fits it again
        if (byHand <= 0 && area && contents >= 0 && dock->height() > titleHeight && settled) {
            byHand = qBound(settledMinimum, natural, settledMaximum);
            settled(dock, byHand);
        }
        m.byHand = byHand > 0;
        m.want = m.byHand ? byHand : natural;
        // never below what its own layout needs (Qt wouldn't make it smaller); a fold animating caps it, below that too
        m.minimum = qMax(m.minimum, qMax(titleHeight, qMax(dock->minimumSizeHint().height(), dock->minimumHeight())));
        m.minimum = qMin(m.minimum, dock->maximumHeight());
        m.want = qBound(m.minimum, m.want, dock->maximumHeight());
        return m;
    }

    void arrange()
    {
        m_remeasure = false;
        QList<QDockWidget *> shown;
        for (QDockWidget *dock : panels()) {
            if (!dock->isHidden()) shown << dock;
        }
        QVector<Measure> sizes;
        int total = shown.size() * gap();
        for (QDockWidget *dock : shown) {
            sizes << measure(dock);
            total += sizes.last().want;
        }
        // measured again after the column was sized: it follows
        if (total != m_wanted && m_passes < 3) {
            m_passes++;
            m_wanted = total;
            updateGeometry();
            QTimer::singleShot(0, this, [this]() {
                if (wantsHeight) wantsHeight();
            });
        }
        // too tall: the panels give up room down to their minimum, those fitted to their contents first
        int deficit = total - height();
        for (int round = 0; round < 2 && deficit > 0; round++) {
            const bool byHand = round == 1;
            int room = 0;
            for (int i = 0; i < sizes.size(); i++) {
                if (sizes[i].byHand == byHand && shown[i] != m_dragged) room += sizes[i].want - sizes[i].minimum;
            }
            if (room <= 0) continue;
            const int take = qMin(deficit, room);
            int taken = 0;
            for (int i = 0; i < sizes.size(); i++) {
                if (sizes[i].byHand != byHand || shown[i] == m_dragged) continue;
                const int share = int(qint64(take) * (sizes[i].want - sizes[i].minimum) / room);
                sizes[i].want -= share;
                taken += share;
            }
            // what rounding left over, from the first that can give it
            for (int i = 0; i < sizes.size() && taken < take; i++) {
                if (sizes[i].byHand != byHand || shown[i] == m_dragged) continue;
                const int share = qMin(take - taken, sizes[i].want - sizes[i].minimum);
                sizes[i].want -= share;
                taken += share;
            }
            deficit -= taken;
        }

        while (m_handles.size() < shown.size()) {
            auto *handle = new PanelHandle(this);
            m_handles << handle;
        }
        int y = 0;
        for (int i = 0; i < shown.size(); i++) {
            QDockWidget *dock = shown[i];
            // only when it moves: a geometry set again can make a panel ask for another layout
            const QRect place(0, y, width(), sizes[i].want);
            if (dock->geometry() != place) dock->setGeometry(place);
            y += sizes[i].want;
            PanelHandle *handle = m_handles[i];
            handle->setVisible(true);
            handle->setGeometry(0, y, width(), gap());
            handle->raise();
            QPointer<QDockWidget> target = dock;
            handle->pressed = [this, target]() { m_dragStart = target ? target->height() : 0; };
            handle->dragged = [this, target](int dy, bool done) {
                if (!target) return;
                m_dragged = done ? nullptr : target.data();
                if (resizedByUser) resizedByUser(target, qMax(1, m_dragStart + dy), done);
                relayout(true);
            };
            handle->fit = [this, target]() {
                if (target && resizedByUser) resizedByUser(target, 0, true);
                relayout(true);
            };
            handle->setBetween(i < shown.size() - 1);
            // after the tap is done with: taking the panels apart moves them to another stack
            handle->tapped = [this, target]() {
                QTimer::singleShot(0, this, [this, target]() {
                    if (target && separated) separated(target);
                });
            };
            y += gap();
        }
        for (int i = shown.size(); i < m_handles.size(); i++) m_handles[i]->hide();
        if (m_remeasure && m_passes < 3) {
            m_passes++;
            m_timer.start();
        }
    }

    /// Lays out again when a panel's contents change size
    void watch(QDockWidget *dock)
    {
        if (!m_watcher) {
            m_watcher = new Watcher(this);
        }
        dock->installEventFilter(m_watcher);
        QAbstractScrollArea *area = mainScrollArea(dock);
        auto *view = qobject_cast<QAbstractItemView *>(area);
        if (view && view->model() && !m_models.contains(view->model())) {
            QAbstractItemModel *model = view->model();
            m_models << model;
            for (auto signal : {&QAbstractItemModel::rowsInserted, &QAbstractItemModel::rowsRemoved}) {
                QObject::connect(model, signal, this, [this]() { later(); });
            }
            QObject::connect(model, &QAbstractItemModel::modelReset, this, [this]() { later(); });
            QObject::connect(model, &QAbstractItemModel::layoutChanged, this, [this]() { later(); });
        }
        if (auto *tree = qobject_cast<QTreeView *>(view)) {
            QObject::connect(tree, &QTreeView::expanded, this, [this]() { later(); }, Qt::UniqueConnection);
            QObject::connect(tree, &QTreeView::collapsed, this, [this]() { later(); }, Qt::UniqueConnection);
        }
    }

    class Watcher : public QObject
    {
    public:
        explicit Watcher(PanelStack *stack) : QObject(stack), m_stack(stack) {}

    protected:
        bool eventFilter(QObject *watched, QEvent *event) override
        {
            // a panel since moved to another column, or parked, is that one's
            if (watched->parent() != m_stack) return false;
            switch (event->type()) {
            case QEvent::Show:
            case QEvent::Hide:
            case QEvent::LayoutRequest:
                m_stack->later();
                break;
            case QEvent::ShowToParent:
            case QEvent::HideToParent:
                // also while its group is hidden (it has no other panel shown), when Show doesn't come
                m_stack->later();
                if (m_stack->shownChanged) m_stack->shownChanged();
                break;
            default:
                break;
            }
            return false;
        }

    private:
        PanelStack *m_stack;
    };

    QList<QPointer<QDockWidget>> m_panels;
    struct FoundArea {
        QPointer<QWidget> contents;
        QPointer<QAbstractScrollArea> area;
    };
    QHash<const QDockWidget *, FoundArea> m_scrollAreas;
    QList<PanelHandle *> m_handles;
    QList<QPointer<QAbstractItemModel>> m_models;
    Watcher *m_watcher {nullptr};
    QPointer<QDockWidget> m_dragged;
    int m_dragStart {0};
    QTimer m_timer;
    int m_wanted {0};
    bool m_remeasure {false};
    /// Layouts redone in a row to measure panels that weren't laid out yet
    int m_passes {0};
};

/// Holds the tool rail over the side of the canvas, as tall as its tools
class RailOverlay : public QWidget
{
public:
    /// Called when dragged and dropped: the side of the canvas it is on and its place from that side (see shareOf())
    std::function<void(bool left, const QPointF &spot)> moved;
    std::function<void()> tapped;
    /// It was placed somewhere else (see railRectProperty)
    std::function<void()> reachChanged;
    /// It turned, lying flat along the top or bottom edge, or standing along a side
    std::function<void(bool flat)> flatChanged;

    explicit RailOverlay(QWidget *canvasArea)
        : QWidget(canvasArea)
    {
        setObjectName(QStringLiteral("studioToolRail"));
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(KisModernStyle::space1, KisModernStyle::space1, KisModernStyle::space1, KisModernStyle::space2);
        layout->setSpacing(KisModernStyle::space2);
        layout->setSizeConstraint(QLayout::SetFixedSize);
        m_grip = new DragGrip(this);
        layout->addWidget(m_grip);
        m_glide = new KisStudioGlide(this);
        canvasArea->installEventFilter(this);
        setTucked(false);
    }

    /// Its parts in a row (lying along the top or bottom of the canvas) or a column
    void setFlat(bool flat)
    {
        if (m_flat == flat) return;
        m_flat = flat;
        static_cast<QBoxLayout *>(layout())->setDirection(flat ? QBoxLayout::LeftToRight : QBoxLayout::TopToBottom);
        // the grip runs across the rail
        if (flat) {
            m_grip->setMinimumHeight(0);
            m_grip->setMaximumHeight(QWIDGETSIZE_MAX);
            m_grip->setFixedWidth(14);
        } else {
            m_grip->setMinimumWidth(0);
            m_grip->setMaximumWidth(QWIDGETSIZE_MAX);
            m_grip->setFixedHeight(14);
        }
        if (flatChanged) flatChanged(flat);
        layout()->activate();
        adjustSize();
        place();
    }
    bool isFlat() const { return m_flat; }

    void setPlace(bool left, const QPointF &spot)
    {
        m_left = left;
        m_spot = spot;
        place();
    }

    /// When tucked, only the grip shows, as wide as the rail was
    void setTucked(bool tucked)
    {
        if (tucked && !m_tucked && !m_flat) {
            m_grip->setFixedWidth(qMax(m_grip->width(), 30));
            m_openHeight = layout()->sizeHint().height();
        }
        if (!tucked && !m_flat) {
            m_grip->setMinimumWidth(0);
            m_grip->setMaximumWidth(QWIDGETSIZE_MAX);
        }
        m_tucked = tucked;
        for (int i = 0; i < layout()->count(); i++) {
            QWidget *widget = layout()->itemAt(i)->widget();
            if (widget && widget != m_grip) widget->setVisible(!tucked);
        }
        m_grip->setToolTip(tucked ? i18nc("@info:tooltip", "Click: show tools\nDrag: move")
                                  : i18nc("@info:tooltip", "Drag: move\nClick: hide"));
        place();
    }

    void place()
    {
        const QRect room = this->room();
        const QSize grown = roomGrowth(this);
        const int across = offsetOf(m_spot.x(), room.width(), grown.width());
        m_glide->moveTo(QPoint(m_left ? room.left() + across : room.left() + room.width() - across, room.top() + offsetOf(m_spot.y(), room.height(), grown.height())),
                        m_byPointer);
        KisStudioLayout::keepAbove(this);
        updateReach();
    }

    /// Tells the panels where it is, so they keep off it: where it lands, not on the way while dragged or gliding
    void updateReach()
    {
        QWidget *area = parentWidget();
        const QRect at = isVisibleTo(area) ? QRect(m_glide->target(), size()) : QRect();
        if (area->property(railRectProperty).toRect() == at) return;
        area->setProperty(railRectProperty, at);
        if (!m_moving && reachChanged) reachChanged();
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == parentWidget()
            && (event->type() == QEvent::Resize || event->type() == QEvent::ChildAdded || event->type() == QEvent::LayoutRequest)) {
            place();
        }
        return QWidget::eventFilter(watched, event);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        // pressed on the grip or between the tools: the rail follows the pointer
        if (event->button() == Qt::LeftButton) {
            m_pressed = true;
            m_moving = false;
            m_grab = event->pos();
            m_pressPos = globalMousePos(event);
            m_onGrip = m_grip->geometry().contains(event->pos());
            event->accept();
            return;
        }
        QWidget::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (!m_pressed) return;
        if (!(event->buttons() & Qt::LeftButton)) {
            // let go in another window, whose release never comes here: dropped where it is, and no tap
            if (!m_moving) {
                m_pressed = false;
                return;
            }
            QMouseEvent release(QEvent::MouseButtonRelease, event->localPos(), event->windowPos(), event->screenPos(), Qt::LeftButton,
                                Qt::NoButton, event->modifiers());
            mouseReleaseEvent(&release);
            return;
        }
        const QPoint global = globalMousePos(event);
        if (!m_moving) {
            if ((global - m_pressPos).manhattanLength() < KisModernStyle::dragDistance(event)) return;
            m_moving = true;
            // what is carried goes over the other overlays
            raise();
            setCursor(Qt::ClosedHandCursor);
            update();
        }
        // caught by the edges and the middle as it goes, so it shows where it will stay
        const QWidget *area = parentWidget();
        QPoint at = area->mapFromGlobal(global) - m_grab;
        QRect room = this->room();
        // against the top or bottom edge it lies flat, against a side it stands; in a corner or in the open it stays as it is
        {
            const int alongTopOrBottom = qMin(at.y() - room.top(), room.bottom() - at.y());
            const int alongSide = qMin(at.x() - room.left(), room.right() - at.x());
            const int touching = 24;
            const int clear = 60;
            const bool lie = alongTopOrBottom < touching && alongSide > clear;
            const bool stand = alongSide < touching && alongTopOrBottom > clear;
            if ((lie && !m_flat) || (stand && m_flat)) {
                setFlat(lie);
                // held by the grip as before, which is at its start either way
                m_grab = m_grip->geometry().center();
                at = area->mapFromGlobal(global) - m_grab;
                room = this->room();
            }
        }
        m_left = at.x() + width() / 2 < area->width() / 2;
        const int across = m_left ? at.x() - room.left() : room.left() + room.width() - at.x();
        const QSize grown = roomGrowth(this);
        m_spot = QPointF(shareOf(across, room.width(), grown.width()), shareOf(at.y() - room.top(), room.height(), grown.height()));
        m_byPointer = QPoint(qBound(room.left(), at.x(), room.left() + room.width()), qBound(room.top(), at.y(), room.top() + room.height()));
        place();
        m_byPointer.reset();
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (!m_pressed || event->button() != Qt::LeftButton) {
            QWidget::mouseReleaseEvent(event);
            return;
        }
        m_pressed = false;
        if (!m_moving) {
            if (m_onGrip && tapped) tapped();
            return;
        }
        m_moving = false;
        unsetCursor();
        update();
        if (moved) moved(m_left, m_spot);
        if (reachChanged) reachChanged();
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        paintGlass(painter, this, 14, m_moving ? 0.3 : 0.12);
    }

    void showEvent(QShowEvent *event) override
    {
        QWidget::showEvent(event);
        updateReach();
    }

    void resizeEvent(QResizeEvent *event) override
    {
        QWidget::resizeEvent(event);
        updateReach();
    }

    void hideEvent(QHideEvent *event) override
    {
        QWidget::hideEvent(event);
        updateReach();
    }

private:
    /// Where its corner can go, as offset and extent: the whole width (the panels keep off it), clear of the bars
    QRect room() const
    {
        const QWidget *area = parentWidget();
        const int top = area->property(topInsetProperty).toInt();
        const int bottom = area->property(bottomInsetProperty).toInt();
        const int minX = 10;
        const int maxX = area->width() - 10 - width();
        // below the window's handle too, which stays when the top bar goes elsewhere
        const int handle = area->property(handleBottomProperty).toInt();
        const int minY = qMax(top > 0 ? top + 6 : 10, handle > 0 ? handle + 6 : 0);
        // placed as if open, so the grip stays where it was when tucked
        const int maxY = area->height() - (bottom > 0 ? bottom + 6 : 10) - (m_tucked ? qMax(height(), m_openHeight) : height());
        return QRect(minX, minY, qMax(0, maxX - minX), qMax(0, maxY - minY));
    }

    DragGrip *m_grip {nullptr};
    KisStudioGlide *m_glide {nullptr};
    /// Where the pointer alone puts it, while dragged
    std::optional<QPoint> m_byPointer;
    bool m_left {true};
    bool m_flat {false};
    QPointF m_spot;
    bool m_pressed {false};
    bool m_moving {false};
    bool m_tucked {false};
    int m_openHeight {0};
    bool m_onGrip {false};
    QPoint m_grab;
    QPoint m_pressPos;
};

/**
 * A glass pill over the canvas, sized to its contents: the tool and brush bars
 * (along the top unless dragged elsewhere by its grip), or the window's buttons
 * in the top right corner. Its empty space drags the window like a title bar.
 */
class TopPill : public QWidget
{
public:
    static constexpr int margin = 6;

    TopPill(QWidget *canvasArea, bool right)
        : QWidget(canvasArea)
        , m_right(right)
    {
        setObjectName(right ? QStringLiteral("studioWindowPill") : QStringLiteral("studioTopPill"));
        setProperty(captionProperty, true);
        auto *layout = new QHBoxLayout(this);
        layout->setContentsMargins(right ? 4 : 3, 1, 4, 1);
        layout->setSpacing(0);
        layout->setSizeConstraint(QLayout::SetFixedSize);
        if (!right) {
            m_grip = new TopGrip(this);
            m_grip->setFixedSize(22, 30);
            layout->addWidget(m_grip);
            m_grip->pressed = [this](const QPoint &globalPos) { m_grab = mapFromGlobal(globalPos); };
            m_grip->dragged = [this](const QPoint &globalPos) { dragTo(globalPos); };
            m_grip->dropped = [this]() { if (moved) moved(m_spot); };
        }
        // hidden when the pill is tucked away to its grip
        m_contents = new QWidget(this);
        auto *contents = new QHBoxLayout(m_contents);
        contents->setContentsMargins(0, 0, 0, 0);
        contents->setSpacing(0);
        layout->addWidget(m_contents);
        m_glide = new KisStudioGlide(this);
        setTucked(false);
        canvasArea->installEventFilter(this);
        hide();
    }

    /// Inserts \p widget at \p index, counting the grip
    void insert(int index, QWidget *widget)
    {
        QLayout *contents = m_contents->layout();
        const int at = m_grip ? index - 1 : index;
        static_cast<QHBoxLayout *>(contents)->insertWidget(qBound(0, at, contents->count()), widget);
        keepHeight();
    }

    QWidget *contents() const { return m_contents; }
    TopGrip *grip() const { return m_grip; }

    /// What gives up width when the pill doesn't fit its row (the tool chip, whose name is shortened)
    void setSqueezable(QWidget *widget) { m_squeezable = widget; }

    /// Keeps the grip as tall as the open bar, so tucking it doesn't move the window handle or what is below
    void keepHeight()
    {
        if (m_grip) m_grip->setFixedHeight(qMax(30, m_contents->sizeHint().height()));
    }

    void setTucked(bool tucked)
    {
        if (tucked && m_contents->isVisibleTo(this)) m_openWidth = layout()->sizeHint().width();
        m_tucked = tucked;
        m_contents->setVisible(!tucked);
        keepHeight();
        if (m_grip) {
            m_grip->setToolTip(tucked ? i18nc("@info:tooltip", "Click: show the bar\nDrag: move")
                                      : i18nc("@info:tooltip", "Drag: move\nClick: hide"));
        }
    }

    void place()
    {
        QWidget *area = parentWidget();
        if (m_right) {
            m_glide->moveTo(QPoint(area->width() - margin - width(), margin));
        } else {
            const QSize grown = roomGrowth(this);
            int y = margin + offsetOf(m_spot.y(), roomY(), grown.height());
            const bool topRow = y < margin + height();
            QPair<int, int> span = spanAt(y);
            // in the top row while it fits there, its name shortened if need be; else in a row of its own under the
            // window's buttons, still clear of the window handle (a large font in a narrow window)
            int squeezed = squeezedTo(area->width() - 2 * margin - (topRow ? m_leftInset + m_rightInset : 0));
            if (topRow && squeezed < 0) {
                squeezed = squeezedTo(area->width() - 2 * margin - m_leftInset);
                y = margin + height() + KisModernStyle::space1;
                span.second = qMax(span.first, area->width() - margin - openWidth());
            }
            if (m_squeezable && !m_tucked && qAbs(squeezed) != m_squeezable->maximumWidth()) m_squeezable->setMaximumWidth(qAbs(squeezed));
            // its width follows the tool: against an edge or in the middle it stays there, elsewhere its grip stays
            // where it was dropped (a share of the room it had at the width it had then)
            int x = span.first + offsetOf(m_spot.x(), span.second - span.first, grown.width());
            if (m_spot.x() > 0 && m_spot.x() < 1 && m_spot.x() != 0.5 && m_refWidth > 0) {
                const int refSpan = span.second + openWidth() - m_refWidth;
                x = qBound(span.first, span.first + offsetOf(m_spot.x(), refSpan - span.first, grown.width()), span.second);
            }
            m_glide->moveTo(QPoint(x, y), m_byPointer);
        }
        KisStudioLayout::keepAbove(this);
    }

    /// Where it was dragged to, as shares of the room it has (see shareOf()), and its width then
    void setSpot(const QPointF &spot, int width)
    {
        m_spot = spot;
        m_refWidth = width;
        place();
    }
    QPointF spot() const { return m_spot; }
    int spotWidth() const { return m_refWidth; }

    /// Whether it sits along the top of the canvas, or else along the bottom
    bool isAtTop() const { return m_spot.y() < 0.5; }

    /// Space in the top row reserved for the window handle on the left and the window's buttons on the right
    void setInsets(int left, int right)
    {
        if (left == m_leftInset && right == m_rightInset) return;
        m_leftInset = left;
        m_rightInset = right;
        place();
    }

    /// Called when resized, moved, shown or hidden, so whatever is placed around it can follow
    std::function<void()> changed;
    /// Called when dropped after a drag by its grip
    std::function<void(const QPointF &spot)> moved;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == parentWidget() && event->type() == QEvent::Resize) {
            place();
        }
        return QWidget::eventFilter(watched, event);
    }

    void resizeEvent(QResizeEvent *event) override
    {
        QWidget::resizeEvent(event);
        place();
        if (changed) changed();
    }

    void showEvent(QShowEvent *event) override
    {
        QWidget::showEvent(event);
        if (changed) changed();
    }

    void hideEvent(QHideEvent *event) override
    {
        QWidget::hideEvent(event);
        if (changed) changed();
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        paintGlass(painter, this, 12, 0.12);
    }

private:
    int roomY() const { return parentWidget()->height() - 2 * margin - height(); }

    /// The squeezable widget's widest for the open pill to fit \p room (QWIDGETSIZE_MAX: it fits whole); negated
    /// when even its narrowest doesn't fit
    int squeezedTo(int room) const
    {
        if (!m_squeezable || m_tucked) return QWIDGETSIZE_MAX;
        const int natural = m_squeezable->sizeHint().width();
        const int whole = layout()->sizeHint().width() - qMin(natural, m_squeezable->maximumWidth()) + natural;
        if (whole <= room) return QWIDGETSIZE_MAX;
        // its icon and a few letters stay
        const int least = qMin(natural, m_squeezable->height() + 6 * m_squeezable->fontMetrics().averageCharWidth());
        const int most = natural - (whole - room);
        return most >= least ? most : -least;
    }

    /// The span its left edge can take at height \p y; in the top row, between the window handle and the window's buttons
    QPair<int, int> spanAt(int y) const
    {
        const bool topRow = y < margin + height();
        const int from = margin + (topRow ? m_leftInset : 0);
        return {from, qMax(from, parentWidget()->width() - margin - (topRow ? m_rightInset : 0) - openWidth())};
    }

    /// Placed as if open, so the grip stays where it was when tucked
    int openWidth() const { return m_tucked ? qMax(width(), m_openWidth) : width(); }

    /// Follows the grip, caught by the edges and the middle as it goes
    void dragTo(const QPoint &globalPos)
    {
        const QPoint at = parentWidget()->mapFromGlobal(globalPos) - m_grab;
        const QSize grown = roomGrowth(this);
        const qreal down = shareOf(at.y() - margin, roomY(), grown.height());
        const QPair<int, int> span = spanAt(margin + offsetOf(down, roomY(), grown.height()));
        m_spot = QPointF(shareOf(at.x() - span.first, span.second - span.first, grown.width()), down);
        m_refWidth = openWidth();
        m_byPointer = at;
        place();
        m_byPointer.reset();
        if (changed) changed();
    }

    bool m_right {false};
    int m_leftInset {0};
    int m_rightInset {0};
    bool m_tucked {false};
    int m_openWidth {0};
    QPointF m_spot;
    int m_refWidth {0};
    QPoint m_grab;
    TopGrip *m_grip {nullptr};
    QWidget *m_contents {nullptr};
    QPointer<QWidget> m_squeezable;
    KisStudioGlide *m_glide {nullptr};
    /// Where the pointer alone puts it, while dragged
    std::optional<QPoint> m_byPointer;
};

/// The application icon in the top-left corner, capping the tool rail: a click goes home, a drag moves the window
class WindowHandle : public QWidget
{
public:
    std::function<void()> clicked;

    explicit WindowHandle(QWidget *canvasArea)
        : QWidget(canvasArea)
    {
        setObjectName(QStringLiteral("studioWindowHandle"));
        setToolTip(i18nc("@info:tooltip the app's icon in the window's corner: your artworks", "Home\nDrag: move"));
        hide();
    }

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton) return;
        m_press = event->globalPos();
        m_pressed = true;
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (!m_pressed || (event->globalPos() - m_press).manhattanLength() <= KisModernStyle::dragDistance(event)) return;
        m_pressed = false;
        if (QWindow *window = this->window()->windowHandle()) window->startSystemMove();
    }

    void mouseReleaseEvent(QMouseEvent *) override
    {
        if (!m_pressed) return;
        m_pressed = false;
        if (clicked) clicked();
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        paintGlass(painter, this, 12, 0.12);
        const int side = qMin(20, qMin(width(), height()) - 12);
        painter.setOpacity(0.85);
        KisIconUtils::loadIcon(QStringLiteral("krita-branding")).paint(&painter, QRect(QPoint(), QSize(side, side)).translated(rect().center() - QPoint(side / 2, side / 2)));
    }

private:
    QPoint m_press;
    bool m_pressed {false};
};

/// A button on the window pill: the menu, Settings or Find
class PillButton : public QAbstractButton
{
public:
    /// Called on a right click or long press
    std::function<void()> contextClicked;

    PillButton(const QString &iconName, const QString &toolTip, QWidget *parent)
        : QAbstractButton(parent)
        , m_iconName(iconName)
    {
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::NoFocus);
        setAttribute(Qt::WA_Hover);
        setFixedSize(34, 30);
        setToolTip(toolTip);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        if (underMouse()) {
            QColor background = palette().color(QPalette::ButtonText);
            background.setAlphaF(0.09);
            painter.setPen(Qt::NoPen);
            painter.setBrush(background);
            painter.drawPath(KisModernStyle::squircle(QRectF(rect()).adjusted(1, 1, -1, -1), KisModernStyle::tileRadius));
        }
        const int iconSize = 18;
        KisIconUtils::loadIcon(m_iconName).paint(&painter, QRect((width() - iconSize) / 2, (height() - iconSize) / 2, iconSize, iconSize));
    }

    void contextMenuEvent(QContextMenuEvent *event) override
    {
        if (!contextClicked) {
            QAbstractButton::contextMenuEvent(event);
            return;
        }
        event->accept();
        contextClicked();
    }

private:
    QString m_iconName;
};

/// The line between two lanes of a group of panels: dragged, it sets their widths; a click takes them apart
class LaneDivider : public QWidget
{
public:
    static constexpr int thickness = 7;

    std::function<void()> pressed;
    /// How far it was dragged since the press
    std::function<void(int dx)> dragged;
    std::function<void()> released;
    std::function<void()> clicked;

    explicit LaneDivider(QWidget *parent)
        : QWidget(parent)
    {
        setObjectName(QStringLiteral("studioPanelsLanes"));
        setAttribute(Qt::WA_Hover);
        setCursor(Qt::SizeHorCursor);
        setFixedWidth(thickness);
        setToolTip(i18nc("@info:tooltip the line between groups of panels joined side by side", "Joined\nDrag: resize\nClick: separate"));
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        QColor ink = palette().color(QPalette::WindowText);
        ink.setAlphaF(underMouse() || m_pressed ? 0.5 : 0.22);
        const int x = width() / 2;
        painter.fillRect(QRect(x, KisModernStyle::space2, 1, height() - 2 * KisModernStyle::space2), ink);
    }

    bool event(QEvent *event) override
    {
        if (event->type() == QEvent::HoverEnter || event->type() == QEvent::HoverLeave) update();
        return QWidget::event(event);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton) {
            QWidget::mousePressEvent(event);
            return;
        }
        m_pressed = true;
        m_dragging = false;
        m_pressX = globalMousePos(event).x();
        if (pressed) pressed();
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (!m_pressed) return;
        if (!(event->buttons() & Qt::LeftButton)) {
            // let go in another window, whose release never comes here: done where it is
            m_pressed = false;
            update();
            if (m_dragging && released) released();
            m_dragging = false;
            return;
        }
        const int dx = globalMousePos(event).x() - m_pressX;
        if (!m_dragging && qAbs(dx) < KisModernStyle::dragDistance(event)) return;
        m_dragging = true;
        if (dragged) dragged(dx);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton || !m_pressed) return;
        m_pressed = false;
        update();
        if (m_dragging) {
            if (released) released();
        } else if (clicked) {
            clicked();
        }
    }

private:
    bool m_pressed {false};
    bool m_dragging {false};
    int m_pressX {0};
};

} // namespace KisStudioLayoutParts

using namespace KisStudioLayoutParts;

#endif // KISSTUDIOLAYOUTCHROME_P_H
