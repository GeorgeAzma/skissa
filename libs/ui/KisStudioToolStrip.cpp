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

#include <KisOptionButtonStrip.h>
#include <KoGroupButton.h>
#include <klocalizedstring.h>
#include <cmath>
#include "kis_icon_utils.h"
#include <KoToolFactoryBase.h>
#include <KoToolRegistry.h>
#include <KoToolManager.h>

#include "widgets/kis_selection_options.h"

namespace {

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

/// A small picture of what a gradient shape looks like, which no word says as well
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
        clip.addRoundedRect(r, size * 0.16, size * 0.16);
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

/// Two opposite arrows: the gradient runs the other way
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

/// The multibrush modes as pictures: symmetry, mirror, translate, snowflake, copy translate
QIcon multihandIcon(int type, const QColor &ink)
{
    QIcon icon;
    for (int size : {20, 40}) {
        QPixmap pixmap(size, size);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const qreal s = size / 20.0;
        QPen pen(ink, 1.4 * s);
        pen.setCapStyle(Qt::RoundCap);
        painter.setPen(pen);
        const QPointF c(10 * s, 10 * s);
        switch (type) {
        case 0: // symmetry: spokes around a center
            for (int i = 0; i < 6; i++) {
                const qreal a = i * M_PI / 3;
                painter.drawLine(c + QPointF(2.5 * s * std::cos(a), 2.5 * s * std::sin(a)), c + QPointF(7.5 * s * std::cos(a), 7.5 * s * std::sin(a)));
            }
            break;
        case 1: // mirror: a stroke and its reflection
            painter.drawLine(QPointF(10 * s, 3 * s), QPointF(10 * s, 17 * s));
            painter.drawArc(QRectF(3 * s, 5 * s, 6 * s, 10 * s), 90 * 16, 180 * 16);
            painter.drawArc(QRectF(11 * s, 5 * s, 6 * s, 10 * s), 270 * 16, 180 * 16);
            break;
        case 2: // translate: copies side by side
            for (int i = 0; i < 3; i++) painter.drawLine(QPointF((4 + 6 * i) * s, 6 * s), QPointF((4 + 6 * i) * s, 14 * s));
            break;
        case 3: // snowflake: spokes with mirrored ticks
            for (int i = 0; i < 6; i++) {
                const qreal a = i * M_PI / 3;
                const QPointF tip = c + QPointF(7.5 * s * std::cos(a), 7.5 * s * std::sin(a));
                painter.drawLine(c, tip);
                const QPointF mid = c + QPointF(5 * s * std::cos(a), 5 * s * std::sin(a));
                painter.drawLine(mid, mid + QPointF(2 * s * std::cos(a + 0.8), 2 * s * std::sin(a + 0.8)));
                painter.drawLine(mid, mid + QPointF(2 * s * std::cos(a - 0.8), 2 * s * std::sin(a - 0.8)));
            }
            break;
        default: // copy translate: two strokes
            painter.drawRoundedRect(QRectF(3 * s, 4 * s, 7 * s, 7 * s), 1.5 * s, 1.5 * s);
            painter.drawRoundedRect(QRectF(10 * s, 9 * s, 7 * s, 7 * s), 1.5 * s, 1.5 * s);
            break;
        }
        painter.end();
        icon.addPixmap(pixmap);
    }
    return icon;
}

const QString brushToolId = QStringLiteral("KritaShape/KisToolBrush");
const QString multiBrushToolId = QStringLiteral("KritaShape/KisToolMultiBrush");

} // namespace

KisStudioToolStrip::KisStudioToolStrip(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("studioToolStrip"));
    m_layout = new QHBoxLayout(this);
    m_layout->setContentsMargins(4, 0, 4, 0);
    m_layout->setSpacing(2);
}

void KisStudioToolStrip::clear()
{
    while (QLayoutItem *item = m_layout->takeAt(0)) {
        if (QWidget *widget = item->widget()) {
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
    QVector<QToolButton *> buttons;
    for (int i = 0; i < indexes.size(); i++) {
        const int index = indexes[i];
        if (index >= combo->count()) {
            continue;
        }
        QToolButton *button = addButton(icons.value(i), combo->itemText(index));
        button->setChecked(combo->currentIndex() == index);
        connect(button, &QToolButton::clicked, this, [target, index]() {
            if (target) {
                target->setCurrentIndex(index);
            }
        });
        connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged), button, [button, index](int current) {
            button->setChecked(current == index);
        });
        buttons << button;
    }
    // a shape that has no button here still unchecks the others
    if (!indexes.contains(combo->currentIndex())) {
        Q_FOREACH (QToolButton *button, buttons) button->setChecked(false);
    }
}

void KisStudioToolStrip::setOptionWidgets(const QList<QPointer<QWidget>> &widgets)
{
    clear();
    // full contrast: the icons are small, the shape must read at a glance
    const QColor ink = palette().color(QPalette::ButtonText);
    const QColor paper = palette().color(QPalette::Window).darker(170);


    // Brush and Multibrush are one tool: the multibrush is a toggle of the brush
    const QString tool = KoToolManager::instance()->activeToolId();
    if ((tool == brushToolId || tool == multiBrushToolId) && KoToolRegistry::instance()->value(multiBrushToolId)) {
        const bool multi = tool == multiBrushToolId;
        QToolButton *toggle = addButton(KisIconUtils::loadIcon(KoToolRegistry::instance()->value(multiBrushToolId)->iconName()),
                                        i18nc("@info:tooltip", "Multibrush: paint with symmetry, mirrors and copies"));
        toggle->setChecked(multi);
        connect(toggle, &QToolButton::clicked, this, [multi]() {
            KoToolManager::instance()->switchToolRequested(multi ? brushToolId : multiBrushToolId);
        });
    }

    Q_FOREACH (const QPointer<QWidget> &widget, widgets) {
        if (!widget) {
            continue;
        }

        // multibrush: its modes, while it is on
        if (QComboBox *type = widget->findChild<QComboBox *>(QStringLiteral("multihandTypeCombobox"))) {
            QVector<int> indexes;
            QVector<QIcon> icons;
            for (int i = 0; i < qMin(type->count(), 5); i++) {
                indexes << i;
                icons << multihandIcon(i, ink);
            }
            mirrorCombo(type, indexes, icons);
        }

        // transform: every mode, warp and liquify included
        if (widget->findChild<QAbstractButton *>(QStringLiteral("freeTransformButton"))) {
            addGap();
            for (const char *name : {"freeTransformButton", "perspectiveTransformButton", "warpButton",
                                     "cageButton", "liquifyButton", "meshButton"}) {
                mirror(widget->findChild<QAbstractButton *>(QLatin1String(name)));
            }
        }

        // selections: replace, add, subtract, intersect (shift and alt do the same while dragging)
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
        }

        // gradient: its shape, shown, and the direction
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
    }

    m_hasOptions = m_layout->count() > 0;
    updateGeometry();
}
