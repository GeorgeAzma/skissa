/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioToolStrip.h"

#include <QAbstractButton>
#include <QCheckBox>
#include <QComboBox>
#include <QDockWidget>
#include <QHBoxLayout>
#include <QPainter>
#include <QPainterPath>
#include <QScrollArea>
#include <QToolButton>
#include <QVBoxLayout>
#include <QTimer>
#include <QBoxLayout>
#include <QFormLayout>
#include <QGridLayout>
#include <QLabel>
#include "KisStudioBarButtons.h"
#include <QButtonGroup>
#include <QActionGroup>
#include <QStackedWidget>
#include <QLineEdit>
#include <QAbstractSlider>
#include <KisOptionCollectionWidget.h>


#include <KisModernStyle.h>
#include <KisOptionButtonStrip.h>
#include <KoGroupButton.h>

#include "widgets/kis_selection_options.h"
#include "KisStudioScrubber.h"
#include "KisStudioSelectionAdjust.h"

#include <klocalizedstring.h>

#include <QMenu>
#include <QSpinBox>
#include <QDoubleSpinBox>

#include <KisMainWindow.h>
#include <KisPart.h>
#include <KisViewManager.h>
#include <kis_image.h>
#include <KisSentenceCase.h>
#include <kactioncollection.h>
#include <kis_icon_utils.h>
#include <kis_canvas2.h>
#include <kis_painting_assistants_decoration.h>
#include <ksharedconfig.h>
#include "KisSettingsRegistry.h"
#include <kconfiggroup.h>

#include <functional>

#include <KoCanvasResourceProvider.h>
#include <KoSvgTextProperties.h>
#include <KoSvgTextPropertyData.h>
#include <kis_canvas_resource_provider.h>
#include <kis_pattern_chooser.h>
#include <resources/KoPattern.h>

#include "KisStudioFontPicker.h"
#include "KisStudioSymbols.h"
#include "widgets/KisStudioPopup.h"
#include "KisStudioTools.h"

namespace {

/// A picture drawn on a 20 px grid, \p draw given the painter and the scale
QIcon drawnIcon(const std::function<void(QPainter &, qreal)> &draw)
{
    QIcon icon;
    for (int size : {20, 40}) {
        QPixmap pixmap(size, size);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing, true);
        draw(painter, size / 20.0);
        painter.end();
        icon.addPixmap(pixmap);
    }
    return icon;
}

QPen line(const QColor &ink, qreal width)
{
    QPen pen(ink, width);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    return pen;
}

/// Two layers, the front one filled; with \p pointer, both outlined and an arrow over them
QIcon layersIcon(const QColor &ink, bool pointer)
{
    return drawnIcon([ink, pointer](QPainter &p, qreal s) {
        p.setPen(line(ink, 1.4 * s));
        p.setBrush(Qt::NoBrush);
        p.drawPath(KisModernStyle::squircle(QRectF(6 * s, 3 * s, 10 * s, 10 * s), 2.5 * s));
        p.setBrush(pointer ? QBrush(Qt::NoBrush) : QBrush(ink));
        p.drawPath(KisModernStyle::squircle(QRectF(3 * s, 6 * s, 10 * s, 10 * s), 2.5 * s));
        if (pointer) {
            QPainterPath arrow;
            arrow.moveTo(10 * s, 9 * s);
            arrow.lineTo(10 * s, 18.5 * s);
            arrow.lineTo(12.5 * s, 16 * s);
            arrow.lineTo(14.5 * s, 19.5 * s);
            arrow.lineTo(16 * s, 18.7 * s);
            arrow.lineTo(14 * s, 15.3 * s);
            arrow.lineTo(17.5 * s, 15.3 * s);
            arrow.closeSubpath();
            p.setPen(line(ink, 1.0 * s));
            p.setBrush(ink);
            p.drawPath(arrow);
        }
    });
}

/// A frame of \p ratio (width over height), dashed when free (0)
QIcon aspectIcon(const QColor &ink, qreal ratio)
{
    return drawnIcon([ink, ratio](QPainter &p, qreal s) {
        QPen pen = line(ink, 1.4 * s);
        if (ratio <= 0) pen.setDashPattern({2.0, 1.6});
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        const qreal r = ratio > 0 ? ratio : 1.3;
        const qreal w = r >= 1 ? 16 : 16 * r;
        const qreal h = r >= 1 ? 16 / r : 16;
        p.drawPath(KisModernStyle::squircle(QRectF((10 - w / 2) * s, (10 - h / 2) * s, w * s, h * s), 2 * s));
    });
}

/// A small picture: the whole image
QIcon imageIcon(const QColor &ink)
{
    return drawnIcon([ink](QPainter &p, qreal s) {
        p.setPen(line(ink, 1.4 * s));
        p.setBrush(Qt::NoBrush);
        p.drawPath(KisModernStyle::squircle(QRectF(2.5 * s, 4 * s, 15 * s, 12 * s), 2.5 * s));
        p.drawPolyline(QPolygonF({QPointF(4.5 * s, 14 * s), QPointF(8.5 * s, 9.5 * s), QPointF(11 * s, 12 * s), QPointF(13 * s, 10 * s), QPointF(15.5 * s, 13 * s)}));
    });
}

QIcon checkIcon(const QColor &ink)
{
    return drawnIcon([ink](QPainter &p, qreal s) {
        p.setPen(line(ink, 2 * s));
        p.drawPolyline(QPolygonF({QPointF(4 * s, 10.5 * s), QPointF(8.5 * s, 15 * s), QPointF(16 * s, 5.5 * s)}));
    });
}

/// A shape's fill: none, the foreground color, the background color or a pattern
QIcon fillIcon(const QColor &ink, const QColor &paper, int fill)
{
    return drawnIcon([ink, paper, fill](QPainter &p, qreal s) {
        const QRectF r(3 * s, 3 * s, 14 * s, 14 * s);
        const QPainterPath shape = KisModernStyle::squircle(r, 3.5 * s);
        if (fill == 1) p.fillPath(shape, ink);
        if (fill == 2) p.fillPath(shape, paper);
        if (fill == 3) {
            p.save();
            p.setClipPath(shape);
            for (int y = 0; y < 4; y++) {
                for (int x = 0; x < 4; x++) {
                    if ((x + y) % 2) p.fillRect(QRectF(r.left() + x * 3.5 * s, r.top() + y * 3.5 * s, 3.5 * s, 3.5 * s), ink);
                }
            }
            p.restore();
        }
        QPen pen = line(ink, 1.2 * s);
        if (fill == 0) pen.setDashPattern({2.0, 1.6});
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawPath(shape);
    });
}

/// A shape's outline: none, the brush, or the brush in the background color
QIcon outlineIcon(const QColor &ink, const QColor &paper, int outline)
{
    return drawnIcon([ink, paper, outline](QPainter &p, qreal s) {
        const QPainterPath shape = KisModernStyle::squircle(QRectF(4 * s, 4 * s, 12 * s, 12 * s), 3 * s);
        if (outline == 0) {
            QPen pen = line(ink, 1.2 * s);
            pen.setDashPattern({2.0, 1.6});
            p.setPen(pen);
        } else {
            // a thick line, with a thin ink edge when it is the (paler) background color
            p.setPen(line(ink, 3.2 * s));
            p.drawPath(shape);
            if (outline == 2) p.setPen(line(paper, 1.6 * s));
        }
        p.setBrush(Qt::NoBrush);
        p.drawPath(shape);
    });
}

/// The widget of \p root a KisOptionCollectionWidget keeps under \p id (it names the wrapper, not the widget)
template <typename T>
T *withId(QWidget *root, const QString &id)
{
    Q_FOREACH (T *widget, root->findChildren<T *>()) {
        if (widget->parent() && widget->parent()->property("id").toString() == id) return widget;
    }
    return nullptr;
}

/// The id of the KisOptionCollectionWidget section a widget is in, e.g. "sectionAction"
QString sectionId(QObject *object)
{
    for (; object; object = object->parent()) {
        const QVariant id = object->property("id");
        if (id.isValid() && id.toString().startsWith(QLatin1String("section"))) {
            return id.toString();
        }
    }
    return QString();
}

KisOptionButtonStrip *stripInSection(QWidget *root, const QString &section)
{
    Q_FOREACH (KisOptionButtonStrip *strip, root->findChildren<KisOptionButtonStrip *>()) {
        if (sectionId(strip) == section) {
            return strip;
        }
    }
    return nullptr;
}

QIcon gradientShapeIcon(int shape, const QColor &ink, const QColor &paper)
{
    QIcon icon;
    for (int size : {20, 40}) {
        QPixmap pixmap(size, size);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QRectF r = QRectF(0, 0, size, size).adjusted(size * 0.08, size * 0.08, -size * 0.08, -size * 0.08);
        QPainterPath clip;
        clip.addPath(KisModernStyle::squircle(r, size * 0.16));
        painter.setClipPath(clip);
        switch (shape) {
        case 0: { // linear
            QLinearGradient g(r.topLeft(), r.topRight());
            g.setColorAt(0, ink);
            g.setColorAt(1, paper);
            painter.fillRect(r, g);
            break;
        }
        case 1: { // bi-linear
            QLinearGradient g(r.topLeft(), r.topRight());
            g.setColorAt(0, paper);
            g.setColorAt(0.5, ink);
            g.setColorAt(1, paper);
            painter.fillRect(r, g);
            break;
        }
        case 2: { // radial
            QRadialGradient g(r.center(), r.width() * 0.62);
            g.setColorAt(0, ink);
            g.setColorAt(1, paper);
            painter.fillRect(r, g);
            break;
        }
        case 3: { // square
            const int steps = 8;
            for (int i = 0; i < steps; i++) {
                const qreal t = qreal(i) / (steps - 1);
                const qreal inset = r.width() / 2 * t;
                painter.fillRect(r.adjusted(inset, inset, -inset, -inset),
                                 QColor::fromRgbF(paper.redF() + (ink.redF() - paper.redF()) * t,
                                                  paper.greenF() + (ink.greenF() - paper.greenF()) * t,
                                                  paper.blueF() + (ink.blueF() - paper.blueF()) * t));
            }
            break;
        }
        default: { // conical
            QConicalGradient g(r.center(), 90);
            g.setColorAt(0, ink);
            g.setColorAt(1, paper);
            painter.fillRect(r, g);
            break;
        }
        }
        painter.end();
        icon.addPixmap(pixmap);
    }
    return icon;
}

