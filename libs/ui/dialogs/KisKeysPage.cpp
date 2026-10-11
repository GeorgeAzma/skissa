/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisKeysPage.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QButtonGroup>
#include <QComboBox>
#include <QDir>
#include <QDrag>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QToolButton>
#include <QToolTip>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWidgetAction>

#include <KConfig>
#include <KConfigGroup>
#include <KSharedConfig>
#include <klocalizedstring.h>

#include <KisModernStyle.h>
#include <KoResourcePaths.h>
#include <kactioncollection.h>
#include <kis_action_registry.h>
#include <kxmlguiclient.h>
#include <kxmlguifactory.h>

#include "KisMainWindow.h"
#include "KisPart.h"
#include "KisStudioFind.h"
#include "KisStudioRings.h"
#include "input/kis_abstract_input_action.h"
#include "input/kis_input_profile.h"
#include "input/kis_input_profile_manager.h"
#include "input/kis_shortcut_configuration.h"
#include "kis_config.h"

namespace
{

const char *const commandMime = "application/x-krita-keys-command";

QString cleanText(QString text)
{
    text.remove(QLatin1Char('&'));
    if (text.endsWith(QStringLiteral("..."))) text.chop(3);
    if (text.endsWith(QStringLiteral("…"))) text.chop(1);
    return text.trimmed();
}

bool isModifierKey(int key)
{
    return key == Qt::Key_Control || key == Qt::Key_Shift || key == Qt::Key_Alt || key == Qt::Key_Meta || key == Qt::Key_AltGr;
}

Qt::KeyboardModifier modifierOf(int key)
{
    switch (key) {
    case Qt::Key_Control: return Qt::ControlModifier;
    case Qt::Key_Shift: return Qt::ShiftModifier;
    case Qt::Key_Alt: return Qt::AltModifier;
    case Qt::Key_Meta: return Qt::MetaModifier;
    default: return Qt::NoModifier;
    }
}

/**
 * A key and its modifiers as one number, as the key is on the keyboard: a
 * shifted symbol is its key with Shift ("{" is Shift+[), a letter its
 * capital. A modifier alone is its own key, with no modifiers.
 */
int chordOf(int key, Qt::KeyboardModifiers modifiers)
{
    static const QHash<int, int> shifted = {
        {Qt::Key_AsciiTilde, Qt::Key_QuoteLeft}, {Qt::Key_Exclam, Qt::Key_1}, {Qt::Key_At, Qt::Key_2},
        {Qt::Key_NumberSign, Qt::Key_3}, {Qt::Key_Dollar, Qt::Key_4}, {Qt::Key_Percent, Qt::Key_5},
        {Qt::Key_AsciiCircum, Qt::Key_6}, {Qt::Key_Ampersand, Qt::Key_7}, {Qt::Key_Asterisk, Qt::Key_8},
        {Qt::Key_ParenLeft, Qt::Key_9}, {Qt::Key_ParenRight, Qt::Key_0}, {Qt::Key_Underscore, Qt::Key_Minus},
        {Qt::Key_Plus, Qt::Key_Equal}, {Qt::Key_BraceLeft, Qt::Key_BracketLeft}, {Qt::Key_BraceRight, Qt::Key_BracketRight},
        {Qt::Key_Bar, Qt::Key_Backslash}, {Qt::Key_Colon, Qt::Key_Semicolon}, {Qt::Key_QuoteDbl, Qt::Key_Apostrophe},
        {Qt::Key_Less, Qt::Key_Comma}, {Qt::Key_Greater, Qt::Key_Period}, {Qt::Key_Question, Qt::Key_Slash},
    };
    modifiers &= Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier | Qt::MetaModifier;
    if (shifted.contains(key)) {
        key = shifted.value(key);
        modifiers |= Qt::ShiftModifier;
    }
    if (key == Qt::Key_Backtab) key = Qt::Key_Tab;
    if (key == Qt::Key_Enter) key = Qt::Key_Return;
    return int(modifiers) | key;
}

int chordOf(const QKeySequence &sequence)
{
    if (sequence.isEmpty()) return 0;
    const int combined = sequence[0];
    return chordOf(combined & ~int(Qt::KeyboardModifierMask), Qt::KeyboardModifiers(combined & int(Qt::KeyboardModifierMask)));
}

/// The keys a held shortcut is made of, as a chord: its modifiers and its first other key, or a modifier alone
int chordOf(const QList<Qt::Key> &keys)
{
    Qt::KeyboardModifiers modifiers;
    int main = 0;
    for (Qt::Key key : keys) {
        if (isModifierKey(key)) modifiers |= modifierOf(key);
        else if (!main) main = key;
    }
    if (!main && keys.size() == 1) return keys.first();
    if (!main) return 0;
    return chordOf(main, modifiers);
}

/// A key on the drawn keyboard, in key widths
struct KeyCell {
    int key;
    QString label;
    qreal x, y, w, h;
    bool offHand;
};

QVector<KeyCell> keyboardLayout()
{
    QVector<KeyCell> cells;
    auto row = [&cells](qreal y, qreal h, qreal x, const QVector<std::tuple<int, QString, qreal, bool>> &keys) {
        for (const auto &k : keys) {
            const int key = std::get<0>(k);
            if (key) cells.append({key, std::get<1>(k), x, y, std::get<2>(k), h, std::get<3>(k)});
            x += std::get<2>(k);
        }
    };
    const QString ctrl = QStringLiteral("Ctrl"), shift = QStringLiteral("Shift"), alt = QStringLiteral("Alt");
    row(0, 0.75, 0, {{Qt::Key_Escape, QStringLiteral("Esc"), 1, true}, {0, {}, 1, false},
                     {Qt::Key_F1, QStringLiteral("F1"), 1, true}, {Qt::Key_F2, QStringLiteral("F2"), 1, true},
                     {Qt::Key_F3, QStringLiteral("F3"), 1, true}, {Qt::Key_F4, QStringLiteral("F4"), 1, true}, {0, {}, 0.5, false},
                     {Qt::Key_F5, QStringLiteral("F5"), 1, false}, {Qt::Key_F6, QStringLiteral("F6"), 1, false},
                     {Qt::Key_F7, QStringLiteral("F7"), 1, false}, {Qt::Key_F8, QStringLiteral("F8"), 1, false}, {0, {}, 0.5, false},
                     {Qt::Key_F9, QStringLiteral("F9"), 1, false}, {Qt::Key_F10, QStringLiteral("F10"), 1, false},
                     {Qt::Key_F11, QStringLiteral("F11"), 1, false}, {Qt::Key_F12, QStringLiteral("F12"), 1, false}});
    row(1, 1, 0, {{Qt::Key_QuoteLeft, QStringLiteral("`"), 1, true}, {Qt::Key_1, QStringLiteral("1"), 1, true},
                  {Qt::Key_2, QStringLiteral("2"), 1, true}, {Qt::Key_3, QStringLiteral("3"), 1, true},
                  {Qt::Key_4, QStringLiteral("4"), 1, true}, {Qt::Key_5, QStringLiteral("5"), 1, true},
                  {Qt::Key_6, QStringLiteral("6"), 1, false}, {Qt::Key_7, QStringLiteral("7"), 1, false},
                  {Qt::Key_8, QStringLiteral("8"), 1, false}, {Qt::Key_9, QStringLiteral("9"), 1, false},
                  {Qt::Key_0, QStringLiteral("0"), 1, false}, {Qt::Key_Minus, QStringLiteral("-"), 1, false},
                  {Qt::Key_Equal, QStringLiteral("="), 1, false}, {Qt::Key_Backspace, QStringLiteral("⌫"), 2, false},
                  {0, {}, 0.5, false}, {Qt::Key_Insert, QStringLiteral("Ins"), 1, false},
                  {Qt::Key_Home, QStringLiteral("Home"), 1, false}, {Qt::Key_PageUp, QStringLiteral("PgUp"), 1, false}});
    row(2, 1, 0, {{Qt::Key_Tab, QStringLiteral("Tab"), 1.5, true}, {Qt::Key_Q, QStringLiteral("Q"), 1, true},
                  {Qt::Key_W, QStringLiteral("W"), 1, true}, {Qt::Key_E, QStringLiteral("E"), 1, true},
                  {Qt::Key_R, QStringLiteral("R"), 1, true}, {Qt::Key_T, QStringLiteral("T"), 1, true},
                  {Qt::Key_Y, QStringLiteral("Y"), 1, false}, {Qt::Key_U, QStringLiteral("U"), 1, false},
                  {Qt::Key_I, QStringLiteral("I"), 1, false}, {Qt::Key_O, QStringLiteral("O"), 1, false},
                  {Qt::Key_P, QStringLiteral("P"), 1, false}, {Qt::Key_BracketLeft, QStringLiteral("["), 1, false},
                  {Qt::Key_BracketRight, QStringLiteral("]"), 1, false}, {Qt::Key_Backslash, QStringLiteral("\\"), 1.5, false},
                  {0, {}, 0.5, false}, {Qt::Key_Delete, QStringLiteral("Del"), 1, false},
                  {Qt::Key_End, QStringLiteral("End"), 1, false}, {Qt::Key_PageDown, QStringLiteral("PgDn"), 1, false}});
    row(3, 1, 0, {{Qt::Key_CapsLock, QStringLiteral("Caps"), 1.75, true}, {Qt::Key_A, QStringLiteral("A"), 1, true},
                  {Qt::Key_S, QStringLiteral("S"), 1, true}, {Qt::Key_D, QStringLiteral("D"), 1, true},
                  {Qt::Key_F, QStringLiteral("F"), 1, true}, {Qt::Key_G, QStringLiteral("G"), 1, true},
                  {Qt::Key_H, QStringLiteral("H"), 1, false}, {Qt::Key_J, QStringLiteral("J"), 1, false},
                  {Qt::Key_K, QStringLiteral("K"), 1, false}, {Qt::Key_L, QStringLiteral("L"), 1, false},
                  {Qt::Key_Semicolon, QStringLiteral(";"), 1, false}, {Qt::Key_Apostrophe, QStringLiteral("'"), 1, false},
                  {Qt::Key_Return, QStringLiteral("Enter"), 2.25, false}});
    row(4, 1, 0, {{Qt::Key_Shift, shift, 2.25, true}, {Qt::Key_Z, QStringLiteral("Z"), 1, true},
                  {Qt::Key_X, QStringLiteral("X"), 1, true}, {Qt::Key_C, QStringLiteral("C"), 1, true},
                  {Qt::Key_V, QStringLiteral("V"), 1, true}, {Qt::Key_B, QStringLiteral("B"), 1, true},
                  {Qt::Key_N, QStringLiteral("N"), 1, false}, {Qt::Key_M, QStringLiteral("M"), 1, false},
                  {Qt::Key_Comma, QStringLiteral(","), 1, false}, {Qt::Key_Period, QStringLiteral("."), 1, false},
                  {Qt::Key_Slash, QStringLiteral("/"), 1, false}, {Qt::Key_Shift, shift, 2.75, false},
                  {0, {}, 1.5, false}, {Qt::Key_Up, QStringLiteral("↑"), 1, false}});
    row(5, 1, 0, {{Qt::Key_Control, ctrl, 1.25, true}, {Qt::Key_Meta, QStringLiteral("Win"), 1.25, true},
                  {Qt::Key_Alt, alt, 1.25, true}, {Qt::Key_Space, QStringLiteral("Space"), 6.25, true},
                  {Qt::Key_Alt, alt, 1.25, false}, {Qt::Key_Meta, QStringLiteral("Win"), 1.25, false},
                  {Qt::Key_Menu, QStringLiteral("☰"), 1.25, false}, {Qt::Key_Control, ctrl, 1.25, false},
                  {0, {}, 0.5, false}, {Qt::Key_Left, QStringLiteral("←"), 1, false},
                  {Qt::Key_Down, QStringLiteral("↓"), 1, false}, {Qt::Key_Right, QStringLiteral("→"), 1, false}});
    return cells;
}

QColor mixed(const QColor &a, const QColor &b, qreal t);

/// A key's mark and what it means: a bar along its front edge (held on the canvas), a red badge (two commands want it)
class KeyLegend : public QWidget
{
public:
    enum Mark { Held, Clash };

