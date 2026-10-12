/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSETTINGSREGISTRY_H
#define KISSETTINGSREGISTRY_H

#include <QHash>
#include <QObject>
#include <QStringList>
#include <QVariant>
#include <QVector>

#include <functional>

#include "kritaui_export.h"

class KConfig;

/**
 * Every setting Settings shows, described once: where it is kept, its kind,
 * default and valid values. Showing, finding, resetting, undoing and moving
 * settings all go through here, and a value is only written once it is
 * checked against its description. Values are as stored (seconds, a zoom
 * factor): the cards convert them for showing.
 */
class KRITAUI_EXPORT KisSettingsRegistry : public QObject
{
    Q_OBJECT
public:
    enum class Kind {
        Toggle, ///< bool
        Number, ///< int or double, as its default is
        Choice, ///< one of its choices' values (int or string)
        Color,  ///< QColor, alpha included
        Text,   ///< QString
    };

    enum Flag {
        Restart = 1 << 0,   ///< takes effect the next time Krita starts
        Machine = 1 << 1,   ///< about this computer (paths, hardware, screens): never moved to another
        Fork = 1 << 2,      ///< this Krita's own: Krita itself doesn't know it
        NewViews = 1 << 3,  ///< for images opened from now on
    };

    struct Choice {
        QVariant value;
        QString text;
    };

    struct Setting {
        QString id;    ///< stable, used in files and lists: the key, "[group] key" in a group
        QString group; ///< kritarc's group; empty: the top, where KisConfig keeps its settings
        QString key;
        Kind kind {Kind::Toggle};
        QVariant fallback; ///< what it is until changed
        /// A default that follows other settings, instead of fallback
        std::function<QVariant()> fallbackNow;
        int flags {0};
        qreal min {0};
        qreal max {0};
        QVector<Choice> choices;
        QString label; ///< its name in lists (an import's changes, Find)
        QString help;  ///< what it does, if more than its name says: shown on hover
        bool alpha {false}; ///< a Color that can be see-through; the others are opaque
        /// More than the kind says (a text's format); for a Choice with choices only known at run time, the choices
        std::function<bool(const QVariant &value)> valid;
        std::function<QVector<Choice>()> dynamicChoices;
        /// Kept elsewhere than in kritarc: how it is read and written
        std::function<QVariant()> read;
        std::function<void(const QVariant &value)> write;
        /// Told after it is written, besides the general notification
        std::function<void(const QVariant &value)> written;
        /// How a value reads in a list (e.g. "7 min" for 420 seconds); default: its number, choice, or On and Off
        std::function<QString(const QVariant &value)> format;
        /// Whether a change from \p startup to \p now waits for a restart (default: any change, for Restart)
        std::function<bool(const QVariant &startup, const QVariant &now)> waitsForRestart;

        bool storedInKritarc() const { return !read; }
    };

    static KisSettingsRegistry *instance();

    const QVector<Setting> &settings() const;
    const Setting *setting(const QString &id) const;
    /// Choices known only at run time are looked for again (a color profile was installed)
    void refreshChoices();

    QVariant value(const QString &id) const;
    QVariant defaultValue(const QString &id) const;
    bool isDefault(const QString &id) const;
    /**
     * Writes \p value, if it is valid for the setting (see normalized());
     * false if not. Krita is told of it a moment later, once for everything
     * changed together (a slider dragged), and kritarc is written then.
     */
    bool setValue(const QString &id, const QVariant &value);
    void reset(const QString &id);

    /// \p value as the setting holds it (from a file, JSON or text), or an invalid QVariant if it isn't one of its values
    QVariant normalized(const Setting &setting, const QVariant &value) const;
    bool sameValue(const Setting &setting, const QVariant &a, const QVariant &b) const;
    QVector<Choice> choices(const Setting &setting) const;
    /// \p value as it reads in a list
    QString display(const Setting &setting, const QVariant &value) const;

    /// The values now, to go back to (an import undone, the panel's Undo)
    QHash<QString, QVariant> snapshot() const;
    void restore(const QHash<QString, QVariant> &snapshot);

    /// The settings waiting for a restart: changed since Krita started, where that takes a restart
    QStringList waitingForRestart() const;

    /// Tells Krita now of what was written, instead of a moment later
    void flush();

    /// A setting's value in another kritarc (for moving settings between Kritas); invalid if it has none or isn't valid
    QVariant valueIn(const Setting &setting, KConfig &config) const;
    void writeIn(const Setting &setting, KConfig &config, const QVariant &value) const;

Q_SIGNALS:
    /// \p id was written (by Settings, an import, a reset)
    void valueChanged(const QString &id);

private:
    KisSettingsRegistry();
    void schedule();
    /// Tells Krita of what was written (what the Preferences dialog did after OK)
    void notifyNow();

    struct Private;
    Private *const d;
};

#endif // KISSETTINGSREGISTRY_H
