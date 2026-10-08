/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioItemCard.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QDrag>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QTimer>
#include <QVBoxLayout>

#include <KSharedConfig>
#include <kactioncollection.h>
#include <kconfiggroup.h>
#include <kis_action_registry.h>
#include <klocalizedstring.h>
#include <kxmlguiclient.h>
#include <kxmlguifactory.h>

#include "KisMainWindow.h"
#include "widgets/KisPopupCloser.h"

namespace {

constexpr int shadowMargin = 14;

QColor mix(const QColor &a, const QColor &b, qreal t)
{
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t, a.greenF() + (b.greenF() - a.greenF()) * t,
                            a.blueF() + (b.blueF() - a.blueF()) * t);
}

QString cleanText(QString text)
{
    text.remove(QLatin1Char('&'));
    if (text.endsWith(QStringLiteral("..."))) text.chop(3);
    return text.trimmed();
}

QList<QAction *> allActions(KisMainWindow *mainWindow)
{
    // the collections, and actions added to the window directly (rail items, panels)
    QList<QAction *> actions = mainWindow->actionCollection()->actions() + mainWindow->actions();
    if (mainWindow->guiFactory()) {
        Q_FOREACH (KisKXMLGUIClient *client, mainWindow->guiFactory()->clients()) {
            if (client && client->actionCollection()) {
                Q_FOREACH (QAction *action, client->actionCollection()->actions()) {
                    if (!actions.contains(action)) actions << action;
                }
            }
        }
    }
    return actions;
}

/// The shortcut as a key cap; a click records a new one
class KeyCap : public QAbstractButton
{
public:
    explicit KeyCap(QWidget *parent) : QAbstractButton(parent)
    {
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::NoFocus);
        setAttribute(Qt::WA_Hover);
    }

    void setRecording(bool recording)
    {
        m_recording = recording;
        updateGeometry();
        update();
    }

    QSize sizeHint() const override
    {
        const QString shown = m_recording ? QStringLiteral("Ctrl ⇧ W") : (text().isEmpty() ? QStringLiteral("+") : text());
        return QSize(qMax(36, fontMetrics().horizontalAdvance(shown) + 22), 30);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QPalette &pal = palette();
        const QColor window = pal.color(QPalette::Window);
        const QColor ink = pal.color(QPalette::ButtonText);
        const QRectF cap = QRectF(rect()).adjusted(1, 1, -1, -3);
        // a key: a top face and a darker edge below it
        painter.setPen(Qt::NoPen);
        painter.setBrush(mix(window, Qt::black, 0.35));
        painter.drawRoundedRect(cap.translated(0, 2), 6, 6);
        painter.setBrush(mix(window, ink, underMouse() ? 0.22 : 0.15));
        painter.drawRoundedRect(cap, 6, 6);
        if (m_recording) {
            painter.setPen(QPen(pal.color(QPalette::Link), 1.5));
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(cap.adjusted(0.75, 0.75, -0.75, -0.75), 6, 6);
        }
        QColor textColor = ink;
        if (!m_recording && text().isEmpty()) textColor.setAlphaF(0.5);
        painter.setPen(textColor);
        const QString shown = m_recording ? QStringLiteral("…") : (text().isEmpty() ? QStringLiteral("+") : text());
        painter.drawText(cap, Qt::AlignCenter, shown);
    }

private:
    bool m_recording {false};
};

/// A variant in the card: click picks it, drag takes it out as an item, right click hides it
class ChoiceTile : public QAbstractButton
{
public:
    ChoiceTile(const KisStudioItemCard::Choice &choice, QWidget *parent)
        : QAbstractButton(parent), m_choice(choice)
    {
        setFixedSize(40, 40);
        setToolTip(choice.title);
        setFocusPolicy(Qt::NoFocus);
        setAttribute(Qt::WA_Hover);
    }

