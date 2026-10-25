/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioControls.h"

#include <QApplication>
#include <QDrag>
#include <QDragEnterEvent>
#include <QFrame>
#include <QLabel>
#include <QMenu>
#include <QMimeData>
#include <QPointer>
#include <QTimer>
#include <QPainter>
#include <QToolButton>
#include <QVBoxLayout>

#include <klocalizedstring.h>

#include <KisModernStyle.h>
#include <KoDockRegistry.h>
#include <kis_icon_utils.h>

#include "KisMainWindow.h"
#include "KisSettingsRegistry.h"
#include "KisStudioBrushEditor.h"
#include "KisStudioItemCard.h"
#include "KisStudioPenPopup.h"
#include "KisViewManager.h"
#include "kis_config.h"
#include "kis_paintop_box.h"
#include "kis_paintop_option.h"
#include "widgets/KisStudioPicker.h"
#include "widgets/KisStudioValueRow.h"

using R = KisSettingsRegistry;

namespace {

const QString brushPrefix = QStringLiteral("brushsetting:");
const QString settingPrefix = QStringLiteral("setting:");
const QString itemsKey = QStringLiteral("StudioControlItems");

/// A brush setting: its row, for the current brush; a click shows how the pen drives it
class BrushControl : public KisStudioValueRow
{
    Q_OBJECT
public:
    BrushControl(KisMainWindow *mainWindow, KisPaintOpOption *option, QWidget *parent)
        : KisStudioValueRow(option->label(), parent)
    {
        // the settings follow the brush while the row is there: changed by keys or on the canvas, it shows that
        m_box = mainWindow->viewManager() ? mainWindow->viewManager()->paintOpBox() : nullptr;
        if (m_box) m_box->useOptionWidget(true);
        KisStudioBrushEditor::bindRow(this, option, KisStudioBrushEditor::brushSettings(mainWindow));
        const QPointer<KisPaintOpOption> target(option);
        connect(this, &KisStudioValueRow::typingStarted, this, [this]() {
            if (m_pen) m_pen->close();
        });
        connect(this, &KisStudioValueRow::clicked, this, [this, mainWindow, target]() {
            if (!target || target->penInputs().isEmpty()) return;
            if (m_pen) {
                m_pen->close();
                return;
            }
            m_pen = new KisStudioPenPopup(target, mainWindow);
            setSelected(true);
            connect(m_pen, &QObject::destroyed, this, [this]() { setSelected(false); });
            m_pen->popupBeside(this, Qt::BottomEdge);
        });
    }

    ~BrushControl() override
    {
        if (m_pen) m_pen->close();
        if (m_box) m_box->useOptionWidget(false);
    }

private:
    QPointer<KisStudioPenPopup> m_pen;
    QPointer<KisPaintopBox> m_box;
};

/// One of Krita's settings: a number drags, a switch toggles, a choice opens its choices
class SettingControl : public KisStudioValueRow
{
    Q_OBJECT
public:
    SettingControl(const QString &id, QWidget *parent)
        : KisStudioValueRow(QString(), parent)
        , m_id(id)
    {
        const R::Setting *setting = R::instance()->setting(id);
        setName(setting->label);
        setToolTip(setting->help);
        const bool number = setting->kind == R::Kind::Number;
        setDraggable(number);
        // a fraction (0 to a few) reads as percent, as the settings show it
        const bool real = setting->fallback.type() == QVariant::Double;
        m_scale = real && setting->max <= 4 ? 100.0 : 1.0;
        if (number) {
            Range range;
            range.minimum = setting->min * m_scale;
            range.maximum = setting->max * m_scale;
            range.decimals = real && m_scale == 1.0 ? 2 : 0;
            if (m_scale != 1.0) range.suffix = i18nc("percent, after the number", "%");
            setRange(range);
            setDefaultValue(R::instance()->defaultValue(id).toDouble() * m_scale);
        }
        connect(this, &KisStudioValueRow::valueChanged, this, [this, real](qreal value) {
            const R::Setting *setting = R::instance()->setting(m_id);
            if (!setting || setting->kind != R::Kind::Number) return;
            value /= m_scale;
            R::instance()->setValue(m_id, real ? QVariant(value) : QVariant(qRound(value)));
        });
        connect(this, &KisStudioValueRow::clicked, this, &SettingControl::clickedOn);
        connect(R::instance(), &R::valueChanged, this, [this](const QString &changed) {
            if (changed == m_id) refresh();
        });
        refresh();
    }

protected:
    void contextMenuEvent(QContextMenuEvent *event) override
    {
        // a right click resets it, whatever its kind
        R::instance()->reset(m_id);
        event->accept();
    }

private:
    void refresh()
    {
        const R::Setting *setting = R::instance()->setting(m_id);
        if (!setting) return;
        const QVariant value = R::instance()->value(m_id);
        if (setting->kind == R::Kind::Number) {
            setValue(value.toDouble() * m_scale);
            setValueText(setting->format ? setting->format(value) : QString());
        } else {
            setValueText(R::instance()->display(*setting, value));
            if (setting->kind == R::Kind::Toggle) setInactive(!value.toBool());
        }
    }