/// Two opposite arrows
QIcon reverseIcon(const QColor &ink)
{
    QIcon icon;
    for (int size : {20, 40}) {
        QPixmap pixmap(size, size);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const qreal s = size / 20.0;
        QPen pen(ink, 1.5 * s);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        painter.setPen(pen);
        painter.drawLine(QPointF(4 * s, 7 * s), QPointF(16 * s, 7 * s));
        painter.drawPolyline(QPolygonF({QPointF(13 * s, 4 * s), QPointF(16 * s, 7 * s), QPointF(13 * s, 10 * s)}));
        painter.drawLine(QPointF(4 * s, 13 * s), QPointF(16 * s, 13 * s));
        painter.drawPolyline(QPolygonF({QPointF(7 * s, 10 * s), QPointF(4 * s, 13 * s), QPointF(7 * s, 16 * s)}));
        painter.end();
        icon.addPixmap(pixmap);
    }
    return icon;
}

/// A guide of Krita's type \p id, drawn as it lies on the canvas
QIcon guideIcon(const QString &id, const QColor &ink)
{
    return drawnIcon([id, ink](QPainter &p, qreal s) {
        const QPen pen = line(ink, 1.4 * s);
        QPen thin = line(ink, 1.0 * s);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        auto dot = [&p, ink, s](qreal x, qreal y) {
            p.save();
            p.setPen(Qt::NoPen);
            p.setBrush(ink);
            p.drawEllipse(QPointF(x * s, y * s), 2.0 * s, 2.0 * s);
            p.restore();
        };
        auto seg = [&p, s](qreal x1, qreal y1, qreal x2, qreal y2) { p.drawLine(QPointF(x1 * s, y1 * s), QPointF(x2 * s, y2 * s)); };
        if (id == QLatin1String("ruler")) {
            seg(4, 16, 16, 4);
            dot(4, 16);
            dot(16, 4);
        } else if (id == QLatin1String("infinite ruler")) {
            seg(1, 19, 19, 1);
            dot(7, 13);
            dot(13, 7);
        } else if (id == QLatin1String("parallel ruler")) {
            seg(3, 12, 12, 3);
            dot(3, 12);
            dot(12, 3);
            p.setPen(thin);
            seg(7, 17, 17, 7);
        } else if (id == QLatin1String("vanishing point")) {
            p.setPen(thin);
            for (qreal x : {1.0, 7.0, 13.0, 19.0}) seg(10, 6, x, 18);
            dot(10, 6);
        } else if (id == QLatin1String("two point")) {
            p.setPen(thin);
            seg(2, 7, 18, 7);
            for (qreal y : {10.0, 17.0}) {
                seg(2, 7, 10, y);
                seg(18, 7, 10, y);
            }
            p.setPen(pen);
            seg(10, 10, 10, 17);
            dot(2, 7);
            dot(18, 7);
        } else if (id == QLatin1String("perspective")) {
            QPolygonF quad({QPointF(6 * s, 4 * s), QPointF(14 * s, 4 * s), QPointF(18.5 * s, 16 * s), QPointF(1.5 * s, 16 * s)});
            p.drawPolygon(quad);
            p.setPen(thin);
            seg(10, 4, 10, 16);
            seg(3.8, 9.6, 16.2, 9.6);
        } else if (id == QLatin1String("fisheye-point")) {
            p.drawEllipse(QPointF(10 * s, 10 * s), 8 * s, 8 * s);
            p.setPen(thin);
            p.drawEllipse(QPointF(10 * s, 10 * s), 3.5 * s, 8 * s);
            seg(2, 10, 18, 10);
        } else if (id == QLatin1String("curvilinear-perspective")) {
            p.setPen(thin);
            for (qreal r : {5.0, 9.0}) p.drawArc(QRectF((10 - r) * s, (10 - r) * s, 2 * r * s, 2 * r * s), 20 * 16, 140 * 16);
            for (qreal r : {5.0, 9.0}) p.drawArc(QRectF((10 - r) * s, (10 - r) * s, 2 * r * s, 2 * r * s), 200 * 16, 140 * 16);
            p.setPen(pen);
            seg(1, 10, 19, 10);
        } else if (id == QLatin1String("ellipse")) {
            // tilted, held by its axis' ends
            p.save();
            p.translate(10 * s, 10 * s);
            p.rotate(-30);
            p.drawEllipse(QPointF(0, 0), 8.5 * s, 4.5 * s);
            p.restore();
            dot(10 + 7.36, 10 - 4.25);
            dot(10 - 7.36, 10 + 4.25);
        } else if (id == QLatin1String("concentric ellipse")) {
            p.drawEllipse(QPointF(10 * s, 10 * s), 8.5 * s, 6 * s);
            p.setPen(thin);
            p.drawEllipse(QPointF(10 * s, 10 * s), 4.5 * s, 3 * s);
        } else if (id == QLatin1String("perspective ellipse")) {
            p.setPen(thin);
            p.drawPolygon(QPolygonF({QPointF(6 * s, 4 * s), QPointF(14 * s, 4 * s), QPointF(19 * s, 16 * s), QPointF(1 * s, 16 * s)}));
            p.setPen(pen);
            p.drawEllipse(QRectF(4 * s, 5.5 * s, 12 * s, 9 * s));
        } else {
            QPainterPath curve;
            curve.moveTo(3 * s, 16 * s);
            curve.cubicTo(3 * s, 4 * s, 17 * s, 16 * s, 17 * s, 4 * s);
            p.drawPath(curve);
            p.setPen(thin);
            seg(3, 16, 3, 9);
            seg(17, 4, 17, 11);
            dot(3, 16);
            dot(17, 4);
        }
    });
}

/// Vertical lines too: a vertical through the horizon's two points
QIcon verticalIcon(const QColor &ink)
{
    return drawnIcon([ink](QPainter &p, qreal s) {
        p.setPen(line(ink, 1.0 * s));
        p.drawLine(QPointF(2 * s, 8 * s), QPointF(18 * s, 8 * s));
        p.setPen(line(ink, 1.6 * s));
        p.drawLine(QPointF(10 * s, 2 * s), QPointF(10 * s, 18 * s));
    });
}

/// A guide kept to an area: a line ending at a dashed box
QIcon areaIcon(const QColor &ink)
{
    return drawnIcon([ink](QPainter &p, qreal s) {
        QPen dashed = line(ink, 1.2 * s);
        dashed.setDashPattern({2.0, 1.8});
        p.setPen(dashed);
        p.drawPath(KisModernStyle::squircle(QRectF(4 * s, 4 * s, 12 * s, 12 * s), 2.5 * s));
        p.setPen(line(ink, 1.4 * s));
        p.drawLine(QPointF(6.5 * s, 13.5 * s), QPointF(13.5 * s, 6.5 * s));
    });
}

/// Pulled onto the guides: a magnet
QIcon magnetIcon(const QColor &ink)
{
    return drawnIcon([ink](QPainter &p, qreal s) {
        QPainterPath body;
        body.moveTo(5 * s, 3 * s);
        body.lineTo(5 * s, 10 * s);
        body.arcTo(QRectF(5 * s, 5 * s, 10 * s, 10 * s), 180, 180);
        body.lineTo(15 * s, 3 * s);
        p.setPen(line(ink, 3.0 * s));
        p.setBrush(Qt::NoBrush);
        p.drawPath(body);
        // the poles, cut off by a gap
        p.setCompositionMode(QPainter::CompositionMode_Clear);
        p.setPen(line(Qt::black, 1.0 * s));
        p.drawLine(QPointF(3.5 * s, 5.5 * s), QPointF(6.5 * s, 5.5 * s));
        p.drawLine(QPointF(13.5 * s, 5.5 * s), QPointF(16.5 * s, 5.5 * s));
    });
}

/// The guides shown on the canvas
QIcon eyeIcon(const QColor &ink)
{
    return drawnIcon([ink](QPainter &p, qreal s) {
        p.setPen(line(ink, 1.4 * s));
        QPainterPath eye;
        eye.moveTo(2 * s, 10 * s);
        eye.quadTo(10 * s, 2 * s, 18 * s, 10 * s);
        eye.quadTo(10 * s, 18 * s, 2 * s, 10 * s);
        p.drawPath(eye);
        p.setBrush(ink);
        p.drawEllipse(QPointF(10 * s, 10 * s), 2.2 * s, 2.2 * s);
    });
}

/// The window \p widget is in, whose tools and image it shows (the active window may be another)
KisMainWindow *windowOf(const QWidget *widget)
{
    if (auto *window = qobject_cast<KisMainWindow *>(widget->window())) return window;
    return KisPart::instance()->currentMainwindow();
}

/// Follows a widget's own shown state (not its window's) onto its mirror on the strip
class FollowShown : public QObject
{
public:
    FollowShown(QWidget *source, QWidget *mirror)
        : QObject(mirror)
        , m_mirror(mirror)
    {
        source->installEventFilter(this);
        mirror->setVisible(!source->isHidden());
    }

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        // hidden by Tucked, it still shows as far as its tool is concerned
        if (event->type() == QEvent::HideToParent && watched->property("studioTucking").toBool()) return false;
        if (event->type() == QEvent::ShowToParent || event->type() == QEvent::HideToParent) {
            m_mirror->setVisible(event->type() == QEvent::ShowToParent);
        }
        return QObject::eventFilter(watched, event);
    }

