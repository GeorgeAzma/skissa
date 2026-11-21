/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIOPANELCOLUMN_P_H
#define KISSTUDIOPANELCOLUMN_P_H

#include "KisStudioLayoutChrome_p.h"

// A group of pinned panels over the canvas, and a panel opened as a flyout; private to KisStudioLayout*.cpp


/**
 * The pinned panels in a glass pill over the canvas, below the strip of panel
 * buttons (or a grip), in lanes side by side, each a stack of panels. Dragging
 * the strip moves it: against an edge it is docked there and the canvas makes
 * room for it, anywhere else it floats; dropped against another group, it
 * joins it (see joining). Dragging the inner edge sets the width; the line
 * between two lanes sets theirs, and a click on it takes them apart.
 */
class KisStudioPanelColumn : public QWidget
{
public:
    /// Distance from the canvas area's edges
    static constexpr int margin = 6;
    /// Width of the draggable inner edge
    static constexpr int edge = 5;
    static constexpr int minimumColumnWidth = 220;

    /// Called when the geometry changes; \p done is true once a width drag ends
    std::function<void(bool done)> resized;
    /// Called when dragged by the strip and dropped, or resized: the side of the canvas it is on and its place from that side
    std::function<void(bool left, const QPointF &spot)> moved;
    std::function<void()> tapped;
    /// Each lane's stack as it is made, to set up
    std::function<void(PanelStack *stack)> stackMade;
    /// Dragged by its strip to \p placed (as if open) and, if \p dropped, let go there: whether it would join
    /// another group there, which it then does
    std::function<bool(KisStudioPanelColumn *column, const QRect &placed, bool dropped)> joining;
    /// The line before \p lane was clicked: the lanes from it on go apart
    std::function<void(int lane)> separated;
    /// The handle under \p above in \p lane was tapped: the panels below it go apart
    std::function<void(int lane, QDockWidget *above)> separatedBelow;
    /// Its group (see groupColumn())
    int group {0};

