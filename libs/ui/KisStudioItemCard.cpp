/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioItemCard.h"
#include "KisStudioGlass.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QDrag>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QScreen>
#include <QStyle>
#include <QStyleOption>
#include <QTimer>
#include <QToolTip>
#include <QVBoxLayout>

#include <KSharedConfig>
#include <kactioncollection.h>
#include <kconfiggroup.h>
#include <kis_action_registry.h>
#include <klocalizedstring.h>
#include <kxmlguiclient.h>
#include <kxmlguifactory.h>

#include "KisMainWindow.h"
#include "KisPart.h"
#include "KisStudioMotion.h"
#include "widgets/KisPopupCloser.h"
#include <KisModernStyle.h>

namespace {

constexpr int glassShadowMargin = 14;

/// No shadow on a solid window (see KisStudioGlass::seeThroughWindows)
int shadowMarginOf(const QWidget *window)
{
    return window->testAttribute(Qt::WA_TranslucentBackground) ? glassShadowMargin : 0;
}

QColor mix(const QColor &a, const QColor &b, qreal t)
{
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t, a.greenF() + (b.greenF() - a.greenF()) * t,
                            a.blueF() + (b.blueF() - a.blueF()) * t);
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
        using KeyCap = KisModernStyle::KeyCap;
        const KeyCap state = m_recording ? KeyCap::Recording : text().isEmpty() ? KeyCap::Add : underMouse() ? KeyCap::Pointed : KeyCap::Shown;
        KisModernStyle::drawKeyCap(&painter, QRectF(rect()).adjusted(1, 1, -1, -2), font(), text(), palette(), state);
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
            painter.drawPath(KisModernStyle::squircle(QRectF(rect()).adjusted(1, 1, -1, -1), 8));
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

/**
 * Every menu's shortcuts can be changed where they are shown: a click on a
 * key cap (or the "+" of an entry without one) records the next keys
 * pressed; the menu stays open.
 */
class MenuShortcutEditor : public QObject
{
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        QMenu *menu = qobject_cast<QMenu *>(watched);
        if (!menu) {
            return false;
        }
        switch (event->type()) {
        case QEvent::MouseButtonPress: {
            auto *mouse = static_cast<QMouseEvent *>(event);
            QAction *entry = menu->actionAt(mouse->pos());
            QAction *target = KisModernStyle::shortcutTarget(entry);
            stop(menu);
            if (mouse->button() != Qt::LeftButton || !target || !entry->isEnabled()) {
                return false;
            }
            const QString shortcut = KisModernStyle::keyCapText(target->shortcut());
            const QRectF cap = KisModernStyle::keyCapRect(QRectF(menu->actionGeometry(entry)).adjusted(0, 0, -10, 0),
                                                          KisModernStyle::keyCapFont(menu->font()), shortcut,
                                                          shortcut.isEmpty() ? KisModernStyle::KeyCap::Add : KisModernStyle::KeyCap::Pointed);
            if (!cap.adjusted(-4, -4, 4, 4).contains(mouse->pos())) {
                return false;
            }
            menu->setProperty("kisRecordingShortcut", QVariant::fromValue<QObject *>(entry));
            m_swallowRelease = menu;
            menu->update();
            return true;
        }
        case QEvent::MouseButtonRelease:
            if (m_swallowRelease == menu) {
                m_swallowRelease = nullptr;
                return true;
            }
            return false;
        case QEvent::ShortcutOverride:
            if (recording(menu)) {
                event->accept();
                return true;
            }
            return false;
        case QEvent::KeyPress:
            if (recording(menu)) {
                record(menu, static_cast<QKeyEvent *>(event));
                return true;
            }
            return false;
        case QEvent::Hide:
            stop(menu);
            return false;
        default:
            return false;
        }
    }

private:
    static QAction *recording(QMenu *menu)
    {
        return qobject_cast<QAction *>(menu->property("kisRecordingShortcut").value<QObject *>());
    }

    static void stop(QMenu *menu)
    {
        if (recording(menu)) {
            menu->setProperty("kisRecordingShortcut", QVariant());
            menu->update();
        }
    }