private:
    QWidget *m_mirror;
};

/// Follows a widget's enabled state onto its mirror on the strip
class FollowEnabled : public QObject
{
public:
    FollowEnabled(QWidget *source, QWidget *mirror)
        : QObject(mirror)
        , m_mirror(mirror)
    {
        source->installEventFilter(this);
        mirror->setEnabled(source->isEnabled());
    }

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::EnabledChange) m_mirror->setEnabled(static_cast<QWidget *>(watched)->isEnabled());
        return QObject::eventFilter(watched, event);
    }

private:
    QWidget *m_mirror;
};

/// Keeps a part of a tool's options out of sight while the bar does what it did, even when the tool shows it again
class Tucked : public QObject
{
public:
    explicit Tucked(QWidget *widget)
        : QObject(widget)
        , m_widget(widget)
    {
        widget->installEventFilter(this);
        hide(widget);
    }

    bool eventFilter(QObject *, QEvent *event) override
    {
        if (event->type() == QEvent::ShowToParent) {
            QPointer<QWidget> widget = m_widget;
            QTimer::singleShot(0, m_widget, [widget]() {
                if (widget) hide(widget);
            });
        }
        return false;
    }

    /// Hidden so that a mirror following it (FollowShown) stays
    static void hide(QWidget *widget)
    {
        widget->setProperty("studioTucking", true);
        widget->hide();
        widget->setProperty("studioTucking", false);
    }

private:
    QWidget *m_widget;
};

/// Keeps a part of a tool's options out of sight while \p when holds; apply() after it may have changed
class TuckedWhile : public QObject
{
public:
    TuckedWhile(QWidget *widget, std::function<bool()> when)
        : QObject(widget)
        , m_widget(widget)
        , m_when(when)
    {
        widget->installEventFilter(this);
        apply();
    }

    void apply()
    {
        if (m_when()) {
            Tucked::hide(m_widget);
        } else {
            m_widget->show();
        }
    }

    bool eventFilter(QObject *, QEvent *event) override
    {
        if (event->type() == QEvent::ShowToParent && m_when()) {
            QPointer<QWidget> widget = m_widget;
            QTimer::singleShot(0, m_widget, [widget]() {
                if (widget) Tucked::hide(widget);
            });
        }
        return false;
    }

private:
    QWidget *m_widget;
    std::function<bool()> m_when;
};

/// The title the tool options panel shows over \p widget, one of its options
QLabel *headerOf(QWidget *widget)
{
    if (!widget || !widget->parentWidget() || widget->windowTitle().isEmpty()) return nullptr;
    Q_FOREACH (QLabel *label, widget->parentWidget()->findChildren<QLabel *>(QString(), Qt::FindDirectChildrenOnly)) {
        if (label->text() == widget->windowTitle()) return label;
    }
    return nullptr;
}

void tuck(QWidget *widget)
{
    if (!widget || widget->property("studioTucked").toBool()) return;
    widget->setProperty("studioTucked", true);
    new Tucked(widget);
}

/// The label naming \p field in its row: a form's label, a grid's first column, or the label just before it in a row
QLabel *captionOf(QWidget *field)
{
    QWidget *parent = field ? field->parentWidget() : nullptr;
    if (!parent) return nullptr;
    Q_FOREACH (QLayout *layout, parent->findChildren<QLayout *>()) {
        const int at = layout->indexOf(field);
        if (at < 0) continue;
        if (auto *form = qobject_cast<QFormLayout *>(layout)) return qobject_cast<QLabel *>(form->labelForField(field));
        if (auto *grid = qobject_cast<QGridLayout *>(layout)) {
            int row = 0, column = 0, rows = 0, columns = 0;
            grid->getItemPosition(at, &row, &column, &rows, &columns);
            QLayoutItem *first = column > 0 ? grid->itemAtPosition(row, 0) : nullptr;
            return first ? qobject_cast<QLabel *>(first->widget()) : nullptr;
        }
        if (qobject_cast<QBoxLayout *>(layout) && at > 0) return qobject_cast<QLabel *>(layout->itemAt(at - 1)->widget());
    }
    return nullptr;
}

/// Tucks \p field with the label naming it
void tuckRow(QWidget *field)
{
    if (!field) return;
    tuck(field);
    tuck(captionOf(field));
}

/// Tucks \p field: its row of an option collection (KisOptionCollectionWidget), or itself with its label
void tuckOption(QWidget *field)
{
    if (!field) return;
    if (field->parentWidget() && field->parentWidget()->property("id").isValid()) {
        tuck(field->parentWidget());
    } else {
        tuckRow(field);
    }
}

/// Hides \p strip's section header, and the section with it while nothing else in it shows
void tuckSection(KisOptionButtonStrip *strip)
{
    auto *section = strip ? qobject_cast<KisOptionCollectionWidgetWithHeader *>(strip->parentWidget()) : nullptr;
    if (!section || section->property("studioTucked").toBool()) return;
    QWidget *wrapper = section;
    while (wrapper && !qobject_cast<KisOptionCollectionWidget *>(wrapper->parentWidget())) wrapper = wrapper->parentWidget();
    if (!wrapper) return;
    section->setProperty("studioTucked", true);
    auto *collection = qobject_cast<KisOptionCollectionWidget *>(wrapper->parentWidget());
    const QString id = wrapper->property("id").toString();
    // what else it holds goes on without the title, which named the buttons
    tuck(section->findChild<QLabel *>(QString(), Qt::FindDirectChildrenOnly));
    auto update = [section, collection, id]() {
        section->setPrimaryWidgetVisible(false);
        collection->setWidgetVisible(id, section->numberOfVisibleWidgets() > 0);
    };
    update();
    // a fill with a pattern shows its scale and turn there
    Q_FOREACH (KoGroupButton *button, strip->buttons()) {
        QObject::connect(button, &QAbstractButton::toggled, section, update);
    }
}

/// True for a control a person can change
bool isInput(const QWidget *widget)
{
    return qobject_cast<const QAbstractButton *>(widget) || qobject_cast<const QAbstractSpinBox *>(widget)
           || qobject_cast<const QComboBox *>(widget) || qobject_cast<const QAbstractSlider *>(widget)
           || qobject_cast<const QLineEdit *>(widget);
}

} // namespace

KisStudioToolStrip::KisStudioToolStrip(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("studioToolStrip"));
    m_layout = new QHBoxLayout(this);
    m_layout->setContentsMargins(KisModernStyle::space1, 0, KisModernStyle::space1, 0);
    m_layout->setSpacing(KisModernStyle::hairline);
}

