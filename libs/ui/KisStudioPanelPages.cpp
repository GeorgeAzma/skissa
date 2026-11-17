/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioPanelPages.h"

#include <QAbstractItemView>
#include <QAction>
#include <QCoreApplication>
#include <QBoxLayout>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QLabel>
#include <QListView>
#include <QCursor>
#include <QContextMenuEvent>
#include <QMenu>
#include <QSet>
#include <QSharedPointer>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QSpinBox>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QToolButton>

#include <kactioncollection.h>
#include <klocalizedstring.h>

#include <KisModernStyle.h>
#include <KisSentenceCase.h>
#include <KoColor.h>
#include <KoColorSpaceRegistry.h>
#include <kis_color_button.h>
#include <kis_icon_utils.h>

#include "KisMainWindow.h"
#include "KisDocument.h"
#include "KisPart.h"
#include "KisView.h"
#include "kundo2stack.h"
#include "KisStudioFind.h"
#include "KisStudioRecipes.h"
#include "widgets/KisStudioHistoryScrubber.h"
#include "widgets/KisStudioInlineEdit.h"
#include "widgets/KisStudioColorField.h"
#include "widgets/KisStudioValueRow.h"

namespace
{

/// Keeps a control of Krita's panel out of sight while the page is there, also when the panel shows it again
class Kept : public QObject
{
public:
    explicit Kept(QWidget *widget)
        : QObject(widget)
        , m_widget(widget)
        , m_wasHidden(widget->isHidden())
    {
        widget->installEventFilter(this);
        widget->hide();
    }

    void giveBack()
    {
        m_widget->removeEventFilter(this);
        m_widget->setVisible(!m_wasHidden);
        deleteLater();
    }

    bool eventFilter(QObject *, QEvent *event) override
    {
        if (event->type() == QEvent::ShowToParent) {
            // the panel showing it means it would be shown without the page
            m_wasHidden = false;
            QPointer<QWidget> widget = m_widget;
            QTimer::singleShot(0, m_widget, [widget]() {
                if (widget) widget->hide();
            });
        }
        return false;
    }

private:
    QWidget *m_widget;
    bool m_wasHidden;
};

qreal valueOf(QAbstractSpinBox *spin)
{
    if (auto *whole = qobject_cast<QSpinBox *>(spin)) return whole->value();
    if (auto *real = qobject_cast<QDoubleSpinBox *>(spin)) return real->value();
    return 0;
}

void setValueOf(QAbstractSpinBox *spin, qreal value)
{
    if (auto *whole = qobject_cast<QSpinBox *>(spin)) whole->setValue(qRound(value));
    else if (auto *real = qobject_cast<QDoubleSpinBox *>(spin)) real->setValue(value);
}

/// The page: what it added and what it hid, to be undone together
class Page : public QWidget
{
public:
    explicit Page(QWidget *parent)
        : QWidget(parent)
    {
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(KisModernStyle::space1 + KisModernStyle::hairline);
    }

    void keep(QWidget *widget)
    {
        if (widget) m_kept << new Kept(widget);
    }

