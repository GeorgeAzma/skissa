/*
 *  SPDX-FileCopyrightText: 2024 Wolthera van Hövell tot Westerflier <griffinvalley@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "TextPropertiesDock.h"

#include <QQuickWidget>
#include <QQmlEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QStringListModel>
#include <QQuickStyle>
#include <QColorDialog>

#include <KisQQuickWidget.h>

#include <KisViewManager.h>
#include <kis_canvas_resource_provider.h>

#include <KLocalizedContext>

#include "TextPropertiesCanvasObserver.h"

struct TextPropertiesDock::Private
{
    Private(QObject *parent = nullptr)
        : canvasObserver(new TextPropertiesCanvasObserver(parent)) {

    }
    TextPropertiesCanvasObserver *canvasObserver{nullptr};
};

TextPropertiesDock::TextPropertiesDock()
    : QDockWidget(i18n("Text Properties"))
    , d(new Private)
{
    m_quickWidget = new KisQQuickWidget(this);

    setWidget(m_quickWidget);
    setEnabled(true);

    m_quickWidget->setMinimumHeight(100);

    // its QML is made the first time it is shown (see eventFilter): making it takes most of a second at
    // startup, for a docker that is hidden more often than not
    m_quickWidget->installEventFilter(this);

    m_quickWidget->setPalette(this->palette());
}

bool TextPropertiesDock::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_quickWidget && event->type() == QEvent::Show && m_quickWidget->source().isEmpty()) {
        m_quickWidget->setSource(QUrl("qrc:/TextProperties.qml"));

        if (!m_quickWidget->errors().empty()) {
            qWarning() << "Errors in " << windowTitle() << ":" << m_quickWidget->errors();
        } else {
            QObject *root = m_quickWidget->rootObject();
            root->setProperty("canvasObserver", QVariant::fromValue(d->canvasObserver));
            // what the observer's signals would have brought it, had it been there before now
            QMetaObject::invokeMethod(root, "setProperties");
        }
    }
    return QDockWidget::eventFilter(watched, event);
}

TextPropertiesDock::~TextPropertiesDock()
{
    delete m_quickWidget;
}

QString TextPropertiesDock::observerName()
{
    return d->canvasObserver->observerName();
}

void TextPropertiesDock::setViewManager(KisViewManager *kisview)
{
    d->canvasObserver->setViewManager(kisview);
}

void TextPropertiesDock::setCanvas(KoCanvasBase *canvas)
{
    d->canvasObserver->setCanvas(canvas);
}

void TextPropertiesDock::unsetCanvas()
{
    d->canvasObserver->unsetCanvas();
}

QColor TextPropertiesDock::modalColorDialog(QColor oldColor)
{
    QColor c = QColorDialog::getColor(oldColor);
    return c.isValid()? c: oldColor;
}