    void clickedOn()
    {
        const R::Setting *setting = R::instance()->setting(m_id);
        if (!setting) return;
        if (setting->kind == R::Kind::Toggle) {
            R::instance()->setValue(m_id, !R::instance()->value(m_id).toBool());
        } else if (setting->kind == R::Kind::Choice) {
            auto *menu = new QMenu(this);
            menu->setAttribute(Qt::WA_DeleteOnClose);
            const QVariant current = R::instance()->value(m_id);
            const QString id = m_id;
            Q_FOREACH (const R::Choice &choice, R::instance()->choices(*setting)) {
                QAction *action = menu->addAction(choice.text);
                action->setCheckable(true);
                action->setChecked(R::instance()->sameValue(*setting, choice.value, current));
                const QVariant value = choice.value;
                connect(action, &QAction::triggered, this, [id, value]() { R::instance()->setValue(id, value); });
            }
            menu->popup(mapToGlobal(QPoint(0, height())));
        }
    }

    QString m_id;
    qreal m_scale {1.0};
};

} // namespace

KisStudioValueRow *KisStudioControls::create(KisMainWindow *mainWindow, const QString &key, QWidget *parent)
{
    if (key.startsWith(brushPrefix)) {
        KisPaintOpOption *option = KisStudioBrushEditor::brushOption(mainWindow, key.mid(brushPrefix.size()));
        return option ? new BrushControl(mainWindow, option, parent) : nullptr;
    }
    if (key.startsWith(settingPrefix)) {
        const QString id = key.mid(settingPrefix.size());
        return canPinSetting(id) ? new SettingControl(id, parent) : nullptr;
    }
    return nullptr;
}

bool KisStudioControls::isControl(const QString &key)
{
    return key.startsWith(brushPrefix) || (key.startsWith(settingPrefix) && canPinSetting(key.mid(settingPrefix.size())));
}

QString KisStudioControls::settingKey(const QString &id)
{
    return settingPrefix + id;
}

bool KisStudioControls::canPinSetting(const QString &id)
{
    const R::Setting *setting = R::instance()->setting(id);
    if (!setting || (setting->flags & R::Restart)) return false;
    return setting->kind == R::Kind::Toggle || setting->kind == R::Kind::Number || setting->kind == R::Kind::Choice;
}

struct KisStudioControlsPanel::Private
{
    QStringList items;
    QWidget *body {nullptr};
    QVBoxLayout *layout {nullptr};
    QFrame *indicator {nullptr};
    QLabel *empty {nullptr};
    QToolButton *add {nullptr};
    QList<QWidget *> rows;
    /// The item each row is (a pinned setting this brush lacks has none)
    QVector<int> rowItems;
    QPointer<KisMainWindow> mainWindow;
};

KisStudioControlsPanel::KisStudioControlsPanel()
    : d(new Private)
{
    setWindowTitle(i18nc("@title:window the panel of the settings the user collected", "Controls"));
    d->items = KisConfig(true).readEntry<QStringList>(itemsKey, QStringList());
    d->body = new QWidget(this);
    d->layout = new QVBoxLayout(d->body);
    d->layout->setContentsMargins(KisModernStyle::space2, KisModernStyle::space2, KisModernStyle::space2, KisModernStyle::space2);
    d->layout->setSpacing(KisModernStyle::space1);
    setWidget(d->body);
    setAcceptDrops(true);

    d->indicator = new QFrame(d->body);
    d->indicator->setAutoFillBackground(true);
    d->indicator->setBackgroundRole(QPalette::Highlight);
    d->indicator->hide();
}

KisStudioControlsPanel::~KisStudioControlsPanel()
{
}

QString KisStudioControlsPanel::id()
{
    return QStringLiteral("StudioControls");
}

void KisStudioControlsPanel::registerFactory()
{
    if (!KoDockRegistry::instance()->contains(id())) {
        KoDockRegistry::instance()->add(new KisStudioControlsPanelFactory);
    }
}

QStringList KisStudioControlsPanel::items() const
{
    return d->items;
}

