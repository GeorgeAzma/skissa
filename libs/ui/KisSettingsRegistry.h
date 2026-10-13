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
 * Describes every setting the Settings panel shows: where it is stored, its kind,
 * default and valid values. Reading, writing, resetting, undoing and transferring
 * settings all go through here, and values are validated before they are written.
 * Values are kept in their stored units (seconds, a zoom factor); the UI converts them.
 */
class KRITAUI_EXPORT KisSettingsRegistry : public QObject
{
    Q_OBJECT
public:
    enum class Kind {
        Toggle, ///< bool
        Number, ///< int or double, matching its default
        Choice, ///< the value of one of its choices (int or string)
        Color,  ///< QColor, alpha included
        Text,   ///< QString
    };

    enum Flag {
        Restart = 1 << 0,   ///< takes effect the next time Krita starts
        Machine = 1 << 1,   ///< specific to this computer (paths, hardware, screens); never transferred
        Fork = 1 << 2,      ///< exists only in this fork; upstream Krita doesn't read it
        NewViews = 1 << 3,  ///< applies only to images opened afterwards
    };

    struct Choice {
        QVariant value;
        QString text;
    };

    struct Setting {
        QString id;    ///< stable identifier used in files: the key, or "[group] key" for a grouped one
        QString group; ///< kritarc group; empty for the top-level group KisConfig uses
        QString key;
        Kind kind {Kind::Toggle};
        QVariant fallback; ///< the default value
        /// A default computed from other settings, used instead of fallback
        std::function<QVariant()> fallbackNow;
        int flags {0};
        qreal min {0};
        qreal max {0};
        QVector<Choice> choices;
        QString label; ///< user-visible name
        QString help;  ///< tooltip, for when the name alone doesn't explain it
        bool alpha {false}; ///< whether a Color keeps its alpha; otherwise it is made opaque
        /// Extra validation beyond the kind (e.g. a text's format)
        std::function<bool(const QVariant &value)> valid;
        /// For a Choice whose choices are only known at run time
        std::function<QVector<Choice>()> dynamicChoices;
        /// For a setting stored outside kritarc: how to read and write it
        std::function<QVariant()> read;
        std::function<void(const QVariant &value)> write;
        /// Called after the value is written, in addition to the general config notification
        std::function<void(const QVariant &value)> written;
        /// How a value is displayed (e.g. "7 min" for 420 seconds); defaults to the number, choice text, or On/Off
        std::function<QString(const QVariant &value)> format;
        /// Whether going from \p startup to \p now needs a restart; by default any change of a Restart setting does
        std::function<bool(const QVariant &startup, const QVariant &now)> waitsForRestart;

        bool storedInKritarc() const { return !read; }
    };

    static KisSettingsRegistry *instance();

    const QVector<Setting> &settings() const;
    const Setting *setting(const QString &id) const;
    /// Drops the cached run-time choices, e.g. after a color profile or theme was added
    void refreshChoices();

    QVariant value(const QString &id) const;
    QVariant defaultValue(const QString &id) const;
    bool isDefault(const QString &id) const;
    /**
     * Writes \p value if it is valid for the setting (see normalized()), otherwise returns false.
     * Krita is notified shortly afterwards, once per burst of changes such as a slider drag,
     * and kritarc is synced once the changes stop.
     */
    bool setValue(const QString &id, const QVariant &value);
    void reset(const QString &id);

    /// Converts \p value (from a file, JSON or text) to the setting's type; invalid if it isn't an allowed value
    QVariant normalized(const Setting &setting, const QVariant &value) const;
    bool sameValue(const Setting &setting, const QVariant &a, const QVariant &b) const;
    QVector<Choice> choices(const Setting &setting) const;
    /// \p value formatted for display
    QString display(const Setting &setting, const QVariant &value) const;

    /// All current values, to go back to with restore()
    QHash<QString, QVariant> snapshot() const;
    void restore(const QHash<QString, QVariant> &snapshot);

    /// The settings changed since startup whose change needs a restart
    QStringList waitingForRestart() const;

    // Undo history of settings changes (see KisSettingsUndo). Quick successive changes
    // of one setting, such as a slider drag, form a single step.

    /// Milliseconds since the change Undo would revert was made, or -1 if there is none
    qint64 undoAge() const;
    QString undoName() const;
    bool undoChange();
    bool canRedoChange() const;
    QString redoName() const;
    bool redoChange();

    /// Sends any pending change notification and syncs kritarc right away
    void flush();

    /// The setting's value in another kritarc; invalid if it is unset or not valid there
    QVariant valueIn(const Setting &setting, KConfig &config) const;
    void writeIn(const Setting &setting, KConfig &config, const QVariant &value) const;

Q_SIGNALS:
    /// Emitted whenever \p id is written
    void valueChanged(const QString &id);

private:
    KisSettingsRegistry();
    void schedule();
    /// Notifies Krita of written settings, as the Preferences dialog does on OK
    void notifyNow();

    struct Private;
    Private *const d;
};

#endif // KISSETTINGSREGISTRY_H
