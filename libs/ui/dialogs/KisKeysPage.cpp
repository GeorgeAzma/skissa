/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisKeysPage.h"
#include <KisProfile.h>

#include <QAbstractItemView>
#include <QApplication>
#include <QComboBox>
#include <QDir>
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

#include <algorithm>

#include <KConfig>
#include <KConfigGroup>
#include <KSharedConfig>
#include <klocalizedstring.h>

#include <KisModernStyle.h>
#include <KoResourcePaths.h>
#include <kis_action_registry.h>
#include <KisSentenceCase.h>
#include <KoToolFactoryBase.h>
#include <KoToolRegistry.h>

#include "KisMainWindow.h"
#include "KisPart.h"
#include "KisStudioFind.h"
#include "KisStudioItemCard.h"
#include "KisStudioRings.h"
#include "KisStudioTips.h"
#include "input/kis_abstract_input_action.h"
#include "input/kis_input_profile.h"
#include "input/kis_input_profile_manager.h"
#include "input/kis_shortcut_configuration.h"
#include "kis_config.h"

namespace
{

const char *const commandMime = "application/x-krita-keys-command";

using KisStudioShortcuts::isModifierKey;
using KisStudioShortcuts::plainText;

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

/// Shifted symbols mapped to their base keys on the drawn (US) keyboard
const QHash<int, int> &shiftedSymbols()
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
    return shifted;
}

