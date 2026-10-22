/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioTips.h"
#include "KisSettingsRegistry.h"

#include <QAbstractButton>
#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QHelpEvent>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QTextDocument>
#include <QToolTip>
#include <QUrl>
#include <QtMath>

#include <KisModernStyle.h>
#include <KisProfile.h>

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

QString css(const QColor &color)
{
    return QStringLiteral("rgba(%1,%2,%3,%4)").arg(color.red()).arg(color.green()).arg(color.blue()).arg(color.alphaF(), 0, 'f', 2);
}

std::function<QKeySequence(QAbstractButton *)> shortcutOf;

/// Shows tooltips with their gestures drawn and their shortcut, for every widget
class TipFilter : public QObject
{
protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() != QEvent::ToolTip) return false;
        // all of them off in Settings, the lists' and the brushes' cards too
        if (!KisSettingsRegistry::instance()->value(QStringLiteral("StudioTooltips")).toBool()) {
            QToolTip::hideText();
            return true;
        }
        auto *widget = qobject_cast<QWidget *>(watched);
        if (!widget) return false;
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
        const QPoint at = static_cast<QHelpEvent *>(event)->globalPos();
        if (keyOnly) {
            if (keyText.isEmpty()) return false;
            QToolTip::showText(at, keyText, widget);
            return true;
        }
        const QString shown = KisStudioTips::html(plain, keyText);
        if (shown == plain) return false;
        QToolTip::showText(at, shown, widget);
        return true;
    }
};

} // namespace

QString KisStudioTips::html(const QString &tooltip, const QString &keys)
{
    if (Qt::mightBeRichText(tooltip)) {
        return keys.isEmpty() ? tooltip : QStringLiteral("%1&nbsp;&nbsp;&nbsp;%2").arg(tooltip, keys.toHtmlEscaped());
    }
    QStringList text;
    QList<Line> gestures;
    Q_FOREACH (const QString &part, tooltip.split(QLatin1Char('\n'))) {
        Line line;
        if (readLine(part, &line)) gestures << line;
        else text << part;
    }
    if (gestures.isEmpty() && keys.isEmpty()) return tooltip;

    const QColor ink = QToolTip::palette().color(QPalette::ToolTipText);
    QColor quiet = ink;
    quiet.setAlphaF(0.55);
    QColor glyphInk = ink;
    glyphInk.setAlphaF(0.85);
    const QColor lit = QApplication::palette().color(QPalette::Highlight);
    QString out;
    QStringList lines;
    Q_FOREACH (const QString &line, text) lines << line.toHtmlEscaped();
    if (!lines.isEmpty() && !keys.isEmpty()) {
        lines[0] += QStringLiteral("&nbsp;&nbsp;&nbsp;<span style=\"color:%1\">%2</span>").arg(css(quiet), keys.toHtmlEscaped());
    }
    if (!lines.isEmpty()) out += QStringLiteral("<p style=\"margin:0\">%1</p>").arg(lines.join(QStringLiteral("<br>")));
    if (gestures.isEmpty()) return out;

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
    return out;
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
}