    KeyLegend(Mark mark, const QString &text, QWidget *parent)
        : QWidget(parent)
        , m_mark(mark)
        , m_text(text)
    {
        setToolTip(mark == Held ? i18nc("@info:tooltip", "Held on the canvas") : i18nc("@info:tooltip", "Two commands on one key"));
    }

    QSize sizeHint() const override
    {
        return QSize(cap + 6 + fontMetrics().horizontalAdvance(m_text) + 10, qMax(cap + 3, fontMetrics().height()));
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QColor window = palette().color(QPalette::Window);
        const QColor text = palette().color(QPalette::WindowText);
        const QRectF r(0.5, (height() - cap - 2.5) / 2.0, cap, cap);
        const QColor face = mixed(window, text, 0.15);
        painter.setPen(Qt::NoPen);
        painter.setBrush(mixed(face, QColor(0, 0, 0), 0.45));
        painter.drawPath(KisModernStyle::squircle(r.translated(0, 2.5), 4));
        painter.setBrush(face);
        painter.drawPath(KisModernStyle::squircle(r, 4));
        if (m_mark == Held) {
            QColor bar = text;
            bar.setAlphaF(0.6);
            painter.setBrush(bar);
            painter.drawRoundedRect(QRectF(r.center().x() - 4, r.bottom() - 4, 8, 2), 1, 1);
        } else {
            painter.setBrush(QColor(226, 92, 92));
            painter.drawEllipse(QRectF(r.right() - 8, r.top() + 2, 6, 6));
        }
        QColor ink = text;
        ink.setAlphaF(0.6);
        painter.setPen(ink);
        painter.drawText(QRectF(cap + 6, 0, width() - cap - 6, height()), Qt::AlignLeft | Qt::AlignVCenter, m_text);
    }

private:
    static constexpr int cap = 18;
    Mark m_mark;
    QString m_text;
};

QColor mixed(const QColor &a, const QColor &b, qreal t)
{
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t, a.greenF() + (b.greenF() - a.greenF()) * t,
                            a.blueF() + (b.blueF() - a.blueF()) * t, a.alphaF() + (b.alphaF() - a.alphaF()) * t);
}

/// What a key does, for the keyboard
struct KeyUse {
    QString name;
    QIcon icon;
    bool held {false};
};

/**
 * The keyboard, drawn as key caps: on each key the icon of what it does with
 * the modifiers of the layer shown (its name if it has no icon; all of them
 * in the tooltip). A number when it does more than one thing, red when two
 * commands want it; a bar under what is held on the canvas. A click on a key
 * tells which it was; a command dropped on it is given it.
 */
class KeyboardMap : public QWidget
{
public:
    /// What a chord does, and how many of those are commands (more than one: they clash)
    std::function<QVector<KeyUse>(int chord, int *commandCount)> usesOn;
    std::function<void(int chord)> clicked;
    std::function<void(const QMimeData *mime, int chord)> dropped;
    /// The chords of what is selected in the list, drawn lit
    QSet<int> lit;
    Qt::KeyboardModifiers layer;