    std::function<void()> undo()
    {
        QPointer<Page> page = this;
        QList<QPointer<Kept>> kept;
        for (Kept *one : m_kept) kept << one;
        return [page, kept]() {
            for (const QPointer<Kept> &one : kept) {
                if (one) one->giveBack();
            }
            if (page) page->deleteLater();
        };
    }

private:
    QList<Kept *> m_kept;
};

template <typename T>
T *named(QWidget *root, const char *name)
{
    return root->findChild<T *>(QLatin1String(name));
}

std::function<void()> onionSkinsPage(QWidget *content)
{
    auto *box = qobject_cast<QBoxLayout *>(content->layout());
    QWidget *tint = named<QWidget>(content, "doubleTintFactor");
    if (!box || !tint) return {};
    auto *page = new Page(content);
    for (const char *name : {"btnShowHide", "lblPrevColor", "lblNextColor", "btnBackwardColor", "btnForwardColor", "doubleTintFactor", "line"}) {
        page->keep(named<QWidget>(content, name));
    }
    // shown without the panel's own disclosure, as always shown
    KisStudioValueRow::Range percent;
    percent.maximum = 100;
    percent.suffix = QStringLiteral("%");
    auto *spin = qobject_cast<QAbstractSpinBox *>(tint);
    if (spin) {
        // the row follows a control that is kept hidden: it is shown by itself
        auto *row = new KisStudioValueRow(i18nc("onion skins: how strongly frames are tinted", "Tint"), page);
        row->setClickSetsValue(true);
        row->setRange(percent);
        row->setDefaultValue(75);
        row->setValue(valueOf(spin));
        QPointer<QAbstractSpinBox> target = spin;
        QObject::connect(row, &KisStudioValueRow::valueChanged, page, [target](qreal value) {
            if (target) setValueOf(target, value);
        });
        if (auto *real = qobject_cast<QDoubleSpinBox *>(spin)) {
            QObject::connect(real, QOverload<double>::of(&QDoubleSpinBox::valueChanged), row, [row](double value) { row->setValue(value); });
        }
        page->layout()->addWidget(row);
    }
    const QVector<QPair<QString, const char *>> colors = {{i18nc("onion skins: tint of the frames before", "Before"), "btnBackwardColor"},
                                                          {i18nc("onion skins: tint of the frames after", "After"), "btnForwardColor"}};
    for (const auto &color : colors) {
        auto *button = named<KisColorButton>(content, color.second);
        if (!button) continue;
        auto *row = new QWidget(page);
        auto *rowLayout = new QHBoxLayout(row);
        // the name where the value rows' names are
        rowLayout->setContentsMargins(KisModernStyle::space1 + KisModernStyle::hairline, 0, 0, 0);
        rowLayout->addWidget(new QLabel(color.first, row), 1);
        auto *field = new KisStudioColorField(row);
        field->setColor(button->color().toQColor());
        QPointer<KisColorButton> target = button;
        QObject::connect(field, &KisStudioColorField::colorChanged, page, [target](const QColor &picked) {
            if (!target) return;
            target->setColor(KoColor(picked, KoColorSpaceRegistry::instance()->rgb8()));
        });
        QObject::connect(button, &KisColorButton::changed, field, [field](const KoColor &now) { field->setColor(now.toQColor()); });
        rowLayout->addWidget(field);
        page->layout()->addWidget(row);
    }
    // under the frames' bars, above the filter by color label
    QWidget *filter = named<QWidget>(content, "colorFilterGroupbox");
    const int at = filter ? box->indexOf(filter) : -1;
    box->insertWidget(at >= 0 ? at : box->count(), page);
    return page->undo();
}

/// The animation panels' title bars without Krita's lock and float buttons, which the Studio places itself
std::function<void()> animationTitleBar(QDockWidget *dock)
{
    QWidget *bar = dock->titleBarWidget();
    if (!bar) return {};
    QList<QPointer<QWidget>> hidden;
    Q_FOREACH (QWidget *button, bar->findChildren<QWidget *>()) {
        if (button->inherits("KoDockWidgetTitleBarButton") && !button->isHidden()) hidden << button;
    }
    const QList<QPushButton *> pushes = bar->findChildren<QPushButton *>();
    // the float button comes before the close button
    if (!pushes.isEmpty() && !pushes.first()->isHidden()) hidden << pushes.first();
    for (const QPointer<QWidget> &widget : hidden) widget->hide();
    // the onion skins' settings open under the timeline's onion button
    QPointer<QDockWidget> onion = dock->window() ? dock->window()->findChild<QDockWidget *>(QStringLiteral("OnionSkinsDocker")) : nullptr;
    if (QWidget *button = bar->findChild<QWidget *>(QStringLiteral("btnOnionSkinsMenu")); button && onion) {
        onion->setProperty("studioAnchor", QVariant::fromValue<QObject *>(button));
    }
    // its title in sentence case, as Skissa's text is
    QList<QPair<QPointer<QLabel>, QString>> titles;
    Q_FOREACH (QLabel *title, bar->findChildren<QLabel *>(QString(), Qt::FindDirectChildrenOnly)) {
        titles << qMakePair(QPointer<QLabel>(title), title->text());
        title->setText(KisSentenceCase::of(title->text()));
    }
    return [hidden, titles, onion]() {
        if (onion) onion->setProperty("studioAnchor", QVariant());
        for (const QPointer<QWidget> &widget : hidden) {
            if (widget) widget->show();
        }
        for (const auto &title : titles) {
            if (title.first) title.first->setText(title.second);
        }
    };
}

/// The history's rows as Krita draws them, with the steps chosen for a recipe marked: a dot for a step that will be
/// done again, a ring for one that can't (a stroke)
class RecipeMarks : public QStyledItemDelegate
{
public:
    RecipeMarks(QAbstractItemDelegate *theirs, QObject *parent)
        : QStyledItemDelegate(parent)
        , m_theirs(theirs)
    {
    }