void KisStudioToolStrip::clear()
{
    m_more = nullptr;
    if (m_lent) {
        m_lent->hide();
        m_lent->deleteLater();
    }
    m_moreLater.clear();
    while (QLayoutItem *item = m_layout->takeAt(0)) {
        if (QWidget *widget = item->widget()) {
            // hide it so it doesn't cover the new buttons until it is deleted
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }
}

void KisStudioToolStrip::addGap()
{
    if (m_layout->count() > 0) {
        m_layout->addSpacing(10);
    }
}

QToolButton *KisStudioToolStrip::addButton(const QIcon &icon, const QString &toolTip)
{
    auto *button = new QToolButton(this);
    button->setAutoRaise(true);
    button->setCheckable(true);
    button->setFocusPolicy(Qt::NoFocus);
    button->setIconSize(QSize(20, 20));
    button->setIcon(icon);
    button->setToolTip(QString(toolTip).remove(QLatin1Char('&')));
    m_layout->addWidget(button);
    return button;
}

QToolButton *KisStudioToolStrip::mirror(QAbstractButton *source, const QIcon &icon)
{
    if (!source) {
        return nullptr;
    }
    QToolButton *button = addButton(icon.isNull() ? source->icon() : icon, source->toolTip());
    button->setChecked(source->isChecked());
    QPointer<QAbstractButton> target = source;
    connect(button, &QToolButton::clicked, this, [button, target]() {
        if (target) {
            target->click();
            button->setChecked(target->isChecked());
        }
    });
    connect(source, &QAbstractButton::toggled, button, &QToolButton::setChecked);
    return button;
}

void KisStudioToolStrip::addTransformModes(QWidget *options)
{
    // named by what they do: move, bend, push; Krita's other modes behind ⋯
    addGap();
    const QList<QPair<const char *, QString>> shown = {
        {"freeTransformButton", i18nc("@info:tooltip transform mode", "Move")},
        {"bendButton", i18nc("@info:tooltip transform mode: pins bend the picture", "Bend")},
        {"liquifyButton", i18nc("@info:tooltip transform mode: a brush pushes the picture", "Push")},
    };
    for (const auto &mode : shown) {
        if (QToolButton *button = mirror(options->findChild<QAbstractButton *>(QLatin1String(mode.first)))) {
            button->setToolTip(mode.second);
        }
    }

    // the bar shows the modes, so the options keep only the chosen one's own
    if (QAbstractButton *free = options->findChild<QAbstractButton *>(QStringLiteral("freeTransformButton"))) {
        if (free->parentWidget() != options) tuck(free->parentWidget());
    }
    tuck(options->findChild<QWidget *>(QStringLiteral("lblTransformType")));
    // the handles on the canvas do what the numbers did; the preview is always the accurate one there is
    tuckRow(options->findChild<QWidget *>(QStringLiteral("cmbPreviewMode")));
    for (const char *name : {"topLeftButton", "middleTopButton", "topRightButton", "middleLeftButton", "centerButton", "middleRightButton",
                             "bottomLeftButton", "middleBottomButton", "bottomRightButton", "btnTransformAroundPivotPoint",
                             "freeMoveRadioButton", "freeRotationRadioButton", "freeScaleRadioButton", "freeShearRadioButton",
                             "rotationGroup", "scaleGroup", "moveGroup", "shearGroup", "quickTransformGroup"}) {
        tuck(options->findChild<QWidget *>(QLatin1String(name)));
    }
    // as tall as the mode's own page, not the tallest (Bend's and Push's pages are much taller)
    if (auto *pages = options->findChild<QStackedWidget *>(QStringLiteral("stackedWidget"))) {
        QPointer<QStackedWidget> stack = pages;
        auto fit = [stack]() {
            if (!stack) return;
            for (int i = 0; i < stack->count(); i++) {
                const QSizePolicy::Policy policy = i == stack->currentIndex() ? QSizePolicy::Preferred : QSizePolicy::Ignored;
                stack->widget(i)->setSizePolicy(policy, policy);
            }
            stack->updateGeometry();
        };
        fit();
        connect(pages, &QStackedWidget::currentChanged, pages, fit);
    }
    // flips and quarter turns, behind ⋯
    const QList<QPair<const char *, QString>> quick = {
        {"flipXButton", i18nc("transform: mirror left to right", "Flip horizontally")},
        {"flipYButton", i18nc("transform: mirror top to bottom", "Flip vertically")},
        {"rotateCCWButton", i18nc("transform: a quarter turn", "Turn left")},
        {"rotateCWButton", i18nc("transform: a quarter turn", "Turn right")},
    };
    for (const auto &command : quick) {
        if (QAbstractButton *source = options->findChild<QAbstractButton *>(QLatin1String(command.first))) {
            m_moreLater << MoreEntry{source, command.second, false};
        }
    }

    QList<QPointer<QAbstractButton>> others;
    for (const char *name : {"perspectiveTransformButton", "warpButton", "cageButton", "meshButton"}) {
        if (QAbstractButton *source = options->findChild<QAbstractButton *>(QLatin1String(name))) others << source;
    }
    if (others.isEmpty()) return;

    const QIcon moreIcon = KisIconUtils::loadIcon(QStringLiteral("studio_more"));
    QToolButton *more = addButton(moreIcon, i18nc("@info:tooltip more transform modes", "More"));
    more->setPopupMode(QToolButton::InstantPopup);
    auto *menu = new QMenu(more);
    for (const QPointer<QAbstractButton> &source : others) {
        QAction *action = menu->addAction(source->icon(), source->toolTip());
        QPointer<QAbstractButton> target = source;
        connect(action, &QAction::triggered, source, [target]() {
            if (target) target->click();
        });
    }
    more->setMenu(menu);
    // the transform's other commands go behind it too
    m_more = more;

    // while one of them is the mode, ⋯ shows it, lit
    auto showMode = [more, moreIcon, others]() {
        for (const QPointer<QAbstractButton> &source : others) {
            if (source && source->isChecked()) {
                more->setIcon(source->icon());
                more->setChecked(true);
                return;
            }
        }
        more->setIcon(moreIcon);
        more->setChecked(false);
    };
    for (const QPointer<QAbstractButton> &source : others) {
        connect(source, &QAbstractButton::toggled, more, showMode);
    }
    showMode();
}

void KisStudioToolStrip::mirrorCombo(QComboBox *combo, const QVector<int> &indexes, const QVector<QIcon> &icons)
{
    if (!combo) {
        return;
    }
    QPointer<QComboBox> target = combo;
    for (int i = 0; i < indexes.size(); i++) {
        const int index = indexes[i];
        if (index >= combo->count()) {
            continue;
        }
        QToolButton *button = addButton(icons.value(i), combo->itemText(index));
        button->setChecked(combo->currentIndex() == index);
        connect(button, &QToolButton::clicked, this, [button, target, index]() {
            if (target) {
                target->setCurrentIndex(index);
                // clicking the checked button unchecks it without changing the index
                button->setChecked(target->currentIndex() == index);
            }
        });
        connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged), button, [button, index](int current) {
            button->setChecked(current == index);
        });
    }
}

void KisStudioToolStrip::mirrorComboAsMenu(QComboBox *combo, const QVector<QIcon> &icons, const QString &toolTip)
{
    if (!combo) return;
    QPointer<QComboBox> target = combo;
    QToolButton *button = addButton(icons.value(combo->currentIndex()), QString());
    button->setCheckable(false);
    button->setPopupMode(QToolButton::InstantPopup);
    auto *menu = new QMenu(button);
    for (int i = 0; i < combo->count() && i < icons.size(); i++) {
        QAction *action = menu->addAction(icons[i], KisSentenceCase::of(combo->itemText(i)));
        // the picture is what the button will show
        action->setIconVisibleInMenu(true);
        connect(action, &QAction::triggered, this, [target, i]() {
            if (target) target->setCurrentIndex(i);
        });
    }
    button->setMenu(menu);
    auto show = [button, icons, toolTip, target](int index) {
        if (!target) return;
        button->setIcon(icons.value(index));
        button->setToolTip(toolTip + QLatin1Char('\n') + KisSentenceCase::of(target->itemText(index)));
    };
    show(combo->currentIndex());
    connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged), button, show);
}

void KisStudioToolStrip::mirrorSpin(QSpinBox *spin, int kind, const QString &toolTip, int defaultValue)
{
    if (!spin) return;
    QPointer<QSpinBox> target = spin;
    auto *scrubber = new KisStudioScrubber(KisStudioScrubber::Kind(kind), toolTip, this);
    scrubber->setRange(spin->minimum(), spin->maximum());
    scrubber->setValue(spin->value());
    scrubber->setDefaultValue(defaultValue);
    connect(scrubber, &KisStudioScrubber::valueCommitted, this, [target](qreal value) {
        if (target) target->setValue(qRound(value));
    });
    connect(spin, QOverload<int>::of(&QSpinBox::valueChanged), scrubber, [scrubber](int value) { scrubber->setValue(value); });
    // followed while dragged: a magic wand's selection is made again as it goes
    connect(scrubber, &KisStudioScrubber::valueChanging, this, [target](qreal value) {
        if (target) target->setValue(qRound(value));
    });
    m_layout->addWidget(scrubber);
    // a row of the options of its own
    if (spin->parentWidget() && spin->parentWidget()->property("id").isValid()) tuck(spin->parentWidget());
}

void KisStudioToolStrip::addCropAspects(QWidget *options)
{
    QPointer<QDoubleSpinBox> ratio = options->findChild<QDoubleSpinBox *>(QStringLiteral("doubleRatio"));
    QPointer<QAbstractButton> lock = options->findChild<QAbstractButton *>(QStringLiteral("lockRatioButton"));
    if (!ratio || !lock) return;
    const QColor ink = palette().color(QPalette::ButtonText);
    addGap();
    // 0: free; -1: the image's own
    const QVector<qreal> ratios = {0, 1, 4.0 / 3, 16.0 / 9, -1};
    const QStringList names = {i18nc("@info:tooltip a crop of any shape", "Free"), i18nc("@info:tooltip a crop's shape", "Square"),
                               QStringLiteral("4:3"), QStringLiteral("16:9"), i18nc("@info:tooltip a crop the image's own shape", "As the image")};
    const QPointer<KisMainWindow> own = windowOf(this);
    auto imageRatio = [own]() {
        KisMainWindow *window = own.data();
        KisImageSP image = window && window->viewManager() ? KisImageSP(window->viewManager()->image()) : KisImageSP();
        return image && image->height() > 0 ? qreal(image->width()) / image->height() : 1.0;
    };
    QVector<QPointer<QToolButton>> buttons;
    for (int i = 0; i < ratios.size(); i++) {
        const qreal wanted = ratios[i];
        QToolButton *button = addButton(wanted < 0 ? imageIcon(ink) : aspectIcon(ink, wanted), names[i]);
        buttons << button;
        connect(button, &QToolButton::clicked, this, [ratio, lock, wanted, imageRatio]() {
            if (!ratio || !lock) return;
            if (wanted == 0) {
                lock->setChecked(false);
                return;
            }
            ratio->setValue(wanted < 0 ? imageRatio() : wanted);
            lock->setChecked(true);
        });
    }
    // the one that is so lit: free while unlocked, else the ratio it holds
    auto light = [buttons, ratios, ratio, lock, imageRatio]() {
        if (!ratio || !lock) return;
        bool found = false;
        for (int i = 0; i < buttons.size(); i++) {
            const qreal wanted = ratios[i] < 0 ? imageRatio() : ratios[i];
            const bool on = !found && (ratios[i] == 0 ? !lock->isChecked() : lock->isChecked() && qAbs(ratio->value() - wanted) < 0.01);
            found = found || on;
            if (buttons[i]) buttons[i]->setChecked(on);
        }
    };
    light();
    // gone with the buttons when the strip is made again
    connect(ratio, QOverload<double>::of(&QDoubleSpinBox::valueChanged), buttons.first(), light);
    connect(lock, &QAbstractButton::toggled, buttons.first(), light);
    for (const QPointer<QToolButton> &button : buttons) connect(button, &QToolButton::clicked, button, light);
}

