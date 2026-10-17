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
    }

    m_hasOptions = m_layout->count() > 0;
    updateGeometry();
}