    /// 0: not chosen, 1: chosen, 2: chosen but can't be replayed
    std::function<int(int row)> state;

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        if (m_theirs) m_theirs->paint(painter, option, index);
        else QStyledItemDelegate::paint(painter, option, index);
        const int marked = state ? state(index.row()) : 0;
        if (!marked) return;
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        QColor ink = option.palette.color(QPalette::Text);
        ink.setAlphaF(0.10);
        painter->fillPath(KisModernStyle::squircle(QRectF(option.rect).adjusted(2, 1, -2, -1), KisModernStyle::cornerRadius), ink);
        ink.setAlphaF(0.85);
        const QPointF dot(option.rect.right() - KisModernStyle::space3, option.rect.center().y() + 0.5);
        painter->setPen(marked == 1 ? Qt::NoPen : QPen(ink, 1.3));
        painter->setBrush(marked == 1 ? QBrush(ink) : Qt::NoBrush);
        painter->drawEllipse(dot, 3.5, 3.5);
        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        return m_theirs ? m_theirs->sizeHint(option, index) : QStyledItemDelegate::sizeHint(option, index);
    }

private:
    QPointer<QAbstractItemDelegate> m_theirs;
};

/// A recipe's row: its name cut short to the panel's width rather than widening the panel
class RecipeButton : public QToolButton
{
public:
    RecipeButton(const QString &name, QWidget *parent)
        : QToolButton(parent)
        , m_name(name)
    {
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        setText(name);
    }

    QSize sizeHint() const override
    {
        return QToolButton::sizeHint().boundedTo(QSize(parentWidget() ? parentWidget()->width() : 200, QWIDGETSIZE_MAX));
    }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QToolButton::resizeEvent(event);
        const int room = width() - iconSize().width() - 3 * KisModernStyle::space2;
        const QString shown = fontMetrics().elidedText(m_name, Qt::ElideRight, qMax(0, room));
        if (shown != text()) setText(shown);
        setToolTip(shown == m_name ? QString() : m_name);
    }

private:
    QString m_name;
};

/// Hands a widget's events to a function
class EventHook : public QObject
{
public:
    EventHook(QObject *parent, std::function<bool(QEvent *)> hook)
        : QObject(parent)
        , m_hook(std::move(hook))
    {
    }

    bool eventFilter(QObject *, QEvent *event) override
    {
        return m_hook(event);
    }

private:
    std::function<bool(QEvent *)> m_hook;
};

/// Where each snapshot was kept in the history: after which step, of which image's history
struct KeptAt {
    const void *stack {nullptr};
    const KUndo2Command *after {nullptr}; ///< none: the image as opened (or as the snapshot gone back to)
    bool gone {false};                    ///< its history is no more (another snapshot was gone back to)
};

KUndo2Stack *currentHistory()
{
    KisMainWindow *window = KisPart::instance()->currentMainwindow();
    KisView *view = window ? window->activeView() : nullptr;
    return view && view->document() ? view->document()->undoStack() : nullptr;
}