void KisStudioToolStrip::addSelectionAdjust(bool gap)
{
    if (!m_adjust) m_adjust = new KisStudioSelectionAdjust(this);
    if (gap) addGap();
    // the selection there is, changed as the value is dragged; at rest each is named, as it is an amount to apply
    KisStudioScrubber *grow = new KisStudioScrubber(KisStudioScrubber::Kind::Grow, i18nc("@info:tooltip grow the selection", "Grow"), this);
    grow->setAccessibleName(i18nc("the selection's grow scrubber, at rest", "Grow"));
    grow->setRange(-200, 200);
    KisStudioScrubber *soft = new KisStudioScrubber(KisStudioScrubber::Kind::Soft, i18nc("@info:tooltip soften the selection's edge", "Soft edge"), this);
    soft->setAccessibleName(i18nc("the selection's soft edge scrubber, at rest", "Soft"));
    soft->setRange(0, 200);
    for (KisStudioScrubber *scrubber : {grow, soft}) {
        const bool growing = scrubber == grow;
        connect(scrubber, &KisStudioScrubber::dragStarted, this, [this]() { m_adjust->begin(); });
        connect(scrubber, &KisStudioScrubber::valueChanging, this, [this, growing](qreal value) {
            m_adjust->set(growing ? qRound(value) : 0, growing ? 0 : qRound(value));
        });
        connect(scrubber, &KisStudioScrubber::valueCommitted, this, [this, scrubber, growing](qreal value) {
            // a step, a typed amount: at once; a drag the image ended meanwhile has nothing left to apply
            if (!m_adjust->isActive()) {
                if (m_adjust->takeInterrupted()) {
                    scrubber->setValue(0);
                    return;
                }
                m_adjust->begin();
            }
            m_adjust->set(growing ? qRound(value) : 0, growing ? 0 : qRound(value));
            m_adjust->end();
            scrubber->setValue(0);
        });
        m_layout->addWidget(scrubber);
    }
}

void KisStudioToolStrip::addSelectionCommands()
{
    // letting the selection go and turning it inside out, a pen's reach from the keys
    KisMainWindow *window = windowOf(this);
    if (!window) return;
    bool gap = false;
    for (const auto &command : {qMakePair(QStringLiteral("deselect"), QStringLiteral("studio_deselect")),
                                qMakePair(QStringLiteral("invert_selection"), QStringLiteral("studio_select_invert"))}) {
        QAction *action = window->actionCollection()->action(command.first);
        if (!action) continue;
        if (!gap) addGap();
        gap = true;
        QToolButton *button = addButton(KisIconUtils::loadIcon(command.second), KisSentenceCase::of(QString(action->text()).remove(QLatin1Char('&'))));
        button->setCheckable(false);
        const QPointer<QAction> target = action;
        connect(button, &QToolButton::clicked, this, [target]() {
            if (target) target->trigger();
        });
        // always there, so the bar keeps its width and the buttons their places; usable while there is a selection
        auto follow = [button, target]() { button->setEnabled(target && target->isEnabled()); };
        follow();
        connect(action, &QAction::changed, button, follow);
    }
}

void KisStudioToolStrip::addGuides(QWidget *options, QComboBox *types)
{
    const QColor ink = palette().color(QPalette::ButtonText);
    QPointer<QComboBox> target = types;

    // the chosen guide's box: its grip and its eye (off, the guide fades and pulls no strokes); the rest is on its
    // right click
    for (const char *name : {"showLock", "showDuplicate", "showDelete"}) {
        if (QCheckBox *show = options->findChild<QCheckBox *>(QLatin1String(name))) show->setChecked(false);
    }
    for (const char *name : {"showMove", "showSnap"}) {
        if (QCheckBox *show = options->findChild<QCheckBox *>(QLatin1String(name))) show->setChecked(true);
    }

    // by what is drawn with them, each kind's ways in its menu
    struct Kind {
        QString name;
        QVector<QPair<QString, QString>> ways;
    };
    const QVector<Kind> kinds = {
        {i18nc("a guide: a ruler", "Line"),
         {{QStringLiteral("ruler"), i18nc("a line guide between two points", "Segment")},
          {QStringLiteral("infinite ruler"), i18nc("a line guide through two points, without end", "Endless")},
          {QStringLiteral("parallel ruler"), i18nc("lines parallel to a guide", "Parallel")}}},
        {i18nc("a guide", "Perspective"),
         {{QStringLiteral("vanishing point"), i18nc("perspective guide", "1 point")},
          {QStringLiteral("two point"), i18nc("perspective guide", "2 point")},
          {QStringLiteral("perspective"), i18nc("perspective guide: a grid on a plane", "Grid")},
          {QStringLiteral("fisheye-point"), i18nc("perspective guide", "Fisheye")},
          {QStringLiteral("curvilinear-perspective"), i18nc("perspective guide: curved lines", "Curved")}}},
        {i18nc("a guide", "Ellipse"),
         {{QStringLiteral("ellipse"), i18nc("an ellipse guide", "Ellipse")},
          {QStringLiteral("concentric ellipse"), i18nc("ellipses inside each other", "Concentric")},
          {QStringLiteral("perspective ellipse"), i18nc("an ellipse on a plane in perspective", "In perspective")}}},
        {i18nc("a guide: a spline", "Curve"), {{QStringLiteral("spline"), i18nc("a guide: a spline", "Curve")}}},
    };
    // the way each kind was last used, so a click on it makes that again
    auto rememberedWay = [](const Kind &kind) {
        return KSharedConfig::openConfig()->group(QStringLiteral("StudioGuides")).readEntry(kind.name, kind.ways.first().first);
    };

    addGap();
    for (const Kind &kind : kinds) {
        QToolButton *button = addButton(QIcon(), kind.name);
        auto *menu = new QMenu(button);
        for (const auto &way : kind.ways) {
            QAction *action = menu->addAction(guideIcon(way.first, ink), way.second);
            action->setIconVisibleInMenu(true);
            action->setCheckable(true);
            action->setData(way.first);
            connect(action, &QAction::triggered, this, [target, way]() {
                if (target) target->setCurrentIndex(target->findData(way.first));
            });
        }
        auto inUse = [kind, target]() {
            const QString current = target ? target->currentData().toString() : QString();
            for (const auto &way : kind.ways) {
                if (way.first == current) return true;
            }
            return false;
        };
        // lit while one of its ways is chosen, showing the way it makes
        // picking a kind can remake the bar (the tool's options change): the button may be gone by the time it is shown
        QPointer<QToolButton> guarded = button;
        auto show = [guarded, menu, kind, target, inUse, rememberedWay]() {
            if (!target || !guarded) return;
            QToolButton *button = guarded;
            const QString shown = inUse() ? target->currentData().toString() : rememberedWay(kind);
            QString wayName;
            Q_FOREACH (QAction *action, menu->actions()) {
                action->setChecked(action->data().toString() == shown);
                if (action->isChecked()) wayName = action->text();
            }
            button->setChecked(inUse());
            button->setIcon(guideIcon(shown, button->palette().color(QPalette::ButtonText)));
            button->setToolTip(kind.ways.size() > 1 ? kind.name + QLatin1Char('\n') + wayName : kind.name);
        };
        show();
        connect(types, QOverload<int>::of(&QComboBox::currentIndexChanged), button, [show, kind, target, inUse]() {
            if (target && inUse()) {
                KSharedConfig::openConfig()->group(QStringLiteral("StudioGuides")).writeEntry(kind.name, target->currentData().toString());
                KisSettingsRegistry::instance()->scheduleSync();
            }
            show();
        });
        // a click picks the kind; on the kind in use, its ways, opened without waiting: picking one may remake the bar
        connect(button, &QToolButton::clicked, this, [button, menu, kind, target, show, inUse, rememberedWay]() {
            if (!target) return;
            if (!inUse()) {
                target->setCurrentIndex(target->findData(rememberedWay(kind)));
            } else if (kind.ways.size() > 1) {
                menu->popup(button->mapToGlobal(QPoint(0, button->height())));
            }
            show();
        });
    }

    // the chosen guide's own switches, while it has them
    QAbstractButton *vertical = options->findChild<QAbstractButton *>(QStringLiteral("twoPointUseVerticalCheckbox"));
    QAbstractButton *area = options->findChild<QAbstractButton *>(QStringLiteral("localAssistantCheckbox"));
    if (vertical || area) addGap();
    for (QAbstractButton *source : {vertical, area}) {
        if (!source) continue;
        mirror(source, source == vertical ? verticalIcon(ink) : areaIcon(ink));
        auto *button = qobject_cast<QToolButton *>(m_layout->itemAt(m_layout->count() - 1)->widget());
        if (!button) continue;
        button->setToolTip(source == vertical ? i18nc("@info:tooltip a perspective guide's vertical lines", "Verticals too")
                                              : i18nc("@info:tooltip a guide that works only in an area", "Only in an area"));
        new FollowShown(source, button);
    }

    // showing them, and the rest
    addGap();
    if (KisMainWindow *window = windowOf(this)) {
        if (QAction *eye = window->actionCollection()->action(QStringLiteral("view_toggle_painting_assistants"))) {
            QToolButton *button = addButton(eyeIcon(ink), i18nc("@info:tooltip show the guides on the canvas", "Show guides"));
            button->setChecked(eye->isChecked());
            QPointer<QAction> action = eye;
            connect(button, &QToolButton::clicked, this, [button, action]() {
                if (!action) return;
                action->trigger();
                button->setChecked(action->isChecked());
            });
            connect(eye, &QAction::toggled, button, &QToolButton::setChecked);
        }
    }
    QMenu *menu = addMore();
    auto clickOn = [menu, options](const QString &text, const char *name) {
        QPointer<QAbstractButton> source = options->findChild<QAbstractButton *>(QLatin1String(name));
        if (!source) return;
        connect(menu->addAction(text), &QAction::triggered, source, [source]() {
            if (source) source->click();
        });
    };
    clickOn(i18nc("guides: choose a color for them", "Color…"), "assistantsColor");
    menu->addSeparator();
    clickOn(i18nc("guides: open a saved set", "Open…"), "loadAssistantButton");
    clickOn(i18nc("guides: save them as a set", "Save…"), "saveAssistantButton");
    menu->addSeparator();
    clickOn(i18nc("guides: remove every guide", "Delete all"), "deleteAllAssistantsButton");

    // the options keep what the chosen guide has of its own, and how strongly guides show; the rest is on the bar,
    // and the box beside each guide has only its grip
    for (const char *name : {"label", "availableAssistantsComboBox", "localAssistantCheckbox", "line_2", "twoPointUseVerticalCheckbox",
                             "globalColorLabel", "assistantsColor", "dockerOptionsLine", "showDockerOptionsButton",
                             "showDockerOptionsLabel", "showDockerOptionsPanel", "line", "loadAssistantButton",
                             "saveAssistantButton", "deleteAllAssistantsButton"}) {
        tuck(options->findChild<QWidget *>(QLatin1String(name)));
    }
}

