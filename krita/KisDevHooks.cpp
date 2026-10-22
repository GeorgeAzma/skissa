/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisDevHooks.h"

#include <windows.h>
#include <windowsx.h>

#include <algorithm>
#include <functional>

#include <QtGui/private/qhighdpiscaling_p.h>
#include <QtGui/qpa/qwindowsysteminterface.h>
#include <QAbstractButton>
#include <QAbstractNativeEventFilter>
#include <QApplication>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QMenuBar>
#include <QPainter>
#include <QScreen>
#include <QTabletEvent>
#include <QToolButton>
#include <QTouchDevice>
#include <QWidgetAction>

#include <kactioncollection.h>
#include <kxmlguiclient.h>
#include <kxmlguifactory.h>

#include <KoColorSpaceRegistry.h>
#include <KisColorDrop.h>
#include <KisDocument.h>
#include <KisMainWindow.h>
#include <KisPart.h>
#include <KisStudioFind.h>
#include <KisView.h>
#include <KisViewManager.h>
#include <canvas/kis_canvas2.h>

namespace
{
/// The test hooks (see KisDevHooks)
class DevCaptureFilter : public QAbstractNativeEventFilter
{
public:
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    bool nativeEventFilter(const QByteArray &, void *message, qintptr *) override
#else
    bool nativeEventFilter(const QByteArray &, void *message, long *) override
#endif
    {
        static const UINT captureMessage = RegisterWindowMessageW(L"KritaDevCapture");
        static const UINT mouseMessage = RegisterWindowMessageW(L"KritaDevMouse");
        static const UINT keyMessage = RegisterWindowMessageW(L"KritaDevKey");
        static const UINT exportMessage = RegisterWindowMessageW(L"KritaDevExport");
        static const UINT treeMessage = RegisterWindowMessageW(L"KritaDevTree");
        static const UINT shortcutsMessage = RegisterWindowMessageW(L"KritaDevShortcuts");
        static const UINT colorDropMessage = RegisterWindowMessageW(L"KritaDevColorDrop");
        const MSG *msg = static_cast<MSG *>(message);
        if (msg->message == colorDropMessage) {
            // the low byte: 0 over the canvas, 1 dropped, 2 left, 3 auto flats, 4 recolor the active layer; the rest is the color as 0xRRGGBB
            colorDrop(int(msg->wParam & 0xff), QColor::fromRgb(QRgb(quint64(msg->wParam) >> 8)),
                      QPoint(GET_X_LPARAM(msg->lParam), GET_Y_LPARAM(msg->lParam)));
            return true;
        }
        if (msg->message == shortcutsMessage) {
            shortcuts();
            return true;
        }
        if (msg->message == captureMessage) {
            capture();
            return true;
        }
        if (msg->message == mouseMessage) {
            mouse(int(msg->wParam & 0xff), QPoint(GET_X_LPARAM(msg->lParam), GET_Y_LPARAM(msg->lParam)), int(msg->wParam >> 8) / 1000.0);
            return true;
        }
        if (msg->message == treeMessage) {
            tree();
            return true;
        }
        if (msg->message == exportMessage) {
            // the active document as krita-dev-export.kra
            KisMainWindow *window = KisPart::instance()->currentMainwindow();
            KisDocument *document = window && window->activeView() ? window->activeView()->document() : nullptr;
            if (document) {
                document->exportDocumentSync(QDir::temp().filePath(QStringLiteral("krita-dev-export.kra")), "application/x-krita");
            }
            return true;
        }
        if (msg->message == keyMessage) {
            // the low bits: 1 press only, 2 release only, 0 both
            const int bits = int(msg->lParam);
            key(int(msg->wParam), Qt::KeyboardModifiers(bits & ~0x3), bits & 0x3);
            return true;
        }
        return false;
    }

private:
    /// A color dragged over the active canvas, at \p windowPos in the main window, without a real drag
    static void colorDrop(int phase, const QColor &rgb, const QPoint &windowPos)
    {
        KisMainWindow *window = KisPart::instance()->currentMainwindow();
        if (!window) return;
        if (phase == 3) {
            KisColorDrop::autoFlats(window->viewManager());
            return;
        }
        KisView *view = window->activeView();
        if (!view || !view->canvasBase()) return;
        const QPoint pos = view->canvasBase()->canvasWidget()->mapFrom(window, windowPos);
        const KoColor color(rgb, KoColorSpaceRegistry::instance()->rgb8());
        if (phase == 0) view->colorDrop()->hover(color, pos);
        else if (phase == 1) view->colorDrop()->drop(color, pos);
        else if (phase == 4) KisColorDrop::recolor(view->image(), window->viewManager()->activeNode(), color);
        else view->colorDrop()->leave();
    }