/// History as a line to drag along, with the snapshots on it: a snapshot is a point of the history kept to go back to
std::function<void()> historyPage(QDockWidget *history)
{
    auto *steps = qobject_cast<QAbstractItemView *>(history->widget());
    QWidget *window = history->window();
    QDockWidget *snapshots = window ? window->findChild<QDockWidget *>(QStringLiteral("Snapshot")) : nullptr;
    QWidget *theirs = snapshots ? snapshots->widget() : nullptr;
    auto *list = theirs ? theirs->findChild<QListView *>() : nullptr;
    // Krita's add, switch to and remove, in that order
    const QList<QToolButton *> buttons = theirs ? theirs->findChildren<QToolButton *>() : QList<QToolButton *>();
    if (!steps || !list || !list->model() || buttons.size() < 3) return {};
    QPointer<QToolButton> add = buttons[0];
    QPointer<QToolButton> switchTo = buttons[1];
    QPointer<QToolButton> remove = buttons[2];
    QPointer<QListView> kept = list;

    auto *merged = new QWidget;
    auto *box = new QVBoxLayout(merged);
    box->setContentsMargins(0, 0, 0, 0);
    box->setSpacing(KisModernStyle::space1);
    auto *row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(KisModernStyle::space1);
    auto *scrubber = new KisStudioHistoryScrubber(steps, merged);
    row->addWidget(scrubber, 1);
    auto *snap = new QToolButton(merged);
    snap->setAutoRaise(true);
    snap->setFocusPolicy(Qt::NoFocus);
    snap->setIcon(KisIconUtils::loadIcon(QStringLiteral("studio_snapshots")));
    snap->setIconSize(QSize(18, 18));
    snap->setToolTip(i18nc("@info:tooltip history panel", "Snapshot: a copy of the image to come back to"));
    row->addWidget(snap);
    box->addLayout(row);
    box->addWidget(steps, 1);

    // recipes: steps chosen with Ctrl or Shift, or from a step on with its right click, kept to be done again
    auto chosen = QSharedPointer<QSet<int>>::create();
    auto anchor = QSharedPointer<int>::create(-1);
    QPointer<QAbstractItemView> stepsView = steps;
    auto commandAt = [](int row) {
        KUndo2Stack *stack = currentHistory();
        return stack && row >= 1 && row <= stack->count() ? KisStudioRecipes::commandOf(stack->command(row - 1)) : QString();
    };
    QPointer<QAbstractItemDelegate> theirDelegate = steps->itemDelegate();
    auto *marks = new RecipeMarks(theirDelegate, steps);
    marks->state = [chosen, commandAt](int row) { return !chosen->contains(row) ? 0 : commandAt(row).isEmpty() ? 2 : 1; };
    steps->setItemDelegate(marks);

    auto *keep = new QToolButton(merged);
    keep->setAutoRaise(true);
    keep->setFocusPolicy(Qt::NoFocus);
    keep->setIcon(KisIconUtils::loadIcon(QStringLiteral("studio_add")));
    keep->setIconSize(QSize(18, 18));
    keep->setText(i18nc("@action:button history panel: the chosen steps, kept to be done again", "Keep as recipe"));
    keep->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    keep->hide();
    box->addWidget(keep, 0, Qt::AlignLeft);

    auto *recipesList = new QWidget(merged);
    auto *keptRows = new QVBoxLayout(recipesList);
    keptRows->setContentsMargins(0, 0, 0, 0);
    keptRows->setSpacing(0);
    box->addWidget(recipesList);

    QPointer<QToolButton> keepButton = keep;
    auto showChosen = [chosen, commandAt, keepButton, stepsView]() {
        if (!keepButton || !stepsView) return;
        bool any = false;
        for (int row : *chosen) any = any || !commandAt(row).isEmpty();
        keepButton->setVisible(!chosen->isEmpty());
        keepButton->setEnabled(any);
        keepButton->setToolTip(any ? QString() : i18nc("@info:tooltip history panel", "Strokes can't be done again"));
        stepsView->viewport()->update();
    };
    auto keepChosen = [chosen, commandAt, showChosen]() {
        QList<int> rows = chosen->values();
        std::sort(rows.begin(), rows.end());
        QStringList commands;
        for (int row : rows) {
            const QString command = commandAt(row);
            if (!command.isEmpty()) commands << command;
        }
        if (commands.isEmpty()) return;
        KisStudioRecipes::add(commands);
        chosen->clear();
        showChosen();
    };
    QObject::connect(keep, &QToolButton::clicked, merged, keepChosen);

    auto *choosing = new EventHook(steps->viewport(), [chosen, anchor, stepsView, showChosen, keepChosen](QEvent *event) {
        if (!stepsView) return false;
        if (event->type() == QEvent::ContextMenu) {
            auto *menuEvent = static_cast<QContextMenuEvent *>(event);
            const int row = stepsView->indexAt(menuEvent->pos()).row();
            if (row < 1 && chosen->isEmpty()) return false;
            QMenu menu;
            QAction *keepThem = nullptr;
            QAction *fromHere = nullptr;
            if (!chosen->isEmpty()) {
                keepThem = menu.addAction(i18nc("@action history panel", "Keep as recipe"));
            } else {
                fromHere = menu.addAction(i18nc("@action history panel: this step and the ones after it", "Keep from here as recipe"));
            }
            QAction *picked = menu.exec(menuEvent->globalPos());
            if (picked == fromHere && fromHere) {
                const int last = stepsView->model()->rowCount() - 1;
                for (int r = row; r <= last; r++) chosen->insert(r);
                keepChosen();
            } else if (picked == keepThem && keepThem) {
                keepChosen();
            }
            return true;
        }
        if (event->type() != QEvent::MouseButtonPress && event->type() != QEvent::MouseButtonRelease
            && event->type() != QEvent::MouseButtonDblClick) {
            return false;
        }
        auto *mouse = static_cast<QMouseEvent *>(event);
        // a right click opens the menu; it doesn't go to that step
        if (mouse->button() == Qt::RightButton) return true;
        if (mouse->button() != Qt::LeftButton) return false;
        const bool picking = mouse->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier);
        if (!picking) {
            // a plain click goes to that step, as ever, and lets the choice go
            if (event->type() == QEvent::MouseButtonPress && !chosen->isEmpty()) {
                chosen->clear();
                showChosen();
            }
            return false;
        }
        if (event->type() != QEvent::MouseButtonPress) return true;
        const int row = stepsView->indexAt(mouse->pos()).row();
        if (row < 1) return true;
        if (mouse->modifiers() & Qt::ShiftModifier) {
            const int from = *anchor >= 1 ? *anchor : qMax(1, stepsView->currentIndex().row());
            for (int r = qMin(from, row); r <= qMax(from, row); r++) chosen->insert(r);
        } else if (chosen->contains(row)) {
            chosen->remove(row);
        } else {
            chosen->insert(row);
        }
        *anchor = row;
        showChosen();
        return true;
    });
    steps->viewport()->installEventFilter(choosing);
    // a new step or another image: the rows chosen mean other steps now
    QObject::connect(steps->model(), &QAbstractItemModel::modelReset, merged, [chosen, anchor, showChosen]() {
        chosen->clear();
        *anchor = -1;
        showChosen();
    });

    // the recipes, each a command: a click runs it; its right click renames, deletes, or opens its card (shortcut, tools)
    QPointer<QWidget> keptList = recipesList;
    QPointer<KisMainWindow> mainWindow = qobject_cast<KisMainWindow *>(window);
    auto listRecipes = [keptList, keptRows, mainWindow]() {
        if (!keptList || !mainWindow) return;
        while (QLayoutItem *item = keptRows->takeAt(0)) {
            if (item->widget()) item->widget()->deleteLater();
            delete item;
        }
        for (const KisStudioRecipes::Recipe &recipe : KisStudioRecipes::recipes()) {
            QAction *action = mainWindow->actionCollection()->action(recipe.id);
            if (!action) continue;
            auto *button = new RecipeButton(recipe.name, keptList);
            button->setIcon(action->icon());
            button->setProperty("runsAction", recipe.id);
            QPointer<QAction> runs = action;
            QObject::connect(button, &QToolButton::clicked, button, [runs]() {
                if (runs) runs->trigger();
            });
            button->setAutoRaise(true);
            button->setFocusPolicy(Qt::NoFocus);
            button->setIconSize(QSize(18, 18));
            button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
            button->setProperty("ownContextMenu", true);
            button->setContextMenuPolicy(Qt::CustomContextMenu);
            const QString id = recipe.id;
            const QString name = recipe.name;
            QPointer<QToolButton> guarded = button;
            QObject::connect(button, &QToolButton::customContextMenuRequested, button, [guarded, id, name, mainWindow](const QPoint &pos) {
                if (!guarded) return;
                QMenu menu;
                QAction *rename = menu.addAction(i18nc("@action", "Rename"));
                QAction *card = menu.addAction(i18nc("@action a recipe's shortcut and place on the tools", "Shortcut and tools…"));
                menu.addSeparator();
                QAction *forget = menu.addAction(KisIconUtils::loadIcon(QStringLiteral("studio_delete")), i18nc("@action", "Delete"));
                QAction *picked = menu.exec(guarded->mapToGlobal(pos));
                if (!guarded) return;
                if (picked == rename) {
                    const int left = guarded->iconSize().width() + KisModernStyle::space3;
                    KisStudioInlineEdit::open(guarded, guarded->rect().adjusted(left, 0, 0, 0), name, Qt::AlignLeft | Qt::AlignVCenter,
                                              guarded->font(), [id](const QString &typed) { KisStudioRecipes::rename(id, typed); });
                } else if (picked == card && mainWindow && mainWindow->studioFind()) {
                    mainWindow->studioFind()->showCard(QStringLiteral("action:") + id, guarded);
                } else if (picked == forget) {
                    KisStudioRecipes::remove(id);
                }
            });
            keptRows->addWidget(button);
        }
        keptList->setVisible(keptRows->count() > 0);
    };
    QObject::connect(KisStudioRecipes::instance(), &KisStudioRecipes::changed, merged, listRecipes);
    listRecipes();

    auto places = QSharedPointer<QHash<QString, KeptAt>>::create();
    QPointer<KisStudioHistoryScrubber> line = scrubber;
    auto nameOf = [kept](int row) { return kept ? kept->model()->index(row, 0).data(Qt::DisplayRole).toString() : QString(); };
    // set once the marks' refresh is made below; going back needs it
    auto refreshMarks = QSharedPointer<std::function<void()>>::create();
    QWeakPointer<std::function<void()>> marksLater = refreshMarks;
    auto goBack = [kept, switchTo, places, nameOf, marksLater](int row) {
        if (!kept || !switchTo) return;
        kept->setCurrentIndex(kept->model()->index(row, 0));
        switchTo->click();
        // the history starts again from it; the others' histories are gone
        KUndo2Stack *stack = currentHistory();
        for (auto it = places->begin(); it != places->end(); ++it) {
            if (it->stack == stack) it->gone = true;
        }
        (*places)[nameOf(row)] = {stack, nullptr, false};
        if (const auto refreshNow = marksLater.toStrongRef()) (*refreshNow)();
    };
    auto drop = [kept, remove](int row) {
        if (!kept || !remove) return;
        kept->setCurrentIndex(kept->model()->index(row, 0));
        remove->click();
    };
    // each snapshot on the step it was kept after, or before the line when that history is gone
    auto refresh = [line, kept, places, nameOf, goBack, drop]() {
        if (!line || !kept) return;
        KUndo2Stack *stack = currentHistory();
        QVector<KisStudioHistoryScrubber::Mark> marks;
        for (int row = 0; row < kept->model()->rowCount(); row++) {
            const QString name = nameOf(row);
            const KeptAt place = places->value(name);
            int step = -1;
            if (stack && place.stack == stack && !place.gone) {
                if (!place.after) step = 0;
                for (int i = 0; place.after && i < stack->count(); i++) {
                    if (stack->command(i) == place.after) step = i + 1;
                }
            }
            marks.append({step, name});
        }
        line->setMarks(marks, [goBack, drop, kept](int mark, bool menu) {
            if (!menu) {
                goBack(mark);
                return;
            }
            QMenu choices;
            QAction *back = choices.addAction(KisIconUtils::loadIcon(QStringLiteral("studio_undo")),
                                              i18nc("@action history panel", "Go back to this snapshot"));
            QAction *forget = choices.addAction(KisIconUtils::loadIcon(QStringLiteral("studio_delete")),
                                                i18nc("@action history panel", "Delete snapshot"));
            QAction *chosen = choices.exec(QCursor::pos());
            if (chosen == back) goBack(mark);
            else if (chosen == forget) drop(mark);
        });
    };
    *refreshMarks = refresh;
    // held as long as the page is
    QObject::connect(snap, &QToolButton::clicked, merged, [add, refreshMarks]() {
        if (add) add->click();
    });
    QAbstractItemModel *keptModel = list->model();
    QObject::connect(keptModel, &QAbstractItemModel::rowsInserted, merged, [places, nameOf, refresh](const QModelIndex &, int, int last) {
        KUndo2Stack *stack = currentHistory();
        const int done = stack ? stack->index() : 0;
        (*places)[nameOf(last)] = {stack, done > 0 ? stack->command(done - 1) : nullptr, false};
        refresh();
    });
    for (QAbstractItemModel *model : {keptModel, steps->model()}) {
        QObject::connect(model, &QAbstractItemModel::rowsRemoved, merged, refresh);
        QObject::connect(model, &QAbstractItemModel::modelReset, merged, refresh);
        QObject::connect(model, &QAbstractItemModel::layoutChanged, merged, refresh);
    }
    QObject::connect(steps->model(), &QAbstractItemModel::rowsInserted, merged, refresh);
    refresh();

    const bool shown = !history->isHidden();
    history->setWidget(merged);
    if (shown) merged->show();

    QPointer<QDockWidget> historyDock = history;
    QPointer<QWidget> stepsList = steps;
    QPointer<QWidget> mergedPage = merged;
    QPointer<EventHook> choosingHook = choosing;
    QPointer<RecipeMarks> marked = marks;
    return [historyDock, stepsList, mergedPage, choosingHook, marked, theirDelegate]() {
        if (choosingHook) choosingHook->deleteLater();
        if (stepsList && marked) {
            static_cast<QAbstractItemView *>(stepsList.data())->setItemDelegate(theirDelegate);
            marked->deleteLater();
        }
        if (historyDock && stepsList) {
            historyDock->setWidget(stepsList);
            stepsList->show();
        }
        if (mergedPage) mergedPage->deleteLater();
    };
}

