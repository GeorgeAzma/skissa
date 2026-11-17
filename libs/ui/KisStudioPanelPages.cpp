/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioPanelPages.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QBoxLayout>
#include <QComboBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QSpinBox>
#include <QStyledItemDelegate>
#include <QTableView>
#include <QTimer>

#include <kcolorbutton.h>
#include <klocalizedstring.h>

#include <KisAngleSelector.h>
#include <KisModernStyle.h>
#include <KisSentenceCase.h>
#include <KoColor.h>
#include <KoColorSpaceRegistry.h>
#include <kis_color_button.h>
#include <kis_icon_utils.h>

#include "widgets/KisStudioChips.h"
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

/// A row of the page shown while the control it drives is, and usable while it is
class Follows : public QObject
{
public:
    Follows(QWidget *source, QWidget *row)
        : QObject(row)
        , m_row(row)
    {
        source->installEventFilter(this);
        row->setVisible(!source->isHidden());
        row->setEnabled(source->isEnabled());
    }

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::ShowToParent || event->type() == QEvent::HideToParent) {
            m_row->setVisible(event->type() == QEvent::ShowToParent);
        } else if (event->type() == QEvent::EnabledChange) {
            m_row->setEnabled(static_cast<QWidget *>(watched)->isEnabled());
        }
        return false;
    }