    /// The visible windows, bottom to top (main window first, popups last), and their bounding rectangle
    static QList<QWidget *> visibleWindows(QRect &bounds)
    {
        QList<QWidget *> windows;
        Q_FOREACH (QWidget *window, QApplication::topLevelWidgets()) {
            if (window->isVisible()) {
                windows << window;
                bounds |= window->geometry();
            }
        }
        auto layer = [](QWidget *window) {
            if (qobject_cast<KisMainWindow *>(window)) return 0;
            switch (window->windowType()) {
            case Qt::Tool: return 2;
            case Qt::Popup: return 3;
            case Qt::ToolTip: return 4;
            default: return 1;
            }
        };
        std::stable_sort(windows.begin(), windows.end(), [&](QWidget *a, QWidget *b) { return layer(a) < layer(b); });
        return windows;
    }

    /// Writes every action of the main window, with its shortcuts, menu path and buttons, to krita-dev-shortcuts.json
    static void shortcuts()
    {
        KisMainWindow *window = KisPart::instance()->currentMainwindow();
        if (!window) return;
        if (KisStudioFind *find = window->studioFind()) find->registerButtons();

        QHash<QAction *, QString> menuPaths;
        std::function<void(QMenu *, const QString &)> walk = [&](QMenu *menu, const QString &path) {
            Q_FOREACH (QAction *action, menu->actions()) {
                const QString text = action->text().remove(QLatin1Char('&'));
                if (action->menu()) walk(action->menu(), path + QStringLiteral(" > ") + text);
                else if (!menuPaths.contains(action)) menuPaths.insert(action, path.mid(3));
            }
        };
        Q_FOREACH (QAction *top, window->menuBar()->actions()) {
            if (top->menu()) walk(top->menu(), QStringLiteral(" > ") + top->text().remove(QLatin1Char('&')));
        }

        // the panel a widget is in and its named parents
        auto whereOf = [window](QWidget *widget) {
            QStringList parts;
            for (QWidget *w = widget->parentWidget(); w && w != window; w = w->parentWidget()) {
                if (auto *dock = qobject_cast<QDockWidget *>(w)) {
                    parts.prepend(QStringLiteral("panel:") + dock->windowTitle());
                    break;
                }
                if (!w->objectName().isEmpty() && !w->objectName().startsWith(QLatin1String("qt_"))) parts.prepend(w->objectName());
            }
            return parts.join(QLatin1Char('/'));
        };
        QHash<QAction *, QStringList> buttons;
        Q_FOREACH (QAbstractButton *button, window->findChildren<QAbstractButton *>()) {
            auto *tool = qobject_cast<QToolButton *>(button);
            if (tool && tool->defaultAction()) {
                buttons[tool->defaultAction()] << whereOf(button) + (button->isVisible() ? QString() : QStringLiteral(" (hidden)"));
            }
        }

        QJsonArray rows;
        QSet<QAction *> seen;
        auto add = [&](QAction *action, const QString &client) {
            if (!action || seen.contains(action) || action->isSeparator() || action->objectName().isEmpty()) return;
            seen.insert(action);
            QJsonObject row;
            row[QStringLiteral("name")] = action->objectName();
            row[QStringLiteral("text")] = action->text().remove(QLatin1Char('&'));
            row[QStringLiteral("toolTip")] = action->toolTip();
            row[QStringLiteral("whatsThis")] = action->whatsThis();
            QStringList keys;
            Q_FOREACH (const QKeySequence &key, action->shortcuts()) keys << key.toString(QKeySequence::PortableText);
            row[QStringLiteral("shortcuts")] = QJsonArray::fromStringList(keys);
            row[QStringLiteral("client")] = client;
            row[QStringLiteral("menu")] = menuPaths.value(action);
            row[QStringLiteral("buttons")] = QJsonArray::fromStringList(buttons.value(action));
            row[QStringLiteral("checkable")] = action->isCheckable();
            row[QStringLiteral("checked")] = action->isChecked();
            row[QStringLiteral("hasMenu")] = action->menu() != nullptr;
            row[QStringLiteral("widgetAction")] = bool(qobject_cast<QWidgetAction *>(action) && qobject_cast<QWidgetAction *>(action)->defaultWidget());
            rows.append(row);
        };
        Q_FOREACH (KisKXMLGUIClient *client, window->guiFactory()->clients()) {
            if (!client || !client->actionCollection()) continue;
            Q_FOREACH (QAction *action, client->actionCollection()->actions()) add(action, client->actionCollection()->componentName());
        }
        Q_FOREACH (QAction *action, window->actions()) add(action, QStringLiteral("window"));

        QFile file(QDir::temp().absoluteFilePath(QStringLiteral("krita-dev-shortcuts.json")));
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) file.write(QJsonDocument(rows).toJson());
    }

