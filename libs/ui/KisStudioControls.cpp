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

#include "KisStudioControls.moc"