private:
    QWidget *m_row;
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

    void addGap()
    {
        static_cast<QVBoxLayout *>(layout())->addSpacing(KisModernStyle::space2);
    }

    void addSwitch(const QString &name, QAbstractButton *source)
    {
        if (!source) return;
        auto *chips = new KisStudioChips(this);
        chips->setSwitches(true);
        QPointer<QAbstractButton> target = source;
        auto show = [chips, name, target]() {
            if (!target) return;
            KisStudioChips::Chip chip;
            chip.text = name;
            chip.on = target->isChecked();
            chips->setChips({chip});
        };
        show();
        connect(source, &QAbstractButton::toggled, chips, show);
        connect(chips, &KisStudioChips::clicked, this, [target]() {
            if (target) target->click();
        });
        new Follows(source, chips);
        layout()->addWidget(chips);
    }

    void addTabs(QComboBox *combo, const QVector<QPair<int, QString>> &choices)
    {
        if (!combo) return;
        auto *chips = new KisStudioChips(this);
        chips->setTabs(true);
        QPointer<QComboBox> target = combo;
        auto show = [chips, choices, target]() {
            if (!target) return;
            QVector<KisStudioChips::Chip> list;
            for (const auto &choice : choices) {
                KisStudioChips::Chip chip;
                chip.text = choice.second;
                chip.on = target->currentIndex() == choice.first;
                list << chip;
            }
            chips->setChips(list);
        };
        show();
        connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged), chips, show);
        connect(chips, &KisStudioChips::clicked, this, [target, choices](int index) {
            if (target && index >= 0 && index < choices.size()) target->setCurrentIndex(choices[index].first);
        });
        layout()->addWidget(chips);
    }

    /// A value row writing each of \p targets, read from the first
    void addValue(const QString &name, const QVector<QAbstractSpinBox *> &targets, const KisStudioValueRow::Range &range, qreal defaultValue)
    {
        if (targets.isEmpty() || !targets.first()) return;
        auto *row = new KisStudioValueRow(name, this);
        row->setClickSetsValue(true);
        row->setRange(range);
        row->setDefaultValue(defaultValue);
        QVector<QPointer<QAbstractSpinBox>> spins;
        for (QAbstractSpinBox *spin : targets) {
            if (spin) spins << spin;
        }
        row->setValue(valueOf(spins.first()));
        connect(row, &KisStudioValueRow::valueChanged, this, [spins](qreal value) {
            for (const QPointer<QAbstractSpinBox> &spin : spins) {
                if (spin) setValueOf(spin, value);
            }
        });
        auto follow = [row, first = spins.first()]() {
            if (first) row->setValue(valueOf(first));
        };
        if (auto *whole = qobject_cast<QSpinBox *>(spins.first().data())) connect(whole, QOverload<int>::of(&QSpinBox::valueChanged), row, follow);
        if (auto *real = qobject_cast<QDoubleSpinBox *>(spins.first().data())) connect(real, QOverload<double>::of(&QDoubleSpinBox::valueChanged), row, follow);
        new Follows(spins.first(), row);
        layout()->addWidget(row);
    }

    void addAngle(const QString &name, const QVector<KisAngleSelector *> &targets)
    {
        if (targets.isEmpty() || !targets.first()) return;
        auto *row = new KisStudioValueRow(name, this);
        KisStudioValueRow::Range range;
        range.minimum = 1;
        range.maximum = 89;
        range.suffix = QStringLiteral("°");
        row->setRange(range);
        row->setDefaultValue(30);
        QVector<QPointer<KisAngleSelector>> selectors;
        for (KisAngleSelector *selector : targets) {
            if (selector) selectors << selector;
        }
        row->setValue(selectors.first()->angle());
        connect(row, &KisStudioValueRow::valueChanged, this, [selectors](qreal value) {
            for (const QPointer<KisAngleSelector> &selector : selectors) {
                if (selector) selector->setAngle(value);
            }
        });
        connect(selectors.first(), &KisAngleSelector::angleChanged, row, [row](qreal value) { row->setValue(value); });
        new Follows(selectors.first(), row);
        layout()->addWidget(row);
    }

    /// A named color, its field driving \p source (Krita's KColorButton or KisColorButton)
    void addColor(const QString &name, QWidget *source, const QColor &defaultColor)
    {
        if (!source) return;
        auto *row = new QWidget(this);
        auto *rowLayout = new QHBoxLayout(row);
        // the name where the value rows' names are
        rowLayout->setContentsMargins(KisModernStyle::space1 + KisModernStyle::hairline, 0, 0, 0);
        rowLayout->addWidget(new QLabel(name, row), 1);
        auto *field = new KisStudioColorField(row);
        field->setDefaultColor(defaultColor);
        rowLayout->addWidget(field);
        if (auto *plain = qobject_cast<KColorButton *>(source)) {
            QPointer<KColorButton> target = plain;
            field->setAlphaEnabled(true);
            field->setColor(plain->color());
            connect(field, &KisStudioColorField::colorChanged, this, [target](const QColor &color) {
                if (target) target->setColor(color);
            });
            connect(plain, &KColorButton::changed, field, [field](const QColor &color) { field->setColor(color); });
        } else if (auto *kis = qobject_cast<KisColorButton *>(source)) {
            QPointer<KisColorButton> target = kis;
            field->setColor(kis->color().toQColor());
            connect(field, &KisStudioColorField::colorChanged, this, [target](const QColor &color) {
                if (!target) return;
                target->setColor(KoColor(color, KoColorSpaceRegistry::instance()->rgb8()));
            });
            connect(kis, &KisColorButton::changed, field, [field](const KoColor &color) { field->setColor(color.toQColor()); });
        }
        new Follows(source, row);
        layout()->addWidget(row);
    }

    void addCommand(const QString &text, QAbstractButton *source)
    {
        if (!source) return;
        auto *button = new QPushButton(text, this);
        QPointer<QAbstractButton> target = source;
        connect(button, &QPushButton::clicked, this, [target]() {
            if (target) target->click();
        });
        layout()->addWidget(button);
    }

private:
    QList<Kept *> m_kept;
};

template <typename T>
T *named(QWidget *root, const char *name)
{
    return root->findChild<T *>(QLatin1String(name));
}