    std::function<void()> dragged;
    std::function<void()> dropped;

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QPalette &pal = palette();
        if (m_choice.current || underMouse()) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(m_choice.current ? mix(pal.color(QPalette::Window), pal.color(QPalette::Highlight), 0.55)
                                              : mix(pal.color(QPalette::Window), pal.color(QPalette::ButtonText), 0.1));
            painter.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 8, 8);
        }
        m_choice.icon.paint(&painter, QRect(9, 9, 22, 22));
        if (m_choice.action && !m_choice.action->shortcut().isEmpty()) {
            KisStudioShortcuts::paintBadge(painter, rect(), KisStudioShortcuts::badgeText(m_choice.action->shortcut()), pal);
        }
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::RightButton && m_choice.hide) {
            m_choice.hide();
            return;
        }
        m_pressPos = event->pos();
        QAbstractButton::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if ((event->buttons() & Qt::LeftButton) && !m_choice.key.isEmpty()
            && (event->pos() - m_pressPos).manhattanLength() > QApplication::startDragDistance()) {
            setDown(false);
            auto *drag = new QDrag(this);
            auto *mime = new QMimeData();
            mime->setData(KisStudioShortcuts::itemMimeType(), m_choice.key.toUtf8());
            drag->setMimeData(mime);
            drag->setPixmap(m_choice.icon.pixmap(32, 32));
            if (dragged) dragged();
            drag->exec(Qt::CopyAction);
            if (dropped) dropped();
            return;
        }
        QAbstractButton::mouseMoveEvent(event);
    }

private:
    KisStudioItemCard::Choice m_choice;
    QPoint m_pressPos;
};

} // namespace

QString KisStudioShortcuts::itemMimeType()
{
    return QStringLiteral("application/x-krita-studio-item");
}

QString KisStudioShortcuts::badgeText(const QKeySequence &sequence)
{
    if (sequence.isEmpty()) {
        return QString();
    }
    QString text = sequence.toString(QKeySequence::NativeText).section(QLatin1String(", "), 0, 0);
    text.replace(QStringLiteral("Shift+"), QStringLiteral("⇧"));
    text.replace(QStringLiteral("Ctrl+"), QStringLiteral("Ctrl "));
    text.replace(QStringLiteral("Alt+"), QStringLiteral("Alt "));
    text.replace(QStringLiteral("Meta+"), QStringLiteral("Win "));
    return text;
}

void KisStudioShortcuts::paintBadge(QPainter &painter, const QRect &rect, const QString &text, const QPalette &palette)
{
    if (text.isEmpty()) {
        return;
    }
    painter.save();
    QFont font = painter.font();
    font.setPointSizeF(qMax(6.0, font.pointSizeF() * 0.62));
    font.setBold(true);
    painter.setFont(font);
    const QFontMetrics metrics(font);
    const int width = metrics.horizontalAdvance(text) + 5;
    const int height = metrics.height();
    const QRectF badge(rect.right() - width - 1, rect.bottom() - height - 1, width, height);
    QColor background = palette.color(QPalette::Window);
    background.setAlphaF(0.85);
    painter.setPen(Qt::NoPen);
    painter.setBrush(background);
    painter.drawRoundedRect(badge, 3, 3);
    QColor ink = palette.color(QPalette::ButtonText);
    ink.setAlphaF(0.72);
    painter.setPen(ink);
    painter.drawText(badge, Qt::AlignCenter, text);
    painter.restore();
}

QString KisStudioShortcuts::assign(KisMainWindow *mainWindow, QAction *action, const QKeySequence &sequence)
{
    KConfigGroup group(KSharedConfig::openConfig(QStringLiteral("kritashortcutsrc")), QStringLiteral("Shortcuts"));
    QString takenFrom;
    if (!sequence.isEmpty()) {
        Q_FOREACH (QAction *other, allActions(mainWindow)) {
            if (other == action || !other->shortcuts().contains(sequence)) {
                continue;
            }
            QList<QKeySequence> remaining = other->shortcuts();
            remaining.removeAll(sequence);
            other->setShortcuts(remaining);
            if (!other->objectName().isEmpty()) {
                group.writeEntry(other->objectName(), remaining.isEmpty() ? QStringLiteral("none") : QKeySequence::listToString(remaining));
            }
            takenFrom = cleanText(other->text());
        }
    }
    action->setShortcut(sequence);
    if (!action->objectName().isEmpty()) {
        group.writeEntry(action->objectName(), sequence.isEmpty() ? QStringLiteral("none") : sequence.toString(QKeySequence::PortableText));
    }
    group.sync();
    KisActionRegistry::instance()->loadCustomShortcuts();
    return takenFrom;
}

