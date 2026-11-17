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
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QSpinBox>
#include <QStyledItemDelegate>
#include <QTableView>
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
#include "KisPart.h"
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

/// An eye in place of a channel's checkbox, as a layer has
class EyeDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    static QRect eyeRect(const QRect &cell)
    {
        const int side = 18;
        return QRect(cell.left() + KisModernStyle::space1, cell.center().y() - side / 2 + 1, side, side);
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QStyleOptionViewItem view = option;
        initStyleOption(&view, index);
        const QVariant check = index.data(Qt::CheckStateRole);
        if (!check.isValid()) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }
        view.features &= ~QStyleOptionViewItem::HasCheckIndicator;
        const QRect eye = eyeRect(option.rect);
        view.rect.setLeft(eye.right() + KisModernStyle::space1);
        const QWidget *widget = option.widget;
        QStyle *style = widget ? widget->style() : nullptr;
        if (style) style->drawControl(QStyle::CE_ItemViewItem, &view, painter, widget);
        const bool shown = check.toInt() == Qt::Checked;
        painter->save();
        if (!shown) painter->setOpacity(0.55);
        KisIconUtils::loadIcon(shown ? QStringLiteral("studio_visible") : QStringLiteral("studio_hidden")).paint(painter, eye);
        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QSize size = QStyledItemDelegate::sizeHint(option, index);
        return QSize(size.width() + 18, qMax(size.height(), 26));
    }

    bool editorEvent(QEvent *event, QAbstractItemModel *model, const QStyleOptionViewItem &option, const QModelIndex &index) override
    {
        if (!index.data(Qt::CheckStateRole).isValid()) return QStyledItemDelegate::editorEvent(event, model, option, index);
        if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonDblClick) {
            return eyeRect(option.rect).adjusted(-4, -4, 4, 4).contains(static_cast<QMouseEvent *>(event)->pos());
        }
        if (event->type() == QEvent::MouseButtonRelease && eyeRect(option.rect).adjusted(-4, -4, 4, 4).contains(static_cast<QMouseEvent *>(event)->pos())) {
            const bool shown = index.data(Qt::CheckStateRole).toInt() == Qt::Checked;
            model->setData(index, shown ? Qt::Unchecked : Qt::Checked, Qt::CheckStateRole);
            return true;
        }
        return false;
    }
};

