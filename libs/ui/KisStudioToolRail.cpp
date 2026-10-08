/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioToolRail.h"

#include <QAction>
#include <QContextMenuEvent>
#include <QMenu>
#include <QPainter>
#include <QPointer>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <KoCanvasController.h>
#include <KoToolFactoryBase.h>
#include <KoToolManager.h>
#include <KoToolRegistry.h>
#include <kactioncollection.h>
#include <klocalizedstring.h>
#include <KisResourceModel.h>

#include "KisMainWindow.h"
#include "KisResourceServerProvider.h"
#include "KisViewManager.h"
#include "kis_canvas_resource_provider.h"
#include "kis_config.h"
#include "kis_icon_utils.h"
#include "kis_paintop_preset.h"

namespace {

const QString brushToolId = QStringLiteral("KritaShape/KisToolBrush");

enum class BrushMode {
    None,
    Paint,
    Blend,
    Erase,
};

struct ToolSlot {
    QString id;
    QString title;
    QString icon;
    QString shortcutHint; ///< the shortcut of the slot's main tool, for the tooltip
    BrushMode brush;
    QStringList tools;    ///< the variants, the first is the default
};

QVector<ToolSlot> toolSlots()
{
    return {
        {QStringLiteral("paint"), i18nc("@info:tooltip tool slot", "Paint"), QStringLiteral("studio_paint"), QStringLiteral("B"), BrushMode::Paint,
         {brushToolId, QStringLiteral("KritaShape/KisToolDyna"), QStringLiteral("KritaShape/KisToolMultiBrush"),
          QStringLiteral("KritaShape/KisToolSmartPatch")}},
        {QStringLiteral("blend"), i18nc("@info:tooltip tool slot", "Blend"), QStringLiteral("studio_blend"), QString(), BrushMode::Blend,
         {brushToolId}},
        {QStringLiteral("erase"), i18nc("@info:tooltip tool slot", "Erase"), QStringLiteral("studio_erase"), QStringLiteral("E"), BrushMode::Erase,
         {brushToolId}},
        {QStringLiteral("select"), i18nc("@info:tooltip tool slot", "Select"), QStringLiteral("studio_select"), QString(), BrushMode::None,
         {QStringLiteral("KisToolSelectRectangular"), QStringLiteral("KisToolSelectElliptical"), QStringLiteral("KisToolSelectOutline"),
          QStringLiteral("KisToolSelectPolygonal"), QStringLiteral("KisToolSelectContiguous"), QStringLiteral("KisToolSelectSimilar"),
          QStringLiteral("KisToolSelectMagnetic"), QStringLiteral("KisToolSelectPath")}},
        {QStringLiteral("move"), i18nc("@info:tooltip tool slot", "Move and Transform"), QStringLiteral("studio_move"), QString(), BrushMode::None,
         {QStringLiteral("KritaTransform/KisToolMove"), QStringLiteral("KisToolTransform"), QStringLiteral("KisToolCrop"),
          QStringLiteral("InteractionTool")}},
        {QStringLiteral("fill"), i18nc("@info:tooltip tool slot", "Fill"), QStringLiteral("studio_fill"), QString(), BrushMode::None,
         {QStringLiteral("KritaFill/KisToolFill"), QStringLiteral("KritaFill/KisToolGradient"), QStringLiteral("KisToolEncloseAndFill"),
          QStringLiteral("KritaShape/KisToolLazyBrush")}},
        {QStringLiteral("shape"), i18nc("@info:tooltip tool slot", "Shape"), QStringLiteral("studio_shape"), QString(), BrushMode::None,
         {QStringLiteral("KritaShape/KisToolRectangle"), QStringLiteral("KritaShape/KisToolEllipse"), QStringLiteral("KritaShape/KisToolLine"),
          QStringLiteral("KisToolPolygon"), QStringLiteral("KisToolPolyline"), QStringLiteral("KisToolPath"), QStringLiteral("KisToolPencil"),
          QStringLiteral("PathTool"), QStringLiteral("KarbonCalligraphyTool"), QStringLiteral("KritaShape/KisToolKnife")}},
        {QStringLiteral("text"), i18nc("@info:tooltip tool slot", "Text"), QStringLiteral("studio_text"), QString(), BrushMode::None,
         {QStringLiteral("SvgTextTool")}},
        {QStringLiteral("guides"), i18nc("@info:tooltip tool slot", "Guides and References"), QStringLiteral("studio_guides"), QString(), BrushMode::None,
         {QStringLiteral("KisAssistantTool"), QStringLiteral("ToolReferenceImages"), QStringLiteral("KritaShape/KisToolMeasure")}},
    };
}

/**
 * A rail button. Slots with variants get a small corner mark, and holding
 * the button shows the variants, which is the natural gesture with a pen.
 */
class SlotButton : public QToolButton
{
public:
    SlotButton(bool hasVariants, QWidget *parent)
        : QToolButton(parent)
        , m_hasVariants(hasVariants)
    {
        setAutoRaise(true);
        setCheckable(true);
        setFixedSize(40, 40);
        setIconSize(QSize(22, 22));
        setFocusPolicy(Qt::NoFocus);
        m_holdTimer.setSingleShot(true);
        m_holdTimer.setInterval(420);
    }