    KisStudioPanelColumn(QWidget *canvasArea, bool left, const QVector<int> &widths)
        : QWidget(canvasArea)
        , m_settled(++settledCount())
        , m_left(left)
        , m_widths(widths.isEmpty() ? QVector<int>{270} : widths)
    {
        setObjectName(QStringLiteral("studioPanels"));
        m_layout = new QVBoxLayout(this);
        m_layout->setSpacing(0);
        // it is sized by place(): a minimum kept from when its panels showed would keep it from tucking
        m_layout->setSizeConstraint(QLayout::SetNoConstraint);
        m_lanes = new QHBoxLayout();
        m_lanes->setSpacing(0);
        m_layout->addLayout(m_lanes);
        updateMargins();
        setMouseTracking(true);
        // a separate widget, since a cursor set on the column would also apply to the panels
        m_grip = new QWidget(this);
        m_grip->setObjectName(QStringLiteral("studioPanelsEdge"));
        m_grip->setCursor(Qt::SizeHorCursor);
        m_grip->setAttribute(Qt::WA_NoSystemBackground);
        m_grip->installEventFilter(this);
        canvasArea->installEventFilter(this);
        m_glide.setDuration(KisModernStyle::changeDuration);
        m_glide.setEasingCurve(KisModernStyle::motionCurve());
        QObject::connect(&m_glide, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
            setGeometry(m_target.translated(value.toPoint()));
        });
        hide();
    }

    /// Its lanes' stacks, left to right
    QVector<PanelStack *> stacks() const { return m_stacks; }
    /// The width of its first lane (a new group's, until set)
    int columnWidth() const { return m_widths.value(0, 270); }
    /// Each lane's width, left to right, with what is outside its stack
    QVector<int> laneWidths() const { return m_widths; }
    bool isLeft() const { return m_left; }
    /// Against its side's edge, rather than floating over the canvas
    bool isDocked() const { return m_spot.x() <= 0; }
    /// Where it is going, or is when it isn't gliding
    QRect targetGeometry() const { return m_target.isNull() ? geometry() : m_target; }

    /// Where it was dragged to, from its side, as shares of the room it has (see shareOf())
    void setSpot(const QPointF &spot, int refHeight = 0)
    {
        m_spot = spot;
        m_refHeight = refHeight;
        place();
    }
    /// Its height when its place was taken (see place()), 0 if not known
    int refHeight() const { return m_refHeight; }

    /// Takes the place of \p box (canvas area coordinates) from its top and its side's edge, as if dropped there
    void placeAt(const QRect &box)
    {
        const bool left = box.center().x() < parentWidget()->width() / 2;
        setSide(left);
        const QSize size = placedSize(sizeNow());
        m_spot = spotOf(QRect(QPoint(left ? box.left() : box.right() + 1 - size.width(), box.top()), size), left, false);
        m_refHeight = size.height();
        m_settled = ++settledCount();
        m_chosen.start();
        place();
        if (moved) moved(m_left, m_spot);
    }

    void setStrip(QWidget *strip)
    {
        if (m_strip == strip) return;
        m_strip = strip;
        m_layout->insertWidget(0, strip);
    }

    void setLeft(bool left)
    {
        setSide(left);
        place();
    }

    /// Shows the panels below the strip, or only the strip
    void setPanelsShown(bool shown)
    {
        m_shownPanels = shown;
        for (PanelStack *stack : qAsConst(m_stacks)) stack->setVisible(shown);
        for (LaneDivider *divider : qAsConst(m_dividers)) divider->setVisible(shown);
        m_layout->invalidate();
        place();
        placeGrip();
    }
    bool panelsShown() const { return m_shownPanels; }

    QList<QDockWidget *> panels() const
    {
        QList<QDockWidget *> docks;
        for (PanelStack *stack : m_stacks) docks += stack->panels();
        return docks;
    }

    /// Takes \p lanes of panels, left to right, each top to bottom keeping its visibility, \p widths wide
    void setLanes(const QList<QList<QDockWidget *>> &lanes, const QVector<int> &widths)
    {
        // always a lane, perhaps empty: the column with the panel buttons has one without panels
        const int count = qMax(1, int(lanes.size()));
        while (m_stacks.size() < count) {
            auto *stack = new PanelStack(this);
            stack->wantsHeight = [this]() { place(); };
            stack->separated = [this, stack](QDockWidget *above) {
                if (separatedBelow) separatedBelow(m_stacks.indexOf(stack), above);
            };
            if (stackMade) stackMade(stack);
            stack->setVisible(m_shownPanels);
            m_stacks << stack;
        }
        for (int i = 0; i < count; i++) m_stacks[i]->setPanels(lanes.value(i));
        while (m_stacks.size() > count) {
            PanelStack *stack = m_stacks.takeLast();
            // a panel it still holds is on its way to another group's lane (taken later in the same arranging):
            // it stays as shown as it was, rather than going with the stack
            for (QDockWidget *dock : stack->findChildren<QDockWidget *>(QString(), Qt::FindDirectChildrenOnly)) {
                const bool shown = !dock->isHidden();
                dock->setParent(parentWidget());
                dock->setVisible(shown);
            }
            stack->deleteLater();
        }
        m_widths = widths;
        while (m_widths.size() < count) m_widths << columnWidth();
        m_widths.resize(count);
        rebuildLanes();
        place();
    }

    /// The canvas area's part a lane covers
    QRect laneRect(int lane) const
    {
        if (lane < 0 || lane >= m_stacks.size()) return QRect();
        // the first and the last reach the column's edges
        const QRect stack = m_stacks[lane]->geometry();
        const int left = lane == 0 ? 0 : stack.x();
        const int right = lane == m_stacks.size() - 1 ? width() : stack.right() + 1;
        return QRect(mapToParent(QPoint(left, 0)), QSize(right - left, height()));
    }

    /// Places it over the canvas area, as tall as its tallest lane wants within it (just the strip without them)
    void place()
    {
        QWidget *area = parentWidget();
        const QPair<int, int> span = spanY();
        applyWidths();
        // with only the strip showing, placed as if open, so the strip stays where it was; while the group is
        // dragged its panels don't change, so they are measured once rather than at every move
        if (!m_moving || m_movingSize.isEmpty()) m_movingSize = sizeNow();
        const QSize size = m_movingSize;
        const QSize placed = placedSize(size);
        const QSize grown = roomGrowth(area);
        const int across = offsetOf(m_spot.x(), area->width() - 2 * margin - placed.width(), grown.width());
        // the height its place was taken at: as it grows or folds it keeps its top, or against the bottom its bottom
        // a place saved without it gets the height it settled at, kept with it from then on
        const bool learnt = m_refHeight <= 0 && isVisible() && m_shown.isValid() && m_shown.elapsed() >= 500;
        if (learnt) m_refHeight = placed.height();
        const int refHeight = m_refHeight > 0 ? m_refHeight : placed.height();
        int y = span.first + offsetOf(m_spot.y(), span.second - span.first - refHeight, grown.height());
        if (m_spot.y() >= 1) y = span.second - placed.height();
        y = qMax(span.first, qMin(y, span.second - placed.height()));
        const QRect wanted(QPoint(m_left ? margin + across : area->width() - margin - placed.width() - across, y), placed);
        // while dragged, it makes way for all the others, as it will once dropped: it shows where it will stay
        const QRect at = clearOfOthers(wanted, span, m_moving ? std::numeric_limits<quint64>::max() : m_settled);
        m_placed = at;
        // the group placed last made way by choice: where it went is its place, so the next start agrees; made
        // to make way later (the window smaller), it goes back once there is room
        bool madeWay = false;
        if (!m_moving && at != wanted && m_settled == settledCount() && isVisible() && m_shown.isValid() && m_shown.elapsed() >= 500
            && m_chosen.isValid() && m_chosen.elapsed() < 1500) {
            m_spot = spotOf(at, m_left, false);
            m_refHeight = at.height();
            madeWay = true;
        }
        // a column on the right keeps its right edge when only its strip shows
        const QRect geometry(m_left ? at.x() : at.right() + 1 - size.width(), at.y(), size.width(), size.height());
        const bool changed = geometry != m_target;
        glideTo(geometry, at.topLeft() - wanted.topLeft(), m_moving);
        KisStudioLayout::keepAbove(this);
        if (changed && resized) resized(false);
        // told once it is on its way there: what it sets off (the insets) places the groups again, this one too
        if ((madeWay || learnt) && moved) moved(m_left, m_spot);
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == parentWidget() && event->type() == QEvent::Resize && isVisible()) {
            place();
        }
        if (watched == m_grip) {
            switch (event->type()) {
            case QEvent::MouseButtonPress:
                if (static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton) {
                    m_resizing = true;
                    // the outer edge stays where it is
                    m_outer = m_left ? x() : geometry().right() + 1;
                    update();
                    return true;
                }
                break;
            case QEvent::MouseMove:
                if (m_resizing && !(static_cast<QMouseEvent *>(event)->buttons() & Qt::LeftButton)) {
                    // let go in another window, whose release never comes here: done at the width it has
                    m_resizing = false;
                    if (resized) resized(true);
                    if (!isDocked() && moved) moved(m_left, m_spot);
                    update();
                    return true;
                }
                if (m_resizing) {
                    const QWidget *area = parentWidget();
                    const int x = area->mapFromGlobal(globalMousePos(static_cast<QMouseEvent *>(event))).x();
                    // the lane at the edge takes the change
                    const int lane = m_left ? m_widths.size() - 1 : 0;
                    const int others = totalWidth() - laneWidth(lane);
                    m_widths[lane] = qBound(minimumColumnWidth, (m_left ? x - m_outer : m_outer - x) - others, qMax(minimumColumnWidth, area->width() / 2));
                    if (!isDocked()) {
                        const int from = m_left ? m_outer - margin : area->width() - margin - m_outer;
                        m_spot.setX(shareAt(from, area->width() - 2 * margin - totalWidth(), roomGrowth(area).width()));
                    }
                    place();
                    return true;
                }
                break;
            case QEvent::MouseButtonRelease:
                if (m_resizing && static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton) {
                    m_resizing = false;
                    if (resized) resized(true);
                    if (!isDocked() && moved) moved(m_left, m_spot);
                    update();
                    return true;
                }
                break;
            default:
                break;
            }
        }
        return QWidget::eventFilter(watched, event);
    }

    void showEvent(QShowEvent *event) override
    {
        QWidget::showEvent(event);
        m_shown.start();
        place();
        placeGrip();
        // settled in: the height its place is kept with (see place())
        if (m_refHeight <= 0) QTimer::singleShot(600, this, [this]() { place(); });
    }

    void resizeEvent(QResizeEvent *event) override
    {
        QWidget::resizeEvent(event);
        placeGrip();
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        paintGlass(painter, this, 12, m_resizing || m_moving ? 0.3 : 0.12);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        // pressed on the strip: the pill follows the pointer
        if (event->button() != Qt::LeftButton || !m_strip || event->pos().y() > m_strip->geometry().bottom()) {
            QWidget::mousePressEvent(event);
            return;
        }
        m_pressed = true;
        m_moving = false;
        m_grab = event->pos();
        m_pressPos = globalMousePos(event);
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (!m_pressed) {
            QWidget::mouseMoveEvent(event);
            return;
        }
        if (!(event->buttons() & Qt::LeftButton)) {
            // its release went elsewhere (the group was hidden or the window lost the pointer): dropped where it is
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
            m_movingSize = QSize();
            // what is carried goes over the other overlays
            raise();
            setCursor(Qt::ClosedHandCursor);
            update();
        }
        // caught by the edges and the middle as it goes, so it shows where it will stay
        const QWidget *area = parentWidget();
        const QPoint at = area->mapFromGlobal(global) - m_grab;
        const QSize placed = placedSize(size());
        const QRect rect(QPoint(m_left ? at.x() : at.x() + width() - placed.width(), at.y()), placed);
        setSide(rect.center().x() < area->width() / 2);
        m_spot = spotOf(rect, m_left, true);
        m_refHeight = rect.height();
        place();
        if (joining) joining(this, m_placed, false);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton || !m_pressed) {
            QWidget::mouseReleaseEvent(event);
            return;
        }
        m_pressed = false;
        if (!m_moving) {
            if (tapped) tapped();
            return;
        }
        m_moving = false;
        unsetCursor();
        update();
        // the group moved last makes way for the others
        m_settled = ++settledCount();
        m_chosen.start();
        if (joining && joining(this, m_placed, true)) return;
        place();
        if (moved) moved(m_left, m_spot);
    }

