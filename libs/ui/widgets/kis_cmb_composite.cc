/*
 *  kis_cmb_composite.cc - part of KImageShop/Krayon/Krita
 *
 *  SPDX-FileCopyrightText: 2004 Boudewijn Rempt (boud@valdyas.org)
 *  SPDX-FileCopyrightText: 2011 Silvio Heinrich <plassy@web.de>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "kis_cmb_composite.h"

#include <KisModernStyle.h>
#include "KisBlendModePicker.h"

#include <KoCompositeOp.h>
#include <KoCompositeOpRegistry.h>

#include "kis_composite_ops_model.h"
#include "kis_categorized_item_delegate.h"
#include <kis_action.h>
#include <QWheelEvent>
#include <QApplication>
#include <QFocusEvent>
#include <QHideEvent>
#include <QMouseEvent>
#include "kis_action_manager.h"

//////////////////////////////////////////////////////////////////////////////////////////
// ---- KisCompositeOpListWidget ------------------------------------------------------ //

KisCompositeOpListWidget::KisCompositeOpListWidget(QWidget* parent):
    KisCategorizedListView(parent),
    m_model(new KisSortedCompositeOpListModel(false, this))
{
    setModel(m_model);
    setItemDelegate(new KisCategorizedItemDelegate(this));
}

KisCompositeOpListWidget::~KisCompositeOpListWidget()
{
}

KoID KisCompositeOpListWidget::selectedCompositeOp() const {
    KoID op;

    if (m_model->entryAt(op, currentIndex())) {
        return op;
    }

    return KoCompositeOpRegistry::instance().getDefaultCompositeOp();
}

void KisCompositeOpListWidget::setCompositeOp(const KoID &id)
{
    const QModelIndex index = m_model->indexOf(id);
    if (index.isValid()) {
        setCurrentIndex(index);
    } else {
        qWarning() << "KisCompositeOpListWidget::setCompositeOp: ailed to find index for blendmode" << ppVar(id);
    }
}

//////////////////////////////////////////////////////////////////////////////////////////
// ---- KisCompositeOpComboBox -------------------------------------------------------- //

KisCompositeOpComboBox::KisCompositeOpComboBox(QWidget* parent)
    : KisCompositeOpComboBox(false, parent)
{
}

KisCompositeOpComboBox::KisCompositeOpComboBox(bool limitToLayerStyles, QWidget* parent)
    : KisSqueezedComboBox(parent),
      m_model(new KisSortedCompositeOpListModel(limitToLayerStyles, this)),
      m_allowToHidePopup(true),
      m_limitToLayerStyles(limitToLayerStyles)
{
    m_view = new KisCategorizedListView();
    m_view->setCompositeBoxControl(true);

    setMaxVisibleItems(100);
    setSizeAdjustPolicy(AdjustToContents);
    m_view->setResizeMode(QListView::Adjust);

    setToolTip(limitToLayerStyles ? i18n("Blending Mode") : i18nc("@info:tooltip", "Blend mode"));

    setModel(m_model);
    setView(m_view);
    setItemDelegate(new KisCategorizedItemDelegate(this));

    connect(m_view, SIGNAL(sigCategoryToggled(QModelIndex,bool)), SLOT(slotCategoryToggled(QModelIndex,bool)));
    connect(m_view, SIGNAL(sigEntryChecked(QModelIndex)), SLOT(slotEntryChecked(QModelIndex)));

    selectCompositeOp(KoCompositeOpRegistry::instance().getDefaultCompositeOp());
}

KisCompositeOpComboBox::~KisCompositeOpComboBox()
{
    delete m_view;
}

void KisCompositeOpComboBox::connectBlendmodeActions(KisActionManager *manager)
{
    KisAction *action = 0;

    action = manager->createAction("Next Blending Mode");
    connect(action, SIGNAL(triggered()), SLOT(slotNextBlendingMode()));

    action = manager->createAction("Previous Blending Mode");
    connect(action, SIGNAL(triggered()), SLOT(slotPreviousBlendingMode()));

    action = manager->createAction("Select Normal Blending Mode");
    connect(action, SIGNAL(triggered()), SLOT(slotNormal()));

    action = manager->createAction("Select Dissolve Blending Mode");
    connect(action, SIGNAL(triggered()), SLOT(slotDissolve()));

    action = manager->createAction("Select Behind Blending Mode");
    connect(action, SIGNAL(triggered()), SLOT(slotBehind()));

    action = manager->createAction("Select Clear Blending Mode");
    connect(action, SIGNAL(triggered()), SLOT(slotClear()));

    action = manager->createAction("Select Darken Blending Mode");
    connect(action, SIGNAL(triggered()), SLOT(slotDarken()));

    action = manager->createAction("Select Multiply Blending Mode");
    connect(action, SIGNAL(triggered()), SLOT(slotMultiply()));

    action = manager->createAction("Select Color Burn Blending Mode");
    connect(action, SIGNAL(triggered()), SLOT(slotColorBurn()));

    action = manager->createAction("Select Linear Burn Blending Mode");
    connect(action, SIGNAL(triggered()), SLOT(slotLinearBurn()));

    action = manager->createAction("Select Lighten Blending Mode");
    connect(action, SIGNAL(triggered()), SLOT(slotLighten()));

    action = manager->createAction("Select Screen Blending Mode");
    connect(action, SIGNAL(triggered()), SLOT(slotScreen()));

    action = manager->createAction("Select Color Dodge Blending Mode");
    connect(action, SIGNAL(triggered()), SLOT(slotColorDodge()));

    action = manager->createAction("Select Linear Dodge Blending Mode");
    connect(action, SIGNAL(triggered()), SLOT(slotLinearDodge()));

    action = manager->createAction("Select Overlay Blending Mode");
    connect(action, SIGNAL(triggered()), SLOT(slotOverlay()));

    action = manager->createAction("Select Hard Overlay Blending Mode");
    connect(action, SIGNAL(triggered()), SLOT(slotHardOverlay()));

    action = manager->createAction("Select Soft Light Blending Mode");
    connect(action, SIGNAL(triggered()), SLOT(slotSoftLight()));

    action = manager->createAction("Select Hard Light Blending Mode");
    connect(action, SIGNAL(triggered()), SLOT(slotHardLight()));

    action = manager->createAction("Select Vivid Light Blending Mode");
    connect(action, SIGNAL(triggered()), SLOT(slotVividLight()));

    action = manager->createAction("Select Linear Light Blending Mode");
    connect(action, SIGNAL(triggered()), SLOT(slotLinearLight()));

    action = manager->createAction("Select Pin Light Blending Mode");
    connect(action, SIGNAL(triggered()), SLOT(slotPinLight()));

    action = manager->createAction("Select Hard Mix Blending Mode");
    connect(action, SIGNAL(triggered()), SLOT(slotHardMix()));

    action = manager->createAction("Select Difference Blending Mode");
    connect(action, SIGNAL(triggered()), SLOT(slotDifference()));

    action = manager->createAction("Select Exclusion Blending Mode");
    connect(action, SIGNAL(triggered()), SLOT(slotExclusion()));

    action = manager->createAction("Select Hue Blending Mode");
    connect(action, SIGNAL(triggered()), SLOT(slotHue()));

    action = manager->createAction("Select Saturation Blending Mode");
    connect(action, SIGNAL(triggered()), SLOT(slotSaturation()));

    action = manager->createAction("Select Color Blending Mode");
    connect(action, SIGNAL(triggered()), SLOT(slotColor()));

    action = manager->createAction("Select Luminosity Blending Mode");
    connect(action, SIGNAL(triggered()), SLOT(slotLuminosity()));
}

void KisCompositeOpComboBox::validate(const KoColorSpace *cs) {
    m_model->validate(cs);
}

void KisCompositeOpComboBox::selectCompositeOp(const KoID &op) {
    KoID currentOp;
    if (m_model->entryAt(currentOp, m_model->index(currentIndex(), 0)) &&
            currentOp == op) {

        return;
    }

    QModelIndex index = m_model->indexOf(op);

    setCurrentIndex(index.row());
    Q_EMIT activated(index.row());
    Q_EMIT textActivated(op.name());
}

KoID KisCompositeOpComboBox::selectedCompositeOp() const {
    KoID op;

    if (m_model->entryAt(op, m_model->index(currentIndex(), 0))) {
        return op;
    }
    return KoCompositeOpRegistry::instance().getDefaultCompositeOp();
}

void KisCompositeOpComboBox::slotCategoryToggled(const QModelIndex& index, bool toggled)
{
    Q_UNUSED(index);
    Q_UNUSED(toggled);

    //NOTE: this will (should) fit the size of the
    //      popup widget to the view
    //      don't know if this is expected behaviour
    //      on all supported platforms.
    //      There is nothing written about this in the docs.
    showPopup();
}

void KisCompositeOpComboBox::slotEntryChecked(const QModelIndex& index)
{
    Q_UNUSED(index);
    m_allowToHidePopup = false;
}

void KisCompositeOpComboBox::hidePopup()
{
    if (m_allowToHidePopup) {
        QComboBox::hidePopup();
    }
    else  {
        QComboBox::showPopup();
    }

    m_allowToHidePopup = true;
}

bool KisCompositeOpComboBox::isUsable(const QString &id) const
{
    const QModelIndex index = m_model->indexOf(KoCompositeOpRegistry::instance().getKoID(id));
    return index.isValid() && (index.flags() & Qt::ItemIsEnabled);
}

void KisCompositeOpComboBox::pick(const KoID &op)
{
    const QModelIndex index = m_model->indexOf(op);
    if (!index.isValid()) {
        return;
    }
    setCurrentIndex(index.row());
    Q_EMIT activated(index.row());
    Q_EMIT textActivated(op.name());
}

void KisCompositeOpComboBox::showPopup()
{
    // layer styles allow only a few modes: the plain list fits them better
    if (m_limitToLayerStyles) {
        KisSqueezedComboBox::showPopup();
        return;
    }

    QSet<QString> available;
    for (int row = 0; row < m_model->rowCount(); row++) {
        KoID op;
        const QModelIndex index = m_model->index(row, 0);
        if (m_model->entryAt(op, index) && (index.flags() & Qt::ItemIsEnabled)) {
            available.insert(op.id());
        }
    }

    auto *picker = new KisBlendModePicker(selectedCompositeOp().id(), available, this,
                                          m_pictures ? m_pictures() : KisBlendModePicker::Pictures());
    connect(picker, &KisBlendModePicker::previewed, this, &KisCompositeOpComboBox::sigPreviewBlendMode);
    // reported even when the box shows it already: the preview may have synced the box, not the layer
    connect(picker, &KisBlendModePicker::picked, this, &KisCompositeOpComboBox::pick);
    picker->popupBelow(this);
}

void KisCompositeOpComboBox::slotNextBlendingMode()
{
    selectNeighbouringBlendMode(true);
}

void KisCompositeOpComboBox::slotPreviousBlendingMode()
{
    selectNeighbouringBlendMode(false);
}

void KisCompositeOpComboBox::slotNormal()
{
    selectCompositeOp(KoCompositeOpRegistry::instance().getKoID(COMPOSITE_OVER));
}

void KisCompositeOpComboBox::slotDissolve()
{
    selectCompositeOp(KoCompositeOpRegistry::instance().getKoID(COMPOSITE_DISSOLVE));
}

void KisCompositeOpComboBox::slotBehind()
{
    selectCompositeOp(KoCompositeOpRegistry::instance().getKoID(COMPOSITE_BEHIND));
}

void KisCompositeOpComboBox::slotClear()
{
    selectCompositeOp(KoCompositeOpRegistry::instance().getKoID(COMPOSITE_CLEAR));
}

void KisCompositeOpComboBox::slotDarken()
{
    selectCompositeOp(KoCompositeOpRegistry::instance().getKoID(COMPOSITE_DARKEN));
}

void KisCompositeOpComboBox::slotMultiply()
{
    selectCompositeOp(KoCompositeOpRegistry::instance().getKoID(COMPOSITE_MULT));
}

void KisCompositeOpComboBox::slotColorBurn()
{
    selectCompositeOp(KoCompositeOpRegistry::instance().getKoID(COMPOSITE_BURN));
}

void KisCompositeOpComboBox::slotLinearBurn()
{
    selectCompositeOp(KoCompositeOpRegistry::instance().getKoID(COMPOSITE_LINEAR_BURN));
}

void KisCompositeOpComboBox::slotLighten()
{
    selectCompositeOp(KoCompositeOpRegistry::instance().getKoID(COMPOSITE_LIGHTEN));
}

void KisCompositeOpComboBox::slotScreen()
{
    selectCompositeOp(KoCompositeOpRegistry::instance().getKoID(COMPOSITE_SCREEN));
}

void KisCompositeOpComboBox::slotColorDodge()
{
    selectCompositeOp(KoCompositeOpRegistry::instance().getKoID(COMPOSITE_DODGE));
}

void KisCompositeOpComboBox::slotLinearDodge()
{
    selectCompositeOp(KoCompositeOpRegistry::instance().getKoID(COMPOSITE_LINEAR_DODGE));
}

void KisCompositeOpComboBox::slotOverlay()
{
    selectCompositeOp(KoCompositeOpRegistry::instance().getKoID(COMPOSITE_OVERLAY));
}

void KisCompositeOpComboBox::slotHardOverlay()
{
    selectCompositeOp(KoCompositeOpRegistry::instance().getKoID(COMPOSITE_HARD_OVERLAY));
}

void KisCompositeOpComboBox::slotSoftLight()
{
    selectCompositeOp(KoCompositeOpRegistry::instance().getKoID(COMPOSITE_SOFT_LIGHT_PHOTOSHOP));
}

void KisCompositeOpComboBox::slotHardLight()
{
    selectCompositeOp(KoCompositeOpRegistry::instance().getKoID(COMPOSITE_HARD_LIGHT));
}

void KisCompositeOpComboBox::slotVividLight()
{
    selectCompositeOp(KoCompositeOpRegistry::instance().getKoID(COMPOSITE_VIVID_LIGHT));
}

void KisCompositeOpComboBox::slotLinearLight()
{
    selectCompositeOp(KoCompositeOpRegistry::instance().getKoID(COMPOSITE_LINEAR_LIGHT));
}

void KisCompositeOpComboBox::slotPinLight()
{
    selectCompositeOp(KoCompositeOpRegistry::instance().getKoID(COMPOSITE_PIN_LIGHT));
}

void KisCompositeOpComboBox::slotHardMix()
{
    selectCompositeOp(KoCompositeOpRegistry::instance().getKoID(COMPOSITE_HARD_MIX_PHOTOSHOP));
}

void KisCompositeOpComboBox::slotDifference()
{
    selectCompositeOp(KoCompositeOpRegistry::instance().getKoID(COMPOSITE_DIFF));
}

void KisCompositeOpComboBox::slotExclusion()
{
    selectCompositeOp(KoCompositeOpRegistry::instance().getKoID(COMPOSITE_EXCLUSION));
}

void KisCompositeOpComboBox::slotHue()
{
    selectCompositeOp(KoCompositeOpRegistry::instance().getKoID(COMPOSITE_HUE));
}

void KisCompositeOpComboBox::slotSaturation()
{
    selectCompositeOp(KoCompositeOpRegistry::instance().getKoID(COMPOSITE_SATURATION));
}

void KisCompositeOpComboBox::slotColor()
{
    selectCompositeOp(KoCompositeOpRegistry::instance().getKoID(COMPOSITE_COLOR));
}

void KisCompositeOpComboBox::slotLuminosity()
{
    selectCompositeOp(KoCompositeOpRegistry::instance().getKoID(COMPOSITE_LUMINIZE));
}

void KisCompositeOpComboBox::selectNeighbouringBlendMode(bool down)
{
    // your modes first, then the others; no wrapping, so that up always heads back to your first ones
    if (!m_limitToLayerStyles) {
        const QStringList order = KisBlendModePicker::scrollOrder();
        const int position = order.indexOf(selectedCompositeOp().id());
        if (position >= 0) {
            for (int i = position + (down ? 1 : -1); i >= 0 && i < order.size(); i += down ? 1 : -1) {
                if (isUsable(order[i])) {
                    selectCompositeOp(KoCompositeOpRegistry::instance().getKoID(order[i]));
                    return;
                }
            }
            return;
        }
    }

    const int rowCount = count();
    int newIndex = currentIndex();

    QAbstractItemModel *model = this->model();
    KoID op;

    if (!down) {
        newIndex--;
        while ((newIndex >= 0) &&
               (!(model->flags(model->index(newIndex, modelColumn(), rootModelIndex())) & Qt::ItemIsEnabled) ||
                !m_model->entryAt(op, m_model->index(newIndex, modelColumn()))))

            newIndex--;
    } else {
        newIndex++;
        while (newIndex < rowCount &&
               (!(model->index(newIndex, modelColumn(), rootModelIndex()).flags() & Qt::ItemIsEnabled) ||
                !m_model->entryAt(op, m_model->index(newIndex, modelColumn()))))

            newIndex++;
    }

    if (newIndex >= 0 && newIndex < rowCount && newIndex != currentIndex()) {
        setCurrentIndex(newIndex);

        Q_EMIT activated(newIndex);
        if (m_model->entryAt(op, m_model->index(newIndex, 0))) {
            Q_EMIT textActivated(op.name());
        }
    }
}

void KisCompositeOpComboBox::setPreviewPictures(std::function<KisBlendModePicker::Pictures()> pictures)
{
    m_pictures = std::move(pictures);
}

void KisCompositeOpComboBox::mousePressEvent(QMouseEvent *e)
{
    if (m_limitToLayerStyles || e->button() != Qt::LeftButton || !isEnabled()) {
        KisSqueezedComboBox::mousePressEvent(e);
        return;
    }
    // the picker opens on the release, unless this becomes a scrub
    m_pressed = true;
    m_scrubbing = false;
    m_pressPos = e->pos();
    e->accept();
}

void KisCompositeOpComboBox::mouseMoveEvent(QMouseEvent *e)
{
    if (!m_pressed) {
        KisSqueezedComboBox::mouseMoveEvent(e);
        return;
    }
    const QPoint delta = e->pos() - m_pressPos;
    const QStringList order = KisBlendModePicker::scrollOrder();
    if (!m_scrubbing) {
        // a scrub as soon as the pointer clearly goes sideways
        if (qAbs(delta.x()) < qMax(3, QApplication::startDragDistance() / 2) || qAbs(delta.x()) < qAbs(delta.y())) {
            return;
        }
        m_scrubbing = true;
        m_scrubStart = order.indexOf(selectedCompositeOp().id());
        m_scrubId.clear();
        setCursor(Qt::SizeHorCursor);
    }
    // a mode per step, in the order of scrolling; the first half a step from the press
    constexpr int step = 18;
    int target = qBound(0, m_scrubStart + qRound(qreal(delta.x()) / step), order.size() - 1);
    const int direction = delta.x() >= 0 ? 1 : -1;
    while (target >= 0 && target < order.size() && !isUsable(order[target])) {
        target += direction;
    }
    if (target < 0 || target >= order.size() || order[target] == m_scrubId) {
        return;
    }
    m_scrubId = order[target];
    // the box shows it (a brush's box applies it as it changes), a layer previews it
    setCurrentIndex(m_model->indexOf(KoCompositeOpRegistry::instance().getKoID(m_scrubId)).row());
    Q_EMIT sigPreviewBlendMode(m_scrubId);
}

void KisCompositeOpComboBox::mouseReleaseEvent(QMouseEvent *e)
{
    if (!m_pressed) {
        KisSqueezedComboBox::mouseReleaseEvent(e);
        return;
    }
    e->accept();
    if (m_scrubbing) {
        endScrub();
        return;
    }
    m_pressed = false;
    // a click, not a press that ended elsewhere
    if (rect().contains(e->pos())) {
        showPopup();
    }
}

void KisCompositeOpComboBox::focusOutEvent(QFocusEvent *e)
{
    // the release may never come (another window, a dialog): keep the scrubbed mode as a release would
    endScrub();
    KisSqueezedComboBox::focusOutEvent(e);
}

void KisCompositeOpComboBox::hideEvent(QHideEvent *e)
{
    endScrub();
    KisSqueezedComboBox::hideEvent(e);
}

void KisCompositeOpComboBox::endScrub()
{
    if (!m_pressed) {
        return;
    }
    m_pressed = false;
    if (!m_scrubbing) {
        return;
    }
    m_scrubbing = false;
    unsetCursor();
    if (m_scrubId.isEmpty()) {
        return;
    }
    Q_EMIT sigPreviewBlendMode(QString());
    pick(KoCompositeOpRegistry::instance().getKoID(m_scrubId));
}

void KisCompositeOpComboBox::wheelEvent(QWheelEvent *e)
{
    // your modes, then the others, like the shortcuts
    if (!m_limitToLayerStyles) {
        const int steps = KisModernStyle::wheelSteps(e, this);
        for (int i = 0; i < qAbs(steps); i++) {
            selectNeighbouringBlendMode(steps < 0);
        }
        e->accept();
        return;
    }

    /**
     * This code is a copy of QComboBox::wheelEvent. It does the same thing,
     * except that it skips "Category" items, by checking m_model->entryAt()
     * on each step.
     */

    QStyleOptionComboBox opt;
    initStyleOption(&opt);

    if (style()->styleHint(QStyle::SH_ComboBox_AllowWheelScrolling, &opt, this)) {
        const int steps = KisModernStyle::wheelSteps(e, this);
        for (int i = 0; i < qAbs(steps); i++) {
            selectNeighbouringBlendMode(steps < 0);
        }

        e->accept();
    } else {
        KisSqueezedComboBox::wheelEvent(e);
    }
}