    /// Writes the visible widget tree with geometry and painting attributes to krita-dev-tree.txt
    static void tree()
    {
        QFile file(QDir::temp().absoluteFilePath(QStringLiteral("krita-dev-tree.txt")));
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) return;
        std::function<void(QWidget *, int)> write = [&](QWidget *widget, int depth) {
            const QRect r(widget->mapTo(widget->window(), QPoint()), widget->size());
            QString line = QString(depth * 2, QLatin1Char(' ')) + QString::fromLatin1(widget->metaObject()->className());
            if (!widget->objectName().isEmpty()) line += QLatin1Char('#') + widget->objectName();
            line += QStringLiteral(" %1,%2 %3x%4").arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height());
            if (widget->testAttribute(Qt::WA_TranslucentBackground)) line += QStringLiteral(" translucent");
            if (widget->testAttribute(Qt::WA_NoSystemBackground)) line += QStringLiteral(" nosysbg");
            if (widget->testAttribute(Qt::WA_OpaquePaintEvent)) line += QStringLiteral(" opaque");
            if (widget->testAttribute(Qt::WA_SetPalette)) line += QStringLiteral(" ownpalette");
            line += QStringLiteral(" window=%1 text=%2").arg(widget->palette().color(QPalette::Window).name(QColor::HexArgb),
                                                             widget->palette().color(QPalette::ButtonText).name());
            if (widget->autoFillBackground()) line += QStringLiteral(" autofill");
            if (widget == QApplication::focusWidget()) line += QStringLiteral(" FOCUS");
            if (widget == QWidget::keyboardGrabber()) line += QStringLiteral(" KEYGRAB");
            if (widget->graphicsEffect()) line += QStringLiteral(" effect");
            if (widget->windowOpacity() < 1.0) line += QStringLiteral(" opacity=%1").arg(widget->windowOpacity());
            if (!widget->mask().isEmpty()) {
                const QRect m = widget->mask().boundingRect();
                line += QStringLiteral(" mask=%1,%2 %3x%4 (%5 rects)").arg(m.x()).arg(m.y()).arg(m.width()).arg(m.height()).arg(widget->mask().rectCount());
            }
            Q_FOREACH (const QByteArray &name, widget->dynamicPropertyNames()) {
                if (name.startsWith("studioGlass")) line += QStringLiteral(" %1=%2").arg(QString::fromLatin1(name), widget->property(name).toString());
            }
            if (widget->inherits("QQuickWidget")) {
                line += QStringLiteral(" qml status=%1 source=%2").arg(widget->property("status").toInt()).arg(widget->property("source").toUrl().toString());
            }
            file.write(line.toUtf8() + '\n');
            Q_FOREACH (QObject *child, widget->children()) {
                QWidget *w = qobject_cast<QWidget *>(child);
                if (w && w->isVisible() && !w->isWindow()) write(w, depth + 1);
            }
        };
        QRect bounds;
        Q_FOREACH (QWidget *window, visibleWindows(bounds)) {
            write(window, 0);
        }
    }

    /// Renders the visible windows, popups included, into one PNG at %KRITA_DEV_CAPTURE% (or krita-dev-capture.png)
    static void capture()
    {
        QRect bounds;
        const QList<QWidget *> windows = visibleWindows(bounds);
        if (windows.isEmpty()) {
            return;
        }

        const qreal dpr = windows.first()->devicePixelRatioF();
        QImage image(bounds.size() * dpr, QImage::Format_ARGB32_Premultiplied);
        image.setDevicePixelRatio(dpr);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        Q_FOREACH (QWidget *window, windows) {
            painter.drawPixmap(window->geometry().topLeft() - bounds.topLeft(), window->grab());
        }
        painter.end();

        QString path = qEnvironmentVariable("KRITA_DEV_CAPTURE");
        if (path.isEmpty()) {
            path = QDir::temp().absoluteFilePath(QStringLiteral("krita-dev-capture.png"));
        }
        // written aside and renamed so a waiting reader never sees half a file
        const QString partial = path + QStringLiteral(".part");
        if (image.save(partial, "PNG")) {
            QFile::remove(path);
            QFile::rename(partial, path);
        }
    }

    /**
     * Input at a point of the capture image (lParam: x, y) through Qt's input queue, without moving the
     * system cursor. wParam: 0 move, 1/2 left press/release, 3/4 right press/release, 5/6 wheel up/down,
     * 7/8/9 touch down/move/up, 10/11 middle press/release, 12/13/14 pen down/move/up with the pressure in thousandths
     * above the action's byte.
     */
    static void mouse(int action, const QPoint &imagePos, qreal pressure = 1.0)
    {
        static Qt::MouseButtons buttons;
        QRect bounds;
        const QList<QWidget *> windows = visibleWindows(bounds);
        if (windows.isEmpty()) {
            return;
        }
        const qreal dpr = windows.first()->devicePixelRatioF();
        const QPointF global = QPointF(bounds.topLeft()) + QPointF(imagePos) / dpr;

        QWidget *target = nullptr;
        for (int i = windows.size() - 1; i >= 0 && !target; i--) {
            if (QRectF(windows[i]->geometry()).contains(global)) target = windows[i];
        }
        if (!target || !target->windowHandle()) {
            return;
        }
        QWindow *window = target->windowHandle();
        // native events are in device pixels
        const QPointF local = QHighDpi::toNativeLocalPosition(global - QPointF(target->geometry().topLeft()), window);
        const QPointF nativeGlobal = QHighDpi::toNativePixels(global, window);

        Qt::MouseButton button = Qt::NoButton;
        QEvent::Type type = QEvent::MouseMove;
        switch (action) {
        case 1: button = Qt::LeftButton; type = QEvent::MouseButtonPress; buttons |= button; break;
        case 2: button = Qt::LeftButton; type = QEvent::MouseButtonRelease; buttons &= ~button; break;
        case 3: button = Qt::RightButton; type = QEvent::MouseButtonPress; buttons |= button; break;
        case 4: button = Qt::RightButton; type = QEvent::MouseButtonRelease; buttons &= ~button; break;
        case 10: button = Qt::MiddleButton; type = QEvent::MouseButtonPress; buttons |= button; break;
        case 11: button = Qt::MiddleButton; type = QEvent::MouseButtonRelease; buttons &= ~button; break;
        case 5:
        case 6:
            // with the modifiers held through "KritaDevKey"
            QWindowSystemInterface::handleWheelEvent(window, local, nativeGlobal, QPoint(), QPoint(0, action == 5 ? 120 : -120),
                                                     QGuiApplication::keyboardModifiers());
            return;
        case 7:
        case 8:
        case 9: {
            static QTouchDevice *device = nullptr;
            if (!device) {
                device = new QTouchDevice();
                device->setName(QStringLiteral("KritaDevTouch"));
                device->setType(QTouchDevice::TouchScreen);
                device->setCapabilities(QTouchDevice::Position | QTouchDevice::Area);
                QWindowSystemInterface::registerTouchDevice(device);
            }
            QWindowSystemInterface::TouchPoint point;
            point.id = 1;
            point.state = action == 7 ? Qt::TouchPointPressed : action == 8 ? Qt::TouchPointMoved : Qt::TouchPointReleased;
            point.area = QHighDpi::toNativePixels(QRectF(global - QPointF(2, 2), QSizeF(4, 4)), window);
            const QRect screen = window->screen() ? window->screen()->geometry() : QRect(0, 0, 1, 1);
            point.normalPosition = QPointF((global.x() - screen.x()) / screen.width(), (global.y() - screen.y()) / screen.height());
            point.pressure = action == 9 ? 0 : 1;
            QWindowSystemInterface::handleTouchEvent(window, device, {point});
            return;
        }
        case 12:
        case 13:
        case 14: {
            static Qt::MouseButtons pen;
            if (action == 12) {
                QWindowSystemInterface::handleTabletEnterProximityEvent(QTabletEvent::Stylus, QTabletEvent::Pen, 1);
                pen = Qt::LeftButton;
            }
            if (action == 14) pen = Qt::NoButton;
            QWindowSystemInterface::handleTabletEvent(window, local, nativeGlobal, QTabletEvent::Stylus, QTabletEvent::Pen, pen,
                                                      action == 14 ? 0.0 : pressure, 0, 0, 0, 0, 0, 1, QGuiApplication::keyboardModifiers());
            // lifted away, so that the mouse is the mouse again
            if (action == 14) QWindowSystemInterface::handleTabletLeaveProximityEvent(QTabletEvent::Stylus, QTabletEvent::Pen, 1);
            return;
        }
        default: break;
        }
        QWindowSystemInterface::handleMouseEvent(window, local, nativeGlobal, buttons, button, type, QGuiApplication::keyboardModifiers());
    }

    /// A key press and/or release (wParam: Qt::Key, lParam: modifiers | phase) for the focused or topmost window
    static void key(int qtKey, Qt::KeyboardModifiers modifiers, int phase = 0)
    {
        QWindow *window = QGuiApplication::focusWindow();
        if (!window) {
            QRect bounds;
            const QList<QWidget *> windows = visibleWindows(bounds);
            if (windows.isEmpty() || !windows.last()->windowHandle()) return;
            window = windows.last()->windowHandle();
            // activated for Qt, so that its shortcuts work
            QWindowSystemInterface::handleWindowActivated(window, Qt::OtherFocusReason);
            QCoreApplication::processEvents();
        }
        const QString text = qtKey >= Qt::Key_Space && qtKey <= Qt::Key_AsciiTilde && !(modifiers & Qt::ControlModifier)
                                 ? (modifiers & Qt::ShiftModifier ? QString(QChar(qtKey)).toUpper() : QString(QChar(qtKey)).toLower())
                                 : QString();
        if (phase != 2) QWindowSystemInterface::handleKeyEvent(window, QEvent::KeyPress, qtKey, modifiers, text);
        if (phase != 1) QWindowSystemInterface::handleKeyEvent(window, QEvent::KeyRelease, qtKey, modifiers, text);
    }
};
} // namespace

void KisDevHooks::install(QCoreApplication &app)
{
    app.installNativeEventFilter(new DevCaptureFilter);
}