void KisStudioControlsPanel::setItems(const QStringList &items)
{
    d->items = items;
    d->items.removeDuplicates();
    KisConfig(false).writeEntry(itemsKey, d->items);
    rebuild();
}

void KisStudioControlsPanel::showEvent(QShowEvent *event)
{
    QDockWidget::showEvent(event);
    auto *mainWindow = qobject_cast<KisMainWindow *>(window());
    if (mainWindow && mainWindow != d->mainWindow) {
        d->mainWindow = mainWindow;
        // the brush's settings follow the brush
        if (KisPaintopBox *box = mainWindow->viewManager() ? mainWindow->viewManager()->paintOpBox() : nullptr) {
            connect(box, &KisPaintopBox::sigOptionWidgetChanged, this, &KisStudioControlsPanel::rebuild, Qt::UniqueConnection);
        }
    }
    rebuild();
}

void KisStudioControlsPanel::rebuild()
{
    Q_FOREACH (QWidget *row, d->rows) {
        row->hide();
        row->deleteLater();
    }
    d->rows.clear();
    d->rowItems.clear();
    while (QLayoutItem *item = d->layout->takeAt(0)) {
        delete item;
    }
    if (!d->empty) {
        d->empty = new QLabel(i18nc("@info an empty list", "No controls yet"), d->body);
        d->empty->setAlignment(Qt::AlignCenter);
        d->empty->setEnabled(false);
    }

    for (int i = 0; i < d->items.size(); i++) {
        const QString key = d->items[i];
        KisStudioValueRow *row = d->mainWindow ? KisStudioControls::create(d->mainWindow, key, d->body) : nullptr;
        if (!row) continue;
        row->setPinnable(true, true);
        row->setPinOnHoverOnly(true);
        connect(row, &KisStudioValueRow::pinClicked, this, [this, key]() {
            QTimer::singleShot(0, this, [this, key]() {
                QStringList items = d->items;
                items.removeAll(key);
                setItems(items);
            });
        });
        connect(row, &KisStudioValueRow::pulledOut, this, [this, key, row]() {
            // the rows may be rebuilt while it runs: the drag isn't the row's
            auto *drag = new QDrag(qApp);
            auto *mime = new QMimeData();
            mime->setData(KisStudioShortcuts::itemMimeType(), key.toUtf8());
            drag->setMimeData(mime);
            drag->setPixmap(row->grab());
            drag->setHotSpot(QPoint(row->width() / 2, row->height() / 2));
            // moved elsewhere or dropped off: it leaves the panel, unless the drag was cancelled with Esc
            const Qt::DropAction done = drag->exec(Qt::MoveAction | Qt::CopyAction);
            drag->deleteLater();
            if (done == Qt::IgnoreAction && KisModernStyle::pointerHeld()) return;
            if (done == Qt::MoveAction && property("droppedHere").toBool()) return;
            QTimer::singleShot(0, this, [this, key]() {
                QStringList items = d->items;
                items.removeAll(key);
                setItems(items);
            });
        });
        d->layout->addWidget(row);
        d->rows << row;
        d->rowItems << i;
    }
    d->empty->setVisible(d->rows.isEmpty());
    d->layout->addWidget(d->empty);

    // any of Krita's settings, found by name
    if (!d->add) {
        d->add = new QToolButton(d->body);
        d->add->setAutoRaise(true);
        d->add->setIcon(KisIconUtils::loadIcon(QStringLiteral("studio_add")));
        d->add->setToolTip(i18nc("@info:tooltip", "Add a setting"));
        connect(d->add, &QToolButton::clicked, this, &KisStudioControlsPanel::pickSetting);
    }
    d->layout->addWidget(d->add, 0, Qt::AlignLeft);
    d->layout->addStretch(1);
}