void KisCompositeOpComboBox::keyPressEvent(QKeyEvent *e)
{
    /**
     * This code is a copy of QComboBox::keyPressEvent. It does the same thing,
     * except that it skips "Category" items, by checking m_model->entryAt()
     * on each step.
     */

    enum Move { NoMove=0 , MoveUp , MoveDown , MoveFirst , MoveLast};

    Move move = NoMove;
    int newIndex = currentIndex();
    switch (e->key()) {
    case Qt::Key_Up:
        if (e->modifiers() & Qt::ControlModifier)
            break; // pass to line edit for auto completion
        Q_FALLTHROUGH();
    case Qt::Key_PageUp:
        move = MoveUp;
        break;
    case Qt::Key_Down:
        if (e->modifiers() & Qt::AltModifier) {
            showPopup();
            return;
        } else if (e->modifiers() & Qt::ControlModifier)
            break; // pass to line edit for auto completion
        Q_FALLTHROUGH();
    case Qt::Key_PageDown:
        move = MoveDown;
        break;
    case Qt::Key_Home:
        move = MoveFirst;
        break;
    case Qt::Key_End:
        move = MoveLast;
        break;
    case Qt::Key_F4:
        if (!e->modifiers()) {
            showPopup();
            return;
        }
        break;
    case Qt::Key_Space:
        showPopup();
        return;
    default:
        break;
    }

    const int rowCount = count();

    if (move != NoMove) {
        KoID op;

        e->accept();
        switch (move) {
        case MoveFirst:
            newIndex = -1;
            Q_FALLTHROUGH();
        case MoveDown:
            newIndex++;
            while (newIndex < rowCount &&
                   (!(model()->index(newIndex, modelColumn(), rootModelIndex()).flags() & Qt::ItemIsEnabled) ||
                    !m_model->entryAt(op, m_model->index(newIndex, modelColumn()))))
                newIndex++;
            break;
        case MoveLast:
            newIndex = rowCount;
            Q_FALLTHROUGH();
        case MoveUp:
            newIndex--;
            while ((newIndex >= 0) &&
                   (!(model()->flags(model()->index(newIndex, modelColumn(), rootModelIndex())) & Qt::ItemIsEnabled) ||
                    !m_model->entryAt(op, m_model->index(newIndex, modelColumn()))))
                newIndex--;
            break;
        default:
            e->ignore();
            break;
        }

        if (newIndex >= 0 && newIndex < rowCount && newIndex != currentIndex()) {
            setCurrentIndex(newIndex);
            Q_EMIT activated(newIndex);

            if (m_model->entryAt(op, m_model->index(newIndex, 0))) {
                Q_EMIT textActivated(op.name());
            }
        }
    } else {
        KisSqueezedComboBox::keyPressEvent(e);
    }
}

KisLayerStyleCompositeOpComboBox::KisLayerStyleCompositeOpComboBox(QWidget* parent)
    : KisCompositeOpComboBox(true, parent)
{
}