    explicit KeyboardMap(QWidget *parent)
        : QWidget(parent)
        , m_cells(keyboardLayout())
    {
        setMouseTracking(true);
        setAcceptDrops(true);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

    QSize sizeHint() const override
    {
        return QSize(int(m_units * 50), heightForWidth(int(m_units * 50)));
    }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override
    {
        return int(5.85 * qMin<qreal>(width / m_units, 52.0)) + 4;
    }

protected:
    qreal unit() const { return qMin<qreal>(width() / m_units, 52.0); }

    QRectF cellRect(const KeyCell &cell) const
    {
        const qreal u = unit();
        const qreal left = (width() - m_units * u) / 2;
        // the function row closer to the rest: a real keyboard's gap, not a whole row
        const qreal y = cell.y < 0.5 ? 0 : (cell.y - 0.15) * u;
        return QRectF(left + cell.x * u + 2, y + 2, cell.w * u - 4, cell.h * u - 5);
    }

    int cellAt(const QPointF &pos) const
    {
        for (int i = 0; i < m_cells.size(); i++) {
            if (cellRect(m_cells[i]).adjusted(-2, -2, 2, 3).contains(pos)) return i;
        }
        return -1;
    }

    /// The chord a key stands for in the layer shown; a modifier key is itself
    int chordAt(int cell) const
    {
        const int key = m_cells[cell].key;
        return isModifierKey(key) ? key : chordOf(key, layer);
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        const QPalette &pal = palette();
        const QColor window = pal.color(QPalette::Window);
        const QColor text = pal.color(QPalette::WindowText);
        const QColor accent = pal.color(QPalette::Highlight);
        const QColor clashColor(226, 92, 92);
        const qreal u = unit();
        QFont legendFont = font();
        legendFont.setPixelSize(qMax(9, int(u * 0.2)));
        QFont nameFont = legendFont;
        nameFont.setPixelSize(qMax(9, int(u * 0.21)));
        QFont badgeFont = legendFont;
        badgeFont.setPixelSize(qMax(8, int(u * 0.18)));
        badgeFont.setBold(true);

        for (int i = 0; i < m_cells.size(); i++) {
            const KeyCell &cell = m_cells[i];
            const QRectF r = cellRect(cell);
            const int chord = chordAt(i);
            int commandCount = 0;
            const QVector<KeyUse> uses = usesOn ? usesOn(chord, &commandCount) : QVector<KeyUse>();
            const bool modifierOn = isModifierKey(cell.key) && (layer & modifierOf(cell.key));
            const bool isLit = lit.contains(chord);
            const bool used = !uses.isEmpty();
            const bool hovered = i == m_hover || i == m_dropTarget;

            // the cap: its face, and under it its side, as a key stands up from the board
            QColor face = mixed(window, text, used ? 0.15 : 0.06);
            if (hovered) face = mixed(face, text, 0.08);
            if (modifierOn || isLit) face = mixed(window, accent, 0.75);
            const QColor side = mixed(face, QColor(0, 0, 0), 0.45);
            const qreal radius = qMin<qreal>(7, r.height() * 0.18);
            painter.setPen(Qt::NoPen);
            painter.setBrush(side);
            painter.drawPath(KisModernStyle::squircle(r.translated(0, 2.5), radius));
            painter.setBrush(face);
            painter.drawPath(KisModernStyle::squircle(r, radius));
            if (i == m_dropTarget) {
                painter.setPen(QPen(accent, 2));
                painter.setBrush(Qt::NoBrush);
                painter.drawPath(KisModernStyle::squircle(r.adjusted(1, 1, -1, -1), radius));
            }

            const QColor ink = (modifierOn || isLit) ? pal.color(QPalette::HighlightedText) : text;
            QColor legendInk = ink;
            legendInk.setAlphaF(used || modifierOn ? 0.75 : 0.42);
            painter.setPen(legendInk);
            painter.setFont(legendFont);
            painter.drawText(r.adjusted(u * 0.12, u * 0.06, -u * 0.1, 0), Qt::AlignLeft | Qt::AlignTop, cell.label);

            if (used) {
                const KeyUse &first = uses.first();
                const QRectF body = r.adjusted(u * 0.1, u * 0.3, -u * 0.1, -u * 0.08);
                if (!first.icon.isNull()) {
                    const qreal size = qMin(body.height(), u * 0.42);
                    const QRectF iconRect(body.center().x() - size / 2, body.center().y() - size / 2 + u * 0.02, size, size);
                    first.icon.paint(&painter, iconRect.toAlignedRect(), Qt::AlignCenter, isLit ? QIcon::Selected : QIcon::Normal);
                } else {
                    // no icon: its first word, as much as fits
                    QColor nameInk = ink;
                    nameInk.setAlphaF(0.9);
                    painter.setPen(nameInk);
                    painter.setFont(nameFont);
                    const QFontMetricsF metrics(nameFont);
                    const QString word = first.name.section(QLatin1Char(' '), 0, cell.w > 1.6 ? 1 : 0);
                    painter.drawText(body, Qt::AlignHCenter | Qt::AlignVCenter, metrics.elidedText(word, Qt::ElideRight, body.width()));
                }
                // held on the canvas: a bar along its front edge
                bool held = false;
                for (const KeyUse &use : uses) held = held || use.held;
                if (held) {
                    QColor bar = ink;
                    bar.setAlphaF(0.6);
                    painter.setPen(Qt::NoPen);
                    painter.setBrush(bar);
                    painter.drawRoundedRect(QRectF(r.center().x() - u * 0.14, r.bottom() - u * 0.09, u * 0.28, 2), 1, 1);
                }
                // more than one: how many, red when two commands want it
                if (uses.size() > 1) {
                    const QString count = QString::number(uses.size());
                    const qreal d = u * 0.3;
                    const QRectF badge(r.right() - d - u * 0.05, r.top() + u * 0.05, d, d);
                    painter.setPen(Qt::NoPen);
                    painter.setBrush(commandCount > 1 ? clashColor : mixed(face, text, 0.35));
                    painter.drawEllipse(badge);
                    painter.setPen(commandCount > 1 ? QColor(Qt::white) : window);
                    painter.setFont(badgeFont);
                    painter.drawText(badge, Qt::AlignCenter, count);
                }
            }
        }
    }

    bool event(QEvent *event) override
    {
        if (event->type() == QEvent::ToolTip) {
            auto *help = static_cast<QHelpEvent *>(event);
            const int cell = cellAt(help->pos());
            if (cell >= 0 && usesOn) {
                int count = 0;
                const QVector<KeyUse> uses = usesOn(chordAt(cell), &count);
                const QString key = isModifierKey(m_cells[cell].key) ? m_cells[cell].label
                                                                     : KisModernStyle::keyCapText(QKeySequence(chordAt(cell)));
                QStringList lines;
                for (const KeyUse &use : uses) {
                    lines << (use.held ? i18nc("@info:tooltip a key held on the canvas", "%1 (held)", use.name) : use.name);
                }
                QToolTip::showText(help->globalPos(), lines.isEmpty() ? i18nc("@info:tooltip a key with no shortcut", "%1: free", key)
                                                                      : QStringLiteral("<b>%1</b><br>%2").arg(key.toHtmlEscaped(), lines.join(QStringLiteral("<br>")).toHtmlEscaped().replace(QStringLiteral("&lt;br&gt;"), QStringLiteral("<br>"))),
                                   this);
            } else {
                QToolTip::hideText();
            }
            return true;
        }
        return QWidget::event(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        const int cell = cellAt(event->pos());
        if (cell != m_hover) {
            m_hover = cell;
            update();
        }
    }

    void leaveEvent(QEvent *) override
    {
        m_hover = -1;
        update();
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        const int cell = cellAt(event->pos());
        if (cell < 0 || event->button() != Qt::LeftButton) return;
        const int key = m_cells[cell].key;
        if (isModifierKey(key) && key != Qt::Key_Meta) {
            // a modifier key shows its layer, or the plain one again
            layer ^= modifierOf(key);
            if (clicked) clicked(-1);
            update();
            return;
        }
        if (clicked) clicked(chordAt(cell));
    }

    void dragEnterEvent(QDragEnterEvent *event) override
    {
        if (event->mimeData()->hasFormat(QString::fromLatin1(commandMime))) event->acceptProposedAction();
    }

    void dragMoveEvent(QDragMoveEvent *event) override
    {
        const int cell = cellAt(event->pos());
        if (cell != m_dropTarget) {
            m_dropTarget = cell;
            update();
        }
        if (cell >= 0) event->acceptProposedAction();
        else event->ignore();
    }

    void dragLeaveEvent(QDragLeaveEvent *) override
    {
        m_dropTarget = -1;
        update();
    }

    void dropEvent(QDropEvent *event) override
    {
        const int cell = cellAt(event->pos());
        m_dropTarget = -1;
        if (cell >= 0 && dropped) {
            dropped(event->mimeData(), chordAt(cell));
            event->acceptProposedAction();
        }
        update();
    }

private:
    QVector<KeyCell> m_cells;
    const qreal m_units = 18.5;
    int m_hover {-1};
    int m_dropTarget {-1};
};

/// The list, its rows dragged out as what they are (a command, or a held key) for the keyboard
class KeysTree : public QTreeWidget
{
public:
    using QTreeWidget::QTreeWidget;

protected:
    QStringList mimeTypes() const override
    {
        return {QString::fromLatin1(commandMime)};
    }

    QMimeData *mimeData(const QList<QTreeWidgetItem *> items) const override
    {
        if (items.isEmpty() || !items.first()->data(0, Qt::UserRole).isValid()) return nullptr;
        auto *mime = new QMimeData;
        mime->setData(QString::fromLatin1(commandMime), QByteArray::number(items.first()->data(0, Qt::UserRole).toInt()));
        return mime;
    }
};

constexpr int RowRole = Qt::UserRole;      ///< the row: a command (its index), a held key (-1 - its index)
constexpr int GroupRole = Qt::UserRole + 1; ///< a group's heading

} // namespace

struct KisKeysPage::Private
{
    struct Command {
        QPointer<QAction> action;
        QString id;
        QString title;
        QString group;
        QString search;   ///< what it is found by: its name, id, group, the words other applications use
        QList<QKeySequence> current;
        bool registered {false};
        QTreeWidgetItem *item {nullptr};
    };
    struct Hold {
        /// What is in the profile; none for a way to set it off that has no keys yet
        KisShortcutConfiguration *live {nullptr};
        KisShortcutConfiguration staged;
        /// Taken off (a right click): out of the profile with OK
        bool removed {false};
        QString title;
        QString shortTitle; ///< what it does, without its mode: for the keyboard
        bool editable {false};
        bool changed {false};
        QTreeWidgetItem *item {nullptr};
    };

    KisKeysPage *q {nullptr};
    KisMainWindow *mainWindow {nullptr};
    QVector<Command> commands;
    QVector<Hold> holds;
    QString scheme;
    QString savedScheme;
    QString profile;
    QString savedProfile;
    bool resetHeld {false};
    QHash<QString, QHash<QString, QString>> schemeEntries;

    QLineEdit *search {nullptr};
    QToolButton *pressKeys {nullptr};
    QToolButton *changedOnly {nullptr};
    QComboBox *startFrom {nullptr};
    QList<QToolButton *> layerButtons;
    KeyboardMap *map {nullptr};
    KeysTree *tree {nullptr};
    QLabel *note {nullptr};
    QTimer noteTimer;
    QTreeWidgetItem *heldGroup {nullptr};

    /// The key filter (a chord), from the keyboard or a key pressed into the search
    int keyFilter {0};
    /// Recording: the row (as RowRole), which of its shortcuts (-1: one more)
    bool recording {false};
    int recordRow {0};
    int recordIndex {-1};
    /// A modifier pressed alone while recording a held key: it is the key, if nothing follows before it is let go
    int pendingModifier {0};
    /// The layer held on the real keyboard, over the one picked
    Qt::KeyboardModifiers pickedLayer;

    QString hint() const
    {
        return i18nc("@info keys page", "Drag commands onto keys · right-click a key to clear it");
    }

    void showNote(const QString &text)
    {
        note->setText(text);
        noteTimer.start();
    }

    // ------------------------------------------------------------- defaults and schemes

    QString defaultShortcutText(const QString &id, const QString &forScheme)
    {
        if (forScheme != QLatin1String("Default")) {
            if (!schemeEntries.contains(forScheme)) {
                QHash<QString, QString> entries;
                const QString file = schemeFile(forScheme);
                if (!file.isEmpty()) {
                    KConfig config(file, KConfig::SimpleConfig);
                    const QMap<QString, QString> map = config.group(QStringLiteral("Shortcuts")).entryMap();
                    for (auto it = map.constBegin(); it != map.constEnd(); ++it) entries.insert(it.key(), it.value());
                }
                schemeEntries.insert(forScheme, entries);
            }
            const QHash<QString, QString> &entries = schemeEntries[forScheme];
            if (entries.contains(id)) return entries.value(id);
        }
        return KisActionRegistry::instance()->getActionProperty(id, QStringLiteral("shortcut"));
    }

