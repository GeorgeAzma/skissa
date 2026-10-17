/* This file is part of the KDE project
 * SPDX-FileCopyrightText: 2008 Boudewijn Rempt <boud@valdyas.org>
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
 */

#ifndef KIS_PAINTOP_OPTION_H
#define KIS_PAINTOP_OPTION_H

#include <kis_types.h>
#include <kritaui_export.h>
#include <kis_properties_configuration.h>
#include <brushengine/kis_locked_properties_proxy.h>
#include <KisPaintopPropertiesBase.h>

#include <lager/reader.hpp>
#include <lager/cursor.hpp>

#include <functional>
#include <optional>

#include <QVector>
#include <QStringList>

class QWidget;
class QString;
class KisPaintopLodLimitations;


/**
 * Base interface for paintop options. A paintop option
 * can be enabled/disabled, has a configuration page
 * (for example, a curve), a user-visible name and can
 * be serialized and deserialized into KisPaintOpPresets
 *
 * Because KisPaintOpOption classes create a QWidget in
 * their constructor (the configuration page) you CANNOT
 * create those objects in a KisPaintOp. KisPaintOps are
 * created in non-gui threads.
 *
 * Options are disabled by default.
 */
class KRITAUI_EXPORT KisPaintOpOption : public QObject
{
    Q_OBJECT
public:

    using OptionalLodLimitationsReader = std::optional<lager::reader<KisPaintopLodLimitations>>;

    enum PaintopCategory {
        GENERAL,
        COLOR,
        TEXTURE,
        FILTER,
        MASKING_BRUSH
    };

    KisPaintOpOption(const QString &label, KisPaintOpOption::PaintopCategory category, bool checked);
    KisPaintOpOption(const QString &label, KisPaintOpOption::PaintopCategory category,
                     lager::cursor<bool> checkedCursor);
    KisPaintOpOption(const QString &label, KisPaintOpOption::PaintopCategory category,
                     lager::cursor<bool> checkedCursor,
                     lager::reader<bool> externallyEnabledLink);
    ~KisPaintOpOption() override;

    KisPaintOpOption::PaintopCategory category() const;
    virtual bool isCheckable() const;

    virtual bool isChecked() const;
    virtual void setChecked(bool checked);

    bool isEnabled() const;

    void setLocked(bool value);
    bool isLocked() const;

    QString label() const;
    /// Names the option across brushes and sessions (pinned settings use it); the object name unless more is needed
    virtual QString id() const;

    /**
     * Reimplement this to use the image in the option widget
     */
    virtual void setImage(KisImageWSP image);
    virtual void setNode(KisNodeWSP node);
    virtual void setResourcesInterface(KisResourcesInterfaceSP resourcesInterface);
    virtual void setCanvasResourcesInterface(KoCanvasResourcesInterfaceSP canvasResourcesInterface);

    void startReadOptionSetting(const KisPropertiesConfigurationSP setting);
    void startWriteOptionSetting(KisPropertiesConfigurationSP setting) const;

    QWidget *configurationPage() const;

    virtual void lodLimitations(KisPaintopLodLimitations *l) const;
    OptionalLodLimitationsReader effectiveLodLimitations() const;

    /// What the option's row in the list shows and is dragged to change: its strength
    struct RowValue {
        qreal value {0.0};
        qreal minimum {0.0};
        qreal maximum {1.0};
        int decimals {0};
        QString suffix;
        /// A size: shown and dragged by ratios, as the top bar's
        bool size {false};
        /// Used while the row is unchecked too (Opacity: the checkbox is only the pen's)
        bool appliesUnchecked {false};
        /// Shown instead of the number (Spacing: "Auto"); dragging sets a number
        QString text;
    };

    /// None for an option whose row has no value
    std::optional<RowValue> rowValue() const;
    void setRowValue(qreal value);
    /// Another value for the row than its own (the Size row is the brush's diameter); an empty \p get: its own again
    void setRowValueSource(std::function<std::optional<RowValue>()> get, std::function<void(qreal)> set);
    /// Says the row's value changed, for a value of another source
    void notifyRowValueChanged();

    /// A pen input that can drive the option, with the curve it is read through
    struct PenInput {
        QString id;
        QString name;
        bool active {false};
        QString curve;
    };
    /// The pen inputs that can drive the option; none when the pen can't
    virtual QVector<PenInput> penInputs() const;
    /// Turns input \p id on or off
    virtual void setPenInputActive(const QString &id, bool active);
    /// Gives input \p id its own \p curve
    virtual void setPenCurve(const QString &id, const QString &curve);
    /// How inputs that are on together combine, an index into penModeNames(); -1 when they can't
    virtual int penMode() const;
    virtual void setPenMode(int mode);
    static QStringList penModeNames();
    /// Settings of input \p id itself (Fade's length), made for \p parent; null when it has none
    virtual QWidget *createPenInputPage(const QString &id, QWidget *parent);

    /**
     * A small page for the quick brush editor's tab of \p category, made for \p parent:
     * pictures and direct handles rather than the full page. Null when the option has none there.
     */
    virtual QWidget *createQuickPage(PaintopCategory category, QWidget *parent);
    /// Whether createQuickPage() makes a page for \p category
    virtual bool hasQuickPage(PaintopCategory category) const;
    /// Whether its quick page has all its own page has, so the full page needn't be offered beside it
    virtual bool quickPageIsWhole() const { return false; }

protected:
    virtual OptionalLodLimitationsReader lodLimitationsReader() const;
    virtual std::optional<RowValue> ownRowValue() const;
    virtual void setOwnRowValue(qreal value);
    void setConfigurationPage(QWidget *page);
    /// The part of the page the option's checkbox enables, when not all of it (call before setConfigurationPage)
    void setCheckedPart(QWidget *part);

    KisResourcesInterfaceSP resourcesInterface() const;
    KoCanvasResourcesInterfaceSP canvasResourcesInterface() const;

protected:
    /**
     * Re-implement this to save the configuration to the paint configuration.
     */
    virtual void writeOptionSetting(KisPropertiesConfigurationSP setting) const {
        Q_UNUSED(setting);
    }

    /**
     * Re-implement this to set the widgets with the values in @p setting.
     */
    virtual void readOptionSetting(const KisPropertiesConfigurationSP setting) {
        Q_UNUSED(setting);
    }

protected Q_SLOTS:
    void emitSettingChanged();
    void emitCheckedChanged(bool checked);
    void emitEnabledChanged(bool enabled);

Q_SIGNALS:

    /**
     * Q_EMIT this whenever a setting has changed. It will update the preview
     */
    void sigSettingChanged();

    /**
     * Q_EMIT this whenever a checked state of the option has changed. It as always
     * emitted *before* sigSettingChanged()
     */
    void sigCheckedChanged(bool value);
    void sigEnabledChanged(bool value);
    void sigRowValueChanged();

private:
    void slotEnablePageWidget(bool value);

protected:

    bool m_checkable {false};
    bool m_locked {false};

private:

    struct Private;
    Private* const m_d;
};

#endif
