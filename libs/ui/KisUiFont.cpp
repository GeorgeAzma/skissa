/*
 * SPDX-FileCopyrightText: 2016 The Qt Company Ltd.
 * SPDX-FileCopyrightText: 2020 Halla Rempt <halla@valdyas.org>
 * SPDX-FileCopyrightText: 2021 Alvin Wong <alvin@alvinhc.com>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 *
 * This file contains code that was borrowed and modified from the Qt Project,
 * originally licensed under LGPL-3.0-or-later or GPL-2.0-or-later.
 */

#include "KisUiFont.h"

#include <kis_config.h>

#include <boost/optional.hpp>

#include <QtGlobal>
#include <QFontDatabase>
#include <QLocale>

#if defined(Q_OS_WIN) && QT_VERSION < 0x060000
# include <qt_windows.h>
#endif

namespace KisUiFont
{

static const QString useCustomSystemFontCfgName = QStringLiteral("use_custom_system_font");
static const QString customSystemFontCfgName = QStringLiteral("custom_system_font");
static const QString customFontSizeCfgName = QStringLiteral("custom_font_size");

/**
 * @brief Gets the system default UI font.
 * @return the system default UI font.
 */
static QFont systemDefaultUiFont();

/**
 * @brief Returns the font the user has configured.
 * @return the user font if it has been set, otherwise `boost::none`.
 */
static boost::optional<QFont> userCfgUiFont();

#if defined(Q_OS_WIN) && QT_VERSION < 0x060000
static QFont windowsSystemUiFont();
#endif

QFont systemDefaultUiFont()
{
#if defined(Q_OS_WIN) && QT_VERSION < 0x060000
    return windowsSystemUiFont();
#else
    return QFontDatabase::systemFont(QFontDatabase::GeneralFont);
#endif
}

boost::optional<QFont> userCfgUiFont()
{
    KisConfig cfg(true);
    if (cfg.readEntry<bool>(useCustomSystemFontCfgName, false)) {
        QString fontName = cfg.readEntry<QString>(customSystemFontCfgName, QString());
        if (fontName.isEmpty()) {
            return boost::none;
        }
        int fontSize = cfg.readEntry<int>(customFontSizeCfgName, -1);
        if (fontSize <= 6) {
            // Font size may be in pixels (on Android) or points (everywhere else.)
            QFont systemFont = systemDefaultUiFont();
            fontSize = systemFont.pointSize();
            if (fontSize == -1) {
                fontSize = systemFont.pixelSize();
            }
        }
        return QFont(fontName, fontSize);
    } else {
        return boost::none;
    }
}

QFont normalFont()
{
    QFont font;
    if (boost::optional<QFont> userFont = userCfgUiFont()) {
        font = *userFont;
    } else {
        font = systemDefaultUiFont();
#ifdef Q_OS_WIN
        /**
         * The Windows 11 UI face, at the size Windows 11 uses for body text.
         * Segoe UI at 9 pt through GDI is what makes a Qt application look
         * like it is from 2010.
         */
        static const bool hasVariable = QFontDatabase().families().contains(QStringLiteral("Segoe UI Variable Text"));
        if (hasVariable) {
            font.setFamily(QStringLiteral("Segoe UI Variable Text"));
            font.setPointSizeF(qMax(font.pointSizeF(), 10.0));
        }
#endif
    }
#ifdef Q_OS_WIN
    /**
     * Full hinting selects Qt's GDI font engine: thin, jagged, dated text.
     * Vertical hinting selects DirectWrite, which renders like the rest of
     * Windows. Full hinting stays for CJK locales, where DirectWrite with
     * vertical hinting gets blurry.
     */
    const QLocale::Script script = QLocale().script();
    const bool cjk = script == QLocale::SimplifiedHanScript || script == QLocale::TraditionalHanScript
        || script == QLocale::JapaneseScript || script == QLocale::KoreanScript || script == QLocale::HanScript;
    font.setHintingPreference(cjk ? QFont::PreferFullHinting : QFont::PreferVerticalHinting);
#endif
    return font;
}

QFont dockFont()
{
    QFont font = normalFont();
    // Font size may be in pixels (on Android) or points (everywhere else.)
    qreal ratio = 0.9;
    if (font.pixelSize() == -1) {
        font.setPointSizeF(font.pointSizeF() * ratio);
    } else {
        font.setPixelSize(qRound(font.pixelSize() * ratio));
    }
    return font;
}

#if defined(Q_OS_WIN) && QT_VERSION < 0x060000

// From qtbase/src/platformsupport/fontdatabases/windows/qwindowsfontdatabase.cpp
static int defaultVerticalDPI()
{
    static int vDPI = -1;
    if (vDPI == -1) {
        if (HDC defaultDC = GetDC(0)) {
            vDPI = GetDeviceCaps(defaultDC, LOGPIXELSY);
            ReleaseDC(0, defaultDC);
        } else {
            // FIXME: Resolve now or return 96 and keep unresolved?
            vDPI = 96;
        }
    }
    return vDPI;
}

// From qtbase/src/gui/text/qplatformfontdatabase.cpp
QFont::Weight weightFromInteger(int weight)
{
    if (weight < 150) {
        return QFont::Thin;
    }
    if (weight < 250) {
        return QFont::ExtraLight;
    }
    if (weight < 350) {
        return QFont::Light;
    }
    if (weight < 450) {
        return QFont::Normal;
    }
    if (weight < 550) {
        return QFont::Medium;
    }
    if (weight < 650) {
        return QFont::DemiBold;
    }
    if (weight < 750) {
        return QFont::Bold;
    }
    if (weight < 850) {
        return QFont::ExtraBold;
    }
    return QFont::Black;
}

// From qtbase/src/platformsupport/fontdatabases/windows/qwindowsfontdatabase.cpp
static QFont LOGFONT_to_QFont(const LOGFONTW& logFont)
{
    const int verticalDPI_In = defaultVerticalDPI();
    QFont qFont(QString::fromWCharArray(logFont.lfFaceName));
    qFont.setItalic(logFont.lfItalic);
    if (logFont.lfWeight != FW_DONTCARE) {
        qFont.setWeight(weightFromInteger(logFont.lfWeight));
    }
    const qreal logFontHeight = qAbs(logFont.lfHeight);
    qFont.setPointSizeF(logFontHeight * 72.0 / qreal(verticalDPI_In));
    qFont.setUnderline(logFont.lfUnderline);
    qFont.setOverline(false);
    qFont.setStrikeOut(logFont.lfStrikeOut);
    return qFont;
}

// From qtbase/src/platformsupport/fontdatabases/windows/qwindowsfontdatabase.cpp
static QFont windowsSystemUiFont()
{
    // Qt 6: Obtain default GUI font (typically "Segoe UI, 9pt", see QTBUG-58610)
    NONCLIENTMETRICSW ncm;
    ncm.cbSize = FIELD_OFFSET(NONCLIENTMETRICSW, lfMessageFont) + sizeof(LOGFONTW);
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, ncm.cbSize , &ncm, 0);
    const QFont systemFont = LOGFONT_to_QFont(ncm.lfMessageFont);
    return systemFont;
}
#endif

} // namespace KisUiFont