    QList<QKeySequence> defaultsOf(const Command &command, const QString &forScheme)
    {
        if (!command.registered) return {};
        const QString text = defaultShortcutText(command.id, forScheme);
        if (text.isEmpty() || text == QLatin1String("none")) return {};
        return QKeySequence::listFromString(text);
    }

    static QHash<QString, QString> schemeFiles()
    {
        QHash<QString, QString> files;
        Q_FOREACH (const QString &file, KoResourcePaths::findAllAssets("kis_shortcuts", "*.shortcuts")) {
            const QString name = QFileInfo(file).completeBaseName();
            if (name != QLatin1String("Default") && !files.contains(name)) files.insert(name, QFileInfo(file).canonicalFilePath());
        }
        return files;
    }

    static QString schemeFile(const QString &name)
    {
        return schemeFiles().value(name);
    }

    static QString schemeTitle(const QString &name)
    {
        static const QHash<QString, QString> titles = {
            {QStringLiteral("Default"), i18nc("shortcut set", "Krita")},
            {QStringLiteral("krita_default"), i18nc("shortcut set", "Krita (file)")},
            {QStringLiteral("photoshop_compatible"), i18nc("shortcut set", "Photoshop")},
            {QStringLiteral("clip_studio_paint_compatible"), i18nc("shortcut set", "Clip Studio Paint")},
            {QStringLiteral("paint_tool_sai_compatible"), i18nc("shortcut set", "Paint Tool SAI")},
            {QStringLiteral("tablet_pro"), i18nc("shortcut set", "Tablet Pro")},
        };
        return titles.value(name, name);
    }

    /// The input profile (the keys held on the canvas) that goes with a set of shortcuts
    QString profileFor(const QString &forScheme) const
    {
        static const QHash<QString, QString> prefixes = {
            {QStringLiteral("Default"), QStringLiteral("Krita Default")},
            {QStringLiteral("krita_default"), QStringLiteral("Krita Default")},
            {QStringLiteral("photoshop_compatible"), QStringLiteral("Photoshop")},
            {QStringLiteral("clip_studio_paint_compatible"), QStringLiteral("Clip Studio")},
            {QStringLiteral("paint_tool_sai_compatible"), QStringLiteral("Paint Tool Sai")},
            {QStringLiteral("tablet_pro"), QStringLiteral("Tablet Pro")},
        };
        const QString prefix = prefixes.value(forScheme);
        if (prefix.isEmpty()) return profile;
        Q_FOREACH (const QString &name, KisInputProfileManager::instance()->profileNames()) {
            if (name.startsWith(prefix, Qt::CaseInsensitive)) return name;
        }
        return profile;
    }

    // ------------------------------------------------------------- what is listed

    void collectCommands()
    {
        QList<QAction *> actions = mainWindow->actionCollection()->actions() + mainWindow->actions();
        if (mainWindow->guiFactory()) {
            Q_FOREACH (KisKXMLGUIClient *client, mainWindow->guiFactory()->clients()) {
                if (client && client->actionCollection()) actions += client->actionCollection()->actions();
            }
        }
        // the menu each is in: its group, if nothing says better
        QHash<QAction *, QString> menuOf;
        QStringList menuOrder;
        std::function<void(QMenu *, const QString &)> walk = [&](QMenu *menu, const QString &top) {
            Q_FOREACH (QAction *action, menu->actions()) {
                if (action->menu()) walk(action->menu(), top);
                else if (!menuOf.contains(action)) menuOf.insert(action, top);
            }
        };
        Q_FOREACH (QAction *top, mainWindow->menuBar()->actions()) {
            if (!top->menu()) continue;
            menuOrder << cleanText(top->text());
            walk(top->menu(), cleanText(top->text()));
        }
        // the words other applications use for them, from Find
        KisStudioFind *find = mainWindow->studioFind();

        KisActionRegistry *registry = KisActionRegistry::instance();
        const QString tools = i18nc("keys group", "Tools");
        const QString painting = i18nc("keys group", "Painting");
        const QString panels = i18nc("keys group", "Panels");
        const QString rings = i18nc("keys group", "Rings");
        const QString other = i18nc("keys group", "Other");
        QSet<QAction *> seen;
        Q_FOREACH (QAction *action, actions) {
            if (!action || seen.contains(action)) continue;
            seen.insert(action);
            const QString id = action->objectName();
            const QString title = cleanText(action->text());
            auto *widgetAction = qobject_cast<QWidgetAction *>(action);
            if (id.isEmpty() || title.isEmpty() || action->isSeparator() || action->menu() || (widgetAction && widgetAction->defaultWidget())) continue;
            // the old toolbars and their switches: nothing in the studio layout
            if (id.endsWith(QLatin1String("ToolBar")) || id == QLatin1String("BrushesAndStuff") || id.startsWith(QLatin1String("customToolBar"))) continue;

            Command command;
            command.action = action;
            command.id = id;
            command.title = title;
            command.registered = registry->hasAction(id);
            command.current = action->shortcuts();
            const QString category = command.registered ? registry->fetchActionCategory(id).categoryName : QString();
            if (id.startsWith(QLatin1String("studio_ring_"))) {
                command.group = rings;
            } else if (id.startsWith(QLatin1String("studio_panel_")) || id.startsWith(QLatin1String("studio_button_"))) {
                command.group = panels;
            } else if (category == QLatin1String("tool-shortcuts") || id.startsWith(QLatin1String("studio_slot_")) || id.startsWith(QLatin1String("studio_item_tool"))
                       || id.startsWith(QLatin1String("Kis")) || id.contains(QLatin1Char('/')) || id.endsWith(QLatin1String("Tool"))) {
                command.group = tools;
            } else if (menuOf.contains(action)) {
                command.group = menuOf.value(action);
            } else if (category == QLatin1String("Painting") || category == QLatin1String("BrushesAndStuff") || category == QLatin1String("Blending Modes")
                       || category == QLatin1String("Resources") || category == QLatin1String("EditFill")) {
                command.group = painting;
            } else {
                command.group = other;
            }
            QStringList words = {title, id, command.group, action->toolTip()};
            KisStudioFind::Item item;
            if (find && find->item(QStringLiteral("action:") + id, &item)) words += item.aliases;
            command.search = words.join(QLatin1Char(' ')).toLower();
            commands.append(command);
        }
        groupOrder = QStringList{tools, painting, heldTitle(), rings} + menuOrder + QStringList{panels, other};
    }

    /// Krita's names of the canvas' input actions, as people say them
    static QString heldName(const QString &name)
    {
        static const QHash<QString, QString> names = {
            {QStringLiteral("Pan Canvas"), i18nc("held key", "Pan")},
            {QStringLiteral("Rotate Canvas"), i18nc("held key", "Rotate")},
            {QStringLiteral("Zoom Canvas"), i18nc("held key", "Zoom")},
            {QStringLiteral("Zoom and Rotate Canvas"), i18nc("held key", "Zoom and rotate")},
            {QStringLiteral("Change Primary Setting"), i18nc("held key: drag to change the brush size", "Brush size")},
            {QStringLiteral("Alternate Invocation"), i18nc("held key: the tool's other use, e.g. sampling a color", "Alternate")},
            {QStringLiteral("Tool Invocation"), i18nc("held key", "Use the tool")},
            {QStringLiteral("Show Popup Widget"), i18nc("held key", "Pop-up palette")},
            {QStringLiteral("Select Layer"), i18nc("held key", "Pick layer")},
            {QStringLiteral("Switch Time"), i18nc("held key: step through animation frames", "Frames")},
            {QStringLiteral("Temporary Straight Ruler"), i18nc("held key", "Straight line")},
            {QStringLiteral("Exposure or Gamma"), i18nc("held key", "Exposure")},
            {QStringLiteral("Touch Gestures"), i18nc("held key", "Touch")},
            {QStringLiteral("Drag to Change"), i18nc("held key: drag to change a brush value", "Drag")},
        };
        return names.value(name, name);
    }

    QString heldTitle() const
    {
        return i18nc("keys group", "Held on the canvas");
    }

    QStringList groupOrder;

