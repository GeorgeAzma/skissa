/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioPeek.h"
#include <KisLanczos.h>

#include <QAction>
#include <QApplication>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QPainter>
#include <QPointer>
#include <QTimer>
#include <QWidget>

#include <KisModernStyle.h>
#include <kis_canvas2.h>
#include <kis_config.h>
#include <kis_coordinates_converter.h>
#include <kis_image.h>
#include <kis_paint_device.h>

namespace
{
class Springs : public QObject
{
public:
    QVector<QPointer<QAction>> actions;
    QPointer<QAction> pressed;
    int key {0};
    bool wasOn {false};
    QElapsedTimer held;

    bool eventFilter(QObject *, QEvent *event) override
    {
        if (event->type() != QEvent::ShortcutOverride && event->type() != QEvent::KeyRelease) return false;
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        if (event->type() == QEvent::ShortcutOverride) {
            // held, the key repeats: the switch turns once
            if (keyEvent->isAutoRepeat()) {
                if (keyEvent->key() != key || !pressed) return false;
                keyEvent->accept();
                return true;
            }
            const QKeySequence sequence(int(keyEvent->modifiers() & ~Qt::KeypadModifier) | keyEvent->key());
            for (const QPointer<QAction> &action : qAsConst(actions)) {
                if (!action || !action->isEnabled() || action->shortcut() != sequence) continue;
                pressed = action;
                key = keyEvent->key();
                wasOn = action->isChecked();
                held.start();
                break;
            }
            return false;
        }
        if (keyEvent->isAutoRepeat() || keyEvent->key() != key) return false;
        key = 0;
        QPointer<QAction> action = pressed;
        pressed.clear();
        if (action && held.elapsed() >= KisModernStyle::holdInterval() && action->isChecked() != wasOn) action->trigger();
        return false;
    }
};

/// Box blurs \p image (premultiplied) three times along each axis, close to a gaussian
void blur(QImage &image, int radius)
{
    const int w = image.width();
    const int h = image.height();
    QVector<QRgb> line(qMax(w, h));
    auto pass = [&](QRgb *start, int count, int stride) {
        for (int i = 0; i < count; i++) line[i] = start[i * stride];
        const int span = 2 * radius + 1;
        int sum[4] = {0, 0, 0, 0};
        auto at = [&](int i) { return line[qBound(0, i, count - 1)]; };
        for (int i = -radius; i <= radius; i++) {
            const QRgb c = at(i);
            sum[0] += qAlpha(c); sum[1] += qRed(c); sum[2] += qGreen(c); sum[3] += qBlue(c);
        }
        for (int i = 0; i < count; i++) {
            start[i * stride] = qRgba(sum[1] / span, sum[2] / span, sum[3] / span, sum[0] / span);
            const QRgb in = at(i + radius + 1);
            const QRgb out = at(i - radius);
            sum[0] += qAlpha(in) - qAlpha(out); sum[1] += qRed(in) - qRed(out);
            sum[2] += qGreen(in) - qGreen(out); sum[3] += qBlue(in) - qBlue(out);
        }
    };
    QRgb *bits = reinterpret_cast<QRgb *>(image.bits());
    for (int times = 0; times < 3; times++) {
        for (int y = 0; y < h; y++) pass(bits + y * w, w, 1);
        for (int x = 0; x < w; x++) pass(bits + x, h, w);
    }
}

class Squint : public QWidget
{
public:
    Squint(KisCanvas2 *canvas, QWidget *parent)
        : QWidget(parent)
        , m_canvas(canvas)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setObjectName(QStringLiteral("studio_squint"));
        // follows the view and the picture while it is on
        m_watch.setInterval(120);
        connect(&m_watch, &QTimer::timeout, this, [this]() {
            if (!m_canvas) return;
            if (m_dirty || m_canvas->coordinatesConverter()->imageToWidgetTransform() != m_transform || size() != parentWidget()->size()) refresh();
        });
        connect(canvas->image().data(), &KisImage::sigImageUpdated, this, [this]() { m_dirty = true; });
    }

    void refresh()
    {
        m_dirty = false;
        setGeometry(parentWidget()->rect());
        const KisCoordinatesConverter *converter = m_canvas->coordinatesConverter();
        m_transform = converter->imageToWidgetTransform();
        KisImageSP image = m_canvas->image();
        m_rect = converter->widgetRectInImagePixels().toAlignedRect() & image->bounds();
        if (m_rect.isEmpty()) {
            m_picture = QImage();
        } else {
            // about a sixtieth of the view across: big shapes and values, no detail
            const QRectF shown = m_transform.mapRect(QRectF(m_rect));
            const int across = qMax(8, int(qMax(shown.width(), shown.height()) / 6));
            m_picture = image->projection()->createThumbnailUncached(across, across, m_rect).convertToFormat(QImage::Format_ARGB32_Premultiplied);
            blur(m_picture, 2);
        }
        update();
    }

    void start()
    {
        refresh();
        show();
        // under the panels and the pill, over the picture
        lower();
        m_watch.start();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        if (m_picture.isNull()) return;
        QPainter painter(this);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.setTransform(m_transform);
        // see-through parts as the checkers look from afar, so nothing sharp shows through
        KisConfig config(true);
        const QColor a = config.checkersColor1();
        const QColor b = config.checkersColor2();
        painter.fillRect(QRectF(m_rect), QColor((a.red() + b.red()) / 2, (a.green() + b.green()) / 2, (a.blue() + b.blue()) / 2));
        KisLanczos::draw(painter, QRectF(m_rect), m_picture);
    }

private:
    QPointer<KisCanvas2> m_canvas;
    QTimer m_watch;
    QTransform m_transform;
    QRect m_rect;
    QImage m_picture;
    bool m_dirty {false};
};
} // namespace

void KisStudioPeek::springBack(QAction *action)
{
    static Springs *springs = nullptr;
    if (!springs) {
        springs = new Springs;
        qApp->installEventFilter(springs);
    }
    springs->actions.append(action);
}

void KisStudioPeek::setSquint(KisCanvas2 *canvas, bool on)
{
    QWidget *widget = canvas ? canvas->canvasWidget() : nullptr;
    if (!widget) return;
    Squint *squint = widget->findChild<Squint *>(QStringLiteral("studio_squint"), Qt::FindDirectChildrenOnly);
    if (!on) {
        delete squint;
        return;
    }
    if (!squint) squint = new Squint(canvas, widget);
    squint->start();
}