void KisStudioToolStrip::setOptionWidgets(const QList<QPointer<QWidget>> &widgets)
{
    clear();
    m_options = widgets;
    QObject *text = nullptr;
    // full contrast, so the small icons' shapes read at a glance
    const QColor ink = palette().color(QPalette::ButtonText);
    const QColor paper = palette().color(QPalette::Window).darker(170);

    Q_FOREACH (const QPointer<QWidget> &widget, widgets) {
        if (!widget) {
            continue;
        }

        // transform modes
        if (widget->findChild<QAbstractButton *>(QStringLiteral("freeTransformButton"))) {
            addTransformModes(widget);
        }

        // selection modes: replace, add, subtract, intersect (Shift and Alt do the same while dragging)
        KisSelectionOptions *selection = qobject_cast<KisSelectionOptions *>(widget.data());
        if (!selection) {
            selection = widget->findChild<KisSelectionOptions *>();
        }
        if (selection) {
            if (KisOptionButtonStrip *actions = stripInSection(selection, QStringLiteral("sectionAction"))) {
                addGap();
                for (int index : {0, 2, 3, 1}) {
                    mirror(actions->button(index));
                }
                tuckSection(actions);
            }
            addSelectionCommands();
            // the bar's Grow and Soft do what Krita's grow and feather did (kept while set, so nothing set is hidden);
            // the selection actions bar is a setting, not a tool's option
            if (QCheckBox *bar = withId<QCheckBox>(selection, QStringLiteral("sapCheckBox"))) tuck(bar->parentWidget());
            // pixels or vectors, chosen rarely: behind ⋯
            if (KisOptionButtonStrip *modes = stripInSection(selection, QStringLiteral("sectionMode"))) {
                if (modes->isVisibleTo(selection) && modes->buttons().size() == 2) {
                    addMoreSwitch(modes->buttons()[0], i18nc("a selection made of pixels", "Pixel selection"));
                    addMoreSwitch(modes->buttons()[1], i18nc("a selection made of vector shapes", "Vector selection"));
                    tuckSection(modes);
                }
            }
            QWidget *grow = withId<QWidget>(selection, QStringLiteral("containerGrowSelection"));
            QSpinBox *growBy = grow ? grow->findChild<QSpinBox *>() : nullptr;
            if (grow && (!growBy || growBy->value() == 0)) tuck(grow->parentWidget());
            QSpinBox *feather = withId<QSpinBox>(selection, QStringLiteral("sliderFeather"));
            if (feather && feather->value() == 0) tuck(feather->parentWidget());
            // selecting by color: how alike counts as one, changed all the time
            if (QSpinBox *threshold = withId<QSpinBox>(widget, QStringLiteral("sliderThreshold"))) {
                addGap();
                const bool contiguous = withId<QSpinBox>(widget, QStringLiteral("sliderCloseGap"));
                mirrorSpin(threshold, int(KisStudioScrubber::Kind::Threshold),
                           i18nc("@info:tooltip how alike colors are selected as one", "Threshold"), contiguous ? 8 : 20);
            }
            // the values together, the tool's own first
            addSelectionAdjust(!withId<QSpinBox>(widget, QStringLiteral("sliderThreshold")));
        }

        // the color sampler: from the whole image or the layer, and how wide a patch it averages
        if (QComboBox *sources = widget->findChild<QComboBox *>(QStringLiteral("cmbSources"))) {
            addGap();
            mirrorCombo(sources, {0, 1}, {imageIcon(ink), layersIcon(ink, false)});
            tuckRow(sources);
            const QStringList names = {i18nc("@info:tooltip the color sampler takes the color as seen", "Image"),
                                       i18nc("@info:tooltip the color sampler takes the current layer's color", "Layer")};
            for (int i = 0; i < 2; i++) {
                if (QLayoutItem *item = m_layout->itemAt(m_layout->count() - 2 + i)) {
                    if (item->widget()) item->widget()->setToolTip(names[i]);
                }
            }
            if (QSpinBox *radius = widget->findChild<QSpinBox *>(QStringLiteral("radius"))) {
                addGap();
                mirrorSpin(radius, int(KisStudioScrubber::Kind::Size), i18nc("@info:tooltip the color sampler averages a patch this wide", "Sample size"), 1);
                tuckRow(radius);
            }
            // whether a sample sets the paint color, behind ⋯; the channels' numbers are shown by the color panel
            addMoreSwitch(widget->findChild<QAbstractButton *>(QStringLiteral("cbUpdateCurrentColor")));
            tuck(widget->findChild<QWidget *>(QStringLiteral("cbNormaliseValues")));
            tuck(widget->findChild<QWidget *>(QStringLiteral("listViewChannels")));
        }

        // what Move moves: the layer under the pointer, or the current one
        if (widget->objectName() == QLatin1String("WdgMoveTool") || widget->findChild<QAbstractButton *>(QStringLiteral("radioFirstLayer"))) {
            QAbstractButton *underPointer = widget->findChild<QAbstractButton *>(QStringLiteral("radioFirstLayer"));
            QAbstractButton *current = widget->findChild<QAbstractButton *>(QStringLiteral("radioSelectedLayer"));
            if (underPointer && current) {
                addGap();
                mirror(underPointer, layersIcon(ink, true));
                mirror(current, layersIcon(ink, false));
            }
        }

        // the crop: its shape, what it crops, and doing it
        if (widget->findChild<QDoubleSpinBox *>(QStringLiteral("doubleRatio")) && widget->findChild<QAbstractButton *>(QStringLiteral("bnCrop"))) {
            addCropAspects(widget);
            if (QComboBox *type = widget->findChild<QComboBox *>(QStringLiteral("cmbType"))) {
                addGap();
                mirrorCombo(type, {0, 2}, {imageIcon(ink), layersIcon(ink, false)});
            }
            addGap();
            mirror(widget->findChild<QAbstractButton *>(QStringLiteral("bnCrop")), checkIcon(ink));
        }

        // a shape's fill and outline, each a picture with the rest in its menu
        if (QComboBox *fill = widget->findChild<QComboBox *>(QStringLiteral("cmbFill"))) {
            addGap();
            QVector<QIcon> fills;
            for (int i = 0; i < 4; i++) fills << fillIcon(ink, paper, i);
            mirrorComboAsMenu(fill, fills, i18nc("@info:tooltip a shape's fill", "Fill"));
            tuckRow(fill);
            // what is left of them is the pattern's turn and scale: shown, title and all, while it fills with one
            if (widget->findChild<QWidget *>(QStringLiteral("gbPatternTransform"))) {
                QPointer<QComboBox> by = fill;
                auto noPattern = [by]() { return by && by->currentIndex() != 3; };
                QVector<TuckedWhile *> tucked;
                for (QWidget *part : {static_cast<QWidget *>(widget.data()), static_cast<QWidget *>(headerOf(widget))}) {
                    if (part) tucked << new TuckedWhile(part, noPattern);
                }
                connect(fill, QOverload<int>::of(&QComboBox::currentIndexChanged), widget, [tucked]() {
                    for (TuckedWhile *part : tucked) part->apply();
                });
            }
            if (QComboBox *outline = widget->findChild<QComboBox *>(QStringLiteral("cmbOutline"))) {
                QVector<QIcon> outlines;
                for (int i = 0; i < 3; i++) outlines << outlineIcon(ink, paper, i);
                mirrorComboAsMenu(outline, outlines, i18nc("@info:tooltip a shape's outline", "Outline"));
                tuckRow(outline);
            }
        }

        // painting pulled onto the image's guides, while it has some
        if (QCheckBox *snap = widget->findChild<QCheckBox *>(QStringLiteral("chkAssistant"))) {
            KisMainWindow *window = windowOf(this);
            KisViewManager *view = window ? window->viewManager() : nullptr;
            KisCanvas2 *canvas = view ? view->canvasBase() : nullptr;
            if (canvas && canvas->paintingAssistantsDecoration() && !canvas->paintingAssistantsDecoration()->assistants().isEmpty()) {
                QToolButton *button = mirror(snap, magnetIcon(ink));
                button->setToolTip(i18nc("@info:tooltip lines drawn are pulled onto the image's guides", "Snap to guides"));
                // its card records a key for Krita's command that toggles it (the brush tools')
                if (QAction *toggle = window ? window->actionCollection()->action(QStringLiteral("toggle_assistant")) : nullptr) {
                    button->setProperty("studioCardAction", QVariant::fromValue<QObject *>(toggle));
                }
                // a switch of how the brush paints, as symmetry is: among the bar's buttons, before it
                if (KisStudioBarButtons *bar = window ? KisStudioBarButtons::of(window) : nullptr) {
                    m_layout->removeWidget(button);
                    bar->setLeading(button);
                    m_lent = button;
                }
            }
        }

        if (widget->findChild<QAbstractButton *>(QStringLiteral("bnAddReferenceImage"))) {
            addReferenceImages(widget);
        }

        if (widget->findChild<QWidget *>(QStringLiteral("optionButtonStripToolMode"))) {
            addGutter(widget);
        }

        if (QObject *manager = widget->property("studioTextOptions").value<QObject *>()) text = manager;

        // guides: their kinds, the chosen one's switches, showing them
        if (QComboBox *types = widget->findChild<QComboBox *>(QStringLiteral("availableAssistantsComboBox"))) {
            addGuides(widget, types);
        }

        // gradient shape and direction
        if (QComboBox *shape = widget->findChild<QComboBox *>(QStringLiteral("shape_combo"))) {
            addGap();
            QVector<QIcon> icons;
            for (int s = 0; s < 5; s++) icons << gradientShapeIcon(s, ink, paper);
            mirrorCombo(shape, {0, 1, 2, 3, 4}, icons);
            tuckRow(shape);
            if (QCheckBox *reverse = widget->findChild<QCheckBox *>(QStringLiteral("reverse_check"))) {
                addGap();
                mirror(reverse, reverseIcon(ink));
                tuckRow(reverse);
            }
        }

        if (withId<QWidget>(widget, QStringLiteral("sectionEnclosingMethod"))) addEncloseAndFill(widget);

        // fill: what gets filled, and with what
        for (const QString &section : {QStringLiteral("sectionWhatToFill"), QStringLiteral("sectionFillWith")}) {
            if (KisOptionButtonStrip *strip = stripInSection(widget, section)) {
                addGap();
                Q_FOREACH (KoGroupButton *button, strip->buttons()) {
                    mirror(button);
                }
                // foreground, background, pattern: the pattern itself beside its button while it fills with one
                if (section == QLatin1String("sectionFillWith") && strip->buttons().size() >= 3) addPatternChip(strip->buttons()[2]);
                tuckSection(strip);
            }
        }
        // the fill's threshold, the one setting changed all the time
        if (stripInSection(widget, QStringLiteral("sectionWhatToFill"))) {
            if (QSpinBox *threshold = withId<QSpinBox>(widget, QStringLiteral("sliderThreshold"))) {
                addGap();
                mirrorSpin(threshold, int(KisStudioScrubber::Kind::Threshold), i18nc("@info:tooltip how alike colors are filled as one", "Threshold"), 8);
            }
        }
    }

    // the text tool's options are Krita's own panel: its font, the setting changed most, written in itself
    if (KisStudioTools::activeToolId() == QLatin1String("SvgTextTool")) addFontChip();
    // the shape tool also places Krita's symbols (balloons, effects…)
    if (KisStudioTools::activeToolId() == QLatin1String("KritaShape/KisToolSuperellipse")) {
        addGap();
        QToolButton *symbols = addButton(KisIconUtils::loadIcon(QStringLiteral("studio_symbols")), i18nc("@info:tooltip", "Symbols"));
        symbols->setCheckable(false);
        connect(symbols, &QToolButton::clicked, this, [symbols]() { KisStudioSymbols::openPicker(symbols); });
    }
    if (text) addTextCommands(text);
    finishMore();

    m_hasOptions = m_layout->count() > 0;
    updateGeometry();
}