private:
    int laneWidth(int lane) const
    {
        // never narrower than its panels can be: a narrower one runs them past its edge, cut off
        int least = minimumColumnWidth;
        if (lane < m_stacks.size()) {
            const QMargins margins = m_layout->contentsMargins();
            for (QDockWidget *dock : m_stacks[lane]->panels()) {
                if (!dock->isHidden()) least = qMax(least, dock->minimumSizeHint().width() + margins.left() + margins.right());
            }
        }
        return qBound(least, m_widths.value(lane, columnWidth()), qMax(least, parentWidget()->width() / 2));
    }

    /// All its lanes' widths and the lines between them
    int totalWidth() const
    {
        int total = (m_widths.size() - 1) * LaneDivider::thickness;
        for (int i = 0; i < m_widths.size(); i++) total += laneWidth(i);
        return qMin(total, qMax(minimumColumnWidth, parentWidget()->width() - 2 * margin));
    }

    /// Each stack as wide as its lane, less what is outside the stacks at the column's edges
    void applyWidths()
    {
        const QMargins margins = m_layout->contentsMargins();
        for (int i = 0; i < m_stacks.size(); i++) {
            int width = laneWidth(i);
            if (i == 0) width -= margins.left();
            if (i == m_stacks.size() - 1) width -= margins.right();
            if (m_stacks[i]->width() != width || m_stacks[i]->minimumWidth() != width) m_stacks[i]->setFixedWidth(qMax(0, width));
        }
    }

    /// The stacks side by side with a line between each two
    void rebuildLanes()
    {
        while (m_lanes->count() > 0) delete m_lanes->takeAt(0);
        for (LaneDivider *divider : qAsConst(m_dividers)) {
            divider->hide();
            divider->deleteLater();
        }
        m_dividers.clear();
        for (int i = 0; i < m_stacks.size(); i++) {
            if (i > 0) {
                auto *divider = new LaneDivider(this);
                // the lane on its free side takes the change, so the line stays under the pointer
                const int lane = m_left ? i - 1 : i;
                divider->pressed = [this, lane]() { m_dividerStart = m_widths.value(lane); };
                divider->dragged = [this, lane](int dx) {
                    if (lane >= m_widths.size()) return;
                    m_widths[lane] = qBound(minimumColumnWidth, m_dividerStart + (m_left ? dx : -dx), qMax(minimumColumnWidth, parentWidget()->width() / 2));
                    place();
                };
                divider->released = [this]() {
                    if (resized) resized(true);
                };
                // after the click is done with: taking the lanes apart remakes the lines
                divider->clicked = [this, i]() {
                    QTimer::singleShot(0, this, [this, i]() {
                        if (separated) separated(i);
                    });
                };
                divider->setVisible(m_shownPanels);
                m_dividers << divider;
                m_lanes->addWidget(divider);
            }
            m_lanes->addWidget(m_stacks[i]);
        }
        placeGrip();
    }

    /// How far it still is from where it is going
    QPoint glideNow() const { return m_glide.state() == QAbstractAnimation::Running ? m_glide.currentValue().toPoint() : QPoint(); }

    /**
     * Goes to \p target: at once where the pointer takes it, gliding the rest, so a group making way is seen going
     * there. While \p dragged, \p extra is the part of its place the pointer didn't make.
     */
    void glideTo(const QRect &target, const QPoint &extra, bool dragged)
    {
        const QWidget *area = parentWidget();
        // nothing to glide from: just shown (and settling in), or the window changed size
        const bool fresh = !isVisible() || m_target.isNull() || area->size() != m_areaSize || !m_shown.isValid() || m_shown.elapsed() < 500;
        const QPoint now = glideNow();
        QPoint from = fresh ? QPoint() : pos() - target.topLeft();
        if (dragged && !fresh) {
            // dragged, it follows the pointer or stays put (held off a group) at once, each way apart: sliding
            // along a group it is held off follows the pointer one way and stays the other; only a jump glides
            const QPoint moved = target.topLeft() - m_target.topLeft();
            const QPoint byPointer = moved - (extra - m_extra);
            auto jump = [](int moved, int byPointer) { return qAbs(moved) > 3 && qAbs(moved - byPointer) > 3 ? moved : 0; };
            from = now - QPoint(jump(moved.x(), byPointer.x()), jump(moved.y(), byPointer.y()));
        }
        m_areaSize = area->size();
        m_target = target;
        m_extra = extra;
        // what keeps off it (the zoom pill) keeps off where it is going
        setProperty("studioTarget", target);
        if (from.manhattanLength() < 2) {
            m_glide.stop();
            setGeometry(target);
            return;
        }
        // a new jump starts the glide again; else it keeps going
        if (m_glide.state() != QAbstractAnimation::Running || from != now) {
            m_glide.stop();
            m_glide.setStartValue(from);
            m_glide.setEndValue(QPoint());
            m_glide.start();
        }
        setGeometry(target.translated(glideNow()));
    }

    /// Its size now: as tall as its tallest lane wants within the room, or just the strip without them
    QSize sizeNow()
    {
        // no taller than the room between the bars along the top and bottom, so that it fits beside or under them
        const QWidget *area = parentWidget();
        const QPair<int, int> span(area->property(topInsetProperty).toInt() + margin,
                                   area->height() - area->property(bottomInsetProperty).toInt() - margin);
        const QMargins margins = m_layout->contentsMargins();
        // a grip of fixed height has no size hint
        const int strip = m_strip && !m_strip->isHidden() ? qMax(m_strip->sizeHint().height(), m_strip->minimumHeight()) : 0;
        // tucked, just the strip, as wide as when open
        if (!panelsShown()) return QSize(m_openSize.isEmpty() ? totalWidth() : m_openSize.width(), margins.top() + margins.bottom() + strip);
        int tallest = 0;
        for (PanelStack *stack : qAsConst(m_stacks)) tallest = qMax(tallest, stack->wantedHeight());
        const int wanted = margins.top() + margins.bottom() + strip + tallest;
        m_openSize = QSize(totalWidth(), qBound(0, wanted, qMax(0, span.second - span.first)));
        return m_openSize;
    }

    QSize placedSize(const QSize &size) const { return panelsShown() || m_openSize.isEmpty() ? size : m_openSize; }

    /// The top and bottom it can reach: the whole height, the bars along the edges kept off as obstacles (clearOfOthers),
    /// so a group can sit beside the top bar
    QPair<int, int> spanY() const
    {
        return {margin, parentWidget()->height() - margin};
    }

    /// The pills along the canvas's edges and the window's handle, which the groups keep off
    QVector<QRect> barRects() const
    {
        QVector<QRect> bars;
        for (QObject *child : parentWidget()->children()) {
            auto *widget = qobject_cast<QWidget *>(child);
            if (!widget || widget->isHidden()) continue;
            const QString name = widget->objectName();
            if (name == QLatin1String("studioTopPill") || name == QLatin1String("studioWindowPill") || name == QLatin1String("studioWindowHandle")) {
                // a bar gliding somewhere is already there
                const QRect target = widget->property("studioTarget").toRect();
                bars << (target.isValid() ? target : widget->geometry());
            }
        }
        return bars;
    }

    /// The place of a box at \p box, from its side, as shares of the room it has; \p snap: caught by the edges and the middle
    QPointF spotOf(const QRect &box, bool left, bool snap) const
    {
        const QWidget *area = parentWidget();
        const QPair<int, int> span = spanY();
        const int across = left ? box.x() - margin : area->width() - margin - box.x() - box.width();
        const int down = box.y() - span.first;
        const int roomX = area->width() - 2 * margin - box.width();
        const int roomY = span.second - span.first - box.height();
        const QSize grown = roomGrowth(area);
        if (snap) return QPointF(shareOf(across, roomX, grown.width()), shareOf(down, roomY, grown.height()));
        return QPointF(shareAt(across, roomX, grown.width()), shareAt(down, roomY, grown.height()));
    }

    void setSide(bool left)
    {
        if (left == m_left) return;
        m_left = left;
        updateMargins();
        rebuildLanes();
    }

    /// One more each time a group is made or moved: the one moved last makes way for the others
    static quint64 &settledCount()
    {
        static quint64 count = 0;
        return count;
    }

    /// The columns over the canvas area (it has no meta object, so findChildren() would take any widget)
    QList<KisStudioPanelColumn *> columnsBeside() const
    {
        QList<KisStudioPanelColumn *> columns;
        for (QObject *child : parentWidget()->children()) {
            if (auto *column = dynamic_cast<KisStudioPanelColumn *>(child)) columns << column;
        }
        return columns;
    }

    /// \p wanted moved the least it takes off the tool rail and the groups that settled before \p settled (those it overlaps)
    QRect clearOfOthers(QRect wanted, const QPair<int, int> &span, quint64 settled) const
    {
        QVector<QRect> obstacles;
        const QRect rail = parentWidget()->property(railRectProperty).toRect();
        if (!rail.isEmpty()) obstacles << rail;
        const QVector<QRect> bars = barRects();
        // the bars never move for a group, so they count first, with the rail
        const int fixed = obstacles.size() + bars.size();
        obstacles << bars;
        for (KisStudioPanelColumn *other : columnsBeside()) {
            if (other == this || other->isHidden() || other->m_settled > settled) continue;
            obstacles << other->targetGeometry();
        }
        auto clear = [&](const QRect &at, int count) {
            for (int i = 0; i < count; i++) {
                if (obstacles[i].intersects(at)) return false;
            }
            return true;
        };
        if (clear(wanted, obstacles.size())) return wanted;
        // the nearest place clear of them all: beside, above or below any of them, or in a corner two of them make
        const int gap = margin;
        QVector<int> xs{wanted.x()};
        QVector<int> ys{wanted.y()};
        for (const QRect &them : qAsConst(obstacles)) {
            xs << them.right() + 1 + gap << them.left() - gap - wanted.width();
            ys << them.bottom() + 1 + gap << them.top() - gap - wanted.height();
        }
        const int minX = margin;
        const int maxX = parentWidget()->width() - margin - wanted.width();
        // the rail and the bars first: when nothing is clear of all, at least off them
        for (int count : {int(obstacles.size()), fixed}) {
            QRect best;
            for (int x : qAsConst(xs)) {
                for (int y : qAsConst(ys)) {
                    if (x < minX || x > maxX || y < span.first || y + wanted.height() > span.second) continue;
                    const QRect at(QPoint(x, y), wanted.size());
                    if (!clear(at, count)) continue;
                    if (best.isNull() || (at.topLeft() - wanted.topLeft()).manhattanLength() < (best.topLeft() - wanted.topLeft()).manhattanLength()) best = at;
                }
            }
            if (!best.isNull()) return best;
        }
        return wanted;
    }

    /// The resize grip covers the inner edge, only while the panels are shown
    void placeGrip()
    {
        m_grip->setGeometry(m_left ? width() - edge : 0, 0, edge, height());
        m_grip->setVisible(panelsShown());
        m_grip->raise();
    }

    void updateMargins()
    {
        m_layout->setContentsMargins(m_left ? 1 : edge, 1, m_left ? edge : 1, 1);
    }

    QVBoxLayout *m_layout {nullptr};
    QHBoxLayout *m_lanes {nullptr};
    QVector<PanelStack *> m_stacks;
    QVector<LaneDivider *> m_dividers;
    QPointer<QWidget> m_strip;
    QWidget *m_grip {nullptr};
    /// When it was made or last moved (see settledCount())
    quint64 m_settled {0};
    bool m_left {false};
    QPointF m_spot;
    /// Its height when its place was taken (see place())
    int m_refHeight {0};
    QSize m_openSize;
    bool m_shownPanels {true};
    /// Where it was last placed, as if open
    QRect m_placed;
    /// Its size while dragged (see place())
    QSize m_movingSize;
    /// Where it is going (see glideTo()), from what and in which window size
    QRect m_target;
    QVariantAnimation m_glide;
    QPoint m_extra;
    QSize m_areaSize;
    QElapsedTimer m_shown;
    /// Since it was dropped or placed
    QElapsedTimer m_chosen;
    QVector<int> m_widths;
    int m_dividerStart {0};
    bool m_resizing {false};
    int m_outer {0};
    bool m_pressed {false};
    bool m_moving {false};
    QPoint m_grab;
    QPoint m_pressPos;
};

