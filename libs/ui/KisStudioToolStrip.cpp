/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioToolStrip.h"

#include <QAbstractButton>
#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QPainter>
#include <QPainterPath>
#include <QToolButton>


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
#include <kconfiggroup.h>

#include <functional>

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
        if (event->type() == QEvent::ShowToParent || event->type() == QEvent::HideToParent) {
            m_mirror->setVisible(event->type() == QEvent::ShowToParent);
        }
        return QObject::eventFilter(watched, event);
    }

private:
    QWidget *m_mirror;
};

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

void KisStudioToolStrip::mirror(QAbstractButton *source, const QIcon &icon)
{
    if (!source) {
        return;
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
    m_layout->addWidget(scrubber);
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
    auto imageRatio = []() {
        KisMainWindow *window = KisPart::instance()->currentMainwindow();
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

void KisStudioToolStrip::addSelectionAdjust()
{
    if (!m_adjust) m_adjust = new KisStudioSelectionAdjust(this);
    addGap();
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
            // a step, a typed amount: at once
            if (!m_adjust->isActive()) m_adjust->begin();
            m_adjust->set(growing ? qRound(value) : 0, growing ? 0 : qRound(value));
            m_adjust->end();
            scrubber->setValue(0);
        });
        m_layout->addWidget(scrubber);
    }
}

void KisStudioToolStrip::addGuides(QWidget *options, QComboBox *types)
{
    const QColor ink = palette().color(QPalette::ButtonText);
    QPointer<QComboBox> target = types;

    // the box beside each guide keeps only its grip; its commands are on the guide's right click
    for (const char *name : {"showSnap", "showLock", "showDuplicate", "showDelete"}) {
        if (QCheckBox *show = options->findChild<QCheckBox *>(QLatin1String(name))) show->setChecked(false);
    }
    if (QCheckBox *move = options->findChild<QCheckBox *>(QStringLiteral("showMove"))) move->setChecked(true);

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
        auto show = [button, menu, kind, target, inUse, rememberedWay]() {
            if (!target) return;
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
            }
            show();
        });
        // a click picks the kind; on the kind in use, its ways
        connect(button, &QToolButton::clicked, this, [button, menu, kind, target, show, inUse, rememberedWay]() {
            if (!target) return;
            if (!inUse()) {
                target->setCurrentIndex(target->findData(rememberedWay(kind)));
            } else if (kind.ways.size() > 1) {
                menu->exec(button->mapToGlobal(QPoint(0, button->height())));
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
    if (KisMainWindow *window = KisPart::instance()->currentMainwindow()) {
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
    QToolButton *more = addButton(KisIconUtils::loadIcon(QStringLiteral("studio_more")), i18nc("@info:tooltip more for guides", "More"));
    more->setCheckable(false);
    more->setPopupMode(QToolButton::InstantPopup);
    auto *menu = new QMenu(more);
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
    more->setMenu(menu);
}

void KisStudioToolStrip::setOptionWidgets(const QList<QPointer<QWidget>> &widgets)
{
    clear();
    // full contrast, so the small icons' shapes read at a glance
    const QColor ink = palette().color(QPalette::ButtonText);
    const QColor paper = palette().color(QPalette::Window).darker(170);

    Q_FOREACH (const QPointer<QWidget> &widget, widgets) {
        if (!widget) {
            continue;
        }

        // transform modes, including warp and liquify
        if (widget->findChild<QAbstractButton *>(QStringLiteral("freeTransformButton"))) {
            addGap();
            for (const char *name : {"freeTransformButton", "perspectiveTransformButton", "warpButton",
                                     "cageButton", "liquifyButton", "meshButton"}) {
                mirror(widget->findChild<QAbstractButton *>(QLatin1String(name)));
            }
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
            }
            addSelectionAdjust();
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
            if (QComboBox *outline = widget->findChild<QComboBox *>(QStringLiteral("cmbOutline"))) {
                QVector<QIcon> outlines;
                for (int i = 0; i < 3; i++) outlines << outlineIcon(ink, paper, i);
                mirrorComboAsMenu(outline, outlines, i18nc("@info:tooltip a shape's outline", "Outline"));
            }
        }

        // painting pulled onto the image's guides, while it has some
        if (QCheckBox *snap = widget->findChild<QCheckBox *>(QStringLiteral("chkAssistant"))) {
            KisMainWindow *window = KisPart::instance()->currentMainwindow();
            KisViewManager *view = window ? window->viewManager() : nullptr;
            KisCanvas2 *canvas = view ? view->canvasBase() : nullptr;
            if (canvas && canvas->paintingAssistantsDecoration() && !canvas->paintingAssistantsDecoration()->assistants().isEmpty()) {
                addGap();
                mirror(snap, magnetIcon(ink));
                if (QWidget *button = m_layout->itemAt(m_layout->count() - 1)->widget()) {
                    button->setToolTip(i18nc("@info:tooltip lines drawn are pulled onto the image's guides", "Snap to guides"));
                }
            }
        }

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
            if (QCheckBox *reverse = widget->findChild<QCheckBox *>(QStringLiteral("reverse_check"))) {
                addGap();
                mirror(reverse, reverseIcon(ink));
            }
        }

        // fill: what gets filled, and with what
        for (const QString &section : {QStringLiteral("sectionWhatToFill"), QStringLiteral("sectionFillWith")}) {
            if (KisOptionButtonStrip *strip = stripInSection(widget, section)) {
                addGap();
                Q_FOREACH (KoGroupButton *button, strip->buttons()) {
                    mirror(button);
                }
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

    m_hasOptions = m_layout->count() > 0;
    updateGeometry();
}