/// Lets a right-click put the view back to the whole image
class FitOnRightClick : public QObject
{
public:
    using QObject::QObject;

    bool eventFilter(QObject *, QEvent *event) override
    {
        if (event->type() == QEvent::ContextMenu) return true;
        if (event->type() != QEvent::MouseButtonPress && event->type() != QEvent::MouseButtonRelease
            && event->type() != QEvent::MouseButtonDblClick) {
            return false;
        }
        if (static_cast<QMouseEvent *>(event)->button() != Qt::RightButton) return false;
        if (event->type() == QEvent::MouseButtonRelease) {
            KisMainWindow *window = KisPart::instance()->currentMainwindow();
            for (const char *name : {"zoom_to_fit", "reset_canvas_rotation"}) {
                if (QAction *action = window ? window->actionCollection()->action(QLatin1String(name)) : nullptr) action->trigger();
            }
        }
        return true;
    }
};

/// The overview as the picture alone, its view frame dragged; zoom, turning and mirroring are on the canvas
std::function<void()> overviewPage(QWidget *content)
{
    QWidget *picture = nullptr;
    Q_FOREACH (QWidget *child, content->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly)) {
        if (child->inherits("OverviewWidget")) picture = child;
    }
    if (!picture) return {};
    QPointer<QWidget> page = content;
    QPointer<QWidget> shown = picture;
    const int minimum = picture->minimumHeight();
    auto relayout = [page]() {
        if (!page) return;
        QResizeEvent resize(page->size(), page->size());
        QCoreApplication::sendEvent(page, &resize);
    };
    content->setProperty("studioPictureOnly", true);
    // on the panel's glass rather than on a box of its own
    picture->setAutoFillBackground(false);
    picture->setMinimumHeight(140);
    auto *fit = new FitOnRightClick(picture);
    picture->installEventFilter(fit);
    relayout();
    QPointer<FitOnRightClick> filter = fit;
    return [page, shown, minimum, filter, relayout]() {
        if (filter) filter->deleteLater();
        if (shown) {
            shown->setAutoFillBackground(true);
            shown->setMinimumHeight(minimum);
        }
        if (page) page->setProperty("studioPictureOnly", false);
        relayout();
    };
}

} // namespace

std::function<void()> KisStudioPanelPages::apply(QDockWidget *dock)
{
    QWidget *content = dock ? dock->widget() : nullptr;
    if (!content) return {};
    const QString id = dock->objectName();
    if (id == QLatin1String("OnionSkinsDocker")) return onionSkinsPage(content);
    if (id == QLatin1String("History")) return historyPage(dock);
    if (id == QLatin1String("OverviewDocker")) return overviewPage(content);
    if (id == QLatin1String("TimelineDocker") || id == QLatin1String("AnimationCurvesDocker")) return animationTitleBar(dock);
    return {};
}