    void collectHolds()
    {
        holds.clear();
        KisInputProfile *inputProfile = KisInputProfileManager::instance()->profile(profile);
        if (!inputProfile) return;
        Q_FOREACH (KisShortcutConfiguration *config, inputProfile->allShortcuts()) {
            KisAbstractInputAction *action = config->action();
            if (!action) continue;
            Hold hold;
            hold.live = config;
            hold.staged = *config;
            QString mode;
            const QHash<QString, int> indexes = action->shortcutIndexes();
            for (auto it = indexes.constBegin(); it != indexes.constEnd(); ++it) {
                if (it.value() == int(config->mode())) mode = it.key();
            }
            hold.shortTitle = heldName(action->name());
            // the mode, unless it only says the action again ("Pan Mode" of panning)
            const QString first = action->name().section(QLatin1Char(' '), 0, 0);
            const bool plain = mode.isEmpty() || mode == action->name() || mode == QLatin1String("Normal")
                || mode == first + QLatin1String(" Mode") || mode == QLatin1String("Primary Mode");
            hold.title = plain ? hold.shortTitle : hold.shortTitle + QStringLiteral(" · ") + mode;
            hold.editable = config->type() == KisShortcutConfiguration::KeyCombinationType || config->type() == KisShortcutConfiguration::MouseButtonType;
            holds.append(hold);
        }
        /**
         * What has no keys yet is listed too, a "+" to give it some (a held
         * key and a drag: one with a mouse button; the actions set off by
         * touch or a trackpad alone are left out)
         */
        Q_FOREACH (KisAbstractInputAction *action, KisInputProfileManager::instance()->actions()) {
            const QList<KisShortcutConfiguration *> existing = inputProfile->shortcutsForAction(action);
            bool pointer = existing.isEmpty();
            Q_FOREACH (KisShortcutConfiguration *config, existing) {
                pointer = pointer || config->type() == KisShortcutConfiguration::MouseButtonType || config->type() == KisShortcutConfiguration::KeyCombinationType;
            }
            if (!pointer) continue;
            const QHash<QString, int> indexes = action->shortcutIndexes();
            for (auto it = indexes.constBegin(); it != indexes.constEnd(); ++it) {
                bool set = false;
                Q_FOREACH (KisShortcutConfiguration *config, existing) set = set || int(config->mode()) == it.value();
                if (set) continue;
                Hold hold;
                hold.staged.setAction(action);
                hold.staged.setMode(uint(it.value()));
                hold.staged.setType(KisShortcutConfiguration::MouseButtonType);
                hold.staged.setButtons(Qt::LeftButton);
                hold.shortTitle = heldName(action->name());
                const QString first = action->name().section(QLatin1Char(' '), 0, 0);
                const QString mode = it.key();
                const bool plain = mode.isEmpty() || mode == action->name() || mode == QLatin1String("Normal")
                    || mode == first + QLatin1String(" Mode") || mode == QLatin1String("Primary Mode");
                hold.title = plain ? hold.shortTitle : hold.shortTitle + QStringLiteral(" · ") + mode;
                hold.editable = true;
                holds.append(hold);
            }
        }
        std::sort(holds.begin(), holds.end(), [](const Hold &a, const Hold &b) { return a.title.localeAwareCompare(b.title) < 0; });
    }

    // ------------------------------------------------------------- the list

    void rebuildTree()
    {
        tree->clear();
        QHash<QString, QTreeWidgetItem *> groups;
        auto groupItem = [&](const QString &name) {
            if (!groups.contains(name)) {
                auto *item = new QTreeWidgetItem;
                item->setText(0, name);
                item->setData(0, GroupRole, true);
                item->setFlags(Qt::ItemIsEnabled);
                QFont font = tree->font();
                font.setWeight(QFont::DemiBold);
                item->setFont(0, font);
                groups.insert(name, item);
            }
            return groups.value(name);
        };
        for (int i = 0; i < commands.size(); i++) {
            Command &command = commands[i];
            auto *item = new QTreeWidgetItem(groupItem(command.group));
            item->setText(0, command.title);
            if (command.action) item->setIcon(0, command.action->icon());
            item->setData(0, RowRole, i);
            item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled);
            command.item = item;
        }
        heldGroup = groupItem(heldTitle());
        rebuildHoldItems();
        // the groups in their order, then the rest by name
        QList<QTreeWidgetItem *> ordered;
        Q_FOREACH (const QString &name, groupOrder) {
            if (groups.contains(name)) ordered << groups.take(name);
        }
        QStringList rest = groups.keys();
        rest.sort(Qt::CaseInsensitive);
        Q_FOREACH (const QString &name, rest) ordered << groups.value(name);
        Q_FOREACH (QTreeWidgetItem *group, ordered) {
            group->sortChildren(0, Qt::AscendingOrder);
            tree->addTopLevelItem(group);
            group->setFirstColumnSpanned(true);
            group->setExpanded(true);
        }
        filter();
    }

    void rebuildHoldItems()
    {
        if (!heldGroup) return;
        qDeleteAll(heldGroup->takeChildren());
        for (int i = 0; i < holds.size(); i++) {
            auto *item = new QTreeWidgetItem(heldGroup);
            item->setText(0, holds[i].title);
            item->setData(0, RowRole, -1 - i);
            item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | (holds[i].editable ? Qt::ItemIsDragEnabled : Qt::NoItemFlags));
            holds[i].item = item;
        }
    }

    bool changed(const Command &command)
    {
        return command.current != defaultsOf(command, scheme);
    }

    QSet<int> chordsOfRow(int row) const
    {
        QSet<int> chords;
        if (row >= 0 && row < commands.size()) {
            Q_FOREACH (const QKeySequence &sequence, commands[row].current) chords << chordOf(sequence);
        } else if (row < 0 && -1 - row < holds.size() && !holds[-1 - row].removed) {
            if (const int chord = chordOf(holds[-1 - row].staged.keys())) chords << chord;
        }
        chords.remove(0);
        return chords;
    }

    void filter()
    {
        const QStringList words = search->text().toLower().split(QLatin1Char(' '), Qt::SkipEmptyParts);
        auto matches = [&](const QString &haystack, const QSet<int> &chords) {
            if (keyFilter) return chords.contains(keyFilter);
            for (const QString &word : words) {
                if (!haystack.contains(word)) return false;
            }
            return true;
        };
        for (int i = 0; i < commands.size(); i++) {
            Command &command = commands[i];
            QString keys;
            Q_FOREACH (const QKeySequence &sequence, command.current) keys += QLatin1Char(' ') + sequence.toString(QKeySequence::NativeText).toLower();
            const bool shown = matches(command.search + keys, chordsOfRow(i)) && (!changedOnly->isChecked() || changed(command));
            if (command.item) command.item->setHidden(!shown);
        }
        for (int i = 0; i < holds.size(); i++) {
            const bool shown = matches(holds[i].title.toLower() + QLatin1Char(' ') + inputText(holds[i].staged).toLower(), chordsOfRow(-1 - i))
                && (!changedOnly->isChecked() || holds[i].changed);
            if (holds[i].item) holds[i].item->setHidden(!shown);
        }
        for (int g = 0; g < tree->topLevelItemCount(); g++) {
            QTreeWidgetItem *group = tree->topLevelItem(g);
            bool any = false;
            for (int c = 0; c < group->childCount() && !any; c++) any = !group->child(c)->isHidden();
            group->setHidden(!any);
        }
        updateLit();
        map->update();
    }

    void updateLit()
    {
        map->lit.clear();
        if (keyFilter) {
            map->lit << keyFilter;
        } else if (QTreeWidgetItem *item = tree->currentItem()) {
            if (item->data(0, RowRole).isValid()) map->lit = chordsOfRow(item->data(0, RowRole).toInt());
        }
        map->update();
    }

    /// What has \p chord, for the keyboard: the commands first, then what is held on the canvas
    QVector<KeyUse> usesOn(int chord, int *commandCount) const
    {
        QVector<KeyUse> uses;
        *commandCount = 0;
        if (!chord) return uses;
        for (const Command &command : commands) {
            Q_FOREACH (const QKeySequence &sequence, command.current) {
                if (sequence.count() == 1 && chordOf(sequence) == chord) {
                    uses.append({command.title, command.action ? command.action->icon() : QIcon(), false});
                    (*commandCount)++;
                    break;
                }
            }
        }
        QSet<QString> heldNames;
        for (const Hold &hold : holds) {
            if (!hold.removed && chordOf(hold.staged.keys()) == chord && !heldNames.contains(hold.title)) {
                heldNames.insert(hold.title);
                uses.append({hold.title, QIcon(), true});
            }
        }
        return uses;
    }

    /// What has \p chord: the commands (short names, or whole) first, then the held keys
    QStringList commandsOn(int chord, bool *held, int *commandCount, bool shortNames = true) const
    {
        QStringList names;
        *held = false;
        *commandCount = 0;
        if (!chord) return names;
        for (const Command &command : commands) {
            Q_FOREACH (const QKeySequence &sequence, command.current) {
                if (sequence.count() == 1 && chordOf(sequence) == chord) {
                    QString name = command.title;
                    if (shortNames && name.endsWith(QLatin1String(" Tool"))) name.chop(5);
                    names << name;
                    (*commandCount)++;
                    break;
                }
            }
        }
        for (const Hold &hold : holds) {
            if (!hold.removed && chordOf(hold.staged.keys()) == chord) {
                const QString name = shortNames ? hold.shortTitle : hold.title;
                if (!names.contains(name)) names << name;
                *held = true;
            }
        }
        return names;
    }

    // ------------------------------------------------------------- changing them

    /// \p sequence given to command \p row (as its \p index-th shortcut, -1 one more); whoever had it loses it
    void assignCommand(int row, int index, const QKeySequence &sequence)
    {
        Command &command = commands[row];
        QStringList takenFrom;
        if (!sequence.isEmpty()) {
            for (int i = 0; i < commands.size(); i++) {
                if (i != row && commands[i].current.removeAll(sequence) > 0) takenFrom << commands[i].title;
            }
            command.current.removeAll(sequence);
        }
        if (index >= 0 && index < command.current.size()) {
            if (sequence.isEmpty()) command.current.removeAt(index);
            else command.current[index] = sequence;
        } else if (!sequence.isEmpty()) {
            command.current.append(sequence);
        }
        QStringList held;
        for (const Hold &hold : holds) {
            if (!sequence.isEmpty() && chordOf(hold.staged.keys()) == chordOf(sequence)) held << hold.title;
        }
        if (!takenFrom.isEmpty()) {
            showNote(i18nc("@info keys page", "%1: taken from %2", KisModernStyle::keyCapText(sequence), takenFrom.join(QStringLiteral(", "))));
        } else if (!held.isEmpty()) {
            showNote(i18nc("@info keys page", "%1 is also held for %2", KisModernStyle::keyCapText(sequence), held.join(QStringLiteral(", "))));
        }
        refresh();
    }

    void assignHold(int index, const QList<Qt::Key> &keys)
    {
        Hold &hold = holds[index];
        hold.staged.setKeys(keys);
        hold.removed = false;
        hold.changed = hold.live ? !(hold.staged == *hold.live) : !keys.isEmpty();
        refresh();
    }

    /// Taken off: no keys set it off any more (a key held with no keys would be the pen alone)
    void removeHold(int index)
    {
        Hold &hold = holds[index];
        if (hold.live) {
            hold.removed = true;
            hold.changed = true;
        } else {
            hold.staged.setKeys({});
            hold.changed = false;
        }
        refresh();
    }

    void refresh()
    {
        tree->viewport()->update();
        filter();
    }

    void startRecording(int row, int index)
    {
        recording = true;
        recordRow = row;
        recordIndex = index;
        pendingModifier = 0;
        tree->setFocus();
        tree->grabKeyboard();
        showNote(row < 0 ? i18nc("@info keys page", "Hold the keys, then release · Esc cancels")
                         : i18nc("@info keys page", "Press keys · Backspace clears · Esc cancels"));
        tree->viewport()->update();
    }

    void stopRecording()
    {
        if (!recording) return;
        recording = false;
        tree->releaseKeyboard();
        tree->viewport()->update();
        noteTimer.start(0);
    }

    // ------------------------------------------------------------- the key caps of a row

    /// What sets a held action off, in words; never empty (a wheel turned with no keys says so)
    static QString inputText(const KisShortcutConfiguration &config)
    {
        const QString text = config.getInputText();
        if (!text.isEmpty()) return text;
        switch (config.type()) {
        case KisShortcutConfiguration::MouseWheelType: {
            switch (config.wheel()) {
            case KisShortcutConfiguration::WheelUp: return i18nc("held key input", "Wheel up");
            case KisShortcutConfiguration::WheelDown: return i18nc("held key input", "Wheel down");
            case KisShortcutConfiguration::WheelLeft: return i18nc("held key input", "Wheel left");
            case KisShortcutConfiguration::WheelRight: return i18nc("held key input", "Wheel right");
            default: return i18nc("held key input", "Wheel");
            }
        }
        case KisShortcutConfiguration::TouchGestureType: return i18nc("held key input", "Touch gesture");
        case KisShortcutConfiguration::NativeGestureType: return i18nc("held key input", "Trackpad gesture");
        default: return i18nc("held key input: nothing set", "None");
        }
    }

    struct Cap {
        QRectF rect;
        QString text;
        int index;   ///< which shortcut; -1 the "+", -2 the reset
    };

    QVector<Cap> capsOf(const QStyleOptionViewItem &option, int row) const
    {
        QVector<Cap> caps;
        const QFont font = KisModernStyle::keyCapFont(option.font);
        qreal x = option.rect.left() + 4;
        auto add = [&](const QString &text, int index, KisModernStyle::KeyCap state) {
            const QSizeF size = KisModernStyle::keyCapSize(font, text, state);
            caps.append({QRectF(x, option.rect.center().y() - size.height() / 2, size.width(), size.height()), text, index});
            x += size.width() + 4;
        };
        const bool recordingThis = recording && recordRow == row;
        if (row >= 0) {
            const Command &command = commands[row];
            for (int i = 0; i < command.current.size(); i++) {
                const bool rec = recordingThis && recordIndex == i;
                add(rec ? i18nc("@info recording a shortcut", "Press keys") : KisModernStyle::keyCapText(command.current[i]), i,
                    rec ? KisModernStyle::KeyCap::Recording : KisModernStyle::KeyCap::Shown);
            }
            const bool recAdd = recordingThis && recordIndex < 0;
            add(recAdd ? i18nc("@info recording a shortcut", "Press keys") : QString(), -1,
                recAdd ? KisModernStyle::KeyCap::Recording : KisModernStyle::KeyCap::Add);
        } else {
            const Hold &hold = holds[-1 - row];
            const bool none = hold.removed || (!hold.live && hold.staged.keys().isEmpty());
            if (recordingThis) {
                add(i18nc("@info recording a held key", "Hold keys"), 0, KisModernStyle::KeyCap::Recording);
            } else if (none) {
                add(QString(), 0, KisModernStyle::KeyCap::Add);
            } else {
                add(inputText(hold.staged), 0, KisModernStyle::KeyCap::Shown);
            }
        }
        return caps;
    }
};

