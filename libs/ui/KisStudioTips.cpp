/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioTips.h"
#include "KisSettingsRegistry.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QHash>
#include <QHelpEvent>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QRegularExpression>
#include <QScreen>
#include <QTabletEvent>
#include <QTextDocument>
#include <QTimer>
#include <QToolTip>
#include <QUrl>
#include <QtMath>

#include <KisModernStyle.h>
#include <KisProfile.h>
#include <KisSentenceCase.h>
#include <KStandardGuiItem>

#include "KisStudioMotion.h"
#include <KSharedConfig>

namespace
{

enum class Gesture { Click, DoubleClick, RightClick, Drag, DragUpDown, Scroll };

/// One line of a tooltip read as a gesture: "Shift+drag: finer", "Drag onto the image: fill"
struct Line {
    Gesture gesture {Gesture::Click};
    QString modifier;
    QString where;
    QString action;
};

bool readLine(const QString &text, Line *line)
{
    static const QRegularExpression pattern(QStringLiteral("^(?:(Shift|Alt|Ctrl)\\+)?(Double-click|Right-click|Click|Tap|Drag|Scroll)([^:]*): (.+)$"),
                                            QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = pattern.match(text.trimmed());
    if (!match.hasMatch()) return false;
    const QString verb = match.captured(2).toLower();
    line->modifier = match.captured(1);
    line->where = match.captured(3).trimmed();
    line->action = match.captured(4).trimmed();
    if (verb == QLatin1String("double-click")) line->gesture = Gesture::DoubleClick;
    else if (verb == QLatin1String("right-click")) line->gesture = Gesture::RightClick;
    else if (verb == QLatin1String("scroll")) line->gesture = Gesture::Scroll;
    else if (verb == QLatin1String("drag")) {
        const bool upDown = line->where.startsWith(QLatin1String("up"), Qt::CaseInsensitive)
                            || line->where.startsWith(QLatin1String("down"), Qt::CaseInsensitive);
        line->gesture = upDown ? Gesture::DragUpDown : Gesture::Drag;
        // the arrow says which way
        if (upDown) line->where = line->where.section(QLatin1Char(' '), 1);
    } else {
        line->gesture = Gesture::Click;
    }
    return true;
}

/// Glyphs are drawn on a grid 16 high, a mouse alone 15 wide and one with a mark beside it 24, then scaled up
constexpr qreal glyphScale = 1.25;
constexpr int glyphHeight = 20;

int glyphWidth(Gesture gesture)
{
    const bool alone = gesture == Gesture::Click || gesture == Gesture::RightClick;
    return qCeil((alone ? 15 : 24) * glyphScale);
}

/// A little mouse doing \p gesture, with an arrow for a drag
void paintGlyph(QPainter &painter, Gesture gesture, const QColor &ink, const QColor &lit)
{
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.scale(glyphScale, glyphScale);
    const QRectF body(2.5, 1.5, 10, 13);
    const QPainterPath mouse = KisModernStyle::squircle(body, 4.5);
    QColor faint = ink;
    faint.setAlphaF(ink.alphaF() * 0.45);
    // the pressed button lit, so the left and right ones tell apart at a glance
    auto fill = [&](const QRectF &part) {
        QPainterPath button;
        button.addRect(part);
        painter.fillPath(mouse.intersected(button), lit);
    };
    const QRectF left(body.left(), body.top(), body.width() / 2, 5);
    const QRectF right(body.center().x(), body.top(), body.width() / 2, 5);
    switch (gesture) {
    case Gesture::Click:
    case Gesture::DoubleClick:
    case Gesture::Drag:
    case Gesture::DragUpDown:
        fill(left);
        break;
    case Gesture::RightClick:
        fill(right);
        break;
    case Gesture::Scroll:
        break;
    }
    painter.setPen(QPen(ink, 1.3));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(mouse);
    painter.setPen(QPen(faint, 1));
    painter.drawLine(QPointF(body.left(), body.top() + 5), QPointF(body.right(), body.top() + 5));
    painter.drawLine(QPointF(body.center().x(), body.top()), QPointF(body.center().x(), body.top() + 5));

    QPen pen(ink, 1.3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    painter.setPen(pen);
    auto arrowHead = [&](const QPointF &tip, const QPointF &direction) {
        const QPointF back = tip - direction * 2.6;
        const QPointF side(-direction.y() * 2.4, direction.x() * 2.4);
        painter.drawLine(tip, back + side);
        painter.drawLine(tip, back - side);
    };
    switch (gesture) {
    case Gesture::DoubleClick: {
        // a second press, as two ticks over the button
        painter.drawLine(QPointF(15.5, 3), QPointF(17.5, 1.5));
        painter.drawLine(QPointF(15.5, 6), QPointF(19, 5));
        break;
    }
    case Gesture::Drag:
        painter.drawLine(QPointF(15.5, 8), QPointF(22.5, 8));
        arrowHead(QPointF(15.5, 8), QPointF(-1, 0));
        arrowHead(QPointF(22.5, 8), QPointF(1, 0));
        break;
    case Gesture::DragUpDown:
    case Gesture::Scroll:
        if (gesture == Gesture::Scroll) {
            // the wheel
            painter.setPen(Qt::NoPen);
            painter.setBrush(lit);
            painter.drawPath(KisModernStyle::squircle(QRectF(body.center().x() - 1.2, body.top() + 1.6, 2.4, 4.4), 1.2));
            painter.setPen(pen);
        }
        painter.drawLine(QPointF(19, 2.5), QPointF(19, 13.5));
        arrowHead(QPointF(19, 2.5), QPointF(0, -1));
        arrowHead(QPointF(19, 13.5), QPointF(0, 1));
        break;
    default:
        break;
    }
}

/// The picture of \p gesture in \p ink, as a file a tooltip can show (with sharper copies for high density screens)
QString glyphFile(Gesture gesture, const QColor &ink, const QColor &lit)
{
    static QHash<QString, QString> made;
    const QString key = QStringLiteral("%1-%2-%3").arg(int(gesture)).arg(ink.rgba(), 8, 16, QLatin1Char('0')).arg(lit.rgba(), 8, 16, QLatin1Char('0'));
    if (made.contains(key)) return made.value(key);
    const QString folder = KisProfile::tempFolder() + QStringLiteral("/tips");
    QDir().mkpath(folder);
    const QString base = folder + QLatin1Char('/') + key;
    for (const int scale : {1, 2, 3}) {
        QImage image(glyphWidth(gesture) * scale, glyphHeight * scale, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        painter.scale(scale, scale);
        paintGlyph(painter, gesture, ink, lit);
        painter.end();
        image.save(scale == 1 ? base + QStringLiteral(".png") : base + QStringLiteral("@%1x.png").arg(scale));
    }
    const QString url = QUrl::fromLocalFile(base + QStringLiteral(".png")).toString();
    made.insert(key, url);
    return url;
}

/**
 * The length of \p line's lead, a name of a few words before ": " ("Alpha lock: paint only over existing paint"),
 * shown in bold in place of the colon; 0 for none
 */
int leadOf(const QString &line)
{
    const int colon = line.indexOf(QLatin1String(": "));
    if (colon <= 0 || colon > 28) return 0;
    const QString lead = line.left(colon);
    if (lead.count(QLatin1Char(' ')) > 2 || lead.contains(QChar(0x2192)) || !lead.at(0).isLetterOrNumber()) return 0;
    // a name, not a remark ("Note: …") or the middle of a sentence ("Use ';' (Example: …")
    static const QRegularExpression notAName(QStringLiteral("[(\"';]|^(Note|Example|Tip|Warning|Hint)$"), QRegularExpression::CaseInsensitiveOption);
    if (notAName.match(lead).hasMatch()) return 0;
    return colon;
}

QString css(const QColor &color)
{
    return QStringLiteral("rgba(%1,%2,%3,%4)").arg(color.red()).arg(color.green()).arg(color.blue()).arg(color.alphaF(), 0, 'f', 2);
}

std::function<QKeySequence(QAbstractButton *)> shortcutOf;

/// The widest a tooltip's text runs before it wraps
constexpr int widestTip = 380;

/**
 * \p html in a block as wide as its text, up to widestTip: Qt leaves plain text unwrapped however long, and wraps
 * rich text into a narrow column by a guess of its own
 */
QString sized(const QString &html)
{
    QTextDocument document;
    document.setDefaultFont(QToolTip::font());
    document.setDocumentMargin(0);
    document.setHtml(html);
    const int width = qMin(qCeil(document.idealWidth()) + 2, widestTip);
    return QStringLiteral("<table cellspacing=\"0\" cellpadding=\"0\" width=\"%1\"><tr><td>%2</td></tr></table>").arg(width).arg(html);
}

/// The tooltip label Qt is showing, if any
QWidget *shownTip()
{
    Q_FOREACH (QWidget *top, QApplication::topLevelWidgets()) {
        if (top->isVisible() && top->inherits("QTipLabel")) return top;
    }
    return nullptr;
}

/// \p widget as an item view's viewport, the view; else null
QAbstractItemView *viewOf(QWidget *widget)
{
    auto *view = qobject_cast<QAbstractItemView *>(widget->parentWidget());
    return view && view->viewport() == widget ? view : nullptr;
}

/**
 * What a tip at \p pos in \p widget is about, on screen: a button, or a list's row across its width. Empty for the
 * rest (the canvas, a large list's background), whose tip stays at the pointer.
 */
QRect subjectOf(QWidget *widget, const QPoint &pos)
{
    if (QAbstractItemView *view = viewOf(widget)) {
        const QModelIndex index = view->indexAt(pos);
        if (!index.isValid()) return QRect();
        const QRect row = view->visualRect(index);
        return QRect(widget->mapToGlobal(QPoint(0, row.top())), QSize(widget->width(), row.height()));
    }
    if (widget->height() > 48 || widget->width() > 600) return QRect();
    return QRect(widget->mapToGlobal(QPoint(0, 0)), widget->size());
}

/**
 * Moves \p tip beside \p subject, level with it, toward the middle of the window (the free side of a rail or a
 * panel, under a bar along the top): under the pointer it covered the next button or row. A side over \p work, the
 * canvas around the last stroke, is left for the other.
 */
void placeBeside(QWidget *tip, QWidget *widget, const QRect &subject, const QRect &work)
{
    QScreen *screen = widget->screen();
    if (!screen || subject.isEmpty()) return;
    tip->adjustSize();
    const QRect available = screen->availableGeometry();
    const QRect window = widget->window()->geometry();
    // laid along a bar (wider than tall) the tip goes above or below, along a rail or a panel to a side; a list's
    // rows are stacked, whatever the list's shape
    bool across = false;
    for (QWidget *strip = viewOf(widget) ? nullptr : widget->parentWidget(); strip && !strip->isWindow(); strip = strip->parentWidget()) {
        if (strip->width() > 2 * subject.width() || strip->height() > 2 * subject.height()) {
            across = strip->width() > 2 * strip->height();
            break;
        }
    }
    constexpr int gap = KisModernStyle::space2;
    QList<QRect> sides;
    if (across) {
        const QRect below(QPoint(subject.center().x() - tip->width() / 2, subject.bottom() + 1 + gap), tip->size());
        const QRect above(QPoint(below.left(), subject.top() - gap - tip->height()), tip->size());
        sides = subject.center().y() < window.center().y() ? QList<QRect>{below, above} : QList<QRect>{above, below};
    } else {
        const QRect right(QPoint(subject.right() + 1 + gap, subject.center().y() - tip->height() / 2), tip->size());
        const QRect left(QPoint(subject.left() - gap - tip->width(), right.top()), tip->size());
        sides = subject.center().x() < window.center().x() ? QList<QRect>{right, left} : QList<QRect>{left, right};
    }
    QRect chosen;
    for (QRect side : sides) {
        // kept on screen along the subject, never pushed back over it
        if (across) side.moveLeft(qBound(available.left(), side.left(), available.right() - side.width()));
        else side.moveTop(qBound(available.top(), side.top(), available.bottom() - side.height()));
        if (!available.contains(side)) continue;
        if (chosen.isEmpty()) chosen = side;
        if (!side.intersects(work)) {
            chosen = side;
            break;
        }
    }
    if (!chosen.isEmpty()) tip->move(chosen.topLeft());
}

/**
 * Shows tooltips with their gestures drawn and their shortcut, for every widget, and keeps them out of the way:
 * none just after a click, a stroke or a key; a tip seen a few times waits longer before it shows; it shows beside
 * its control, off the canvas being painted, and fades as the pointer moves on.
 */
class TipFilter : public QObject
{
public:
    TipFilter()
    {
        m_clock.start();
        m_pending.setSingleShot(true);
        connect(&m_pending, &QTimer::timeout, this, [this]() { showPending(); });
        m_saveLater.setSingleShot(true);
        m_saveLater.setInterval(3000);
        connect(&m_saveLater, &QTimer::timeout, this, [this]() { saveSeen(); });
        Q_FOREACH (const QString &entry, KSharedConfig::openConfig()->group(QString()).readEntry(seenKey, QStringList())) {
            const int colon = entry.indexOf(QLatin1Char(':'));
            if (colon > 0) m_seen.insert(entry.left(colon), entry.mid(colon + 1).toInt());
        }
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        switch (event->type()) {
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonDblClick:
        case QEvent::MouseButtonRelease: {
            auto *mouse = static_cast<QMouseEvent *>(event);
            worked(watched, mouse->globalPos(), event->type() != QEvent::MouseButtonRelease, mouse->timestamp());
            break;
        }
        case QEvent::TabletPress:
        case QEvent::TabletRelease: {
            auto *tablet = static_cast<QTabletEvent *>(event);
            worked(watched, tablet->globalPos(), event->type() == QEvent::TabletPress, tablet->timestamp());
            break;
        }
        case QEvent::Wheel:
            worked(nullptr, QPoint(), false, 0);
            break;
        case QEvent::KeyPress: {
            const int key = static_cast<QKeyEvent *>(event)->key();
            const bool modifier = key == Qt::Key_Shift || key == Qt::Key_Control || key == Qt::Key_Alt || key == Qt::Key_Meta;
            if (!modifier) worked(nullptr, QPoint(), false, 0);
            break;
        }
        case QEvent::MouseMove:
            moved(static_cast<QMouseEvent *>(event)->globalPos());
            break;
        default:
            break;
        }
        if (event->type() != QEvent::ToolTip) return false;
        // all of them off in Settings, the lists' and the brushes' cards too
        if (!KisSettingsRegistry::instance()->value(QStringLiteral("StudioTooltips")).toBool()) {
            QToolTip::hideText();
            return true;
        }
        auto *widget = qobject_cast<QWidget *>(watched);
        if (!widget) return false;
        auto *help = static_cast<QHelpEvent *>(event);
        if (m_inner) return showOwn(widget, help);

        const QString key = keyOf(widget, help->pos());
        const qint64 now = m_clock.elapsed();
        int wait = 0;
        if (now - m_workedAt < quietAfterWork) {
            // the control just used says nothing; another one waits for the pointer to rest after the work
            if (m_usedOn && (m_usedOn == widget || m_usedOn->isAncestorOf(widget) || widget->isAncestorOf(m_usedOn))) return true;
            wait = int(quietAfterWork - (now - m_workedAt));
        }
        if (m_seen.value(key) >= familiar) wait = qMax(wait, familiarWait);
        if (wait > 0) {
            m_pendingWidget = widget;
            m_pendingPos = help->pos();
            m_pendingGlobal = help->globalPos();
            m_pending.start(wait);
            return true;
        }
        show(widget, help->pos(), help->globalPos(), key);
        return true;
    }

private:
    static constexpr int quietAfterWork = 1000;
    static constexpr int familiar = 3;
    static constexpr int familiarWait = 1100;
    static constexpr int seenKept = 400;
    static constexpr int workReach = 160;
    static const QString seenKey;

    /// A tip's control, by what its tip says, else by where it sits (and in a list, the column)
    static QString keyOf(QWidget *widget, const QPoint &pos)
    {
        QString key = widget->toolTip();
        if (key.isEmpty()) {
            QStringList path;
            for (QWidget *at = widget; at && path.size() < 4; at = at->parentWidget()) {
                path << (at->objectName().isEmpty() ? QString::fromLatin1(at->metaObject()->className()) : at->objectName());
            }
            if (QAbstractItemView *view = viewOf(widget)) path << QString::number(view->indexAt(pos).column());
            key = path.join(QLatin1Char('/'));
        }
        return QString::number(qHash(key), 36);
    }

    /// A click, a stroke, a turn of the wheel or a key: no tips for a moment, and none waiting
    void worked(QObject *on, const QPoint &globalPos, bool pressed, ulong timestamp)
    {
        m_workedAt = m_clock.elapsed();
        m_pending.stop();
        // a tip shown during the work (a value while dragged) is the work's own
        m_shownAt = QPoint();
        auto *widget = qobject_cast<QWidget *>(on);
        if (!widget) return;
        if (widget->inherits("KisOpenGLCanvas2") || widget->inherits("KisQPainterCanvas")) {
            m_workAt = globalPos;
            m_workTime = m_workedAt;
        }
        if (!pressed) return;
        // a press reaches the widget, then each parent that ignores it: counted once
        if (timestamp && timestamp == m_pressStamp) return;
        m_pressStamp = timestamp;
        m_usedOn = widget;
        // using a control is knowing it too
        if (!widget->toolTip().isEmpty() || viewOf(widget)) see(keyOf(widget, widget->mapFromGlobal(globalPos)));
    }

    void moved(const QPoint &globalPos)
    {
        if (m_pending.isActive() && (globalPos - m_pendingGlobal).manhattanLength() > 4) m_pending.stop();
        // the tip goes as the pointer moves on
        if (!m_shownAt.isNull() && (globalPos - m_shownAt).manhattanLength() > 8) {
            m_shownAt = QPoint();
            if (QWidget *tip = shownTip()) KisStudioMotion::hideFaded(tip);
        }
    }

    void showPending()
    {
        if (!m_pendingWidget || !m_pendingWidget->isVisible()) return;
        show(m_pendingWidget, m_pendingPos, m_pendingGlobal, keyOf(m_pendingWidget, m_pendingPos));
    }

    /// Shows the tip at \p pos as \p widget or its parents make it, beside its subject
    void show(QWidget *widget, const QPoint &pos, const QPoint &globalPos, const QString &key)
    {
        if (QWidget *tip = shownTip()) KisStudioMotion::stopFade(tip);
        QHelpEvent help(QEvent::ToolTip, pos, globalPos);
        m_inner = true;
        QApplication::sendEvent(widget, &help);
        m_inner = false;
        QWidget *tip = shownTip();
        if (!tip) return;
        const bool recent = !m_workAt.isNull() && m_clock.elapsed() - m_workTime < 60000;
        const QRect work = recent ? QRect(m_workAt - QPoint(workReach, workReach), QSize(2 * workReach, 2 * workReach)) : QRect();
        placeBeside(tip, widget, subjectOf(widget, pos), work);
        m_shownAt = globalPos;
        see(key);
    }

    /// The tip of \p widget as its own tooltip says, drawn: false leaves it to the widget (lists, cards)
    bool showOwn(QWidget *widget, QHelpEvent *event)
    {
        QString plain = widget->toolTip();
        auto *button = qobject_cast<QAbstractButton *>(widget);
        const bool keyOnly = plain.isEmpty() && button && button->property("keyOnlyTip").toBool();
        if (plain.isEmpty() && !keyOnly) return false;
        const QKeySequence keys = button && shortcutOf ? shortcutOf(button) : QKeySequence();
        QString keyText = keys.toString(QKeySequence::NativeText);
        if (!keyText.isEmpty()) {
            // Krita's own actions already add it in parentheses; it goes after the name instead, so all look alike
            const QString named = QStringLiteral(" (%1)").arg(keyText);
            if (plain.endsWith(named)) plain.chop(named.size());
            else if (plain.contains(keyText)) keyText.clear();
        }
        const QPoint at = event->globalPos();
        // a tip that says what the button says ("Reset" on Reset, Close's stock "Close the current window or document")
        // is no tip
        auto bare = [](QString text) { return text.remove(QLatin1Char('&')).remove(QChar(0x2026)).remove(QStringLiteral("...")).trimmed().toLower(); };
        if (button && !button->text().isEmpty() && keyText.isEmpty() && !keyOnly) {
            static const QString stockClose = KStandardGuiItem::close().toolTip();
            if (bare(plain) == bare(button->text()) || plain == stockClose) return true;
        }
        // nor one that a number box already starts with ("Width" on "Width: 200 px")
        if (auto *spin = qobject_cast<QAbstractSpinBox *>(widget); spin && !plain.contains(QLatin1Char('\n')) && bare(spin->text()).startsWith(bare(plain))) {
            return true;
        }
        if (keyOnly) {
            if (keyText.isEmpty()) return false;
            QToolTip::showText(at, keyText, widget, widget->rect());
            return true;
        }
        // Krita writes much of its own in Title Case ("Add Reference Image From File")
        if (!Qt::mightBeRichText(plain)) plain = KisSentenceCase::of(plain);
        QToolTip::showText(at, KisStudioTips::html(plain, keyText), widget, widget->rect());
        return true;
    }

    /// Counts a tip shown or its control used, up to knowing it
    void see(const QString &key)
    {
        int &count = m_seen[key];
        if (count >= familiar) return;
        count++;
        if (m_seen.size() > seenKept) {
            // the ones seen once make room
            for (auto it = m_seen.begin(); it != m_seen.end();) {
                if (it.value() <= 1 && it.key() != key) it = m_seen.erase(it);
                else ++it;
            }
        }
        m_saveLater.start();
    }

    void saveSeen()
    {
        QStringList entries;
        for (auto it = m_seen.constBegin(); it != m_seen.constEnd(); ++it) entries << it.key() + QLatin1Char(':') + QString::number(it.value());
        KisSettingsRegistry::writeLater(seenKey, entries);
    }

    QElapsedTimer m_clock;
    qint64 m_workedAt {-quietAfterWork};
    QPointer<QWidget> m_usedOn;
    ulong m_pressStamp {0};
    QPoint m_workAt;
    qint64 m_workTime {0};
    bool m_inner {false};
    QTimer m_pending;
    QPointer<QWidget> m_pendingWidget;
    QPoint m_pendingPos;
    QPoint m_pendingGlobal;
    QPoint m_shownAt;
    QHash<QString, int> m_seen;
    QTimer m_saveLater;
};

const QString TipFilter::seenKey = QStringLiteral("StudioTooltipsSeen");

} // namespace

QString KisStudioTips::html(const QString &tooltip, const QString &keys)
{
    if (Qt::mightBeRichText(tooltip)) {
        return sized(keys.isEmpty() ? tooltip : QStringLiteral("%1&nbsp;&nbsp;&nbsp;%2").arg(tooltip, keys.toHtmlEscaped()));
    }
    QStringList text;
    QStringList notes;
    QList<Line> gestures;
    Q_FOREACH (const QString &part, tooltip.split(QLatin1Char('\n'))) {
        Line line;
        if (part.startsWith(QChar(0x2063))) {
            notes << part.mid(1);
        } else if (readLine(part, &line)) {
            gestures << line;
        } else {
            text << part;
        }
    }

    const QColor ink = QToolTip::palette().color(QPalette::ToolTipText);
    QColor quiet = ink;
    quiet.setAlphaF(0.55);
    QColor glyphInk = ink;
    glyphInk.setAlphaF(0.85);
    QColor lit = QApplication::palette().color(QPalette::Highlight);
    // a theme with no accent has a grey one, hardly apart from the mouse: the pressed button in full ink then
    if (lit.hslSaturationF() < 0.2) lit = ink;
    QString out;
    // "Name: what it is" lines: one alone is the name in bold over its sentence, a run of them (choices) a table
    // with the names in a column of their own. What follows the colon starts a line, so it starts with a capital.
    int leads = 0;
    for (const QString &line : text) leads += leadOf(line) > 0 ? 1 : 0;
    auto sentence = [](const QString &what) { return (what.isEmpty() ? what : what.at(0).toUpper() + what.mid(1)).toHtmlEscaped(); };
    const QString shortcut =
        keys.isEmpty() ? QString() : QStringLiteral("&nbsp;&nbsp;&nbsp;<span style=\"color:%1\">%2</span>").arg(css(quiet), keys.toHtmlEscaped());
    QStringList lines;
    auto paragraph = [&]() {
        if (!lines.isEmpty()) out += QStringLiteral("<p style=\"margin:0\">%1</p>").arg(lines.join(QStringLiteral("<br>")));
        lines.clear();
    };
    for (int i = 0; i < text.size(); i++) {
        const int lead = leadOf(text[i]);
        const QString after = i == 0 ? shortcut : QString();
        if (lead <= 0) {
            lines << text[i].toHtmlEscaped() + after;
        } else if (leads == 1) {
            lines << QStringLiteral("<b>%1</b>").arg(text[i].left(lead).toHtmlEscaped()) + after << sentence(text[i].mid(lead + 2));
        } else {
            paragraph();
            out += QStringLiteral("<table cellspacing=\"0\" cellpadding=\"0\">");
            for (; i < text.size() && leadOf(text[i]) > 0; i++) {
                const int at = leadOf(text[i]);
                out += QStringLiteral("<tr><td style=\"padding:1px 10px 1px 0; white-space:nowrap\"><b>%1</b></td><td style=\"padding:1px 0\">%2</td></tr>")
                           .arg(text[i].left(at).toHtmlEscaped(), sentence(text[i].mid(at + 2)));
            }
            out += QStringLiteral("</table>");
            i--;
        }
    }
    paragraph();
    if (!notes.isEmpty()) {
        for (QString &note : notes) note = note.toHtmlEscaped();
        out += QStringLiteral("<p style=\"margin:%1px 0 0 0; color:%2\">%3</p>")
                   .arg(text.isEmpty() ? 0 : 3)
                   .arg(css(quiet), notes.join(QStringLiteral("<br>")));
    }
    if (gestures.isEmpty()) return sized(out);

    // short words go in one row; longer ones a gesture per row, so the tooltip stays narrow
    bool longWords = false;
    for (const Line &line : gestures) longWords = longWords || line.action.size() + line.where.size() > 14;
    const int perRow = longWords ? 1 : 4;
    bool anyModifier = false;
    for (const Line &line : gestures) anyModifier = anyModifier || !line.modifier.isEmpty();
    out += QStringLiteral("<table cellspacing=\"0\" cellpadding=\"0\" style=\"margin-top:%1px\">").arg(text.isEmpty() ? 0 : 4);
    for (int i = 0; i < gestures.size(); i++) {
        const Line &line = gestures[i];
        if (i % perRow == 0) out += QStringLiteral("<tr>");
        QString words = line.action.toHtmlEscaped();
        if (!line.where.isEmpty()) words = line.where.toHtmlEscaped() + QStringLiteral(" → ") + words;
        // the key held, the picture and the word each in a column of their own, so a list of them lines up
        if (anyModifier) {
            out += QStringLiteral("<td valign=\"middle\" align=\"right\" style=\"color:%1; padding:1px 3px 1px 0; white-space:nowrap\">%2</td>")
                       .arg(css(quiet), line.modifier.toHtmlEscaped());
        }
        out += QStringLiteral("<td valign=\"middle\" style=\"padding:1px 0\"><img src=\"%1\" width=\"%2\" height=\"%3\"></td>"
                              "<td valign=\"middle\" style=\"padding:1px 14px 1px 3px; white-space:nowrap\">%4</td>")
                   .arg(glyphFile(line.gesture, glyphInk, lit)).arg(glyphWidth(line.gesture)).arg(glyphHeight).arg(words);
        if (i % perRow == perRow - 1 || i == gestures.size() - 1) out += QStringLiteral("</tr>");
    }
    out += QStringLiteral("</table>");
    return sized(out);
}

void KisStudioTips::install(std::function<QKeySequence(QAbstractButton *)> shortcuts)
{
    static TipFilter *filter = nullptr;
    shortcutOf = shortcuts;
    if (filter) return;
    filter = new TipFilter();
    filter->setParent(qApp);
    qApp->installEventFilter(filter);
    // tooltips in the theme's colors: Qt keeps the system's, whose text is white on a light theme in Windows'
    // dark mode, and the reverse
    QToolTip::setPalette(QApplication::palette());
    QObject::connect(qApp, &QGuiApplication::paletteChanged, filter, [](const QPalette &palette) { QToolTip::setPalette(palette); });
    auto delay = []() { KisModernStyle::setToolTipDelay(KisSettingsRegistry::instance()->value(QStringLiteral("StudioTooltipDelay")).toInt()); };
    delay();
    QObject::connect(KisSettingsRegistry::instance(), &KisSettingsRegistry::valueChanged, filter, [delay](const QString &id) {
        if (id == QLatin1String("StudioTooltipDelay")) delay();
    });
}
