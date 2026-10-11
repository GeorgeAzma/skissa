/*
 * SPDX-FileCopyrightText: 2025 Wolthera van Hovell tot Westerflier <griffinvalley@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISQQUICKWIDGET_H
#define KISQQUICKWIDGET_H

#include <QQuickWidget>
#include <kritaqmlwidgets_export.h>

/**
 * @brief The KisQQuickWidget class
 *
 * The purpose of KisQQuickWidget is to tackle a number of configuration steps
 * that need to be taken when using the QQuickWidget, amongst which the setup
 * of the engine.
 */

class KRITAQMLWIDGETS_EXPORT KisQQuickWidget : public QQuickWidget
{
    Q_OBJECT
public:
    KisQQuickWidget(QWidget *parent = nullptr);

    ~KisQQuickWidget();

    /**
     * @brief connectMinimumHeightToRootObject
     * By default we scale rootObject to widget, but in some situations we need
     * the widget to have the minimum required height of the root object. This
     * sets up a connection that links the minimum height to the root object
     * implicitHeight.
     */
    void connectMinimumHeightToRootObject();
    /**
     * @brief connectMinimumWidthToRootObject
     * Same as for connectMinimumHeightToRootObject,
     * but then for width.
     */
    void connectMinimumWidthToRootObject();

    /**
     * The dynamic property, set on it by what holds it (KisStudioGlass), that
     * makes it glass: drawn see-through, over the rest of its window, its QML
     * told by "studioGlass" in its context (see ThemedControl.qml)
     */
    static const char *const glassProperty;

protected:
    bool event(QEvent *event) override;

private Q_SLOTS:
    void updatePaletteFromConfig();

    void setMinimumHeightFromRoot();
    void setMinimumWidthFromRoot();
};

#endif // KISQQUICKWIDGET_H