namespace
{
/// The key caps of the second column (see KisKeysPage::Private::capsOf)
class KeysDelegate : public QStyledItemDelegate
{
public:
    std::function<void(QPainter *, const QStyleOptionViewItem &, int row)> paintCaps;
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        if (index.column() == 1) {
            const QVariant row = index.sibling(index.row(), 0).data(RowRole);
            QStyleOptionViewItem plain = option;
            plain.text.clear();
            QStyledItemDelegate::paint(painter, plain, index);
            if (row.isValid() && paintCaps) paintCaps(painter, option, row.toInt());
            return;
        }
        QStyledItemDelegate::paint(painter, option, index);
    }

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QSize size = QStyledItemDelegate::sizeHint(option, index);
        size.setHeight(qMax(size.height(), 30));
        return size;
    }
};
} // namespace

KisKeysPage::KisKeysPage(QWidget *parent)
    : QWidget(parent)
    , d(new Private)
{
    d->q = this;
    d->mainWindow = KisPart::instance()->currentMainwindow();
    KConfigGroup schemes = KSharedConfig::openConfig()->group("Shortcut Schemes");
    d->savedScheme = schemes.readEntry("Current Scheme", "Default");
    if (d->savedScheme != QLatin1String("Default") && d->schemeFile(d->savedScheme).isEmpty()) d->savedScheme = QStringLiteral("Default");
    d->scheme = d->savedScheme;
    KisInputProfile *current = KisInputProfileManager::instance()->currentProfile();
    d->savedProfile = current ? current->name() : QString();
    d->profile = d->savedProfile;

    // room for the keyboard: the settings grow to it
    setMinimumWidth(940);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    // find, by words or by keys; only what was changed; the set to start from
    auto *bar = new QHBoxLayout;
    bar->setSpacing(6);
    d->search = new QLineEdit(this);
    d->search->setPlaceholderText(i18nc("@info:placeholder keys page", "Find a command"));
    d->search->setClearButtonEnabled(true);
    bar->addWidget(d->search, 1);
    d->pressKeys = new QToolButton(this);
    d->pressKeys->setText(i18nc("@action:button keys page: find by pressing keys", "Press keys"));
    d->pressKeys->setToolTip(i18nc("@info:tooltip", "Press a key to look it up"));
    d->pressKeys->setCheckable(true);
    bar->addWidget(d->pressKeys);
    d->changedOnly = new QToolButton(this);
    d->changedOnly->setText(i18nc("@action:button keys page: show only changed shortcuts", "Changed"));
    d->changedOnly->setCheckable(true);
    bar->addWidget(d->changedOnly);
    // a ring of your own: made, and its key asked for at once
    auto *newRing = new QToolButton(this);
    newRing->setText(i18nc("@action:button keys page", "New ring"));
    newRing->setToolTip(i18nc("@info:tooltip", "Ring on a key: tap to open, hold and point to pick"));
    bar->addWidget(newRing);
    connect(newRing, &QToolButton::clicked, this, [this]() {
        KisStudioRings *rings = d->mainWindow ? d->mainWindow->studioRings() : nullptr;
        if (!rings) return;
        QAction *ring = rings->createRing();
        Private::Command command;
        command.action = ring;
        command.id = ring->objectName();
        command.title = ring->text();
        command.group = i18nc("keys group", "Rings");
        command.search = (command.title + QLatin1Char(' ') + command.id + QLatin1Char(' ') + command.group).toLower();
        d->commands.append(command);
        d->rebuildTree();
        const int row = d->commands.size() - 1;
        d->tree->scrollToItem(d->commands[row].item);
        d->tree->setCurrentItem(d->commands[row].item);
        d->startRecording(row, -1);
    });
    bar->addSpacing(8);
    bar->addWidget(new QLabel(i18nc("@label:listbox keys page", "Start from"), this));
    d->startFrom = new QComboBox(this);
    QStringList schemeNames = {QStringLiteral("Default")};
    QStringList files = d->schemeFiles().keys();
    files.sort();
    Q_FOREACH (const QString &name, files) {
        // the file of Krita's own is Krita's set again, unless it is the one in use
        if (name == QLatin1String("krita_default") && d->savedScheme != name) continue;
        schemeNames << name;
    }
    Q_FOREACH (const QString &name, schemeNames) d->startFrom->addItem(d->schemeTitle(name), name);
    d->startFrom->setCurrentIndex(qMax(0, d->startFrom->findData(d->scheme)));
    d->startFrom->setToolTip(i18nc("@info:tooltip", "Preset"));
    bar->addWidget(d->startFrom);
    layout->addLayout(bar);

    // the keyboard's layers
    auto *layers = new QHBoxLayout;
    layers->setSpacing(2);
    const QVector<QPair<QString, Qt::KeyboardModifiers>> layerList = {
        {i18nc("keyboard layer: no modifier", "Keys"), Qt::NoModifier},
        {QStringLiteral("Ctrl"), Qt::ControlModifier},
        {QStringLiteral("Shift"), Qt::ShiftModifier},
        {QStringLiteral("Alt"), Qt::AltModifier},
        {QStringLiteral("Ctrl+Shift"), Qt::ControlModifier | Qt::ShiftModifier},
        {QStringLiteral("Ctrl+Alt"), Qt::ControlModifier | Qt::AltModifier},
        {QStringLiteral("Alt+Shift"), Qt::AltModifier | Qt::ShiftModifier},
    };
    for (const auto &entry : layerList) {
        auto *button = new QToolButton(this);
        button->setText(entry.first);
        button->setCheckable(true);
        button->setAutoRaise(true);
        button->setProperty(KisModernStyle::tabButtonProperty, true);
        button->setProperty("layer", int(entry.second));
        button->setToolTip(i18nc("@info:tooltip", "Modifiers"));
        layers->addWidget(button);
        d->layerButtons << button;
    }
    layers->addStretch();
    // what the marks on the keys mean, drawn as the keys draw them
    layers->addWidget(new KeyLegend(KeyLegend::Held, i18nc("@info keys page legend: a key held on the canvas", "Held"), this));
    layers->addWidget(new KeyLegend(KeyLegend::Clash, i18nc("@info keys page legend: two commands on one key", "Conflict"), this));
    layout->addLayout(layers);

    d->map = new KeyboardMap(this);
    d->map->usesOn = [this](int chord, int *commandCount) { return d->usesOn(chord, commandCount); };
    layout->addWidget(d->map);

    d->tree = new KeysTree(this);
    d->tree->setColumnCount(2);
    d->tree->header()->hide();
    d->tree->header()->setStretchLastSection(false);
    d->tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    d->tree->header()->setSectionResizeMode(1, QHeaderView::Fixed);
    d->tree->header()->resizeSection(1, 320);
    d->tree->setRootIsDecorated(false);
    d->tree->setIndentation(10);
    d->tree->setUniformRowHeights(true);
    d->tree->setIconSize(QSize(18, 18));
    d->tree->setDragEnabled(true);
    d->tree->setDragDropMode(QAbstractItemView::DragOnly);
    d->tree->setMouseTracking(true);
    d->tree->viewport()->installEventFilter(this);
    d->tree->installEventFilter(this);
    auto *delegate = new KeysDelegate(d->tree);
    delegate->paintCaps = [this](QPainter *painter, const QStyleOptionViewItem &option, int row) {
        const QFont font = KisModernStyle::keyCapFont(option.font);
        const QPoint pointer = d->tree->viewport()->mapFromGlobal(QCursor::pos());
        Q_FOREACH (const Private::Cap &cap, d->capsOf(option, row)) {
            KisModernStyle::KeyCap state = KisModernStyle::KeyCap::Shown;
            const bool recordingThis = d->recording && d->recordRow == row && (row < 0 || d->recordIndex == cap.index);
            if (recordingThis) state = KisModernStyle::KeyCap::Recording;
            else if (cap.index == -1 || cap.text.isEmpty()) state = KisModernStyle::KeyCap::Add;
            else if (cap.rect.contains(pointer)) state = KisModernStyle::KeyCap::Pointed;
            QPalette pal = option.palette;
            if (row < 0 && !d->holds[-1 - row].editable) {
                // a wheel or a gesture: shown, not changed here
                QColor text = pal.color(QPalette::Text);
                text.setAlphaF(0.55);
                pal.setColor(QPalette::Text, text);
                pal.setColor(QPalette::ButtonText, text);
                pal.setColor(QPalette::WindowText, text);
            }
            KisModernStyle::drawKeyCap(painter, cap.rect, font, cap.text, pal, state);
        }
        // changed: the way back, after the caps
        const bool isChanged = row >= 0 ? d->changed(d->commands[row]) : d->holds[-1 - row].changed;
        if (isChanged) {
            const QRectF back(option.rect.right() - 22, option.rect.center().y() - 8, 16, 16);
            painter->save();
            painter->setRenderHint(QPainter::Antialiasing, true);
            QColor ink = option.palette.color(QPalette::Text);
            ink.setAlphaF(back.contains(pointer) ? 0.9 : 0.5);
            painter->setPen(QPen(ink, 1.4, Qt::SolidLine, Qt::RoundCap));
            painter->setBrush(Qt::NoBrush);
            painter->drawArc(back.adjusted(2, 2, -2, -2), 90 * 16, 270 * 16);
            painter->drawLine(QPointF(back.center().x(), back.top() + 2), QPointF(back.center().x() + 3, back.top() - 0.5));
            painter->drawLine(QPointF(back.center().x(), back.top() + 2), QPointF(back.center().x() + 3, back.top() + 4.5));
            painter->restore();
        }
    };
    d->tree->setItemDelegate(delegate);
    layout->addWidget(d->tree, 1);

    d->note = new QLabel(this);
    d->note->setWordWrap(true);
    QPalette faint = d->note->palette();
    QColor faintInk = faint.color(QPalette::WindowText);
    faintInk.setAlphaF(0.6);
    faint.setColor(QPalette::WindowText, faintInk);
    d->note->setPalette(faint);
    layout->addWidget(d->note);
    d->noteTimer.setSingleShot(true);
    d->noteTimer.setInterval(6000);
    connect(&d->noteTimer, &QTimer::timeout, this, [this]() { d->note->setText(d->hint()); });
    d->note->setText(d->hint());

    d->collectCommands();
    d->collectHolds();
    d->rebuildTree();

    // what the keyboard does
    auto pickLayer = [this](Qt::KeyboardModifiers layer) {
        d->pickedLayer = layer;
        d->map->layer = layer;
        Q_FOREACH (QToolButton *button, d->layerButtons) button->setChecked(Qt::KeyboardModifiers(button->property("layer").toInt()) == layer);
        d->map->update();
    };
    Q_FOREACH (QToolButton *button, d->layerButtons) {
        connect(button, &QToolButton::clicked, this, [pickLayer, button]() { pickLayer(Qt::KeyboardModifiers(button->property("layer").toInt())); });
    }
    pickLayer(Qt::NoModifier);
    d->map->clicked = [this, pickLayer](int chord) {
        if (chord < 0) {
            // a modifier clicked on the keyboard: its layer
            pickLayer(d->map->layer);
            return;
        }
        // what has this key; again, everything
        d->keyFilter = d->keyFilter == chord ? 0 : chord;
        d->search->blockSignals(true);
        d->search->setText(d->keyFilter ? KisModernStyle::keyCapText(QKeySequence(chord)) : QString());
        d->search->blockSignals(false);
        d->filter();
        bool held = false;
        int count = 0;
        if (d->keyFilter && d->commandsOn(chord, &held, &count).isEmpty()) {
            d->showNote(i18nc("@info keys page", "%1 is free", KisModernStyle::keyCapText(QKeySequence(chord))));
        }
    };
    d->map->dropped = [this](const QMimeData *mime, int chord) {
        const int row = mime->data(QString::fromLatin1(commandMime)).toInt();
        if (row >= 0 && row < d->commands.size()) {
            d->assignCommand(row, -1, QKeySequence(chord));
        } else if (row < 0 && -1 - row < d->holds.size() && d->holds[-1 - row].editable) {
            // the keys of the chord, as a held shortcut has them
            QList<Qt::Key> keys;
            const Qt::KeyboardModifiers modifiers(chord & int(Qt::KeyboardModifierMask));
            if (modifiers & Qt::ControlModifier) keys << Qt::Key_Control;
            if (modifiers & Qt::ShiftModifier) keys << Qt::Key_Shift;
            if (modifiers & Qt::AltModifier) keys << Qt::Key_Alt;
            if (modifiers & Qt::MetaModifier) keys << Qt::Key_Meta;
            const int key = chord & ~int(Qt::KeyboardModifierMask);
            if (!keys.contains(Qt::Key(key))) keys << Qt::Key(key);
            d->assignHold(-1 - row, keys);
        }
    };

    connect(d->search, &QLineEdit::textEdited, this, [this]() {
        d->keyFilter = 0;
        d->filter();
    });
    connect(d->changedOnly, &QToolButton::toggled, this, [this]() { d->filter(); });
    connect(d->pressKeys, &QToolButton::toggled, this, [this](bool on) {
        if (on) {
            d->search->setFocus();
            d->search->clear();
            d->search->setPlaceholderText(i18nc("@info:placeholder keys page", "Press keys to see what they do"));
        } else {
            d->search->setPlaceholderText(i18nc("@info:placeholder keys page", "Find a command"));
        }
    });
    d->search->installEventFilter(this);
    connect(d->tree, &QTreeWidget::currentItemChanged, this, [this]() { d->updateLit(); });
    connect(d->startFrom, QOverload<int>::of(&QComboBox::activated), this, [this](int index) {
        const QString name = d->startFrom->itemData(index).toString();
        d->scheme = name;
        d->profile = d->profileFor(name);
        d->resetHeld = false;
        int changes = 0;
        for (Private::Command &command : d->commands) {
            const QList<QKeySequence> defaults = d->defaultsOf(command, name);
            if (command.current != defaults) changes++;
            command.current = defaults;
        }
        d->collectHolds();
        d->rebuildHoldItems();
        d->showNote(i18nc("@info keys page", "%2 changed from %1", d->schemeTitle(name), changes));
        d->refresh();
    });
}