/**
 * An unpinned panel opened in glass over the canvas. It is a child of the main
 * window, above the pills: a separate window could not show QML panels, and a
 * floating dock would be saved with the window state, which crashes on restore.
 */
class FlyoutWindow : public QWidget
{
public:
    explicit FlyoutWindow(QWidget *mainWindow)
        : QWidget(mainWindow)
    {
        setObjectName(QStringLiteral("studioFlyout"));
        setAttribute(Qt::WA_TranslucentBackground);
        KisStudioGlass::setOverOverlays(this);
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(1, 1, 1, 1);
        layout->setSpacing(0);
    }

    void hold(QDockWidget *dock)
    {
        m_panel = dock;
        KisStudioGlass::setSeeThrough(dock, true);
        // a list stays a list here as in a column: shown while still small it turned into an empty strip
        keepUpright(dock, true);
        layout()->addWidget(dock);
        dock->show();
    }

    /// Lets go of the panel, which stays a child of this window until docked elsewhere
    void release()
    {
        if (m_panel) {
            layout()->removeWidget(m_panel);
            KisStudioGlass::setSeeThrough(m_panel, false);
        }
        m_panel = nullptr;
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        paintGlass(painter, this, 12, 0.14);
    }

private:
    QPointer<QDockWidget> m_panel;
};


#endif // KISSTUDIOPANELCOLUMN_P_H
