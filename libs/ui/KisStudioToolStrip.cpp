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
#include <QButtonGroup>
#include <QLabel>
#include <QMenu>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidgetAction>

#include <algorithm>
#include <cmath>

#include <KisModernStyle.h>
#include <KisOptionButtonStrip.h>
#include <KoGroupButton.h>
#include <KoToolManager.h>
#include <kactioncollection.h>
#include <klocalizedstring.h>

#include "KisStudioSymmetry.h"
#include "kis_icon_utils.h"
#include "widgets/kis_selection_options.h"

namespace {

/// The tools that paint with the brush, which are the ones symmetry applies to
bool paintsWithBrush(const QString &tool)
{
    static const QStringList tools = {QStringLiteral("KritaShape/KisToolBrush"), QStringLiteral("KritaShape/KisToolDyna"),
                                      QStringLiteral("KritaShape/KisToolMultiBrush"), QStringLiteral("KritaShape/KisToolLine"),
                                      QStringLiteral("KritaShape/KisToolRectangle"), QStringLiteral("KritaShape/KisToolEllipse"),
                                      QStringLiteral("KisToolPolygon"), QStringLiteral("KisToolPolyline"), QStringLiteral("KisToolPath"),
                                      QStringLiteral("KisToolPencil")};
    return tools.contains(tool);
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

/// Icon for a symmetry mode: mirror, radial or tile
QIcon symmetryIcon(int mode, const QColor &ink)
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
        switch (mode) {
        case KisStudioSymmetry::Mirror:
            painter.drawLine(QPointF(10 * s, 3 * s), QPointF(10 * s, 17 * s));
            painter.drawArc(QRectF(3 * s, 5 * s, 6 * s, 10 * s), 90 * 16, 180 * 16);
            painter.drawArc(QRectF(11 * s, 5 * s, 6 * s, 10 * s), 270 * 16, 180 * 16);
            break;
        case KisStudioSymmetry::Radial:
            for (int i = 0; i < 6; i++) {
                const qreal a = i * M_PI / 3;
                painter.drawLine(c + QPointF(2.5 * s * std::cos(a), 2.5 * s * std::sin(a)), c + QPointF(7.5 * s * std::cos(a), 7.5 * s * std::sin(a)));
            }
            break;
        default:
            for (int i = 0; i < 4; i++) {
                painter.drawPath(KisModernStyle::squircle(QRectF((3.5 + 7 * (i % 2)) * s, (3.5 + 7 * (i / 2)) * s, 6 * s, 6 * s), 1.4 * s));
            }
            break;
        }
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

QMenu *KisStudioToolStrip::symmetryMenu()
{
    if (m_symmetryMenu) {
        return m_symmetryMenu;
    }
    // owned by the strip, not a button: picking a mode can change the tool, which rebuilds the buttons
    m_symmetryMenu = new QMenu(this);
    auto *panel = new QWidget(m_symmetryMenu);
    panel->setFixedWidth(230);
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(KisModernStyle::space3, KisModernStyle::space2, KisModernStyle::space3, KisModernStyle::space2);
    layout->setSpacing(KisModernStyle::space2);
    KisStudioSymmetry *symmetry = KisStudioSymmetry::instance();
    // the menu's own text color, as it sits on its own glass rather than the bar's
    const QColor ink = m_symmetryMenu->palette().color(QPalette::WindowText);

    auto toggle = [panel](const QIcon &icon, const QString &text) {
        auto *button = new QToolButton(panel);
        button->setCheckable(true);
        button->setAutoRaise(true);
        button->setIcon(icon);
        button->setIconSize(QSize(20, 20));
        button->setText(text);
        button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        button->setFocusPolicy(Qt::NoFocus);
        // buttons in a row share its width equally
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        return button;
    };
    auto row = [](QWidget *parent, const QString &label, QWidget *control) {
        auto *line = new QWidget(parent);
        auto *lineLayout = new QHBoxLayout(line);
        lineLayout->setContentsMargins(0, 0, 0, 0);
        lineLayout->addWidget(new QLabel(label, line));
        lineLayout->addStretch(1);
        control->setFixedWidth(96);
        lineLayout->addWidget(control);
        return line;
    };

    // picking a mode also turns symmetry on
    auto *modes = new QHBoxLayout();
    modes->setSpacing(KisModernStyle::hairline);
    auto *modeGroup = new QButtonGroup(panel);
    const QStringList modeNames = {i18nc("symmetry mode", "Mirror"), i18nc("symmetry mode", "Radial"), i18nc("symmetry mode", "Tile")};
    const QStringList modeTips = {
        i18nc("@info:tooltip", "Mirrored across an axis, with every brush tool. Drag the dot on an axis to move it."),
        i18nc("@info:tooltip", "Copies around a center. Drag the center to move it, the dot beside it to turn; double-click the center to reset."),
        i18nc("@info:tooltip", "Each stroke repeats across the image, or the selection, at this spacing."),
    };
    for (int mode = KisStudioSymmetry::Mirror; mode <= KisStudioSymmetry::Tile; mode++) {
        QToolButton *button = toggle(symmetryIcon(mode, ink), modeNames[mode]);
        button->setToolTip(modeTips[mode]);
        modeGroup->addButton(button, mode);
        modes->addWidget(button);
    }
    layout->addLayout(modes);

    // mirror: which axes to use (Krita's mirror axes, moved and rotated on the canvas)
    auto *mirrorPage = new QWidget(panel);
    auto *mirrorLayout = new QHBoxLayout(mirrorPage);
    mirrorLayout->setContentsMargins(0, 0, 0, 0);
    QAction *acrossAction = m_actions ? m_actions->action(QStringLiteral("hmirror_action")) : nullptr;
    QAction *downAction = m_actions ? m_actions->action(QStringLiteral("vmirror_action")) : nullptr;
    QToolButton *across = toggle(acrossAction ? acrossAction->icon() : QIcon(), i18nc("mirror axis", "Across"));
    QToolButton *down = toggle(downAction ? downAction->icon() : QIcon(), i18nc("mirror axis", "Down"));
    mirrorLayout->setSpacing(KisModernStyle::hairline);
    mirrorLayout->addWidget(across);
    mirrorLayout->addWidget(down);
    layout->addWidget(mirrorPage);

    // radial: number of copies, and whether neighbouring copies are mirrored
    auto *radialPage = new QWidget(panel);
    auto *radialLayout = new QVBoxLayout(radialPage);
    radialLayout->setContentsMargins(0, 0, 0, 0);
    auto *copies = new QSpinBox(radialPage);
    copies->setRange(2, KisStudioSymmetry::maximumCopies);
    radialLayout->addWidget(row(radialPage, i18nc("radial symmetry", "Copies"), copies));
    auto *mirrored = new QCheckBox(i18nc("radial symmetry", "Kaleidoscope"), radialPage);
    radialLayout->addWidget(mirrored);
    layout->addWidget(radialPage);

    // tile: spacing between copies
    auto *tilePage = new QWidget(panel);
    auto *tileLayout = new QVBoxLayout(tilePage);
    tileLayout->setContentsMargins(0, 0, 0, 0);
    auto spacingBox = [tilePage]() {
        auto *box = new QSpinBox(tilePage);
        box->setRange(0, 20000);
        box->setSingleStep(10);
        box->setSuffix(i18nc("pixels", " px"));
        box->setSpecialValueText(i18nc("tile spacing of 0", "None"));
        return box;
    };
    QSpinBox *spacingX = spacingBox();
    QSpinBox *spacingY = spacingBox();
    tileLayout->addWidget(row(tilePage, i18nc("tile spacing", "Across"), spacingX));
    tileLayout->addWidget(row(tilePage, i18nc("tile spacing", "Down"), spacingY));
    layout->addWidget(tilePage);

    // give the pages one height, since the open menu doesn't resize when they switch
    const int pageHeight = std::max({mirrorPage->sizeHint().height(), radialPage->sizeHint().height(), tilePage->sizeHint().height()});
    for (QWidget *page : {mirrorPage, radialPage, tilePage}) {
        page->setFixedHeight(pageHeight);
        page->layout()->setAlignment(Qt::AlignTop);
    }

    auto *action = new QWidgetAction(m_symmetryMenu);
    action->setDefaultWidget(panel);
    m_symmetryMenu->addAction(action);

    // sync the controls with the current settings when the menu opens and on every change
    auto show = [=]() {
        const int mode = symmetry->mode();
        if (QAbstractButton *button = modeGroup->button(mode)) button->setChecked(true);
        mirrorPage->setVisible(mode == KisStudioSymmetry::Mirror);
        radialPage->setVisible(mode == KisStudioSymmetry::Radial);
        tilePage->setVisible(mode == KisStudioSymmetry::Tile);
        across->setChecked(symmetry->mirrorAcross());
        down->setChecked(symmetry->mirrorDown());
        const QSignalBlocker blockCopies(copies), blockMirrored(mirrored), blockX(spacingX), blockY(spacingY);
        copies->setValue(symmetry->copies());
        mirrored->setChecked(symmetry->mirrored());
        spacingX->setValue(symmetry->spacing().width());
        spacingY->setValue(symmetry->spacing().height());
    };
    connect(m_symmetryMenu, &QMenu::aboutToShow, panel, show);
    connect(modeGroup, QOverload<int>::of(&QButtonGroup::buttonClicked), panel, [this, symmetry, show](int mode) {
        symmetry->setMode(KisStudioSymmetry::Mode(mode));
        KisStudioSymmetry::setOn(m_actions, true);
        show();
    });
    auto axes = [this, symmetry, across, down]() {
        symmetry->setMirrorAxes(across->isChecked(), down->isChecked());
        if (symmetry->mode() == KisStudioSymmetry::Mirror) KisStudioSymmetry::setOn(m_actions, true);
        across->setChecked(symmetry->mirrorAcross());
        down->setChecked(symmetry->mirrorDown());
    };
    connect(across, &QToolButton::clicked, panel, axes);
    connect(down, &QToolButton::clicked, panel, axes);
    connect(copies, QOverload<int>::of(&QSpinBox::valueChanged), panel, [symmetry](int value) { symmetry->setCopies(value); });
    connect(mirrored, &QCheckBox::toggled, panel, [symmetry](bool on) { symmetry->setMirrored(on); });
    auto spacing = [symmetry, spacingX, spacingY]() { symmetry->setSpacing(QSize(spacingX->value(), spacingY->value())); };
    connect(spacingX, QOverload<int>::of(&QSpinBox::valueChanged), panel, spacing);
    connect(spacingY, QOverload<int>::of(&QSpinBox::valueChanged), panel, spacing);
    return m_symmetryMenu;
}

void KisStudioToolStrip::setOptionWidgets(const QList<QPointer<QWidget>> &widgets)
{
    clear();
    // full contrast, so the small icons' shapes read at a glance
    const QColor ink = palette().color(QPalette::ButtonText);
    const QColor paper = palette().color(QPalette::Window).darker(170);

    // symmetry: a click toggles it, a long press or right click opens its modes
    if (paintsWithBrush(KoToolManager::instance()->activeToolId()) && m_actions) {
        QToolButton *symmetry = addButton(KisIconUtils::loadIcon(QStringLiteral("krita_tool_multihand")),
                                          i18nc("@info:tooltip", "Symmetry (hold or right-click for its modes)"));
        symmetry->setChecked(KisStudioSymmetry::isOn(m_actions));
        symmetry->setMenu(symmetryMenu());
        symmetry->setPopupMode(QToolButton::DelayedPopup);
        symmetry->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(symmetry, &QToolButton::customContextMenuRequested, this, [this, symmetry]() {
            symmetryMenu()->popup(symmetry->mapToGlobal(QPoint(0, symmetry->height())));
        });
        connect(symmetry, &QToolButton::clicked, this, [this, symmetry]() {
            KisStudioSymmetry::setOn(m_actions, !KisStudioSymmetry::isOn(m_actions));
            symmetry->setChecked(KisStudioSymmetry::isOn(m_actions));
        });
        for (const char *name : {"hmirror_action", "vmirror_action"}) {
            if (QAction *mirror = m_actions->action(QLatin1String(name))) {
                connect(mirror, &QAction::toggled, symmetry, [this, symmetry]() { symmetry->setChecked(KisStudioSymmetry::isOn(m_actions)); });
            }
        }

        if (QAction *shapesAction = m_actions->action(QStringLiteral("stroke_shapes"))) {
            QToolButton *shapes = addButton(KisIconUtils::loadIcon(QStringLiteral("studio_shapes")),
                                            i18nc("@info:tooltip", "Shapes: hold the pen still at the end of a stroke"));
            shapes->setChecked(shapesAction->isChecked());
            connect(shapes, &QToolButton::clicked, shapesAction, &QAction::setChecked);
            connect(shapesAction, &QAction::toggled, shapes, &QToolButton::setChecked);
        }
    }

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