    void record(QMenu *menu, QKeyEvent *event)
    {
        QAction *entry = recording(menu);
        QAction *target = KisModernStyle::shortcutTarget(entry);
        QKeySequence sequence;
        const KisStudioShortcuts::KeyPress press = KisStudioShortcuts::recordKey(event, &sequence);
        if (press == KisStudioShortcuts::KeyPress::Modifier) {
            return;
        }
        stop(menu);
        if (!target || press == KisStudioShortcuts::KeyPress::Cancel) {
            return;
        }
        const QString taken = KisStudioShortcuts::assign(KisPart::instance()->currentMainwindow(), target, sequence);
        if (entry != target) {
            // an entry standing in for the action: its shortcut is in its text
            const QString label = entry->text().section(QLatin1Char('\t'), 0, 0);
            entry->setText(sequence.isEmpty() ? label : label + QLatin1Char('\t') + sequence.toString(QKeySequence::NativeText));
        }
        // a longer key cap makes the menu wider: kept on the screen
        QTimer::singleShot(0, menu, [menu]() {
            menu->adjustSize();
            if (QScreen *screen = menu->screen()) {
                const QRect area = screen->availableGeometry();
                QRect geometry = menu->geometry();
                if (geometry.right() > area.right()) geometry.moveRight(area.right());
                if (geometry.left() < area.left()) geometry.moveLeft(area.left());
                menu->move(geometry.topLeft());
            }
        });
        menu->update();
        if (!taken.isEmpty()) {
            const QRect row = menu->actionGeometry(entry);
            QToolTip::showText(menu->mapToGlobal(QPoint(row.right() - 20, row.bottom())), i18nc("@info", "Taken from “%1”", taken), menu);
        }
    }

    QPointer<QMenu> m_swallowRelease;
};

} // namespace

QString KisStudioShortcuts::itemMimeType()
{
    return QStringLiteral("application/x-krita-studio-item");
}

QString KisStudioShortcuts::badgeText(const QKeySequence &sequence)
{
    return KisModernStyle::keyCapText(sequence);
}

void KisStudioShortcuts::paintBadge(QPainter &painter, const QRect &rect, const QString &text, const QPalette &palette)
{
    if (text.isEmpty()) {
        return;
    }
    QFont font = painter.font();
    font.setPointSizeF(qMax(6.0, font.pointSizeF() * 0.62));
    font.setBold(true);
    const QFontMetricsF metrics(font);
    const qreal height = metrics.height() + 2.5;
    const qreal width = qMax(height, metrics.horizontalAdvance(text) + 6);
    KisModernStyle::drawKeyCap(&painter, QRectF(rect.right() - width, rect.bottom() - height, width, height), font, text, palette);
}