    std::function<void()> onHold;

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        if (m_hasVariants && event->button() == Qt::LeftButton) {
            m_held = false;
            QObject::disconnect(m_holdConnection);
            m_holdConnection = QObject::connect(&m_holdTimer, &QTimer::timeout, this, [this]() {
                m_held = true;
                setDown(false);
                if (onHold) onHold();
            });
            m_holdTimer.start();
        }
        QToolButton::mousePressEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        m_holdTimer.stop();
        if (m_held) {
            // the hold already showed the variants, this is not a click
            m_held = false;
            setDown(false);
            event->accept();
            return;
        }
        QToolButton::mouseReleaseEvent(event);
    }

    void paintEvent(QPaintEvent *event) override
    {
        QToolButton::paintEvent(event);
        if (!m_hasVariants) {
            return;
        }
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        QColor mark = palette().color(QPalette::ButtonText);
        mark.setAlphaF(isChecked() ? 0.8 : 0.45);
        painter.setPen(Qt::NoPen);
        painter.setBrush(mark);
        const QPointF corner(width() - 6, height() - 6);
        painter.drawPolygon(QPolygonF() << corner << corner - QPointF(5, 0) << corner - QPointF(0, 5));
    }

private:
    bool m_hasVariants {false};
    bool m_held {false};
    QTimer m_holdTimer;
    QMetaObject::Connection m_holdConnection;
};

} // namespace

struct KisStudioToolRail::Private
{
    KisMainWindow *mainWindow {nullptr};
    QVector<ToolSlot> slotList;
    QVector<SlotButton *> buttons;
    QVector<QString> currentTool; ///< the variant each slot uses now
    QVBoxLayout *layout {nullptr};

    /// Paint or Blend: which slot the brush tool belongs to while not erasing
    int brushSlot {0};
    int activeSlot {-1};
    bool loadingPreset {false};

    QAction *eraseAction() const
    {
        return mainWindow->actionCollection()->action(QStringLiteral("erase_action"));
    }

    KisCanvasResourceProvider *resources() const
    {
        return mainWindow->viewManager() ? mainWindow->viewManager()->canvasResourceProvider() : nullptr;
    }

    int slotIndex(BrushMode mode) const
    {
        for (int i = 0; i < slotList.size(); i++) {
            if (slotList[i].brush == mode) return i;
        }
        return -1;
    }
};

QString KisStudioToolRail::toolName(KoToolFactoryBase *factory)
{
    QString name = factory->toolTip();
    name.remove(QLatin1Char('&'));
    if (name.endsWith(QStringLiteral(" Tool"))) {
        name.chop(5);
    }
    return name;
}