KisKeysPage::~KisKeysPage() = default;

void KisKeysPage::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    // modifiers held on the real keyboard show their layer
    qApp->installEventFilter(this);
}

void KisKeysPage::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    qApp->removeEventFilter(this);
    d->stopRecording();
}

bool KisKeysPage::eventFilter(QObject *watched, QEvent *event)
{
    const QEvent::Type type = event->type();

    // recording: the next keys pressed
    if (d->recording && (type == QEvent::KeyPress || type == QEvent::KeyRelease || type == QEvent::ShortcutOverride)) {
        auto *key = static_cast<QKeyEvent *>(event);
        if (type == QEvent::ShortcutOverride) {
            event->accept();
            return true;
        }
        if (key->isAutoRepeat()) return true;
        const int code = key->key();
        if (type == QEvent::KeyPress) {
            if (code == Qt::Key_Escape) {
                d->stopRecording();
                return true;
            }
            if (isModifierKey(code)) {
                d->pendingModifier = code;
                return true;
            }
            d->pendingModifier = 0;
            const int row = d->recordRow;
            const int index = d->recordIndex;
            d->stopRecording();
            if (row >= 0) {
                if (code == Qt::Key_Backspace || code == Qt::Key_Delete) {
                    if (index >= 0) d->assignCommand(row, index, QKeySequence());
                } else {
                    d->assignCommand(row, index, QKeySequence(chordOf(code, key->modifiers())));
                }
            } else {
                QList<Qt::Key> keys;
                if (key->modifiers() & Qt::ControlModifier) keys << Qt::Key_Control;
                if (key->modifiers() & Qt::ShiftModifier) keys << Qt::Key_Shift;
                if (key->modifiers() & Qt::AltModifier) keys << Qt::Key_Alt;
                if (key->modifiers() & Qt::MetaModifier) keys << Qt::Key_Meta;
                keys << Qt::Key(code);
                d->assignHold(-1 - row, keys);
            }
            return true;
        }
        // a modifier let go with nothing after it: a held key may be a modifier alone
        if (type == QEvent::KeyRelease && d->pendingModifier && code == d->pendingModifier && d->recordRow < 0) {
            const int row = d->recordRow;
            const Qt::Key held = Qt::Key(d->pendingModifier);
            d->stopRecording();
            d->assignHold(-1 - row, {held});
            return true;
        }
        return true;
    }

    // finding by keys: what is pressed in the field
    if (watched == d->search && d->pressKeys->isChecked() && type == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        if (isModifierKey(key->key())) return true;
        if (key->key() == Qt::Key_Escape) {
            d->pressKeys->setChecked(false);
            return true;
        }
        d->keyFilter = chordOf(key->key(), key->modifiers());
        d->search->setText(KisModernStyle::keyCapText(QKeySequence(d->keyFilter)));
        d->filter();
        return true;
    }
    if (watched == d->search && type == QEvent::ShortcutOverride && d->pressKeys->isChecked()) {
        event->accept();
        return true;
    }

    // modifiers held anywhere on the page: their layer, while they are
    if ((type == QEvent::KeyPress || type == QEvent::KeyRelease) && isVisible() && !d->recording) {
        auto *key = static_cast<QKeyEvent *>(event);
        if (isModifierKey(key->key())) {
            Qt::KeyboardModifiers held = key->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier);
            if (type == QEvent::KeyRelease) held &= ~modifierOf(key->key());
            d->map->layer = held ? held : d->pickedLayer;
            Q_FOREACH (QToolButton *button, d->layerButtons) button->setChecked(Qt::KeyboardModifiers(button->property("layer").toInt()) == d->map->layer);
            d->map->update();
        }
    }

    // the list: a click on a key cap records, a right click removes, the arrow puts it back
    if (watched == d->tree->viewport()) {
        if (type == QEvent::MouseMove || type == QEvent::Leave) {
            d->tree->viewport()->update();
            return false;
        }
        if (type == QEvent::MouseButtonPress || type == QEvent::MouseButtonDblClick) {
            auto *mouse = static_cast<QMouseEvent *>(event);
            const QModelIndex at = d->tree->indexAt(mouse->pos());
            if (!at.isValid()) return false;
            const QVariant rowData = at.sibling(at.row(), 0).data(RowRole);
            if (!rowData.isValid()) return false;
            const int row = rowData.toInt();
            QStyleOptionViewItem option;
            option.initFrom(d->tree);
            option.font = d->tree->font();
            option.rect = d->tree->visualRect(at.sibling(at.row(), 1));
            if (!option.rect.contains(mouse->pos())) {
                d->stopRecording();
                return false;
            }
            // the way back
            const QRectF back(option.rect.right() - 24, option.rect.center().y() - 10, 20, 20);
            const bool isChanged = row >= 0 ? d->changed(d->commands[row]) : d->holds[-1 - row].changed;
            if (isChanged && back.contains(mouse->pos()) && mouse->button() == Qt::LeftButton) {
                d->stopRecording();
                if (row >= 0) {
                    d->commands[row].current = d->defaultsOf(d->commands[row], d->scheme);
                } else {
                    Private::Hold &hold = d->holds[-1 - row];
                    if (hold.live) hold.staged = *hold.live;
                    else hold.staged.setKeys({});
                    hold.removed = false;
                    hold.changed = false;
                }
                d->refresh();
                return true;
            }
            Q_FOREACH (const Private::Cap &cap, d->capsOf(option, row)) {
                if (!cap.rect.contains(mouse->pos())) continue;
                d->tree->setCurrentItem(d->tree->itemAt(mouse->pos()));
                if (mouse->button() == Qt::RightButton) {
                    d->stopRecording();
                    if (row >= 0 && cap.index >= 0) d->assignCommand(row, cap.index, QKeySequence());
                    else if (row < 0 && d->holds[-1 - row].editable) d->removeHold(-1 - row);
                    return true;
                }
                if (row < 0 && !d->holds[-1 - row].editable) {
                    d->showNote(i18nc("@info keys page", "Wheels and gestures can't be changed here yet"));
                    return true;
                }
                d->startRecording(row, cap.index);
                return true;
            }
            d->stopRecording();
        }
    }
    return QWidget::eventFilter(watched, event);
}

