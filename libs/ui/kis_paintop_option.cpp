/* This file is part of the KDE project
 * SPDX-FileCopyrightText: 2008 Boudewijn Rempt <boud@valdyas.org>
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#include "kis_paintop_option.h"

#include <KisSentenceCase.h>


#include <QWidget>

#include <klocalizedstring.h>

#include <KisResourcesInterface.h>
#include <KoCanvasResourcesInterface.h>

#include <lager/constant.hpp>
#include <lager/state.hpp>
#include <kis_paintop_lod_limitations.h>

struct KisPaintOpOption::Private
{
public:
    lager::state<bool, lager::automatic_tag> checkedFallback;
    lager::cursor<bool> checkedCursor;
    lager::reader<bool> externallyEnabledReader;
    lager::reader<bool> pageEnabledReader;
    QString label;
    KisPaintOpOption::PaintopCategory category;

    /// the configuration page is owned by the higher level
    /// QStackWidget, so we shouldn't care about that
    QWidget *configurationPage {nullptr};
    /// What the option's checkbox enables in the page: the page, unless set apart
    QWidget *checkedPart {nullptr};
    QWidget *enabledByCheck() const { return checkedPart ? checkedPart : configurationPage; }

    bool updatesBlocked {false};
    bool isWritingSettings {false};

    KisResourcesInterfaceSP resourcesInterface;
    KoCanvasResourcesInterfaceSP canvasResourcesInterface;

    std::function<std::optional<RowValue>()> rowValueGet;
    std::function<void(qreal)> rowValueSet;
    std::function<void(bool)> rowAutomaticSet;
};

KisPaintOpOption::KisPaintOpOption(const QString &label, PaintopCategory category, bool checked)
    : m_checkable(true)
    , m_d(new Private())

{
    m_d->label = label;
    m_d->checkedFallback.set(checked);
    m_d->checkedCursor = m_d->checkedFallback;
    m_d->externallyEnabledReader = lager::make_constant(true);
    m_d->pageEnabledReader = m_d->checkedCursor;
    m_d->category = category;
    m_d->pageEnabledReader.bind(std::bind(&KisPaintOpOption::slotEnablePageWidget, this, std::placeholders::_1));
    m_d->checkedCursor.bind(std::bind(&KisPaintOpOption::sigCheckedChanged, this, std::placeholders::_1));
    m_d->externallyEnabledReader.bind(std::bind(&KisPaintOpOption::sigEnabledChanged, this, std::placeholders::_1));

}

KisPaintOpOption::KisPaintOpOption(const QString &label, KisPaintOpOption::PaintopCategory category,
                                   lager::cursor<bool> checkedCursor)
    : KisPaintOpOption(label, category, checkedCursor, lager::make_constant(true))
{

}

KisPaintOpOption::KisPaintOpOption(const QString &label, KisPaintOpOption::PaintopCategory category,
                                   lager::cursor<bool> checkedCursor,
                                   lager::reader<bool> externallyEnabledLink)
    : m_checkable(true)
    , m_d(new Private())
{
    m_d->label = label;
    m_d->checkedCursor = checkedCursor;
    m_d->externallyEnabledReader = externallyEnabledLink;
    m_d->pageEnabledReader = lager::with(m_d->checkedCursor, m_d->externallyEnabledReader).map(std::logical_and{});
    m_d->category = category;
    m_d->pageEnabledReader.bind(std::bind(&KisPaintOpOption::slotEnablePageWidget, this, std::placeholders::_1));
    m_d->checkedCursor.bind(std::bind(&KisPaintOpOption::sigCheckedChanged, this, std::placeholders::_1));
    m_d->externallyEnabledReader.bind(std::bind(&KisPaintOpOption::sigEnabledChanged, this, std::placeholders::_1));
}

KisPaintOpOption::~KisPaintOpOption()
{
    delete m_d;
}

void KisPaintOpOption::emitSettingChanged()
{
    KIS_ASSERT_RECOVER_RETURN(!m_d->isWritingSettings);

    if (!m_d->updatesBlocked) {
        Q_EMIT sigSettingChanged();
    }
}

void KisPaintOpOption::emitCheckedChanged(bool checked)
{
    KIS_ASSERT_RECOVER_RETURN(!m_d->isWritingSettings);

    if (!m_d->updatesBlocked) {
        Q_EMIT sigCheckedChanged(checked);
    }
}

void KisPaintOpOption::emitEnabledChanged(bool enabled)
{
    KIS_ASSERT_RECOVER_RETURN(!m_d->isWritingSettings);

    if (!m_d->updatesBlocked) {
        Q_EMIT sigEnabledChanged(enabled);
    }
}

void KisPaintOpOption::startReadOptionSetting(const KisPropertiesConfigurationSP setting)
{
    m_d->updatesBlocked = true;
    readOptionSetting(setting);
    m_d->updatesBlocked = false;
}

void KisPaintOpOption::startWriteOptionSetting(KisPropertiesConfigurationSP setting) const
{
    m_d->isWritingSettings = true;
    writeOptionSetting(setting);
    m_d->isWritingSettings = false;
}

void KisPaintOpOption::lodLimitations(KisPaintopLodLimitations *l) const
{
    Q_UNUSED(l);
}

KisPaintOpOption::OptionalLodLimitationsReader KisPaintOpOption::effectiveLodLimitations() const
{
    OptionalLodLimitationsReader reader = lodLimitationsReader();

    if (reader) {
        reader = lager::with(m_d->pageEnabledReader, *reader)
            .map([](bool enabled, const KisPaintopLodLimitations &l) {
                return enabled ? l : KisPaintopLodLimitations();
            });
    }

    return reader;
}

KisPaintOpOption::OptionalLodLimitationsReader KisPaintOpOption::lodLimitationsReader() const
{
    // no limitations by default
    return std::nullopt;
}

std::optional<KisPaintOpOption::RowValue> KisPaintOpOption::rowValue() const
{
    return m_d->rowValueGet ? m_d->rowValueGet() : ownRowValue();
}

void KisPaintOpOption::setRowValue(qreal value)
{
    if (m_d->rowValueGet) {
        if (m_d->rowValueSet) m_d->rowValueSet(value);
    } else {
        setOwnRowValue(value);
    }
}

void KisPaintOpOption::setRowValueSource(std::function<std::optional<RowValue>()> get, std::function<void(qreal)> set,
                                         std::function<void(bool)> setAutomatic)
{
    m_d->rowValueGet = get;
    m_d->rowValueSet = set;
    m_d->rowAutomaticSet = setAutomatic;
    notifyRowValueChanged();
}

void KisPaintOpOption::setRowAutomatic(bool on)
{
    if (m_d->rowAutomaticSet) m_d->rowAutomaticSet(on);
}

void KisPaintOpOption::notifyRowValueChanged()
{
    Q_EMIT sigRowValueChanged();
}

QString KisPaintOpOption::id() const
{
    return objectName().isEmpty() ? label() : objectName();
}

QVector<KisPaintOpOption::PenInput> KisPaintOpOption::penInputs() const
{
    return {};
}

void KisPaintOpOption::setPenInputActive(const QString &id, bool active)
{
    Q_UNUSED(id);
    Q_UNUSED(active);
}

void KisPaintOpOption::setPenCurve(const QString &id, const QString &curve)
{
    Q_UNUSED(id);
    Q_UNUSED(curve);
}

int KisPaintOpOption::penMode() const
{
    return -1;
}

void KisPaintOpOption::setPenMode(int mode)
{
    Q_UNUSED(mode);
}

QStringList KisPaintOpOption::penModeNames()
{
    // the order of Krita's curve modes
    return {i18nc("pen inputs combined", "Multiply"), i18nc("pen inputs combined", "Add"),
            i18nc("pen inputs combined", "Max"), i18nc("pen inputs combined", "Min"),
            i18nc("pen inputs combined", "Difference")};
}

QWidget *KisPaintOpOption::createPenInputPage(const QString &id, QWidget *parent)
{
    Q_UNUSED(id);
    Q_UNUSED(parent);
    return nullptr;
}

QWidget *KisPaintOpOption::createQuickPage(PaintopCategory category, QWidget *parent)
{
    Q_UNUSED(category);
    Q_UNUSED(parent);
    return nullptr;
}

bool KisPaintOpOption::hasQuickPage(PaintopCategory category) const
{
    Q_UNUSED(category);
    return false;
}

std::optional<KisPaintOpOption::RowValue> KisPaintOpOption::ownRowValue() const
{
    return std::nullopt;
}

void KisPaintOpOption::setOwnRowValue(qreal value)
{
    Q_UNUSED(value);
}

KisPaintOpOption::PaintopCategory KisPaintOpOption::category() const
{
    return m_d->category;
}

void KisPaintOpOption::setCategory(KisPaintOpOption::PaintopCategory category)
{
    m_d->category = category;
}

bool KisPaintOpOption::isChecked() const
{
    return m_d->checkedCursor.get();
}

bool KisPaintOpOption::isCheckable() const {
    return m_checkable;
}

void KisPaintOpOption::setChecked(bool checked)
{
    m_d->checkedCursor.set(checked);

    emitSettingChanged();
}

bool KisPaintOpOption::isEnabled() const
{
    return m_d->externallyEnabledReader.get();
}

void KisPaintOpOption::setImage(KisImageWSP image)
{
    Q_UNUSED(image);
}

void KisPaintOpOption::setNode(KisNodeWSP node)
{
    Q_UNUSED(node);
}

void KisPaintOpOption::setResourcesInterface(KisResourcesInterfaceSP resourcesInterface)
{
    m_d->resourcesInterface = resourcesInterface;
}

void KisPaintOpOption::setCanvasResourcesInterface(KoCanvasResourcesInterfaceSP canvasResourcesInterface)
{
    m_d->canvasResourcesInterface = canvasResourcesInterface;
}

KisResourcesInterfaceSP KisPaintOpOption::resourcesInterface() const
{
    return m_d->resourcesInterface;
}

KoCanvasResourcesInterfaceSP KisPaintOpOption::canvasResourcesInterface() const
{
    return m_d->canvasResourcesInterface;
}

void KisPaintOpOption::setConfigurationPage(QWidget * page)
{
    if (m_d->configurationPage && !m_d->checkedCursor.get()) {
        m_d->configurationPage->setEnabled(true);
    }

    m_d->configurationPage = page;

    if (m_d->enabledByCheck()) {
        m_d->enabledByCheck()->setEnabled(m_d->checkedCursor.get());
    }
}

void KisPaintOpOption::setCheckedPart(QWidget *part)
{
    m_d->checkedPart = part;
}

QWidget* KisPaintOpOption::configurationPage() const
{
    return m_d->configurationPage;
}

void KisPaintOpOption::slotEnablePageWidget(bool value)
{
    if (m_d->enabledByCheck()) {
        m_d->enabledByCheck()->setEnabled(value);
    }
}

void KisPaintOpOption::setLocked(bool value)
{
    m_locked = value;
}

bool KisPaintOpOption::isLocked ()const
{
    return m_locked;
}

QString KisPaintOpOption::label() const
{
    // engines name their settings in Title Case ("Snap To Pixel")
    return KisSentenceCase::of(m_d->label);
}
