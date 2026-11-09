/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIOCOLORPANEL_H
#define KISSTUDIOCOLORPANEL_H

#include <QDockWidget>

#include <KoCanvasObserverBase.h>
#include <KoDockFactoryBase.h>

#include <functional>
#include <memory>

#include "kritaui_export.h"

class QLineEdit;

/**
 * The color panel. It offers OKLCH pickers, where equal steps look equally different (a hue ring around a
 * lightness/chroma square, or sliders), and Krita's classic HSV ring and triangle, with the current, previous
 * and recent colors below.
 */
class KRITAUI_EXPORT KisStudioColorPanel : public QDockWidget, public KoCanvasObserverBase
{
    Q_OBJECT
public:
    KisStudioColorPanel();
    ~KisStudioColorPanel() override;

    QString observerName() override { return QStringLiteral("KisStudioColorPanel"); }
    void setCanvas(KoCanvasBase *canvas) override;
    void unsetCanvas() override;

    static QString id();
    /// Registers the dock factory; call once before the main window creates its dockers.
    static void registerFactory();

private:
    /// Limits the height to what the sliders need, leaving the rest to the panels below

    struct Private;
    const std::unique_ptr<Private> d;
};

class KisStudioColorPanelFactory : public KoDockFactoryBase
{
public:
    QString id() const override;
    DockPosition defaultDockPosition() const override { return DockRight; }
    QDockWidget *createDockWidget() override;
};

/**
 * The color panel's picker on its own, with the hues and the shape chosen there, to pick a color in place (an
 * effect's color) rather than the paint color
 */
class KRITAUI_EXPORT KisStudioColorWheel : public QWidget
{
    Q_OBJECT
public:
    explicit KisStudioColorWheel(QWidget *parent = nullptr);
    ~KisStudioColorWheel() override;

    void setColor(const QColor &color);
    QColor color() const;
    /// The height its picker fills at \p width, without room around it
    int heightFor(int width) const;

    /// A color as it is typed in the color panel's field, in any format it reads (00F2FC, rgb(…), oklch(…),
    /// hsv(…), hsl(…)); invalid if it reads none
    static QColor colorOfText(const QString &text);

    /**
     * The color panel's field for a color written out: dim until clicked, never taking the keys unasked, any format
     * typed, Esc putting back what was there, Copy as each format of \p color
     */
    static QLineEdit *textField(QWidget *parent, std::function<QColor()> color);

Q_SIGNALS:
    /// A color picked on it (not one set)
    void colorPicked(const QColor &color);

private:
    struct Private;
    std::unique_ptr<Private> d;
};

#endif // KISSTUDIOCOLORPANEL_H