std::function<void()> gridPage(QWidget *content)
{
    auto *tabs = named<QWidget>(content, "tabWidget");
    auto *box = qobject_cast<QBoxLayout *>(content->layout());
    if (!tabs || !box) return {};
    auto *page = new Page(content);
    page->keep(tabs);

    page->addSwitch(i18nc("grid panel: show the grid", "Grid"), named<QAbstractButton>(content, "chkShowGrid"));
    page->addSwitch(i18nc("grid panel", "Snap to grid"), named<QAbstractButton>(content, "chkSnapToGrid"));
    if (QComboBox *type = named<QComboBox>(content, "gridTypeCombobox"); type && type->count() > 2) {
        page->addTabs(type, {{0, i18nc("grid panel: a grid of squares", "Square")}, {2, i18nc("grid panel: a grid of diamonds", "Isometric")}});
    }
    KisStudioValueRow::Range px;
    px.minimum = 1;
    px.maximum = 500;
    px.size = true;
    px.suffix = i18nc("pixels, after a number", " px");
    page->addValue(i18nc("grid panel: the size of its cells", "Spacing"),
                   {named<QAbstractSpinBox>(content, "intHSpacing"), named<QAbstractSpinBox>(content, "intVSpacing")}, px, 16);
    page->addValue(i18nc("grid panel: the size of an isometric cell", "Size"), {named<QAbstractSpinBox>(content, "cellSizeSpinbox")}, px, 30);
    page->addValue(i18nc("grid panel: the gap of the old isometric grid", "Spacing"), {named<QAbstractSpinBox>(content, "cellSpacingSpinbox")}, px, 10);
    page->addAngle(i18nc("grid panel: how steep an isometric grid's lines are", "Angle"),
                   {named<KisAngleSelector>(content, "angleLeftAngleSelector"), named<KisAngleSelector>(content, "angleRightAngleSelector")});
    KisStudioValueRow::Range count;
    count.minimum = 1;
    count.maximum = 10;
    page->addValue(i18nc("grid panel: lines between the main lines", "Subdivisions"), {named<QAbstractSpinBox>(content, "intSubdivision")}, count, 2);
    page->addColor(i18nc("grid panel: the grid's color", "Color"), named<QWidget>(content, "colorMain"), QColor(200, 200, 200, 200));

    page->addGap();
    page->addSwitch(i18nc("grid panel: show the guides", "Guides"), named<QAbstractButton>(content, "chkShowGuides"));
    page->addSwitch(i18nc("grid panel", "Snap to guides"), named<QAbstractButton>(content, "chkSnapToGuides"));
    page->addSwitch(i18nc("grid panel: guides can't be moved", "Lock guides"), named<QAbstractButton>(content, "chkLockGuides"));
    page->addSwitch(i18nc("grid panel: the rulers along the canvas, guides are dragged out of them", "Rulers"), named<QAbstractButton>(content, "chkShowRulers"));
    page->addColor(i18nc("grid panel: the guides' color", "Guide color"), named<QWidget>(content, "colorGuides"), QColor(Qt::magenta));
    page->addCommand(i18nc("grid panel: remove every guide", "Delete all guides"), named<QAbstractButton>(content, "deleteAllGuidesButton"));

    box->insertWidget(box->indexOf(tabs) + 1, page);
    return page->undo();
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
    // its title in sentence case, as Skissa's text is
    QList<QPair<QPointer<QLabel>, QString>> titles;
    Q_FOREACH (QLabel *title, bar->findChildren<QLabel *>(QString(), Qt::FindDirectChildrenOnly)) {
        titles << qMakePair(QPointer<QLabel>(title), title->text());
        title->setText(KisSentenceCase::of(title->text()));
    }
    return [hidden, titles]() {
        for (const QPointer<QWidget> &widget : hidden) {
            if (widget) widget->show();
        }
        for (const auto &title : titles) {
            if (title.first) title.first->setText(title.second);
        }
    };
}

} // namespace

std::function<void()> KisStudioPanelPages::apply(QDockWidget *dock)
{
    QWidget *content = dock ? dock->widget() : nullptr;
    if (!content) return {};
    const QString id = dock->objectName();
    if (id == QLatin1String("GridDocker")) return gridPage(content);
    if (id == QLatin1String("OnionSkinsDocker")) return onionSkinsPage(content);
    if (id == QLatin1String("ChannelDocker")) return channelsPage(content);
    if (id == QLatin1String("TimelineDocker") || id == QLatin1String("AnimationCurvesDocker")) return animationTitleBar(dock);
    return {};
}