QList<QAction *> KisStudioShortcuts::allActions(KisMainWindow *mainWindow)
{
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

QString KisStudioShortcuts::plainText(QString text)
{
    text.remove(QLatin1Char('&'));
    text = text.trimmed();
    while (text.endsWith(QLatin1Char('.')) || text.endsWith(QChar(0x2026))) {
        text.chop(1);
    }
    return text.trimmed();
}

bool KisStudioShortcuts::isModifierKey(int key)
{
    return key == Qt::Key_Control || key == Qt::Key_Shift || key == Qt::Key_Alt || key == Qt::Key_Meta || key == Qt::Key_AltGr;
}

KisStudioShortcuts::KeyPress KisStudioShortcuts::recordKey(const QKeyEvent *event, QKeySequence *sequence)
{
    *sequence = QKeySequence();
    int key = event->key();
    if (isModifierKey(key) || key == 0 || key == Qt::Key_unknown) {
        return KeyPress::Modifier;
    }
    Qt::KeyboardModifiers modifiers = event->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
    if (key == Qt::Key_Escape) {
        return KeyPress::Cancel;
    }
    if ((key == Qt::Key_Backspace || key == Qt::Key_Delete) && !modifiers) {
        return KeyPress::Clear;
    }
    if (key == Qt::Key_Backtab) {
        key = Qt::Key_Tab;
    }
    const bool symbol = key > Qt::Key_Space && key < Qt::Key_Escape && !QChar::isLetter(uint(key));
    if (symbol && !(event->modifiers() & Qt::KeypadModifier)) {
        modifiers &= ~Qt::ShiftModifier;
    }
    *sequence = QKeySequence(int(modifiers) | key);
    return KeyPress::Shortcut;
}

void KisStudioShortcuts::enableMenuEditing()
{
    static MenuShortcutEditor *editor = nullptr;
    if (editor) return;
    editor = new MenuShortcutEditor(qApp);
    qApp->installEventFilter(editor);
    // for the style: menus draw the "+" of entries that can be given a shortcut
    qApp->setProperty("kisMenuShortcutsEditable", true);
}

void KisStudioShortcuts::restore(QAction *action)
{
    const QString saved = KConfigGroup(KSharedConfig::openConfig(QStringLiteral("kritashortcutsrc")), QStringLiteral("Shortcuts"))
                              .readEntry(action->objectName(), QString());
    if (!saved.isEmpty() && saved != QLatin1String("none")) {
        action->setShortcuts(QKeySequence::listFromString(saved));
    }
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
            takenFrom = plainText(other->text());
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
    QVBoxLayout *commands {nullptr};
    bool recording {false};
};

KisStudioItemCard::KisStudioItemCard(KisMainWindow *mainWindow, const QIcon &icon, const QString &title, QAction *shortcutAction)
    // on Windows not Qt::Popup: translucent popups render a black background there (see KisStudioGlass::popupWindowType)
    : QFrame(mainWindow, KisStudioGlass::popupWindowType() | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint)
    , d(new Private)
{
    d->mainWindow = mainWindow;
    d->action = shortcutAction;
    setAttribute(Qt::WA_TranslucentBackground, KisStudioGlass::seeThroughWindows());
    setAttribute(Qt::WA_DeleteOnClose);
    new KisPopupCloser(this);

    auto *layout = new QVBoxLayout(this);
    const int shadowMargin = shadowMarginOf(this);
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
    d->keyCap->setToolTip(i18nc("@info:tooltip", "Click to set shortcut"));
    connect(d->keyCap, &QAbstractButton::clicked, this, &KisStudioItemCard::startRecording);
    header->addWidget(d->keyCap);
    layout->addLayout(header);

    d->choices = new QHBoxLayout();
    d->choices->setSpacing(2);
    layout->addLayout(d->choices);

    d->commands = new QVBoxLayout();
    d->commands->setSpacing(0);
    d->commands->setContentsMargins(0, 0, 0, 0);
    layout->addLayout(d->commands);

    d->note = new QLabel(this);
    QPalette notePalette = d->note->palette();
    notePalette.setColor(QPalette::WindowText, KisModernStyle::secondaryInk(notePalette.color(QPalette::WindowText)));
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
        // dragged out: out of the way of the drop, then closed
        tile->dragged = [this]() { hide(); };
        tile->dropped = [this]() { close(); };
        d->choices->addWidget(tile);
    }
    d->choices->addStretch(1);
}