struct KisStudioItemCard::Private
{
    KisMainWindow *mainWindow {nullptr};
    QPointer<QAction> action;
    KeyCap *keyCap {nullptr};
    QLabel *note {nullptr};
    QHBoxLayout *choices {nullptr};
    QHBoxLayout *commands {nullptr};
    bool recording {false};
};

KisStudioItemCard::KisStudioItemCard(KisMainWindow *mainWindow, const QIcon &icon, const QString &title, QAction *shortcutAction)
    // not Qt::Popup: translucent popups render a black background on Windows
    : QFrame(mainWindow, Qt::Tool | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint)
    , d(new Private)
{
    d->mainWindow = mainWindow;
    d->action = shortcutAction;
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_DeleteOnClose);
    new KisPopupCloser(this);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(shadowMargin + 12, shadowMargin + 10, shadowMargin + 12, shadowMargin + 10);
    layout->setSpacing(10);

    auto *header = new QHBoxLayout();
    header->setSpacing(10);
    auto *iconLabel = new QLabel(this);
    iconLabel->setPixmap(icon.pixmap(QSize(28, 28)));
    header->addWidget(iconLabel);
    auto *titleLabel = new QLabel(title, this);
    QFont titleFont = titleLabel->font();
    titleFont.setPointSizeF(titleFont.pointSizeF() * 1.08);
    titleFont.setWeight(QFont::DemiBold);
    titleLabel->setFont(titleFont);
    header->addWidget(titleLabel, 1);

    d->keyCap = new KeyCap(this);
    d->keyCap->setVisible(shortcutAction);
    d->keyCap->setToolTip(i18nc("@info:tooltip", "Click, then press the new shortcut. Backspace removes it."));
    connect(d->keyCap, &QAbstractButton::clicked, this, &KisStudioItemCard::startRecording);
    header->addWidget(d->keyCap);
    layout->addLayout(header);

    d->choices = new QHBoxLayout();
    d->choices->setSpacing(2);
    layout->addLayout(d->choices);

    d->commands = new QHBoxLayout();
    d->commands->setSpacing(4);
    d->commands->addStretch(1);
    layout->addLayout(d->commands);

    d->note = new QLabel(this);
    QPalette notePalette = d->note->palette();
    QColor dim = notePalette.color(QPalette::WindowText);
    dim.setAlphaF(0.6);
    notePalette.setColor(QPalette::WindowText, dim);
    d->note->setPalette(notePalette);
    d->note->hide();
    layout->addWidget(d->note);

    updateKeyCap();
}

KisStudioItemCard::~KisStudioItemCard()
{
    if (d->recording) {
        releaseKeyboard();
    }
}

void KisStudioItemCard::setChoices(const QVector<Choice> &choices)
{
    Q_FOREACH (const Choice &choice, choices) {
        auto *tile = new ChoiceTile(choice, this);
        connect(tile, &QAbstractButton::clicked, this, [this, pick = choice.pick]() {
            if (pick) pick();
            close();
        });
        // dragged out of the card: it gets out of the way of the drop, then closes
        tile->dragged = [this]() { hide(); };
        tile->dropped = [this]() { close(); };
        d->choices->addWidget(tile);
    }
    d->choices->addStretch(1);
}

void KisStudioItemCard::addCommand(const Command &command)
{
    class CommandButton : public QAbstractButton
    {
    public:
        using QAbstractButton::QAbstractButton;
        QSize sizeHint() const override { return QSize(34, 34); }

    protected:
        void paintEvent(QPaintEvent *) override
        {
            QPainter painter(this);
            painter.setRenderHint(QPainter::Antialiasing, true);
            const QPalette &pal = palette();
            if (isChecked() || underMouse()) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(isChecked() ? mix(pal.color(QPalette::Window), pal.color(QPalette::Highlight), 0.55)
                                             : mix(pal.color(QPalette::Window), pal.color(QPalette::ButtonText), 0.1));
                painter.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 8, 8);
            }
            icon().paint(&painter, QRect((width() - 20) / 2, (height() - 20) / 2, 20, 20));
        }
    };

    auto *commandButton = new CommandButton(this);
    commandButton->setAttribute(Qt::WA_Hover);
    commandButton->setFocusPolicy(Qt::NoFocus);
    commandButton->setIcon(command.icon);
    commandButton->setToolTip(command.toolTip);
    commandButton->setCheckable(command.checkable);
    commandButton->setChecked(command.checked);
    connect(commandButton, &QAbstractButton::clicked, this, [this, run = command.run]() {
        if (run) run();
        close();
    });
    d->commands->addWidget(commandButton);
}