void KisStudioToolStrip::addFontChip()
{
    KisMainWindow *window = windowOf(this);
    KisViewManager *view = window ? window->viewManager() : nullptr;
    KisCanvasResourceProvider *provider = view ? view->canvasResourceProvider() : nullptr;
    if (!provider || !provider->resourceManager()) return;
    QPointer<KisCanvasResourceProvider> source = provider;
    auto family = [source]() -> QString {
        if (!source) return QString();
        const KoSvgTextPropertyData data = source->textPropertyData();
        const QVariant own = data.commonProperties.property(KoSvgTextProperties::FontFamiliesId);
        const QVariant names = own.isValid() ? own : data.inheritedProperties.propertyOrDefault(KoSvgTextProperties::FontFamiliesId);
        return names.toStringList().value(0);
    };

    addGap();
    auto *chip = new QToolButton(this);
    chip->setAutoRaise(true);
    chip->setToolButtonStyle(Qt::ToolButtonTextOnly);
    chip->setFocusPolicy(Qt::NoFocus);
    chip->setToolTip(i18nc("@info:tooltip the text's font", "Font"));
    auto show = [this, chip, family]() {
        const QString name = family();
        QFont face = font();
        if (!name.isEmpty()) face.setFamily(name);
        chip->setFont(face);
        const QString text = name.isEmpty() ? i18nc("the text's font, when none is set", "Font") : name;
        chip->setText(QFontMetrics(face).elidedText(text, Qt::ElideRight, 14 * fontMetrics().averageCharWidth()));
    };
    show();
    connect(provider->resourceManager(), &KoCanvasResourceProvider::canvasResourceChanged, chip, [show](int key, const QVariant &) {
        if (key == KoCanvasResource::SvgTextPropertyData) show();
    });
    connect(chip, &QToolButton::clicked, this, [chip, source, family]() {
        auto *picker = new KisStudioFontPicker(KisStudioFontPicker::textFamilies(), family(), [source](const QString &picked) {
            if (!source) return;
            KoSvgTextPropertyData data = source->textPropertyData();
            data.commonProperties.setProperty(KoSvgTextProperties::FontFamiliesId, QStringList {picked});
            source->setTextPropertyData(data);
        }, chip->window());
        picker->popupBeside(chip, Qt::BottomEdge);
    });
    m_layout->addWidget(chip);
}

void KisStudioToolStrip::addPatternChip(QAbstractButton *fillsWithPattern)
{
    KisMainWindow *window = windowOf(this);
    KisViewManager *view = window ? window->viewManager() : nullptr;
    KisCanvasResourceProvider *provider = view ? view->canvasResourceProvider() : nullptr;
    if (!provider) return;
    QPointer<KisCanvasResourceProvider> resources = provider;

    auto *chip = new QToolButton(this);
    chip->setAutoRaise(true);
    chip->setFocusPolicy(Qt::NoFocus);
    chip->setIconSize(QSize(20, 20));
    chip->setToolTip(i18nc("@info:tooltip the pattern a fill paints with", "Pattern"));
    auto show = [chip](KoPatternSP pattern) {
        // the pattern's own picture, in a tile as a swatch is
        const qreal ratio = chip->devicePixelRatioF();
        QPixmap tile(QSize(20, 20) * ratio);
        tile.setDevicePixelRatio(ratio);
        tile.fill(Qt::transparent);
        QPainter painter(&tile);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QRectF rect(0.5, 0.5, 19, 19);
        const QPainterPath shape = KisModernStyle::squircle(rect, KisModernStyle::cornerRadius);
        if (pattern && !pattern->pattern().isNull()) {
            painter.setClipPath(shape);
            // a piece at the size it fills, since a whole texture shrunk to a tile is a flat gray
            QImage piece = pattern->pattern();
            piece.setDevicePixelRatio(ratio);
            painter.fillRect(rect, QBrush(piece));
            painter.setClipping(false);
        }
        QColor edge = chip->palette().color(QPalette::WindowText);
        edge.setAlphaF(0.25);
        painter.setPen(QPen(edge, 1));
        painter.drawPath(shape);
        painter.end();
        chip->setIcon(QIcon(tile));
    };
    show(provider->currentPattern());
    connect(provider, &KisCanvasResourceProvider::sigPatternChanged, chip, show);
    chip->setVisible(fillsWithPattern->isChecked());
    connect(fillsWithPattern, &QAbstractButton::toggled, chip, &QWidget::setVisible);

    connect(chip, &QToolButton::clicked, this, [chip, resources]() {
        if (!resources) return;
        auto *popup = new KisStudioPopup(chip->window());
        popup->setAttribute(Qt::WA_DeleteOnClose);
        auto *layout = new QVBoxLayout(popup);
        popup->fitLayout(layout, KisModernStyle::space2);
        auto *chooser = new KisPatternChooser(popup);
        // the patterns are their own preview, and their tooltips name them
        Q_FOREACH (QScrollArea *preview, chooser->findChildren<QScrollArea *>()) {
            if (qobject_cast<QLabel *>(preview->widget())) preview->hide();
        }
        Q_FOREACH (QLabel *name, chooser->findChildren<QLabel *>()) {
            if (name->inherits("KSqueezedTextLabel")) name->hide();
        }
        chooser->setCurrentPattern(resources->currentPattern());
        connect(chooser, &KisPatternChooser::resourceSelected, resources.data(), &KisCanvasResourceProvider::slotPatternActivated);
        layout->addWidget(chooser);
        popup->resize(popup->sizeHint().expandedTo(QSize(300, 320) + QSize(2, 2) * popup->shadowMargin()));
        popup->popupBeside(chip, Qt::BottomEdge);
    });
    m_layout->addWidget(chip);
}

QMenu *KisStudioToolStrip::addMore()
{
    QToolButton *more = addButton(KisIconUtils::loadIcon(QStringLiteral("studio_more")), i18nc("@info:tooltip more of the tool's commands", "More"));
    more->setCheckable(false);
    more->setPopupMode(QToolButton::InstantPopup);
    auto *menu = new QMenu(more);
    more->setMenu(menu);
    m_more = more;
    return menu;
}

void KisStudioToolStrip::addMoreSwitch(QAbstractButton *source, const QString &text)
{
    if (!source) return;
    m_moreLater << MoreEntry{source, text.isEmpty() ? KisSentenceCase::of(source->text().remove(QLatin1Char('&'))) : text, true};
    tuckOption(source);
}

void KisStudioToolStrip::addMoreCommand(QAbstractButton *source, const QString &text)
{
    if (!source) return;
    m_moreLater << MoreEntry{source, text, false};
    tuckOption(source);
}