KisStudioToolRail::KisStudioToolRail(KisMainWindow *mainWindow, QWidget *parent)
    : QWidget(parent)
    , d(new Private)
{
    d->mainWindow = mainWindow;
    d->layout = new QVBoxLayout(this);
    d->layout->setContentsMargins(8, 8, 8, 8);
    d->layout->setSpacing(2);

    KisConfig cfg(true);
    const QVector<ToolSlot> allSlots = toolSlots();
    Q_FOREACH (const ToolSlot &slot, allSlots) {
        // only the variants this build has
        ToolSlot available = slot;
        available.tools.clear();
        Q_FOREACH (const QString &tool, slot.tools) {
            if (KoToolRegistry::instance()->value(tool)) {
                available.tools << tool;
            }
        }
        if (available.tools.isEmpty()) {
            continue;
        }

        const int index = d->slotList.size();
        d->slotList << available;

        const QString remembered = cfg.readEntry<QString>(QStringLiteral("StudioToolSlot_") + slot.id, available.tools.first());
        d->currentTool << (available.tools.contains(remembered) ? remembered : available.tools.first());

        auto *button = new SlotButton(available.tools.size() > 1, this);
        button->setIcon(KisIconUtils::loadIcon(available.icon));
        button->setAccessibleName(available.title);
        button->setContextMenuPolicy(Qt::CustomContextMenu);
        button->onHold = [this, index]() { showVariants(index); };
        connect(button, &QToolButton::clicked, this, [this, index]() {
            // clicking the active slot again shows its variants
            if (index == d->activeSlot && d->slotList[index].tools.size() > 1) {
                d->buttons[index]->setChecked(true);
                showVariants(index);
            } else {
                activateSlot(index);
            }
        });
        connect(button, &QWidget::customContextMenuRequested, this, [this, index]() { showVariants(index); });
        d->buttons << button;

        // a gap after the brush slots and between the groups
        if (index > 0 && d->slotList[index - 1].brush != BrushMode::None && available.brush == BrushMode::None) {
            d->layout->addSpacing(10);
        }
        d->layout->addWidget(button, 0, Qt::AlignHCenter);
    }
    d->layout->addStretch(1);

    connect(KoToolManager::instance(), &KoToolManager::changedTool, this, &KisStudioToolRail::updateActiveSlot);
    if (QAction *erase = d->eraseAction()) {
        connect(erase, &QAction::toggled, this, &KisStudioToolRail::updateActiveSlot);
    }
    if (KisCanvasResourceProvider *resources = d->resources()) {
        connect(resources, &KisCanvasResourceProvider::sigPaintOpPresetChanged, this, &KisStudioToolRail::rememberPreset);
    }

    // the current brush decides between Paint and Blend, see rememberPreset()
    d->brushSlot = d->slotIndex(BrushMode::Paint);
    rememberPreset();
    updateActiveSlot();
}

KisStudioToolRail::~KisStudioToolRail() = default;

void KisStudioToolRail::addBottomWidget(QWidget *widget)
{
    widget->setParent(this);
    d->layout->addWidget(widget, 0, Qt::AlignHCenter);
}

void KisStudioToolRail::activateSlot(int slot)
{
    activateTool(slot, d->currentTool[slot]);
}

void KisStudioToolRail::activateTool(int slot, const QString &toolId)
{
    const ToolSlot &s = d->slotList[slot];
    d->currentTool[slot] = toolId;
    KisConfig(false).writeEntry(QStringLiteral("StudioToolSlot_") + s.id, toolId);

    QAction *erase = d->eraseAction();
    if (s.brush == BrushMode::Erase) {
        KoToolManager::instance()->switchToolRequested(toolId);
        if (erase && !erase->isChecked()) {
            erase->setChecked(true);
        }
    } else if (s.brush == BrushMode::Paint || s.brush == BrushMode::Blend) {
        const bool switchingBrush = d->brushSlot != slot;
        KisCanvasResourceProvider *resources = d->resources();
        if (switchingBrush && resources && resources->currentPreset()) {
            // the slot we leave keeps the brush it was using
            KisConfig(false).writeEntry(QStringLiteral("StudioBrushPreset_") + d->slotList[d->brushSlot].id,
                                        resources->currentPreset()->name());
        }
        d->brushSlot = slot;
        if (erase && erase->isChecked()) {
            erase->setChecked(false);
        }
        KoToolManager::instance()->switchToolRequested(toolId);
        if (switchingBrush) {
            loadPresetForSlot(slot);
        }
    } else {
        KoToolManager::instance()->switchToolRequested(toolId);
    }
    updateActiveSlot();
}

void KisStudioToolRail::showVariants(int slot)
{
    const ToolSlot &s = d->slotList[slot];
    if (s.tools.size() < 2) {
        return;
    }

    QMenu menu;
    menu.addSection(s.title);
    Q_FOREACH (const QString &toolId, s.tools) {
        KoToolFactoryBase *factory = KoToolRegistry::instance()->value(toolId);
        if (!factory) {
            continue;
        }
        QString text = toolName(factory);
        // the shortcut shown after a tab is only a hint, it doesn't register a second shortcut
        if (QAction *toolAction = d->mainWindow->actionCollection()->action(toolId)) {
            const QString shortcut = toolAction->shortcut().toString(QKeySequence::NativeText);
            if (!shortcut.isEmpty()) {
                text += QLatin1Char('\t') + shortcut;
            }
        }
        QAction *action = menu.addAction(KisIconUtils::loadIcon(factory->iconName()), text);
        action->setCheckable(true);
        action->setChecked(toolId == d->currentTool[slot]);
        connect(action, &QAction::triggered, this, [this, slot, toolId]() { activateTool(slot, toolId); });
    }

    // next to the rail, aligned with the button
    QWidget *button = d->buttons[slot];
    menu.exec(button->mapToGlobal(QPoint(button->width() + 6, 0)));
    updateActiveSlot();
}

