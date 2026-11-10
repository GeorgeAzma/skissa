/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIOPENPOPUP_H
#define KISSTUDIOPENPOPUP_H

#include "widgets/KisStudioPopup.h"

#include <memory>

class KisPaintOpOption;

/**
 * How the pen drives one brush setting: its inputs as chips (a dot: on), the selected input's
 * curve, saved curves, and how inputs on together combine.
 */
class KRITAUI_EXPORT KisStudioPenPopup : public KisStudioPopup
{
    Q_OBJECT
public:
    /// \p input: the input to show first; empty: the first one on
    KisStudioPenPopup(KisPaintOpOption *option, QWidget *parent, const QString &input = QString());
    ~KisStudioPenPopup() override;

    KisPaintOpOption *option() const;

    /// Short names of the pen inputs, by Krita's sensor id (a MyPaint input's by the Krita input it is)
    static QString inputName(const QString &id, const QString &fallback);
    /// What a pen input is, as a short tooltip; empty for one without
    static QString inputTip(const QString &id);
    /// The inputs shown before "more", by Krita's sensor id
    static QStringList mainInputs();
    /// The Krita sensor id a pen input \p id stands for: a MyPaint input's counterpart, else \p id
    static QString kritaInput(const QString &id);
    /// Whether input \p id's curve is an offset around zero (MyPaint's), drawn with zero across its middle
    static bool curvesAroundZero(const QString &id);

private:
    void refresh();

    struct Private;
    const std::unique_ptr<Private> d;
};

#endif // KISSTUDIOPENPOPUP_H