void KisStudioItemCard::popupBeside(QWidget *anchor)
{
    adjustSize();
    const QRect anchorRect(anchor->mapToGlobal(QPoint(0, 0)), anchor->size());
    QScreen *screen = anchor->screen();
    const QRect available = screen ? screen->availableGeometry() : QRect(0, 0, 1920, 1080);
    const bool leftHalf = anchorRect.center().x() < available.center().x();
    QPoint pos(leftHalf ? anchorRect.right() + 4 - shadowMargin + 6 : anchorRect.left() - width() + shadowMargin - 6,
               anchorRect.top() - shadowMargin);
    pos.setX(qBound(available.left(), pos.x(), available.right() - width()));
    pos.setY(qBound(available.top(), pos.y(), available.bottom() - height()));
    move(pos);
    show();
    raise();
    activateWindow();
}

void KisStudioItemCard::updateKeyCap()
{
    if (d->action) {
        d->keyCap->setText(KisStudioShortcuts::badgeText(d->action->shortcut()));
        d->keyCap->updateGeometry();
    }
}

void KisStudioItemCard::startRecording()
{
    if (!d->action) {
        return;
    }
    d->recording = true;
    d->keyCap->setRecording(true);
    d->note->setText(i18nc("@info", "Press the new shortcut · Backspace removes it · Esc keeps it"));
    d->note->show();
    adjustSize();
    activateWindow();
    setFocus();
    // the shortcuts of the window must not fire while recording one
    grabKeyboard();
}

void KisStudioItemCard::finishRecording(const QKeySequence &sequence, bool clear)
{
    releaseKeyboard();
    d->recording = false;
    d->keyCap->setRecording(false);
    if (d->action && (clear || !sequence.isEmpty())) {
        const QString taken = KisStudioShortcuts::assign(d->mainWindow, d->action, clear ? QKeySequence() : sequence);
        if (!taken.isEmpty()) {
            d->note->setText(i18nc("@info", "Taken from “%1”", taken));
            d->note->show();
            QTimer::singleShot(2200, d->note, &QWidget::hide);
        } else {
            d->note->hide();
        }
    } else {
        d->note->hide();
    }
    updateKeyCap();
    adjustSize();
}

void KisStudioItemCard::keyPressEvent(QKeyEvent *event)
{
    if (!d->recording) {
        if (event->key() == Qt::Key_Escape) {
            close();
            return;
        }
        QFrame::keyPressEvent(event);
        return;
    }
    const int key = event->key();
    if (key == Qt::Key_Shift || key == Qt::Key_Control || key == Qt::Key_Alt || key == Qt::Key_Meta
        || key == Qt::Key_AltGr || key == 0 || key == Qt::Key_unknown) {
        return; // a modifier alone is not a shortcut yet
    }
    if (key == Qt::Key_Escape) {
        finishRecording(QKeySequence(), false);
        return;
    }
    if ((key == Qt::Key_Backspace || key == Qt::Key_Delete) && event->modifiers() == Qt::NoModifier) {
        finishRecording(QKeySequence(), true);
        return;
    }
    const Qt::KeyboardModifiers modifiers = event->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
    finishRecording(QKeySequence(int(modifiers) | key), false);
}

void KisStudioItemCard::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QRectF panel = QRectF(rect()).adjusted(shadowMargin, shadowMargin, -shadowMargin, -shadowMargin);
    const qreal radius = 12;
    for (int i = shadowMargin; i > 0; i -= 2) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0, 0, 0, int(30.0 * (1.0 - qreal(i) / shadowMargin) + 2)));
        painter.drawRoundedRect(panel.adjusted(-i, -i + 3, i, i + 3), radius + i, radius + i);
    }
    QColor border = palette().color(QPalette::WindowText);
    border.setAlphaF(0.12);
    painter.setPen(QPen(border, 1));
    painter.setBrush(palette().color(QPalette::Window));
    painter.drawRoundedRect(panel, radius, radius);
}