void KisKeysPage::setDefaults()
{
    d->scheme = QStringLiteral("Default");
    d->startFrom->setCurrentIndex(qMax(0, d->startFrom->findData(d->scheme)));
    for (Private::Command &command : d->commands) command.current = d->defaultsOf(command, d->scheme);
    // the keys held on the canvas: Krita's own profile, as installed (with OK)
    d->profile = d->profileFor(d->scheme);
    d->resetHeld = true;
    for (Private::Hold &hold : d->holds) {
        if (hold.live) hold.staged = *hold.live;
        else hold.staged.setKeys({});
        hold.removed = false;
        hold.changed = false;
    }
    d->showNote(i18nc("@info keys page", "Resets all keys on OK"));
    d->refresh();
}

void KisKeysPage::apply()
{
    d->stopRecording();
    KisActionRegistry *registry = KisActionRegistry::instance();

    // the set started from
    if (d->scheme != d->savedScheme) {
        KConfigGroup schemes = KSharedConfig::openConfig()->group("Shortcut Schemes");
        schemes.writeEntry("Current Scheme", d->scheme);
        schemes.sync();
        registry->loadShortcutScheme(d->scheme);
    }

    // what differs from that set, kept; what is as it has it, not
    KConfigGroup custom(KSharedConfig::openConfig(QStringLiteral("kritashortcutsrc")), QStringLiteral("Shortcuts"));
    for (const Private::Command &command : d->commands) {
        if (command.registered && command.current == d->defaultsOf(command, d->scheme)) {
            custom.deleteEntry(command.id);
        } else if (!command.registered && command.action && command.current == command.action->shortcuts()) {
            continue;
        } else {
            custom.writeEntry(command.id, command.current.isEmpty() ? QStringLiteral("none")
                                                                    : QKeySequence::listToString(command.current, QKeySequence::PortableText));
        }
        if (command.action) command.action->setShortcuts(command.current);
    }
    custom.sync();
    registry->settingsPageSaved();

    // the keys held on the canvas
    KisInputProfileManager *input = KisInputProfileManager::instance();
    if (d->resetHeld) {
        QDir profileDir(KoResourcePaths::saveLocation("data", "input/", false));
        if (profileDir.exists()) {
            Q_FOREACH (const QString &file, profileDir.entryList(QStringList() << QStringLiteral("*.profile"), QDir::Files | QDir::NoDotAndDotDot)) {
                profileDir.remove(file);
            }
        }
    } else {
        KisInputProfile *profile = input->profile(d->profile);
        bool held = false;
        for (Private::Hold &hold : d->holds) {
            if (!hold.changed || !profile) continue;
            if (hold.removed && hold.live) {
                profile->removeShortcut(hold.live);
                delete hold.live;
                hold.live = nullptr;
            } else if (hold.live) {
                *hold.live = hold.staged;
            } else if (!hold.staged.keys().isEmpty()) {
                profile->addShortcut(new KisShortcutConfiguration(hold.staged));
            }
            held = true;
        }
        if (held) input->saveProfiles();
    }
    // read again, which tells the canvases: they make their own shortcuts from the profile
    KisConfig(false).setCurrentInputProfile(d->profile);
    input->loadProfiles();
}
