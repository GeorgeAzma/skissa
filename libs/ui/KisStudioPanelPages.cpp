/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioPanelPages.h"

#include "KisStudioHistoryPage.h"

#include <QAbstractItemView>
#include <QAction>
#include <QCoreApplication>
#include <QBoxLayout>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>

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
    if (id == QLatin1String("History")) return KisStudioHistoryPage::apply(dock);
    if (id == QLatin1String("OverviewDocker")) return overviewPage(content);
    if (id == QLatin1String("TimelineDocker") || id == QLatin1String("AnimationCurvesDocker")) return animationTitleBar(dock);
    return {};
}