/// A key and its modifiers as one chord, by physical key: "{" becomes Shift+[
int chordOf(int key, Qt::KeyboardModifiers modifiers)
{
    modifiers &= Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier | Qt::MetaModifier;
    if (shiftedSymbols().contains(key)) {
        key = shiftedSymbols().value(key);
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

/// The chord of a held shortcut's keys: its modifiers plus its first other key, or a lone modifier
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

/// The held-shortcut keys for \p key pressed with \p modifiers
QList<Qt::Key> heldKeys(Qt::KeyboardModifiers modifiers, int key)
{
    QList<Qt::Key> keys;
    if (modifiers & Qt::ControlModifier) keys << Qt::Key_Control;
    if (modifiers & Qt::ShiftModifier) keys << Qt::Key_Shift;
    if (modifiers & Qt::AltModifier) keys << Qt::Key_Alt;
    if (modifiers & Qt::MetaModifier) keys << Qt::Key_Meta;
    if (!keys.contains(Qt::Key(key))) keys << Qt::Key(key);
    return keys;
}

/// A key on the drawn keyboard; position and size are in key widths
struct KeyCell {
    int key;
    QString label;
    qreal x, y, w, h;
};

QVector<KeyCell> keyboardLayout()
{
    QVector<KeyCell> cells;
    auto row = [&cells](qreal y, qreal h, qreal x, const QVector<std::tuple<int, QString, qreal>> &keys) {
        for (const auto &k : keys) {
            const int key = std::get<0>(k);
            if (key) cells.append({key, std::get<1>(k), x, y, std::get<2>(k), h});
            x += std::get<2>(k);
        }
    };
    const QString ctrl = QStringLiteral("Ctrl"), shift = QStringLiteral("Shift"), alt = QStringLiteral("Alt");
    row(0, 0.75, 0, {{Qt::Key_Escape, QStringLiteral("Esc"), 1}, {0, {}, 1},
                     {Qt::Key_F1, QStringLiteral("F1"), 1}, {Qt::Key_F2, QStringLiteral("F2"), 1},
                     {Qt::Key_F3, QStringLiteral("F3"), 1}, {Qt::Key_F4, QStringLiteral("F4"), 1}, {0, {}, 0.5},
                     {Qt::Key_F5, QStringLiteral("F5"), 1}, {Qt::Key_F6, QStringLiteral("F6"), 1},
                     {Qt::Key_F7, QStringLiteral("F7"), 1}, {Qt::Key_F8, QStringLiteral("F8"), 1}, {0, {}, 0.5},
                     {Qt::Key_F9, QStringLiteral("F9"), 1}, {Qt::Key_F10, QStringLiteral("F10"), 1},
                     {Qt::Key_F11, QStringLiteral("F11"), 1}, {Qt::Key_F12, QStringLiteral("F12"), 1}});
    row(1, 1, 0, {{Qt::Key_QuoteLeft, QStringLiteral("`"), 1}, {Qt::Key_1, QStringLiteral("1"), 1},
                  {Qt::Key_2, QStringLiteral("2"), 1}, {Qt::Key_3, QStringLiteral("3"), 1},
                  {Qt::Key_4, QStringLiteral("4"), 1}, {Qt::Key_5, QStringLiteral("5"), 1},
                  {Qt::Key_6, QStringLiteral("6"), 1}, {Qt::Key_7, QStringLiteral("7"), 1},
                  {Qt::Key_8, QStringLiteral("8"), 1}, {Qt::Key_9, QStringLiteral("9"), 1},
                  {Qt::Key_0, QStringLiteral("0"), 1}, {Qt::Key_Minus, QStringLiteral("-"), 1},
                  {Qt::Key_Equal, QStringLiteral("="), 1}, {Qt::Key_Backspace, QStringLiteral("⌫"), 2},
                  {0, {}, 0.5}, {Qt::Key_Insert, QStringLiteral("Ins"), 1},
                  {Qt::Key_Home, QStringLiteral("Home"), 1}, {Qt::Key_PageUp, QStringLiteral("PgUp"), 1}});
    row(2, 1, 0, {{Qt::Key_Tab, QStringLiteral("Tab"), 1.5}, {Qt::Key_Q, QStringLiteral("Q"), 1},
                  {Qt::Key_W, QStringLiteral("W"), 1}, {Qt::Key_E, QStringLiteral("E"), 1},
                  {Qt::Key_R, QStringLiteral("R"), 1}, {Qt::Key_T, QStringLiteral("T"), 1},
                  {Qt::Key_Y, QStringLiteral("Y"), 1}, {Qt::Key_U, QStringLiteral("U"), 1},
                  {Qt::Key_I, QStringLiteral("I"), 1}, {Qt::Key_O, QStringLiteral("O"), 1},
                  {Qt::Key_P, QStringLiteral("P"), 1}, {Qt::Key_BracketLeft, QStringLiteral("["), 1},
                  {Qt::Key_BracketRight, QStringLiteral("]"), 1}, {Qt::Key_Backslash, QStringLiteral("\\"), 1.5},
                  {0, {}, 0.5}, {Qt::Key_Delete, QStringLiteral("Del"), 1},
                  {Qt::Key_End, QStringLiteral("End"), 1}, {Qt::Key_PageDown, QStringLiteral("PgDn"), 1}});
    row(3, 1, 0, {{Qt::Key_CapsLock, QStringLiteral("Caps"), 1.75}, {Qt::Key_A, QStringLiteral("A"), 1},
                  {Qt::Key_S, QStringLiteral("S"), 1}, {Qt::Key_D, QStringLiteral("D"), 1},
                  {Qt::Key_F, QStringLiteral("F"), 1}, {Qt::Key_G, QStringLiteral("G"), 1},
                  {Qt::Key_H, QStringLiteral("H"), 1}, {Qt::Key_J, QStringLiteral("J"), 1},
                  {Qt::Key_K, QStringLiteral("K"), 1}, {Qt::Key_L, QStringLiteral("L"), 1},
                  {Qt::Key_Semicolon, QStringLiteral(";"), 1}, {Qt::Key_Apostrophe, QStringLiteral("'"), 1},
                  {Qt::Key_Return, QStringLiteral("Enter"), 2.25}});
    row(4, 1, 0, {{Qt::Key_Shift, shift, 2.25}, {Qt::Key_Z, QStringLiteral("Z"), 1},
                  {Qt::Key_X, QStringLiteral("X"), 1}, {Qt::Key_C, QStringLiteral("C"), 1},
                  {Qt::Key_V, QStringLiteral("V"), 1}, {Qt::Key_B, QStringLiteral("B"), 1},
                  {Qt::Key_N, QStringLiteral("N"), 1}, {Qt::Key_M, QStringLiteral("M"), 1},
                  {Qt::Key_Comma, QStringLiteral(","), 1}, {Qt::Key_Period, QStringLiteral("."), 1},
                  {Qt::Key_Slash, QStringLiteral("/"), 1}, {Qt::Key_Shift, shift, 2.75},
                  {0, {}, 1.5}, {Qt::Key_Up, QStringLiteral("↑"), 1}});
    row(5, 1, 0, {{Qt::Key_Control, ctrl, 1.25}, {Qt::Key_Meta, QStringLiteral("Win"), 1.25},
                  {Qt::Key_Alt, alt, 1.25}, {Qt::Key_Space, QStringLiteral("Space"), 6.25},
                  {Qt::Key_Alt, alt, 1.25}, {Qt::Key_Meta, QStringLiteral("Win"), 1.25},
                  {Qt::Key_Menu, QStringLiteral("☰"), 1.25}, {Qt::Key_Control, ctrl, 1.25},
                  {0, {}, 0.5}, {Qt::Key_Left, QStringLiteral("←"), 1},
                  {Qt::Key_Down, QStringLiteral("↓"), 1}, {Qt::Key_Right, QStringLiteral("→"), 1}});
    return cells;
}

/// A legend entry for a key mark: a bar for keys held on the canvas, a red badge for conflicts
class KeyLegend : public QWidget
{
public:
    enum Mark { Held, Clash };

    KeyLegend(Mark mark, const QString &text, QWidget *parent)
        : QWidget(parent)
        , m_mark(mark)
        , m_text(text)
    {
        setToolTip(mark == Held ? i18nc("@info:tooltip", "Works while held, like Space to pan") : i18nc("@info:tooltip", "Two commands on one key"));
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
        const QColor face = KisModernStyle::mix(window, text, 0.15);
        painter.setPen(Qt::NoPen);
        painter.setBrush(KisModernStyle::mix(face, QColor(0, 0, 0), 0.45));
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

/// One thing a key does, as shown on the keyboard
struct KeyUse {
    QString name;
    QIcon icon;
    bool held {false};
    /// Covered by the shown tool's own key while that tool is in use
    bool elsewhere {false};
};

/**
 * A drawn keyboard whose keys show the icon (or name) of what they do in the current modifier layer,
 * with a count badge when a key does several things (red on conflict) and a bar for keys held on the canvas.
 */
class KeyboardMap : public QWidget
{
public:
    /// What a chord does, and how many of those are commands (more than one is a conflict)
    std::function<QVector<KeyUse>(int chord, int *commandCount)> usesOn;
    std::function<void(int chord)> clicked;
    std::function<void(const QMimeData *mime, int chord)> dropped;
    /// Chords of the item selected in the list, drawn highlighted
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
        // the bottom row's raised side and its legend too
        return int(5.85 * qMin<qreal>(width / m_units, 52.0)) + 10;
    }

protected:
    qreal unit() const { return qMin<qreal>(width() / m_units, 52.0); }

    QRectF cellRect(const KeyCell &cell) const
    {
        const qreal u = unit();
        const qreal left = (width() - m_units * u) / 2;
        // Keep the function row close to the rest, like the gap on a real keyboard
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

    /// The chord a key stands for in the current layer; a modifier key stands for itself
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

            // The cap's face, with a darker side below so the key looks raised
            QColor face = KisModernStyle::mix(window, text, used ? 0.15 : 0.06);
            if (hovered) face = KisModernStyle::mix(face, text, 0.08);
            if (modifierOn || isLit) face = KisModernStyle::mix(window, accent, 0.75);
            const QColor side = KisModernStyle::mix(face, QColor(0, 0, 0), 0.45);
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
                    // Without an icon, show the first word (two on wide keys), elided
                    QColor nameInk = ink;
                    nameInk.setAlphaF(0.9);
                    painter.setPen(nameInk);
                    painter.setFont(nameFont);
                    const QFontMetricsF metrics(nameFont);
                    const QString word = first.name.section(QLatin1Char(' '), 0, cell.w > 1.6 ? 1 : 0);
                    painter.drawText(body, Qt::AlignHCenter | Qt::AlignVCenter, metrics.elidedText(word, Qt::ElideRight, body.width()));
                }
                // Held on the canvas: a bar along the front edge
                bool held = false;
                for (const KeyUse &use : uses) held = held || use.held;
                if (held) {
                    QColor bar = ink;
                    bar.setAlphaF(0.6);
                    painter.setPen(Qt::NoPen);
                    painter.setBrush(bar);
                    painter.drawRoundedRect(QRectF(r.center().x() - u * 0.14, r.bottom() - u * 0.09, u * 0.28, 2), 1, 1);
                }
                // Several uses: a count badge, red when two commands conflict
                if (uses.size() > 1) {
                    const QString count = QString::number(uses.size());
                    const qreal d = u * 0.3;
                    const QRectF badge(r.right() - d - u * 0.05, r.top() + u * 0.05, d, d);
                    painter.setPen(Qt::NoPen);
                    painter.setBrush(commandCount > 1 ? clashColor : KisModernStyle::mix(face, text, 0.35));
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
                    lines << (use.held ? i18nc("@info:tooltip a key held on the canvas", "%1 (held)", use.name)
                              : use.elsewhere ? i18nc("@info:tooltip a key's command that the tool's own key covers while the tool is in use", "%1 (elsewhere)", use.name)
                                              : use.name);
                }
                QToolTip::showText(help->globalPos(), lines.isEmpty() ? KisStudioTips::html(i18nc("@info:tooltip a key with no shortcut", "Nothing on %1", key))
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
            // Clicking a modifier toggles its layer
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

/// The list, whose rows (commands or held keys) can be dragged onto the keyboard
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

constexpr int RowRole = Qt::UserRole;      ///< a command's index, or -1 - index for a held key
constexpr int GroupRole = Qt::UserRole + 1; ///< set on group headings

} // namespace

struct KisKeysPage::Private
{
    struct Command {
        QPointer<QAction> action;
        /// Other actions of the same name (a copy in another collection), given the same keys
        QList<QPointer<QAction>> copies;
        QString id;
        QString title;
        QString group;
        QString search;   ///< lowercase search text: name, id, group and the names other apps use
        QList<QKeySequence> current;
        /// The tools it is a shortcut of, only while one is in use; empty: everywhere
        QStringList tools;
        /// Changed on this page: only these are written, so keys set elsewhere meanwhile stay
        bool edited {false};
        bool registered {false};
        QTreeWidgetItem *item {nullptr};
    };
    struct Hold {
        /// The profile's entry; null for an action mode that has no keys yet
        KisShortcutConfiguration *live {nullptr};
        KisShortcutConfiguration staged;
        /// Cleared with a right click; removed from the profile on apply()
        bool removed {false};
        QString title;
        bool editable {false};
        bool changed {false};
        QTreeWidgetItem *item {nullptr};
    };

    QPointer<KisMainWindow> mainWindow;
    /// Rings created on this page; removed again unless applied
    QStringList newRings;
    QVector<Command> commands;
    QVector<Hold> holds;
    QString scheme;
    QString savedScheme;
    QString profile;
    QString savedProfile;
    QHash<QString, QHash<QString, QString>> schemeEntries;

    QLineEdit *search {nullptr};
    QToolButton *pressKeys {nullptr};
    QToolButton *changedOnly {nullptr};
    QComboBox *startFrom {nullptr};
    /// Which keys the keyboard shows: those that work everywhere, or while a tool is in use
    QComboBox *contextBox {nullptr};
    /// The tool shown (its id), or empty for everywhere
    QString context;
    /// The tools that have shortcuts of their own, by name
    QList<QPair<QString, QString>> contextTools;
    QList<QToolButton *> layerButtons;
    KeyboardMap *map {nullptr};
    KeysTree *tree {nullptr};
    QLabel *note {nullptr};
    QTimer noteTimer;
    QTreeWidgetItem *heldGroup {nullptr};

    /// Chord to filter by, from a keyboard click or a key pressed into the search
    int keyFilter {0};
    /// While recording: the row (as RowRole) and which of its shortcuts (-1 adds one)
    bool recording {false};
    int recordRow {0};
    int recordIndex {-1};
    /// A modifier pressed alone while recording a held key; it becomes the key if released with nothing else
    int pendingModifier {0};
    /// The layer picked with the buttons; modifiers held on the real keyboard override it
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

    /// The input profile (keys held on the canvas) that matches a shortcut scheme
    QString profileFor(const QString &forScheme) const
    {
        if (forScheme == savedScheme) return savedProfile;
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

    void collectCommands()
    {
        // Each action's top-level menu, used as its group unless a better one applies
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
            menuOrder << plainText(top->text());
            walk(top->menu(), plainText(top->text()));
        }
        // Find knows the names other applications use for commands
        KisStudioFind *find = mainWindow->studioFind();

        KisActionRegistry *registry = KisActionRegistry::instance();
        const QString tools = i18nc("keys group", "Tools");
        const QString painting = i18nc("keys group", "Painting");
        const QString panels = i18nc("keys group", "Panels");
        const QString rings = i18nc("keys group", "Rings");
        const QString other = i18nc("keys group", "Other");
        QStringList toolGroups;
        QHash<QString, int> byId;
        Q_FOREACH (QAction *action, KisStudioShortcuts::allActions(mainWindow)) {
            if (!action) continue;
            const QString id = action->objectName();
            // one row per command: a second action of the same name is a copy of it
            if (!id.isEmpty() && byId.contains(id)) {
                commands[byId.value(id)].copies << action;
                continue;
            }
            const QString title = plainText(action->text());
            auto *widgetAction = qobject_cast<QWidgetAction *>(action);
            if (id.isEmpty() || title.isEmpty() || action->isSeparator() || action->menu() || (widgetAction && widgetAction->defaultWidget())) continue;
            // Skip toolbar toggles: the studio layout has no toolbars
            if (id.endsWith(QLatin1String("ToolBar")) || id == QLatin1String("BrushesAndStuff") || id.startsWith(QLatin1String("customToolBar"))) continue;

            Command command;
            command.action = action;
            command.id = id;
            command.title = title;
            command.registered = registry->hasAction(id);
            command.current = action->shortcuts();
            command.tools = KisStudioShortcuts::toolsOf(action);
            const QString category = command.registered ? registry->fetchActionCategory(id).categoryName : QString();
            if (!command.tools.isEmpty() && command.tools.size() <= ownToolsAtMost) {
                // a tool's own keys, under that tool; those many tools share (the brush size) stay in their groups
                const QString tool = toolName(command.tools.first());
                command.group = i18nc("keys group: shortcuts that work only while this tool is in use, %1 is a tool", "While using %1", tool);
                if (!toolGroups.contains(command.group)) toolGroups << command.group;
            } else if (id.startsWith(QLatin1String("studio_ring_"))) {
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
            byId.insert(id, commands.size());
            commands.append(command);
        }
        toolGroups.sort(Qt::CaseInsensitive);
        groupOrder = QStringList{tools, painting, heldTitle(), rings} + menuOrder + toolGroups + QStringList{panels, other};
    }

    /// A command of at most this many tools is theirs; more (the brush size, for every painting tool) is common
    static constexpr int ownToolsAtMost = 2;

    /// The tools whose own keys cover a key that works everywhere while they are in use: those worth a view
    void findContextTools()
    {
        contextTools.clear();
        QSet<int> everywhere;
        for (const Command &command : qAsConst(commands)) {
            if (!command.tools.isEmpty()) continue;
            Q_FOREACH (const QKeySequence &sequence, command.current) everywhere << chordOf(sequence);
        }
        everywhere.remove(0);
        QStringList ids;
        for (const Command &command : qAsConst(commands)) {
            if (command.tools.isEmpty()) continue;
            bool covers = false;
            Q_FOREACH (const QKeySequence &sequence, command.current) covers = covers || everywhere.contains(chordOf(sequence));
            if (!covers) continue;
            Q_FOREACH (const QString &id, command.tools) {
                if (!ids.contains(id)) ids << id;
            }
        }
        Q_FOREACH (const QString &id, ids) contextTools << qMakePair(id, toolName(id));
        std::sort(contextTools.begin(), contextTools.end(), [](const auto &a, const auto &b) { return a.second.localeAwareCompare(b.second) < 0; });
    }

    static QString toolName(const QString &id)
    {
        KoToolFactoryBase *factory = KoToolRegistry::instance()->value(id);
        // the name, without the help some tools add after it ("Bezier curve tool: Shift-click ends the curve")
        if (!factory) return id;
        const QString name = plainText(factory->toolTip()).section(QLatin1Char(':'), 0, 0).section(QLatin1Char('\n'), 0, 0);
        return KisSentenceCase::of(name.trimmed());
    }

    /// Whether one key on both would clash (see KisStudioShortcuts::shareContext())
    static bool clash(const Command &a, const Command &b)
    {
        return KisStudioShortcuts::shareContext(a.tools, b.tools);
    }

    /// Friendlier names for Krita's canvas input actions
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

    /// A held action's name, plus its mode unless that just repeats the action (e.g. "Pan Mode")
    static QString holdTitle(KisAbstractInputAction *action, const QString &mode)
    {
        const QString name = heldName(action->name());
        const QString first = action->name().section(QLatin1Char(' '), 0, 0);
        const bool plain = mode.isEmpty() || mode == action->name() || mode == QLatin1String("Normal")
            || mode == first + QLatin1String(" Mode") || mode == QLatin1String("Primary Mode");
        return plain ? name : name + QStringLiteral(" · ") + mode;
    }

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
            hold.title = holdTitle(action, mode);
            hold.editable = config->type() == KisShortcutConfiguration::KeyCombinationType || config->type() == KisShortcutConfiguration::MouseButtonType;
            holds.append(hold);
        }
        // Add rows for modes with no keys yet, so they can be given some; skip touch- and trackpad-only actions
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
                hold.title = holdTitle(action, it.key());
                hold.editable = true;
                holds.append(hold);
            }
        }
        std::sort(holds.begin(), holds.end(), [](const Hold &a, const Hold &b) { return a.title.localeAwareCompare(b.title) < 0; });
    }

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
        // Known groups in order, then the rest by name
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
            if (item->data(0, RowRole).isValid()) {
                const int row = item->data(0, RowRole).toInt();
                map->lit = chordsOfRow(row);
                // a tool's own command is shown on the keyboard of that tool
                if (row >= 0 && row < commands.size() && !commands[row].tools.contains(context)) {
                    Q_FOREACH (const QString &tool, commands[row].tools) {
                        if (contextBox && contextBox->findData(tool) >= 0) {
                            showContext(tool);
                            break;
                        }
                    }
                }
            }
        }
        map->update();
    }

    void showContext(const QString &tool)
    {
        context = tool;
        if (contextBox) {
            const QSignalBlocker blocker(contextBox);
            contextBox->setCurrentIndex(qMax(0, contextBox->findData(tool)));
        }
        map->update();
    }

    void fillContexts()
    {
        if (!contextBox) return;
        findContextTools();
        const QSignalBlocker blocker(contextBox);
        contextBox->clear();
        contextBox->addItem(i18nc("keys page: the keys that work everywhere, not only in one tool", "Everywhere"), QString());
        for (const auto &tool : qAsConst(contextTools)) {
            contextBox->addItem(i18nc("keys page: the keys while a tool is in use, %1 is a tool", "While using %1", tool.second), tool.first);
        }
        contextBox->setCurrentIndex(qMax(0, contextBox->findData(context)));
        contextBox->setVisible(!contextTools.isEmpty());
    }

    /**
     * What \p chord does where the keyboard is shown, then actions held on the canvas. Everywhere: the commands
     * that work everywhere, then the tools' own. While using a tool: its own first (they cover the others there),
     * then those that work everywhere. \p commandCount is above 1 only when two of them clash.
     */
    QVector<KeyUse> usesOn(int chord, int *commandCount) const
    {
        QVector<KeyUse> uses;
        *commandCount = 0;
        if (!chord) return uses;
        QVector<const Command *> mine;
        QVector<const Command *> everywhere;
        QVector<const Command *> tools;
        for (const Command &command : commands) {
            bool on = false;
            Q_FOREACH (const QKeySequence &sequence, command.current) on = on || (sequence.count() == 1 && chordOf(sequence) == chord);
            if (!on) continue;
            if (command.tools.isEmpty()) everywhere << &command;
            else if (context.isEmpty()) tools << &command;
            else if (command.tools.contains(context)) mine << &command;
        }
        auto use = [](const Command *command, bool elsewhere) {
            return KeyUse{command->title, command->action ? command->action->icon() : QIcon(), false, elsewhere};
        };
        for (const Command *command : mine) uses << use(command, false);
        for (const Command *command : everywhere) uses << use(command, !mine.isEmpty());
        for (const Command *command : tools) {
            KeyUse own = use(command, false);
            if (command->tools.size() <= ownToolsAtMost) {
                own.name = i18nc("keys page: a command and the tool it works in, e.g. Bold, while using Text tool", "%1, while using %2", own.name, toolName(command->tools.first()));
            }
            uses << own;
        }
        // red only for two that would fire in the same place
        const QVector<const Command *> all = mine + everywhere + tools;
        bool clashes = false;
        for (int a = 0; a < all.size() && !clashes; a++) {
            for (int b = a + 1; b < all.size() && !clashes; b++) clashes = clash(*all[a], *all[b]);
        }
        *commandCount = clashes ? 2 : qMin(1, int(all.size()));
        QSet<QString> heldNames;
        for (const Hold &hold : holds) {
            if (!hold.removed && chordOf(hold.staged.keys()) == chord && !heldNames.contains(hold.title)) {
                heldNames.insert(hold.title);
                uses.append({hold.title, QIcon(), true});
            }
        }
        return uses;
    }

    /// Sets \p sequence as shortcut \p index of command \p row (-1 appends), taking it from any other command
    void assignCommand(int row, int index, const QKeySequence &sequence)
    {
        Command &command = commands[row];
        QStringList takenFrom;
        const int chord = chordOf(sequence);
        if (!sequence.isEmpty()) {
            // Compare chords so "!" and Shift+1 count as the same key
            for (int i = 0; i < commands.size(); i++) {
                QList<QKeySequence> &current = commands[i].current;
                const int before = current.size();
                // a key works everywhere and also as a tool's own, which covers it while the tool is in use
                if (i != row && !clash(commands[i], command)) continue;
                for (int k = current.size() - 1; k >= 0; k--) {
                    // the edited place itself is replaced below; one before it moves it up
                    if (i == row && k == index) continue;
                    if (chordOf(current[k]) != chord) continue;
                    current.removeAt(k);
                    if (i == row && k < index) index--;
                }
                if (current.size() < before) {
                    commands[i].edited = true;
                    if (i != row) takenFrom << commands[i].title;
                }
            }
        }
        command.edited = true;
        if (index >= 0 && index < command.current.size()) {
            if (sequence.isEmpty()) command.current.removeAt(index);
            else command.current[index] = sequence;
        } else if (!sequence.isEmpty()) {
            command.current.append(sequence);
        }
        QStringList held;
        for (const Hold &hold : holds) {
            if (!sequence.isEmpty() && chordOf(hold.staged.keys()) == chord) held << hold.title;
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

    /// Reverts \p hold to the profile's entry
    static void resetHold(Hold &hold)
    {
        if (hold.live) hold.staged = *hold.live;
        else hold.staged.setKeys({});
        hold.removed = false;
        hold.changed = false;
    }

    /// Clears a hold. A hold without keys would trigger on the pen alone, so a live one is marked removed instead.
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

    /// The input that triggers a held action, as text; never empty
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
        int index;   ///< which shortcut, or -1 for the add button
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
/// Draws the key caps in the second column (see KisKeysPage::Private::capsOf)
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
    d->mainWindow = KisPart::instance()->currentMainwindow();
    KConfigGroup schemes = KSharedConfig::openConfig()->group("Shortcut Schemes");
    d->savedScheme = schemes.readEntry("Current Scheme", "Default");
    if (d->savedScheme != QLatin1String("Default") && d->schemeFile(d->savedScheme).isEmpty()) d->savedScheme = QStringLiteral("Default");
    d->scheme = d->savedScheme;
    KisInputProfile *current = KisInputProfileManager::instance()->currentProfile();
    d->savedProfile = current ? current->name() : QString();
    d->profile = d->savedProfile;
    // the profiles are made again when a window opens (or this page applies): what the holds point to is gone
    connect(KisInputProfileManager::instance(), &KisInputProfileManager::currentProfileChanged, this, [this]() {
        d->stopRecording();
        d->collectHolds();
        d->rebuildHoldItems();
    });

    // Wide enough for the keyboard; the settings panel grows to fit
    setMinimumWidth(940);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    // Search by text or keys, the changed-only filter, a new ring, and the scheme to start from
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
    d->changedOnly->setToolTip(i18nc("@info:tooltip", "Only shortcuts you changed"));
    d->changedOnly->setCheckable(true);
    bar->addWidget(d->changedOnly);
    // Creates a ring and records its key right away
    auto *newRing = new QToolButton(this);
    newRing->setText(i18nc("@action:button keys page", "New ring"));
    newRing->setToolTip(i18nc("@info:tooltip", "Tap the key to open the ring, or hold it and point to pick"));
    bar->addWidget(newRing);
    connect(newRing, &QToolButton::clicked, this, [this]() {
        KisStudioRings *rings = d->mainWindow ? d->mainWindow->studioRings() : nullptr;
        if (!rings) return;
        d->stopRecording();
        QAction *ring = rings->createRing();
        d->newRings << ring->objectName().mid(QStringLiteral("studio_ring_").size());
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
        // krita_default duplicates the built-in set; list it only when it is in use
        if (name == QLatin1String("krita_default") && d->savedScheme != name) continue;
        schemeNames << name;
    }
    Q_FOREACH (const QString &name, schemeNames) d->startFrom->addItem(d->schemeTitle(name), name);
    d->startFrom->setCurrentIndex(qMax(0, d->startFrom->findData(d->scheme)));
    d->startFrom->setToolTip(i18nc("@info:tooltip", "A ready-made set of shortcuts to begin from"));
    bar->addWidget(d->startFrom);
    layout->addLayout(bar);

    // Modifier layer buttons
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
        button->setToolTip(entry.second == Qt::NoModifier ? i18nc("@info:tooltip keyboard layer", "The keys alone")
                                                          : i18nc("@info:tooltip keyboard layer, e.g. The keys with Ctrl held", "The keys with %1 held", entry.first));
        layers->addWidget(button);
        d->layerButtons << button;
    }
    // the keys of a tool in use: its own cover the others there
    d->contextBox = new QComboBox(this);
    d->contextBox->setToolTip(i18nc("@info:tooltip", "Keys that work everywhere, or only with a tool"));
    connect(d->contextBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() {
        d->context = d->contextBox->currentData().toString();
        d->map->update();
    });
    layers->addSpacing(KisModernStyle::space3);
    layers->addWidget(d->contextBox);
    layers->addStretch();
    // Legend for the key marks
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
                // Wheels and gestures are shown but can't be edited here
                QColor text = pal.color(QPalette::Text);
                text.setAlphaF(0.55);
                pal.setColor(QPalette::Text, text);
                pal.setColor(QPalette::ButtonText, text);
                pal.setColor(QPalette::WindowText, text);
            }
            KisModernStyle::drawKeyCap(painter, cap.rect, font, cap.text, pal, state);
        }
        // A reset arrow after the caps of a changed row
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
    d->fillContexts();
    d->collectHolds();
    d->rebuildTree();

    // Keyboard interaction
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
            // A modifier was clicked: sync the layer buttons
            pickLayer(d->map->layer);
            return;
        }
        // Filter by this key; clicking it again clears the filter
        d->keyFilter = d->keyFilter == chord ? 0 : chord;
        d->search->blockSignals(true);
        d->search->setText(d->keyFilter ? KisModernStyle::keyCapText(QKeySequence(chord)) : QString());
        d->search->blockSignals(false);
        d->filter();
        int count = 0;
        if (d->keyFilter && d->usesOn(chord, &count).isEmpty()) {
            d->showNote(i18nc("@info keys page", "%1 is free", KisModernStyle::keyCapText(QKeySequence(chord))));
        }
    };
    d->map->dropped = [this](const QMimeData *mime, int chord) {
        const int row = mime->data(QString::fromLatin1(commandMime)).toInt();
        const Qt::KeyboardModifiers modifiers(chord & int(Qt::KeyboardModifierMask));
        int key = chord & ~int(Qt::KeyboardModifierMask);
        if (row >= 0 && row < d->commands.size()) {
            // A modifier alone can't be a shortcut
            if (!isModifierKey(chord)) d->assignCommand(row, -1, QKeySequence(chord));
        } else if (row < 0 && -1 - row < d->holds.size() && d->holds[-1 - row].editable) {
            // The canvas sees a shifted key as the symbol it types
            if (modifiers & Qt::ShiftModifier) key = shiftedSymbols().key(key, key);
            d->assignHold(-1 - row, heldKeys(modifiers, key));
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
        d->stopRecording();
        const QString name = d->startFrom->itemData(index).toString();
        d->scheme = name;
        d->profile = d->profileFor(name);
        int changes = 0;
        for (Private::Command &command : d->commands) {
            const QList<QKeySequence> defaults = d->defaultsOf(command, name);
            if (command.current != defaults) {
                changes++;
                command.edited = true;
            }
            command.current = defaults;
        }
        d->collectHolds();
        d->rebuildHoldItems();
        d->showNote(i18nc("@info keys page", "%2 changed from %1", d->schemeTitle(name), changes));
        d->refresh();
    });
}