std::function<void()> channelsPage(QWidget *content)
{
    auto *table = content->findChild<QTableView *>();
    if (!table) return {};
    QPointer<QAbstractItemDelegate> before = table->itemDelegate();
    auto *eyes = new EyeDelegate(table);
    table->setItemDelegate(eyes);
    QPointer<QTableView> view = table;
    QPointer<EyeDelegate> own = eyes;
    return [view, before, own]() {
        if (view && before) view->setItemDelegate(before);
        if (own) own->deleteLater();
    };
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

/// History with the snapshots under it: a snapshot is a point of the history kept to go back to
std::function<void()> historyPage(QDockWidget *history)
{
    QWidget *steps = history->widget();
    QWidget *window = history->window();
    QDockWidget *snapshots = window ? window->findChild<QDockWidget *>(QStringLiteral("Snapshot")) : nullptr;
    QWidget *theirs = snapshots ? snapshots->widget() : nullptr;
    auto *list = theirs ? theirs->findChild<QListView *>() : nullptr;
    auto *theirLayout = theirs ? qobject_cast<QBoxLayout *>(theirs->layout()) : nullptr;
    // Krita's add, switch to and remove, in that order
    const QList<QToolButton *> buttons = theirs ? theirs->findChildren<QToolButton *>() : QList<QToolButton *>();
    if (!steps || !list || !theirLayout || buttons.size() < 3) return {};
    QPointer<QToolButton> add = buttons[0];
    QPointer<QToolButton> switchTo = buttons[1];
    QPointer<QToolButton> remove = buttons[2];

    auto *merged = new QWidget;
    auto *box = new QVBoxLayout(merged);
    box->setContentsMargins(0, 0, 0, 0);
    box->setSpacing(KisModernStyle::space1);
    box->addWidget(steps, 1);
    box->addWidget(list);

    auto *row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(KisModernStyle::space1);
    auto button = [merged, row](const char *icon, const QString &text, bool named) {
        auto *made = new QToolButton(merged);
        made->setAutoRaise(true);
        made->setFocusPolicy(Qt::NoFocus);
        made->setIcon(KisIconUtils::loadIcon(QLatin1String(icon)));
        made->setIconSize(QSize(18, 18));
        if (named) {
            made->setText(text);
            made->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        } else {
            made->setToolTip(text);
        }
        row->addWidget(made);
        return made;
    };
    QToolButton *snap = button("studio_snapshots", i18nc("history panel: keep the image as it is now, to go back to", "Snapshot"), true);
    snap->setToolTip(i18nc("@info:tooltip", "A copy of the image to come back to"));
    row->addStretch(1);
    QToolButton *back = button("studio_undo", i18nc("@info:tooltip history panel", "Go back to this snapshot"), false);
    QToolButton *drop = button("studio_delete", i18nc("@info:tooltip history panel", "Delete snapshot"), false);
    box->addLayout(row);
    QObject::connect(snap, &QToolButton::clicked, merged, [add]() {
        if (add) add->click();
    });
    QObject::connect(back, &QToolButton::clicked, merged, [switchTo]() {
        if (switchTo) switchTo->click();
    });
    QObject::connect(drop, &QToolButton::clicked, merged, [remove]() {
        if (remove) remove->click();
    });

    // the list as tall as its snapshots, up to four, and gone with none; going back and deleting need one chosen
    QPointer<QListView> view = list;
    auto fit = [view, back, drop]() {
        if (!view || !view->model()) return;
        const int rows = view->model()->rowCount();
        view->setVisible(rows > 0);
        if (rows > 0) view->setFixedHeight(qMin(rows, 4) * view->sizeHintForRow(0) + 2 * view->frameWidth());
        const bool chosen = view->selectionModel() && view->selectionModel()->hasSelection();
        back->setVisible(chosen);
        drop->setVisible(chosen);
    };
    if (QAbstractItemModel *model = list->model()) {
        QObject::connect(model, &QAbstractItemModel::rowsInserted, merged, fit);
        QObject::connect(model, &QAbstractItemModel::rowsRemoved, merged, fit);
        QObject::connect(model, &QAbstractItemModel::modelReset, merged, fit);
        QObject::connect(model, &QAbstractItemModel::rowsInserted, merged, [view](const QModelIndex &, int, int last) {
            if (view) view->setCurrentIndex(view->model()->index(last, 0));
        });
    }
    if (list->selectionModel()) QObject::connect(list->selectionModel(), &QItemSelectionModel::selectionChanged, merged, fit);
    fit();

    const bool shown = !history->isHidden();
    history->setWidget(merged);
    if (shown) merged->show();

    QPointer<QDockWidget> historyDock = history;
    QPointer<QWidget> stepsList = steps;
    QPointer<QWidget> mergedPage = merged;
    QPointer<QBoxLayout> listHome = theirLayout;
    return [historyDock, stepsList, mergedPage, view, listHome]() {
        if (view) {
            view->setMinimumHeight(0);
            view->setMaximumHeight(QWIDGETSIZE_MAX);
            view->show();
            if (listHome) listHome->insertWidget(0, view);
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
    if (id == QLatin1String("ChannelDocker")) return channelsPage(content);
    if (id == QLatin1String("History")) return historyPage(dock);
    if (id == QLatin1String("OverviewDocker")) return overviewPage(content);
    if (id == QLatin1String("TimelineDocker") || id == QLatin1String("AnimationCurvesDocker")) return animationTitleBar(dock);
    return {};
}