void KisStudioToolRail::updateActiveSlot()
{
    const QString toolId = KoToolManager::instance()->activeToolId();
    QAction *erase = d->eraseAction();
    const bool erasing = erase && erase->isChecked();

    int active = -1;
    if (toolId == brushToolId) {
        active = erasing ? d->slotIndex(BrushMode::Erase) : d->brushSlot;
    } else {
        for (int i = 0; i < d->slotList.size() && active < 0; i++) {
            if (d->slotList[i].brush == BrushMode::None && d->slotList[i].tools.contains(toolId)) {
                active = i;
            }
        }
        // a variant of Paint, e.g. the dynamic brush
        if (active < 0) {
            const int paint = d->slotIndex(BrushMode::Paint);
            if (paint >= 0 && d->slotList[paint].tools.contains(toolId)) {
                active = paint;
            }
        }
    }

    if (active >= 0 && d->slotList[active].tools.contains(toolId)) {
        d->currentTool[active] = toolId;
    }
    d->activeSlot = active;

    for (int i = 0; i < d->buttons.size(); i++) {
        d->buttons[i]->setChecked(i == active);

        QString tip = d->slotList[i].title;
        if (d->slotList[i].tools.size() > 1) {
            if (KoToolFactoryBase *factory = KoToolRegistry::instance()->value(d->currentTool[i])) {
                tip = QStringLiteral("%1 · %2").arg(tip, toolName(factory));
            }
        }
        if (!d->slotList[i].shortcutHint.isEmpty()) {
            tip += QStringLiteral("  (%1)").arg(d->slotList[i].shortcutHint);
        }
        if (d->slotList[i].tools.size() > 1) {
            tip += QLatin1Char('\n') + i18nc("@info:tooltip", "Hold or right-click for more");
        }
        d->buttons[i]->setToolTip(tip);
    }
}

void KisStudioToolRail::rememberPreset()
{
    KisCanvasResourceProvider *resources = d->resources();
    if (d->loadingPreset || !resources || !resources->currentPreset()) {
        return;
    }
    const QString name = resources->currentPreset()->name();

    // a brush that one of the slots remembers belongs to that slot, so the
    // highlighted slot always matches the brush in use (e.g. after a restart)
    KisConfig cfg(true);
    for (int i = 0; i < d->slotList.size(); i++) {
        const BrushMode mode = d->slotList[i].brush;
        if ((mode == BrushMode::Paint || mode == BrushMode::Blend)
            && cfg.readEntry<QString>(QStringLiteral("StudioBrushPreset_") + d->slotList[i].id) == name) {
            if (d->brushSlot != i) {
                d->brushSlot = i;
                updateActiveSlot();
            }
            return;
        }
    }

    QAction *erase = d->eraseAction();
    if (KoToolManager::instance()->activeToolId() != brushToolId || (erase && erase->isChecked())) {
        return;
    }
    // a new brush picked while in Paint or Blend becomes that slot's brush
    KisConfig(false).writeEntry(QStringLiteral("StudioBrushPreset_") + d->slotList[d->brushSlot].id, name);
}

void KisStudioToolRail::loadPresetForSlot(int slot)
{
    KisCanvasResourceProvider *resources = d->resources();
    KisPaintOpPresetResourceServer *server = KisResourceServerProvider::instance()->paintOpPresetServer();
    if (!resources || !server) {
        return;
    }

    QString name = KisConfig(true).readEntry<QString>(QStringLiteral("StudioBrushPreset_") + d->slotList[slot].id);
    if (name.isEmpty() && d->slotList[slot].brush == BrushMode::Blend) {
        // first use of Blend: a smudging brush from the bundled ones
        KisResourceModel *model = server->resourceModel();
        for (int row = 0; row < model->rowCount() && name.isEmpty(); row++) {
            const QString candidate = model->index(row, 0).data(Qt::UserRole + KisAbstractResourceModel::Name).toString();
            if (candidate.contains(QStringLiteral("blend"), Qt::CaseInsensitive)
                || candidate.contains(QStringLiteral("smudge"), Qt::CaseInsensitive)
                || candidate.contains(QStringLiteral("smear"), Qt::CaseInsensitive)) {
                name = candidate;
            }
        }
    }
    if (name.isEmpty() || (resources->currentPreset() && resources->currentPreset()->name() == name)) {
        return;
    }

    KisPaintOpPresetSP preset = server->resource(QString(), QString(), name);
    if (preset) {
        d->loadingPreset = true;
        resources->setPaintOpPreset(preset);
        d->loadingPreset = false;
        KisConfig(false).writeEntry(QStringLiteral("StudioBrushPreset_") + d->slotList[slot].id, name);
    }
}