void KisStudioToolStrip::finishMore()
{
    if (m_moreLater.isEmpty()) return;
    QMenu *menu = m_more && m_more->menu() ? m_more->menu() : addMore();
    if (!menu->isEmpty()) menu->addSeparator();
    // one of a set of buttons (a mode) is one of a set in the menu too, drawn as such
    QHash<QButtonGroup *, QActionGroup *> sets;
    for (const MoreEntry &entry : qAsConst(m_moreLater)) {
        if (!entry.source) continue;
        QPointer<QAbstractButton> target = entry.source;
        QAction *action = menu->addAction(entry.text);
        if (entry.checkable) {
            action->setCheckable(true);
            if (QButtonGroup *group = target->group()) {
                if (group->exclusive()) {
                    if (!sets.contains(group)) sets.insert(group, new QActionGroup(menu));
                    sets.value(group)->addAction(action);
                }
            }
            action->setChecked(target->isChecked());
            connect(target, &QAbstractButton::toggled, action, &QAction::setChecked);
        }
        connect(action, &QAction::triggered, target, [target, action](bool on) {
            if (!target) return;
            if (!action->isCheckable() || target->isChecked() != on) target->click();
            // one of a set (a mode) stays chosen when clicked again
            if (action->isCheckable()) action->setChecked(target->isChecked());
        });
        // what the tool can't do now is greyed, as it was in its options
        connect(menu, &QMenu::aboutToShow, action, [target, action]() {
            if (target) action->setEnabled(target->isEnabled());
        });
    }
    m_moreLater.clear();
}

void KisStudioToolStrip::addEncloseAndFill(QWidget *options)
{
    // how it encloses, the tool's first choice: on the bar
    if (KisOptionButtonStrip *methods = stripInSection(options, QStringLiteral("sectionEnclosingMethod"))) {
        addGap();
        Q_FOREACH (KoGroupButton *button, methods->buttons()) mirror(button);
        tuckSection(methods);
    }
    // its threshold, unless the fill's own mirroring has it already
    if (!stripInSection(options, QStringLiteral("sectionWhatToFill"))) {
        if (QSpinBox *threshold = withId<QSpinBox>(options, QStringLiteral("sliderThreshold"))) {
            addGap();
            mirrorSpin(threshold, int(KisStudioScrubber::Kind::Threshold), i18nc("@info:tooltip how alike colors are filled as one", "Threshold"), 8);
        }
    }
    // switches set now and then
    for (const char *id : {"checkBoxRegionSelectionInvert", "checkBoxRegionSelectionIncludeContourRegions", "checkBoxSelectionAsBoundary",
                           "checkBoxAntiAlias", "checkBoxCustomBlendingOptions"}) {
        addMoreSwitch(withId<QAbstractButton>(options, QLatin1String(id)));
    }
    addMoreCommand(withId<QAbstractButton>(options, QStringLiteral("buttonReset")), i18nc("enclose and fill: its options as they first were", "Reset"));
}

bool KisStudioToolStrip::optionsLeft() const
{
    Q_FOREACH (const QPointer<QWidget> &root, m_options) {
        if (!root) continue;
        Q_FOREACH (QWidget *widget, root->findChildren<QWidget *>()) {
            if (isInput(widget) && widget->isVisibleTo(root)) return true;
        }
    }
    return false;
}

void KisStudioToolStrip::showMore()
{
    if (m_more) m_more->showMenu();
}

void KisStudioToolStrip::addReferenceImages(QWidget *options)
{
    // adding one is the tool's first use, so it's on the bar; the options keep what changes the chosen ones
    addGap();
    const QList<QPair<const char *, QString>> adds = {
        {"bnAddReferenceImage", i18nc("@info:tooltip add a reference image", "From a file")},
        {"bnPasteReferenceImage", i18nc("@info:tooltip add a reference image", "Paste")},
        {"bnAddReferenceImageFromCurrentLayer", i18nc("@info:tooltip add a reference image", "From the layer")},
        {"bnAddReferenceImageFromVisible", i18nc("@info:tooltip add a reference image", "From the image")},
    };
    for (const auto &add : adds) {
        QAbstractButton *source = options->findChild<QAbstractButton *>(QLatin1String(add.first));
        if (QToolButton *button = mirror(source)) {
            button->setCheckable(false);
            button->setToolTip(add.second);
            new FollowEnabled(source, button);
        }
        tuck(source);
    }
    QMenu *menu = addMore();
    QPointer<QWidget> root = options;
    auto clickOn = [this, menu, root](const QString &text, const char *name) {
        QPointer<QAbstractButton> source = root->findChild<QAbstractButton *>(QLatin1String(name));
        if (!source) return;
        connect(menu->addAction(text), &QAction::triggered, source, [source]() {
            if (source) source->click();
        });
        tuck(source);
    };
    clickOn(i18nc("reference images: open a saved set", "Open…"), "bnLoad");
    clickOn(i18nc("reference images: save them as a set", "Save…"), "bnSave");
    menu->addSeparator();
    clickOn(i18nc("reference images: remove every one", "Delete all"), "bnDelete");
    for (const char *name : {"label", "label1", "referenceImageOptionsLabel"}) {
        tuck(options->findChild<QWidget *>(QLatin1String(name)));
    }
    Q_FOREACH (QFrame *line, options->findChildren<QFrame *>(QStringLiteral("line"))) tuck(line);
}

void KisStudioToolStrip::addGutter(QWidget *options)
{
    // cutting is the tool and Alt joins, so its modes go, and so does its form of gap kinds and units
    if (auto *modes = options->findChild<KisOptionButtonStrip *>(QStringLiteral("optionButtonStripToolMode"))) {
        if (modes->button(0) && !modes->button(0)->isChecked()) modes->button(0)->click();
        tuck(modes);
    }
    tuck(options->findChild<QWidget *>(QStringLiteral("warningLabel")));
    tuck(options->findChild<QWidget *>(QStringLiteral("gapOptionsWidget")));

    // the width of the kind of gap in use
    const QList<QPair<const char *, const char *>> kinds = {
        {"thickGapWidthRadioButton", "thickGapWidth"}, {"thinGapWidthRadioButton", "thinGapWidth"}, {"specialGapWidthRadioButton", "customGapWidth"}};
    QDoubleSpinBox *width = nullptr;
    for (const auto &kind : kinds) {
        QAbstractButton *chosen = options->findChild<QAbstractButton *>(QLatin1String(kind.first));
        if (chosen && chosen->isChecked()) width = options->findChild<QDoubleSpinBox *>(QLatin1String(kind.second));
    }
    if (!width) return;
    addGap();
    QPointer<QDoubleSpinBox> target = width;
    auto *scrubber = new KisStudioScrubber(KisStudioScrubber::Kind::Size, i18nc("@info:tooltip the width of the gaps between comic panels", "Gutter"), this);
    scrubber->setRange(qMax(1.0, width->minimum()), qMin(200.0, width->maximum()));
    scrubber->setValue(width->value());
    auto set = [target](qreal value) {
        if (target) target->setValue(value);
    };
    connect(scrubber, &KisStudioScrubber::valueCommitted, this, set);
    connect(scrubber, &KisStudioScrubber::valueChanging, this, set);
    connect(width, QOverload<double>::of(&QDoubleSpinBox::valueChanged), scrubber, [scrubber](double value) { scrubber->setValue(value); });
    m_layout->addWidget(scrubber);
}

void KisStudioToolStrip::addTextCommands(QObject *manager)
{
    // Krita's text options are a page of their own: the font is on the bar, the rest behind ⋯
    QPointer<QObject> options = manager;
    addGap();
    // the text's properties, opened under their button; lit while open
    if (options->property("showTextPropertyButton").toBool()) {
        QToolButton *button = addButton(KisIconUtils::loadIcon(QStringLiteral("studio_text_properties")),
                                        i18nc("@info:tooltip the text tool's panel of size, spacing and the like", "Text properties"));
        QPointer<QDockWidget> panel = window()->findChild<QDockWidget *>(QStringLiteral("TextProperties"));
        if (panel) {
            panel->setProperty("studioAnchor", QVariant::fromValue<QObject *>(button));
            button->setChecked(panel->isVisible());
            connect(panel, &QDockWidget::visibilityChanged, button, [button](bool visible) { button->setChecked(visible); });
        }
        connect(button, &QToolButton::clicked, this, [options, panel, button]() {
            if (!options) return;
            if (panel) panel->setProperty("studioAnchor", QVariant::fromValue<QObject *>(button));
            // the tool shows or hides the panel on every change of this, whatever it was
            options->setProperty("textPropertiesOpen", !options->property("textPropertiesOpen").toBool());
        });
    }
    QMenu *menu = addMore();
    connect(menu, &QMenu::aboutToShow, this, [menu, options]() {
        menu->clear();
        if (!options) return;
        auto command = [menu](const QString &text, std::function<void()> run) {
            QObject::connect(menu->addAction(text), &QAction::triggered, menu, run);
        };
        auto toggle = [menu, options](const QString &text, const char *property) {
            QAction *action = menu->addAction(text);
            action->setCheckable(true);
            action->setChecked(options->property(property).toBool());
            QObject::connect(action, &QAction::triggered, menu, [options, property](bool on) {
                if (options) options->setProperty(property, on);
            });
        };
        command(i18nc("text tool: pick a character from the font", "Glyphs…"), [options]() {
            if (options) QMetaObject::invokeMethod(options, "emitGlyphPalette");
        });
        command(i18nc("text tool: edit the text's SVG", "Edit SVG…"), [options]() {
            if (options) QMetaObject::invokeMethod(options, "emitOpenTextEditor");
        });
        toggle(i18nc("text tool: drag single letters to place them", "Move letters"), "typeSettingMode");

        // what the chosen text is: lines as typed, wrapped in a width, or placed letter by letter as SVG 1.1 does
        const int type = options->property("textType").toInt();
        if (type < 0) return;
        menu->addSeparator();
        const QList<QPair<int, QString>> types = {{1, i18nc("text tool: a text's kind", "Lines as typed")},
                                                  {2, i18nc("text tool: a text's kind", "Wrapped")},
                                                  {0, i18nc("text tool: a text's kind, as SVG 1.1 writes it", "Placed by letter")}};
        for (const auto &kind : types) {
            QAction *action = menu->addAction(kind.second);
            action->setCheckable(true);
            action->setChecked(type == kind.first);
            const int value = kind.first;
            QObject::connect(action, &QAction::triggered, menu, [options, value]() {
                if (options) options->setProperty("textType", value);
            });
        }
    });
}