void KisStudioItemCard::addCommand(const Command &command)
{
    /// A row drawn as a menu entry is; its tooltip's words after " (" are only its tooltip
    class CommandRow : public QAbstractButton
    {
    public:
        using QAbstractButton::QAbstractButton;

        QSize sizeHint() const override
        {
            QStyleOptionMenuItem option = menuOption();
            const QSize text(fontMetrics().horizontalAdvance(option.text), fontMetrics().height());
            return style()->sizeFromContents(QStyle::CT_MenuItem, &option, text, this).expandedTo(QSize(0, 30));
        }

    protected:
        QStyleOptionMenuItem menuOption() const
        {
            QStyleOptionMenuItem option;
            option.initFrom(this);
            option.rect = rect();
            option.text = text();
            option.icon = icon();
            option.maxIconWidth = 20;
            option.menuItemType = QStyleOptionMenuItem::Normal;
            option.checkType = isCheckable() ? QStyleOptionMenuItem::NonExclusive : QStyleOptionMenuItem::NotCheckable;
            option.checked = isChecked();
            option.menuHasCheckableItems = isCheckable();
            option.font = font();
            option.state.setFlag(QStyle::State_Selected, underMouse());
            return option;
        }

        void paintEvent(QPaintEvent *) override
        {
            QPainter painter(this);
            const QStyleOptionMenuItem option = menuOption();
            style()->drawControl(QStyle::CE_MenuItem, &option, &painter, this);
        }
    };

    auto *row = new CommandRow(this);
    row->setAttribute(Qt::WA_Hover);
    row->setFocusPolicy(Qt::NoFocus);
    row->setIcon(command.icon);
    const QString name = command.toolTip.section(QStringLiteral(" ("), 0, 0);
    row->setText(name);
    if (name != command.toolTip) row->setToolTip(command.toolTip);
    row->setCheckable(command.checkable);
    row->setChecked(command.checked);
    connect(row, &QAbstractButton::clicked, this, [this, run = command.run]() {
        if (run) run();
        close();
    });
    d->commands->addWidget(row);
}

void KisStudioItemCard::popupBeside(QWidget *anchor)
{
    adjustSize();
    const QRect anchorRect(anchor->mapToGlobal(QPoint(0, 0)), anchor->size());
    QScreen *screen = anchor->screen();
    const QRect available = screen ? screen->availableGeometry() : QRect(0, 0, 1920, 1080);
    const bool leftHalf = anchorRect.center().x() < available.center().x();
    const int shadowMargin = shadowMarginOf(this);
    QPoint pos(leftHalf ? anchorRect.right() + 4 - shadowMargin + 6 : anchorRect.left() - width() + shadowMargin - 6,
               anchorRect.top() - shadowMargin);
    pos.setX(qBound(available.left(), pos.x(), available.right() - width()));
    pos.setY(qBound(available.top(), pos.y(), available.bottom() - height()));
    move(pos);
    KisStudioMotion::showFaded(this);
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
    d->note->setText(i18nc("@info", "Press keys · Backspace clears · Esc cancels"));
    d->note->show();
    adjustSize();
    activateWindow();
    setFocus();
    // the window's shortcuts must not fire while recording one
    grabKeyboard();
}

void KisStudioItemCard::finishRecording(bool assign, const QKeySequence &sequence)
{
    releaseKeyboard();
    d->recording = false;
    d->keyCap->setRecording(false);
    d->note->hide();
    if (assign && d->action) {
        const QString taken = KisStudioShortcuts::assign(d->mainWindow, d->action, sequence);
        if (!taken.isEmpty()) {
            d->note->setText(i18nc("@info", "Taken from “%1”", taken));
            d->note->show();
            QTimer::singleShot(2200, d->note, &QWidget::hide);
        }
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
    QKeySequence sequence;
    switch (KisStudioShortcuts::recordKey(event, &sequence)) {
    case KisStudioShortcuts::KeyPress::Modifier:
        break;
    case KisStudioShortcuts::KeyPress::Cancel:
        finishRecording(false);
        break;
    case KisStudioShortcuts::KeyPress::Clear:
    case KisStudioShortcuts::KeyPress::Shortcut:
        finishRecording(true, sequence);
        break;
    }
}

void KisStudioItemCard::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const int shadowMargin = shadowMarginOf(this);
    const QRectF panel = QRectF(rect()).adjusted(shadowMargin, shadowMargin, -shadowMargin, -shadowMargin);
    const qreal radius = 12;
    if (shadowMargin) KisStudioGlass::paintShadow(painter, panel, radius, shadowMargin, 30, 3);
    const QPainterPath shape = KisStudioGlass::paint(painter, this, panel.adjusted(0.5, 0.5, -0.5, -0.5), radius);
    QColor border = palette().color(QPalette::WindowText);
    border.setAlphaF(0.12);
    KisStudioGlass::paintBorder(painter, this, shape, border);
}