KisKeysPage::~KisKeysPage()
{
    // Rings created here and never applied are discarded
    if (KisStudioRings *rings = d->mainWindow ? d->mainWindow->studioRings() : nullptr) {
        Q_FOREACH (const QString &id, d->newRings) rings->remove(id);
    }
}

void KisKeysPage::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    // Watch key events app-wide so held modifiers switch the layer
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

    // Recording: capture the next keys pressed
    if (d->recording && (type == QEvent::KeyPress || type == QEvent::KeyRelease || type == QEvent::ShortcutOverride)) {
        auto *key = static_cast<QKeyEvent *>(event);
        if (type == QEvent::ShortcutOverride) {
            event->accept();
            return true;
        }
        if (key->isAutoRepeat()) return true;
        const int code = key->key();
        if (type == QEvent::KeyPress) {
            QKeySequence sequence;
            const KisStudioShortcuts::KeyPress press = KisStudioShortcuts::recordKey(key, &sequence);
            if (press == KisStudioShortcuts::KeyPress::Modifier) {
                if (isModifierKey(code)) d->pendingModifier = code;
                return true;
            }
            d->pendingModifier = 0;
            const int row = d->recordRow;
            const int index = d->recordIndex;
            d->stopRecording();
            if (press == KisStudioShortcuts::KeyPress::Cancel) return true;
            if (row >= 0) {
                if (press == KisStudioShortcuts::KeyPress::Shortcut || index >= 0) d->assignCommand(row, index, sequence);
            } else {
                d->assignHold(-1 - row, heldKeys(key->modifiers(), code));
            }
            return true;
        }
        // A modifier released on its own: a held key may be a lone modifier
        if (type == QEvent::KeyRelease && d->pendingModifier && code == d->pendingModifier && d->recordRow < 0) {
            const int row = d->recordRow;
            const Qt::Key held = Qt::Key(d->pendingModifier);
            d->stopRecording();
            d->assignHold(-1 - row, {held});
            return true;
        }
        return true;
    }

    // "Press keys" search: filter by the key pressed in the field
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

    // Show the layer of modifiers held anywhere, while they are held
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

    // In the list: click a cap to record, right-click to clear, click the arrow to reset
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
            // The reset arrow
            const QRectF back(option.rect.right() - 24, option.rect.center().y() - 10, 20, 20);
            const bool isChanged = row >= 0 ? d->changed(d->commands[row]) : d->holds[-1 - row].changed;
            if (isChanged && back.contains(mouse->pos()) && mouse->button() == Qt::LeftButton) {
                d->stopRecording();
                if (row >= 0) {
                    d->commands[row].current = d->defaultsOf(d->commands[row], d->scheme);
                    d->commands[row].edited = true;
                } else {
                    Private::resetHold(d->holds[-1 - row]);
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

void KisKeysPage::apply()
{
    d->stopRecording();
    d->newRings.clear();
    KisActionRegistry *registry = KisActionRegistry::instance();

    // The scheme started from
    if (d->scheme != d->savedScheme) {
        KConfigGroup schemes = KSharedConfig::openConfig()->group("Shortcut Schemes");
        schemes.writeEntry("Current Scheme", d->scheme);
        schemes.sync();
        registry->loadShortcutScheme(d->scheme);
    }

    // Store only the shortcuts that differ from the scheme
    KConfigGroup custom(KSharedConfig::openConfig(KisProfile::configFile(QStringLiteral("kritashortcutsrc"))), QStringLiteral("Shortcuts"));
    for (const Private::Command &command : d->commands) {
        if (!command.edited) continue;
        if (command.registered && command.current == d->defaultsOf(command, d->scheme)) {
            custom.deleteEntry(command.id);
        } else if (!command.registered && command.action && command.current == command.action->shortcuts()) {
            continue;
        } else {
            custom.writeEntry(command.id, command.current.isEmpty() ? QStringLiteral("none")
                                                                    : QKeySequence::listToString(command.current, QKeySequence::PortableText));
        }
        if (command.action) command.action->setShortcuts(command.current);
        for (const QPointer<QAction> &copy : command.copies) {
            if (copy) copy->setShortcuts(command.current);
        }
    }
    custom.sync();
    for (Private::Command &command : d->commands) command.edited = false;
    registry->settingsPageSaved();

    // Keys held on the canvas
    KisInputProfileManager *input = KisInputProfileManager::instance();
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
    // Reload so the canvases rebuild their shortcuts from the profile
    KisConfig(false).setCurrentInputProfile(d->profile);
    input->loadProfiles();
}
