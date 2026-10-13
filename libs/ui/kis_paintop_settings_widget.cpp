/* This file is part of the KDE project
 * SPDX-FileCopyrightText: 2008 Boudewijn Rempt <boud@valdyas.org>
 * SPDX-FileCopyrightText: 2011 Silvio Heinrich <plassy@web.de>
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "kis_paintop_settings_widget.h"
#include "kis_paintop_option.h"
#include "kis_paintop_options_model.h"

#include <QHBoxLayout>
#include <QList>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QPainter>
#include <QStyleOptionViewItem>
#include <QAction>
#include <QShowEvent>
#include <QLineEdit>
#include <QLocale>
#include <QPersistentModelIndex>
#include <QPointer>
#include <QtMath>

#include <brushengine/kis_paintop_preset.h>
#include <kis_cmb_composite.h>
#include <kis_categorized_item_delegate.h>
#include <brushengine/kis_locked_properties_server.h>
#include <brushengine/kis_locked_properties_proxy.h>
#include <brushengine/kis_locked_properties.h>
#include <brushengine/kis_paintop_lod_limitations.h>

#include <lager/constant.hpp>
#include <KisLager.h>


struct KisPaintOpSettingsWidget::Private
{
    Private()
    {
    }

    QList<KisPaintOpOption*>    paintOpOptions;
    KisCategorizedListView*     optionsList;
    KisPaintOpOptionListModel*  model;
    QStackedWidget*             optionsStack;
    std::optional<lager::reader<KisPaintopLodLimitations>> lodLimitations;

    /// A row's value dragged sideways: where, from what, and whether it moved enough to be a drag
    QPersistentModelIndex scrubIndex;
    int scrubX {0};
    qreal scrubFrom {0.0};
    bool scrubbing {false};
    bool scrubFine {false};
    QPointer<QLineEdit> valueEditor;

    KisPaintOpOption *optionAt(const QModelIndex &index) const
    {
        KisOptionInfo info;
        return index.isValid() && model->entryAt(info, index) ? info.option : nullptr;
    }

    QRect valueRect(const QModelIndex &index) const
    {
        QStyleOptionViewItem option;
        option.initFrom(optionsList);
        option.widget = optionsList;
        // as the list paints it: across the whole width, not only as wide as the name
        option.rect = optionsList->visualRect(index);
        option.rect.setRight(optionsList->viewport()->width() - 1);
        return KisCategorizedItemDelegate::valueRect(option, index);
    }

    /// The row's value under \p pos, if it can be changed now
    bool valueAt(const QModelIndex &index, const QPoint &pos) const
    {
        // the whole row from its name on, as a layer's opacity; unchecked too, as a hidden layer's
        if (!index.isValid() || !index.data(__CategorizedListModelBase::ValueFractionRole).isValid()) return false;
        return valueRect(index).contains(pos);
    }

    void scrubTo(int x, bool fine)
    {
        KisPaintOpOption *option = optionAt(scrubIndex);
        const std::optional<KisPaintOpOption::RowValue> value = option ? option->rowValue() : std::nullopt;
        if (!value) return;
        // Shift: a quarter as fast, from where it is now
        if (fine != scrubFine) {
            scrubFine = fine;
            scrubFrom = value->value;
            scrubX = x;
        }
        const qreal width = qMax(40, valueRect(scrubIndex).width());
        const qreal moved = (x - scrubX) / width * (fine ? 0.25 : 1.0);
        qreal next;
        if (value->size && value->minimum > 0) {
            next = qMax(scrubFrom, value->minimum) * std::exp(moved * std::log(value->maximum / value->minimum));
        } else {
            next = scrubFrom + moved * (value->maximum - value->minimum);
        }
        const qreal step = std::pow(10.0, -value->decimals);
        next = qBound(value->minimum, std::round(next / step) * step, value->maximum);
        if (!qFuzzyCompare(next, value->value)) option->setRowValue(next);
    }
};

KisPaintOpSettingsWidget::KisPaintOpSettingsWidget(QWidget * parent)
        : KisPaintOpConfigWidget(parent)
        , m_d(new Private())
{
    setObjectName("KisPaintOpPresetsWidget");

    m_d->model       = new KisPaintOpOptionListModel(this);
    m_d->optionsList = new KisCategorizedListView(this);
    m_d->optionsList->setModel(m_d->model);
    m_d->optionsList->setItemDelegate(new KisCategorizedItemDelegate(m_d->optionsList));
    m_d->optionsList->setSizeAdjustPolicy(QAbstractScrollArea::AdjustToContents);
    m_d->optionsList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    QSizePolicy policy =  QSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    m_d->optionsList->setSizePolicy(policy);

    // wide enough for every name ("Lightness Strength"), and no old box around it
    m_d->optionsList->setMinimumWidth(215);
    m_d->optionsList->setFrameShape(QFrame::NoFrame);

    m_d->optionsStack = new QStackedWidget(this);
    policy = QSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_d->optionsStack->setSizePolicy(policy);

    QHBoxLayout* layout = new QHBoxLayout(this);
    layout->addWidget(m_d->optionsList);
    layout->addWidget(m_d->optionsStack);

    layout->setStretch(0, 0);
    layout->setStretch(1, 1);

    m_saveLockedOption = false;

    connect(m_d->optionsList, SIGNAL(activated(QModelIndex)), this, SLOT(changePage(QModelIndex)));
    connect(m_d->optionsList, SIGNAL(clicked(QModelIndex)), this, SLOT(changePage(QModelIndex)));
    connect(m_d->optionsList, SIGNAL(rightClickedMenuDropSettingsTriggered()), this, SLOT(slotLockPropertiesDrop()));
    connect(m_d->optionsList, SIGNAL(rightClickedMenuSaveSettingsTriggered()), this, SLOT(slotLockPropertiesSave()));
    connect(m_d->optionsList, SIGNAL(sigEntryChecked(QModelIndex)), this, SLOT(slotEntryChecked(QModelIndex)));
    connect (m_d->optionsList, SIGNAL(lockAreaTriggered(QModelIndex)), this, SLOT(lockProperties(QModelIndex)));

    // allow the list viewport to be responsive to input release events
    m_d->optionsList->viewport()->installEventFilter(this);
    // a row's value shows it can be dragged
    m_d->optionsList->setMouseTracking(true);
}


KisPaintOpSettingsWidget::~KisPaintOpSettingsWidget()
{
    qDeleteAll(m_d->paintOpOptions);
    delete m_d;
}

void KisPaintOpSettingsWidget::addPaintOpOption(KisPaintOpOption *option)
{
    addPaintOpOption(option, option->category());
}

void KisPaintOpSettingsWidget::addPaintOpOption(KisPaintOpOption *option, KisPaintOpOption::PaintopCategory category)
{
    addPaintOpOption(option, m_d->model->categoryName(category));
}

void KisPaintOpSettingsWidget::addPaintOpOption(KisPaintOpOption *option, QString category)
{
    if (!option->configurationPage()) return;
    m_d->model->addPaintOpOption(option, m_d->optionsStack->count(), option->label(), category);
    connect(option, SIGNAL(sigSettingChanged()), SIGNAL(sigConfigurationItemChanged()));
    m_d->optionsStack->addWidget(option->configurationPage());
    m_d->paintOpOptions << option;

    m_d->lodLimitations =
        kislager::fold_optional_cursors(std::bit_or{}, m_d->lodLimitations,
                                      option->effectiveLodLimitations());
    paintOpOptionAdded(option);
}

void KisPaintOpSettingsWidget::paintOpOptionAdded(KisPaintOpOption *option)
{
    Q_UNUSED(option);
}

void KisPaintOpSettingsWidget::setConfiguration(const KisPropertiesConfigurationSP  config)
{
    Q_ASSERT(!config->getString("paintop").isEmpty());
    KisLockedPropertiesProxySP propertiesProxy = KisLockedPropertiesServer::instance()->createLockedPropertiesProxy(config);
    int indexcount = 0;
    Q_FOREACH (KisPaintOpOption* option, m_d->paintOpOptions) {
        option->startReadOptionSetting(propertiesProxy);

        KisLockedPropertiesServer::instance()->setPropertiesFromLocked(false);
        KisOptionInfo info;
        info.option = option;
        info.index = indexcount;

        m_d->model->categoriesMapper()->itemFromRow(m_d->model->indexOf(info).row())->setLocked(option->isLocked());
        m_d->model->categoriesMapper()->itemFromRow(m_d->model->indexOf(info).row())->setLockable(true);
        m_d->model->signalDataChanged(m_d->model->indexOf(info));
        indexcount++;
    }
}

void KisPaintOpSettingsWidget::writeConfiguration(KisPropertiesConfigurationSP config) const
{
    KisLockedPropertiesProxySP propertiesProxy = KisLockedPropertiesServer::instance()->createLockedPropertiesProxy(config);
    Q_FOREACH (const KisPaintOpOption* option, m_d->paintOpOptions) {
        option->startWriteOptionSetting(propertiesProxy);
    }
}

KisPaintopLodLimitations KisPaintOpSettingsWidget::lodLimitations() const
{
    KisPaintopLodLimitations l;

    Q_FOREACH (const KisPaintOpOption* option, m_d->paintOpOptions) {
        if (option->isCheckable() && !option->isChecked()) continue;
        option->lodLimitations(&l);
    }

    return l;
}

lager::reader<KisPaintopLodLimitations> KisPaintOpSettingsWidget::lodLimitationsReader() const
{
    return m_d->lodLimitations.value_or(lager::make_constant(KisPaintopLodLimitations()));
}

lager::reader<qreal> KisPaintOpSettingsWidget::effectiveBrushSize() const
{
    return lager::make_constant(1.0);
}

void KisPaintOpSettingsWidget::setNode(KisNodeWSP node)
{
    KisPaintOpConfigWidget::setNode(node);

    Q_FOREACH (KisPaintOpOption* option, m_d->paintOpOptions) {
        option->setNode(node);
    }
}

void KisPaintOpSettingsWidget::setResourcesInterface(KisResourcesInterfaceSP resourcesInterface)
{
    KisPaintOpConfigWidget::setResourcesInterface(resourcesInterface);

    Q_FOREACH (KisPaintOpOption* option, m_d->paintOpOptions) {
        option->setResourcesInterface(resourcesInterface);
    }
}

void KisPaintOpSettingsWidget::setCanvasResourcesInterface(KoCanvasResourcesInterfaceSP canvasResourcesInterface)
{
    KisPaintOpConfigWidget::setCanvasResourcesInterface(canvasResourcesInterface);

    Q_FOREACH (KisPaintOpOption* option, m_d->paintOpOptions) {
        option->setCanvasResourcesInterface(canvasResourcesInterface);
    }
}

void KisPaintOpSettingsWidget::changePage(const QModelIndex& index)
{
    KisOptionInfo info;
    QPalette palette;
    palette.setColor(QPalette::Base, QColor(255,200,200));
    palette.setColor(QPalette::Text, Qt::black);

    if(m_d->model->entryAt(info, index)) {
        m_d->optionsStack->setCurrentIndex(info.index);
    }

    notifyPageChanged();
}

void KisPaintOpSettingsWidget::notifyPageChanged()
{
}

void KisPaintOpSettingsWidget::lockProperties(const QModelIndex& index)
{
    KisOptionInfo info;
    if (m_d->model->entryAt(info, index)) {
        m_d->optionsList->setCurrentIndex(index);
        KisPropertiesConfigurationSP p = new KisPropertiesConfiguration();
        info.option->startWriteOptionSetting(p);

        if (!info.option->isLocked()){
            KisLockedPropertiesServer::instance()->addToLockedProperties(p);
            info.option->setLocked(true);
            m_d->model->categoriesMapper()->itemFromRow(index.row())->setLocked(true);
            m_d->model->signalDataChanged(index);

        }
        else {
            KisLockedPropertiesServer::instance()->removeFromLockedProperties(p);
            info.option->setLocked(false);
            m_d->model->categoriesMapper()->itemFromRow(index.row())->setLocked(false);
            m_d->model->signalDataChanged(index);

            if (m_saveLockedOption){
                Q_EMIT sigSaveLockedConfig(p);
            }
            else {
                Q_EMIT sigDropLockedConfig(p);
            }
            m_saveLockedOption = false;
        }
    }
}
void KisPaintOpSettingsWidget::slotLockPropertiesDrop()
{
    m_saveLockedOption = false;
    lockProperties(m_d->optionsList->currentIndex());
}
void KisPaintOpSettingsWidget::slotLockPropertiesSave()
{
    m_saveLockedOption = true;
    lockProperties(m_d->optionsList->currentIndex());
}
void KisPaintOpSettingsWidget::slotEntryChecked(const QModelIndex &index)
{
    Q_UNUSED(index);
    Q_EMIT sigConfigurationItemChanged();
}

void KisPaintOpSettingsWidget::editRowValue(const QModelIndex &index)
{
    KisPaintOpOption *option = m_d->optionAt(index);
    const std::optional<KisPaintOpOption::RowValue> value = option ? option->rowValue() : std::nullopt;
    if (!value) return;
    delete m_d->valueEditor;
    QLineEdit *editor = new QLineEdit(m_d->optionsList->viewport());
    m_d->valueEditor = editor;
    editor->setFrame(false);
    editor->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    // solid: the row's value under it doesn't show through
    QPalette solid = editor->palette();
    QColor base = palette().color(QPalette::Window);
    base.setAlpha(255);
    solid.setColor(QPalette::Base, base);
    editor->setPalette(solid);
    editor->setAutoFillBackground(true);
    editor->setText(QLocale().toString(value->value, 'f', value->decimals));
    editor->setGeometry(m_d->valueRect(index));
    editor->selectAll();
    editor->show();
    editor->setFocus();
    QPointer<KisPaintOpOption> target(option);
    const KisPaintOpOption::RowValue range = *value;
    connect(editor, &QLineEdit::returnPressed, this, [editor, target, range]() {
        QString text = editor->text().trimmed();
        if (!range.suffix.trimmed().isEmpty()) text.remove(range.suffix.trimmed());
        bool ok = false;
        const qreal typed = QLocale().toDouble(text.trimmed(), &ok);
        if (ok && target) target->setRowValue(qBound(range.minimum, typed, range.maximum));
    });
    // Enter, Escape or a click elsewhere: gone
    connect(editor, &QLineEdit::editingFinished, editor, &QObject::deleteLater);
}

bool KisPaintOpSettingsWidget::eventFilter(QObject *obj, QEvent *event)
{
    if (obj == m_d->optionsList->viewport()) {
        auto *mouse = dynamic_cast<QMouseEvent *>(event);
        switch (event->type()) {
        case QEvent::MouseButtonPress:
            m_d->scrubbing = false;
            m_d->scrubIndex = QPersistentModelIndex();
            if (mouse->button() == Qt::LeftButton) {
                const QModelIndex index = m_d->optionsList->indexAt(mouse->pos());
                if (m_d->valueAt(index, mouse->pos())) {
                    m_d->scrubIndex = index;
                    m_d->scrubX = mouse->pos().x();
                    m_d->scrubFrom = m_d->optionAt(index)->rowValue()->value;
                    m_d->scrubFine = mouse->modifiers() & Qt::ShiftModifier;
                }
            }
            break;
        case QEvent::MouseMove:
            if (m_d->scrubIndex.isValid() && (mouse->buttons() & Qt::LeftButton)) {
                if (!m_d->scrubbing && qAbs(mouse->pos().x() - m_d->scrubX) > 3) {
                    m_d->scrubbing = true;
                    m_d->scrubX = mouse->pos().x();
                }
                if (m_d->scrubbing) {
                    m_d->scrubTo(mouse->pos().x(), mouse->modifiers() & Qt::ShiftModifier);
                    return true;
                }
            } else if (!mouse->buttons()) {
                const QModelIndex index = m_d->optionsList->indexAt(mouse->pos());
                if (m_d->valueAt(index, mouse->pos())) {
                    m_d->optionsList->viewport()->setCursor(Qt::SizeHorCursor);
                } else {
                    m_d->optionsList->viewport()->unsetCursor();
                }
            }
            break;
        case QEvent::MouseButtonDblClick:
            if (mouse->button() == Qt::LeftButton) {
                const QModelIndex index = m_d->optionsList->indexAt(mouse->pos());
                if (m_d->valueAt(index, mouse->pos())) {
                    // typed: a value exactly
                    editRowValue(index);
                    return true;
                }
            }
            break;
        case QEvent::Leave:
            m_d->optionsList->viewport()->unsetCursor();
            break;
        default:
            break;
        }

        if (event->type() == QEvent::MouseButtonRelease && m_d->scrubbing) {
            // a drag of the value ends: its page shown, nothing else
            m_d->scrubbing = false;
            const QModelIndex index = m_d->scrubIndex;
            m_d->scrubIndex = QPersistentModelIndex();
            m_d->optionsList->setCurrentIndex(index);
            changePage(index);
            return true;
        }
    }

    if (event->type() == (QEvent::MouseButtonRelease) || event->type() == QEvent::TabletRelease) {
        // bit of a hack to properly work with the KisCategorizedListView type:
        // get the mouse position and utilize indexAt() instead of currentIndex() to
        // ensure the pages don't swap back and forth if the user repeatedly clicks
        // another option's checkbox too quickly
        const QMouseEvent* mevent = static_cast<const QMouseEvent*>(event);
        QModelIndex index = m_d->optionsList->indexAt(mevent->pos());
        changePage(index);
        event->accept();
    }

    return QObject::eventFilter(obj, event);
}