void KisStudioControlsPanel::pickSetting()
{
    QStringList ids;
    Q_FOREACH (const R::Setting &setting, R::instance()->settings()) {
        if (KisStudioControls::canPinSetting(setting.id) && !setting.label.isEmpty()) ids << setting.id;
    }
    KisStudioPicker::Source source;
    source.pinnedKey = QStringLiteral("StudioControlsPicked");
    source.initialPinned = []() { return QStringList(); };
    source.allIds = [ids]() { return ids; };
    source.otherGroups = [ids](const QStringList &pinned) {
        KisStudioPicker::Group group{i18nc("a group of settings", "Settings"), {}};
        Q_FOREACH (const QString &id, ids) {
            if (!pinned.contains(id)) group.ids << id;
        }
        return QVector<KisStudioPicker::Group>{group};
    };
    source.name = [](const QString &id) {
        const R::Setting *setting = R::instance()->setting(id);
        return setting ? setting->label : id;
    };
    source.paintPreview = [this](QPainter &painter, const QRect &rect, const QString &id) {
        // what kind of control it becomes: a switch, a bar, a choice
        const R::Setting *setting = R::instance()->setting(id);
        if (!setting) return;
        QColor ink = palette().color(QPalette::WindowText);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(QPen(KisModernStyle::secondaryInk(ink), 1.5));
        painter.setBrush(Qt::NoBrush);
        const QRectF box = QRectF(rect).adjusted(3, rect.height() / 2.0 - 5, -3, -(rect.height() / 2.0 - 5));
        if (setting->kind == R::Kind::Toggle) {
            painter.drawRoundedRect(box, 5, 5);
            painter.drawEllipse(QPointF(box.right() - 5, box.center().y()), 3, 3);
        } else if (setting->kind == R::Kind::Number) {
            painter.drawLine(QPointF(box.left(), box.center().y()), QPointF(box.right(), box.center().y()));
            painter.drawEllipse(QPointF(box.center().x(), box.center().y()), 3, 3);
        } else {
            painter.drawLine(QPointF(box.center().x() - 4, box.center().y() - 2), QPointF(box.center().x(), box.center().y() + 2));
            painter.drawLine(QPointF(box.center().x(), box.center().y() + 2), QPointF(box.center().x() + 4, box.center().y() - 2));
        }
    };
    source.recent = []() { return QStringList(); };
    source.yoursTitle = i18nc("a group of settings", "Yours");
    source.searchPlaceholder = i18nc("@info:placeholder", "Search %1 settings", ids.size());
    auto *picker = new KisStudioPicker(source, QString(), {}, window());
    connect(picker, &KisStudioPicker::sigPicked, this, [this](const QString &id) {
        setItems(d->items + QStringList{KisStudioControls::settingKey(id)});
    });
    picker->popupBelow(d->add);
}

int KisStudioControlsPanel::dropIndex(int y) const
{
    for (int r = 0; r < d->rows.size(); r++) {
        if (y < d->rows[r]->geometry().center().y()) return d->rowItems[r];
    }
    return d->items.size();
}

void KisStudioControlsPanel::dragEnterEvent(QDragEnterEvent *event)
{
    const QString key = QString::fromUtf8(event->mimeData()->data(KisStudioShortcuts::itemMimeType()));
    if (KisStudioControls::isControl(key)) event->acceptProposedAction();
}

void KisStudioControlsPanel::dragMoveEvent(QDragMoveEvent *event)
{
    const QString key = QString::fromUtf8(event->mimeData()->data(KisStudioShortcuts::itemMimeType()));
    if (!KisStudioControls::isControl(key)) {
        event->ignore();
        return;
    }
    event->acceptProposedAction();
    const QPoint at = d->body->mapFrom(this, event->pos());
    const int index = dropIndex(at.y());
    int y = d->rows.isEmpty() ? KisModernStyle::space2 : d->rows.last()->geometry().bottom() + 2;
    for (int r = 0; r < d->rows.size(); r++) {
        if (d->rowItems[r] >= index) {
            y = d->rows[r]->geometry().top() - 2;
            break;
        }
    }
    d->indicator->setGeometry(KisModernStyle::space2, y - 1, d->body->width() - 2 * KisModernStyle::space2, 2);
    d->indicator->show();
    d->indicator->raise();
}

void KisStudioControlsPanel::dragLeaveEvent(QDragLeaveEvent *)
{
    d->indicator->hide();
}

void KisStudioControlsPanel::dropEvent(QDropEvent *event)
{
    d->indicator->hide();
    const QString key = QString::fromUtf8(event->mimeData()->data(KisStudioShortcuts::itemMimeType()));
    if (!KisStudioControls::isControl(key)) return;
    int to = dropIndex(d->body->mapFrom(this, event->pos()).y());
    QStringList items = d->items;
    const int from = items.indexOf(key);
    if (from >= 0) {
        items.removeAt(from);
        if (to > from) to--;
    }
    items.insert(qBound(0, to, items.size()), key);
    // a row moved within the panel stays; one from the bar leaves it
    setProperty("droppedHere", from >= 0);
    event->setDropAction(Qt::MoveAction);
    event->accept();
    QTimer::singleShot(0, this, [this, items]() {
        setItems(items);
        setProperty("droppedHere", false);
    });
}

QString KisStudioControlsPanelFactory::id() const
{
    return KisStudioControlsPanel::id();
}

QDockWidget *KisStudioControlsPanelFactory::createDockWidget()
{
    auto *panel = new KisStudioControlsPanel();
    panel->setObjectName(id());
    return panel;
}

#include "KisStudioControls.moc"
