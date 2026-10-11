/*
  SPDX-FileCopyrightText: 2006 Gábor Lehel <illissius@gmail.com>
  SPDX-FileCopyrightText: 2008 Cyrille Berger <cberger@cberger.net>
  SPDX-FileCopyrightText: 2011 José Luis Vergara <pentalis@gmail.com>

  SPDX-License-Identifier: LGPL-2.0-or-later
*/
#include "kis_config.h"
#include "NodeDelegate.h"
#include "kis_node_model.h"
#include "NodeToolTip.h"
#include "NodeView.h"
#include "KisPart.h"
#include "input/kis_input_manager.h"

#include <QtDebug>
#include <QApplication>
#include <QCursor>
#include <QKeyEvent>
#include <QLineEdit>
#include <QModelIndex>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QBitmap>
#include <QToolTip>

#include <klocalizedstring.h>
#include "kis_node_view_color_scheme.h"
#include "kis_icon_utils.h"
#include "kis_layer_properties_icons.h"
#include "krita_utils.h"
#include "kis_config_notifier.h"
#include <kis_painting_tweaks.h>

typedef KisBaseNode::Property* OptionalProperty;

#include <kis_base_node.h>
#include <KisModernStyle.h>

class NodeDelegate::Private
{
public:
    Private(NodeDelegate *_q, NodeView *_view) : q(_q), view(_view), edit(0), tip(_view) { }

    NodeDelegate *q;

    NodeView *view;
    QPointer<QWidget> edit;
    NodeToolTip tip;

    QImage checkers;
    QColor checkersColor1;
    QColor checkersColor2;

    QRect thumbnailGeometry;
    int thumbnailSize {-1};
    int rowHeight {-1};

    QList<QModelIndex> shiftClickedIndexes;

    enum StasisOperation {
        Record,
        Review,
        Restore
    };

    QList<OptionalProperty> rightmostProperties(const KisBaseNode::PropertyList &props) const;
    int numProperties(const QModelIndex &index) const;
    OptionalProperty findProperty(KisBaseNode::PropertyList &props, const OptionalProperty &refProp) const;
    OptionalProperty findVisibilityProperty(KisBaseNode::PropertyList &props) const;

    void toggleProperty(KisBaseNode::PropertyList &props, const OptionalProperty clickedProperty, const Qt::KeyboardModifiers modifier, const QModelIndex &index);
    void togglePropertyRecursive(const QModelIndex &root, const OptionalProperty &clickedProperty, const QList<QModelIndex> &items, StasisOperation record, bool mode);

    bool stasisIsDirty(const QModelIndex &root, const OptionalProperty &clickedProperty, bool on = false, bool off = false);
    void resetPropertyStateRecursive(const QModelIndex &root, const OptionalProperty &clickedProperty);
    void restorePropertyInStasisRecursive(const QModelIndex &root, const OptionalProperty &clickedProperty);

    bool checkImmediateStasis(const QModelIndex &root, const OptionalProperty &clickedProperty);

    void getParentsIndex(QList<QModelIndex> &items, const QModelIndex &index);
    void getChildrenIndex(QList<QModelIndex> &items, const QModelIndex &index);
    void getSiblingsIndex(QList<QModelIndex> &items, const QModelIndex &index);
    boost::optional<KisBaseNode::Property>
    propForMousePos(const QModelIndex &index, const QPoint &mousePos, const QStyleOptionViewItem &option);
};

NodeDelegate::NodeDelegate(NodeView *view, QObject *parent)
    : QAbstractItemDelegate(parent)
    , d(new Private(this, view))
{
    QApplication::instance()->installEventFilter(this);
    connect(KisConfigNotifier::instance(), SIGNAL(configChanged()), SLOT(slotConfigChanged()));
    connect(this, SIGNAL(resetVisibilityStasis()), SLOT(slotResetState()));
    slotConfigChanged();
}

NodeDelegate::~NodeDelegate()
{
    delete d;
}

int NodeDelegate::extraHeight(const QModelIndex &index) const
{
    Q_UNUSED(index);
    return 0;
}

/// Whether \p index is the selected layer's row, with its blend mode and opacity in it
static bool holdsControls(const QAbstractItemView *view, const QModelIndex &index)
{
    const QModelIndex current = view->currentIndex();
    return view->property("rowControls").toBool() && current.isValid() && current.row() == index.row()
        && current.parent() == index.parent();
}

QRect NodeDelegate::inlineControlsRect(const QModelIndex &index, const QRect &row, Qt::LayoutDirection direction) const
{
    QStyleOptionViewItem option;
    option.rect = row;
    option.direction = direction;
    const QRect icons = iconsRect(option, index);
    const int width = d->view->property("rowControlsWidth").toInt();
    const int left = direction == Qt::RightToLeft ? icons.right() + 1 : icons.left() - width;
    return QRect(left, row.top(), width, d->rowHeight);
}

QSize NodeDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    KisNodeViewColorScheme scm;
    if (index.column() == NodeView::VISIBILITY_COL) {
        return QSize(scm.visibilityColumnWidth(), d->rowHeight + extraHeight(index));
    }
    return QSize(option.rect.width(), d->rowHeight + extraHeight(index));
}

void NodeDelegate::paint(QPainter *p, const QStyleOptionViewItem &cell, const QModelIndex &index) const
{
    p->save();

    /**
     * The eye's cell is part of its row: selected and pointed at with it
     * (only the layer's own cell is selectable in the model).
     */
    QStyleOptionViewItem o = cell;
    if (index.column() != NodeView::DEFAULT_COL) {
        const QModelIndex main = index.sibling(index.row(), NodeView::DEFAULT_COL);
        if (d->view->selectionModel() && d->view->selectionModel()->isSelected(main)) {
            o.state |= QStyle::State_Selected;
        }
        const QPoint pointer = d->view->viewport()->mapFromGlobal(QCursor::pos());
        const QModelIndex under = d->view->viewport()->rect().contains(pointer) ? static_cast<QAbstractItemView *>(d->view)->indexAt(pointer) : QModelIndex();
        if (under.isValid() && under.row() == index.row() && under.parent() == index.parent()) {
            o.state |= QStyle::State_MouseOver;
        }
    }

    {
        QStyleOptionViewItem option = getOptions(o, index);
        QStyle *style = option.widget ? option.widget->style() : QApplication::style();
        // the selection is the weight, not the font
        option.font.setBold(false);

        // a rounded highlight behind the row, the eye at its end included
        if (index.column() != NodeView::SELECTED_COL && (option.state & (QStyle::State_Selected | QStyle::State_MouseOver))) {
            // the tint over what is behind the row, not mixed into a color of it: over glass the glass shows through
            // a wash of the ink, as a flat button on the glass is lit: selected the most
            QColor tint = option.palette.color(QPalette::Text);
            tint.setAlphaF((option.state & QStyle::State_Selected) ? (option.state & QStyle::State_MouseOver ? 0.2 : 0.16) : 0.07);
            p->save();
            p->setRenderHint(QPainter::Antialiasing, true);
            p->setPen(Qt::NoPen);
            p->setBrush(tint);
            // one shape over the two cells: each draws its own end of it
            const bool eye = index.column() == NodeView::VISIBILITY_COL;
            const bool leftToRight = option.direction != Qt::RightToLeft;
            const qreal beyond = 48;
            QRectF shape = QRectF(option.rect).adjusted(1, 1, -1, -1);
            if (eye == leftToRight) {
                shape.adjust(-beyond, 0, -1, 0);
            } else {
                shape.adjust(0, 0, beyond, 0);
            }
            p->setClipRect(option.rect);
            p->drawPath(KisModernStyle::squircle(shape, 6));
            p->restore();
        }

        bool shouldGrayOut = index.data(KisNodeModel::ShouldGrayOutRole).toBool();
        if (shouldGrayOut) {
            option.state &= ~QStyle::State_Enabled;
        }

        // the highlight takes the whole row, the layer's own content its top: the rest holds the controls
        option.rect.setHeight(option.rect.height() - extraHeight(index));
        QStyleOptionViewItem top = o;
        top.rect = option.rect;

        drawFrame(p, option, index);

        if (index.column() == NodeView::SELECTED_COL) {
            drawSelectedButton(p, top, index, style);
        } else if (index.column() == NodeView::VISIBILITY_COL) {
            drawVisibilityIcon(p, option, index); // TODO hide when dragging
        } else {
            p->setFont(option.font);
            drawColorLabel(p, option, index);
            drawThumbnail(p, option, index);
            drawText(p, option, index); // BUG: Creating group moves things around (RTL-layout alignment)
            drawIcons(p, option, index);
            drawDecoration(p, option, index);
            drawExpandButton(p, option, index);
            drawAnimatedDecoration(p, option, index);

            drawProgressBar(p, option, index);
        }
    }
    p->restore();
}

void NodeDelegate::drawBranches(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    p->save();
    drawFrame(p, option, index);
    p->restore();

    QModelIndex tmp = index.parent();

    // there is no indentation if we have no parent group, so don't draw a branch
    if (!tmp.isValid()) return;

    const KisNodeViewColorScheme &scm = *KisNodeViewColorScheme::instance();

    int rtlNum = (option.direction == Qt::RightToLeft) ? 1 : -1;
    QPoint nodeCorner = (option.direction == Qt::RightToLeft) ? option.rect.topLeft() : option.rect.topRight();
    int branchSpacing = rtlNum * d->view->indentation();

    QPoint base = nodeCorner + 0.5 * QPoint(branchSpacing, option.rect.height()) + QPoint(0, scm.iconSize()/4);

    QColor color = scm.gridColor(option, d->view);
    QColor bgColor = option.state & QStyle::State_Selected ?
        qApp->palette().color(QPalette::Base) :
        qApp->palette().color(QPalette::Text);
    color = KisPaintingTweaks::blendColors(color, bgColor, 0.9);

    // TODO: if we are a mask type, use dotted lines for the branch style
    // p->setPen(QPen(p->pen().color(), 2, Qt::DashLine, Qt::RoundCap, Qt::RoundJoin));
    p->setPen(QPen(color, 0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));

    QPoint p2 = base - QPoint(rtlNum*(qMin(d->view->indentation(), scm.iconSize())/2), 0);
    QPoint p3 = base - QPoint(0, scm.iconSize()/2);
    p->drawLine(base, p2);
    p->drawLine(base, p3);

    // draw parent lines (keep drawing until x position is less than 0
    QPoint parentBase1 = base + QPoint(branchSpacing, 0);
    QPoint parentBase2 = p3 + QPoint(branchSpacing, 0);

    // indent lines needs to be very subtle to avoid making the docker busy looking
    color = KisPaintingTweaks::blendColors(color, bgColor, 0.9); // makes it a little lighter than L lines
    p->setPen(QPen(color, 0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));


    int levelRowIndex = tmp.row();
    tmp = tmp.parent(); // Ignore the first group as it was already painted

    while (tmp.isValid()) {
        bool moreSiblings = index.model()->rowCount(tmp) > (levelRowIndex + 1);
        if (moreSiblings) {
            p->drawLine(parentBase1, parentBase2);
        }

        parentBase1 += QPoint(branchSpacing, 0);
        parentBase2 += QPoint(branchSpacing, 0);

        levelRowIndex = tmp.row();
        tmp = tmp.parent();
    }
}

void NodeDelegate::drawColorLabel(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    KisNodeViewColorScheme scm;
    const int label = index.data(KisNodeModel::ColorLabelIndexRole).toInt();
    QColor color = scm.colorFromLabelIndex(label);
    if (color.alpha() <= 0) return;

    QColor bgColor = qApp->palette().color(QPalette::Base);
    color = KisPaintingTweaks::blendColors(color, bgColor, 0.3);

    QRect optionRect = (option.state & QStyle::State_Selected) ? iconsRect(option, index) : option.rect;

    p->fillRect(optionRect, color);
}

void NodeDelegate::drawFrame(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    // rows are told apart by spacing and their rounded highlight, not by grid lines
    Q_UNUSED(p);
    Q_UNUSED(option);
    Q_UNUSED(index);
}

QRect NodeDelegate::thumbnailClickRect(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    Q_UNUSED(index);

    QRect rc = d->thumbnailGeometry;

    // Move to current index
    rc.moveTop(option.rect.topLeft().y());
    // Move to correct location.
    if (option.direction == Qt::RightToLeft) {
        rc.moveRight(option.rect.right());
    } else {
        rc.moveLeft(option.rect.left());
    }

    return rc;
}

void NodeDelegate::drawThumbnail(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    KisNodeViewColorScheme scm;

    const qreal devicePixelRatio = p->device()->devicePixelRatioF();
    const int thumbSizeHighRes = d->thumbnailSize*devicePixelRatio;

    const qreal oldOpacity = p->opacity(); // remember previous opacity

    QImage img = index.data(int(KisNodeModel::BeginThumbnailRole) + thumbSizeHighRes).value<QImage>();
    img.setDevicePixelRatio(devicePixelRatio);
    if (!(option.state & QStyle::State_Enabled)) {
        p->setOpacity(0.35);
    }

    QRect fitRect = thumbnailClickRect(option, index);
    // Shrink to icon rect
    fitRect = kisGrowRect(fitRect, -(scm.thumbnailMargin()+scm.border()));

    QPoint offset;
    offset.setX((fitRect.width() - img.width()/devicePixelRatio) / 2);
    offset.setY((fitRect.height() - img.height()/devicePixelRatio) / 2);
    offset += fitRect.topLeft();

    QBrush brush(d->checkers);
    p->setBrushOrigin(offset);
    QRect imageRectLowRes = QRect(img.rect().topLeft(), img.rect().size()/devicePixelRatio);

    // a rounded tile, like a picture on a phone, with a hairline around it
    p->save();
    p->setRenderHint(QPainter::Antialiasing, true);
    QPainterPath tile;
    tile.addPath(KisModernStyle::squircle(QRectF(imageRectLowRes.translated(offset)), 3));
    p->setClipPath(tile);
    p->fillRect(imageRectLowRes.translated(offset), brush);
    p->drawImage(offset, img);
    p->setClipping(false);
    p->setOpacity(oldOpacity); // restore old opacity
    QColor border = option.palette.color(QPalette::Text);
    border.setAlphaF(0.18);
    p->setPen(QPen(border, 1));
    p->setBrush(Qt::NoBrush);
    p->drawPath(tile);
    p->restore();
}

QRect NodeDelegate::iconsRect(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    KisNodeViewColorScheme scm;

    int propCount = d->numProperties(index);

    const int iconsWidth =
        propCount * (scm.iconSize() + 2 * scm.iconMargin()) +
        (propCount + 1) * scm.border();

    QRect fitRect = QRect(0, 0, iconsWidth, d->rowHeight - scm.border());
    // Move to current index
    fitRect.moveTop(option.rect.topLeft().y());
    // Move to correct location.
    if (option.direction == Qt::RightToLeft) {
        fitRect.moveLeft(option.rect.topLeft().x());
    } else {
        fitRect.moveRight(option.rect.topRight().x());
    }

    return fitRect;
}

QRect NodeDelegate::textRect(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    KisNodeViewColorScheme scm;

    static QFont f;
    static int minbearing = 1337 + 666; //can be 0 or negative, 2003 is less likely
    if (minbearing == 2003 || f != option.font) {
        f = option.font; //getting your bearings can be expensive, so we cache them
        minbearing = option.fontMetrics.minLeftBearing() + option.fontMetrics.minRightBearing();
    }

    const QRect decoRect = decorationClickRect(option, index);
    const QRect iconRect = iconsRect(option, index);

    const bool decorated = showsDecoration(index) || (option.state & QStyle::State_Children);
    QRect rc = QRect((option.direction == Qt::RightToLeft) ? iconRect.topRight() : (decorated ? decoRect.topRight() : decoRect.topLeft()),
                     (option.direction == Qt::RightToLeft) ? (decorated ? decoRect.bottomLeft() : decoRect.bottomRight()) : iconRect.bottomLeft());
    // the selected layer's blend mode and opacity by its icons
    if (holdsControls(d->view, index)) {
        const int controls = d->view->property("rowControlsWidth").toInt();
        if (option.direction == Qt::RightToLeft) {
            rc.setLeft(rc.left() + controls);
        } else {
            rc.setRight(rc.right() - controls);
        }
    }
    rc.adjust(-(scm.border()+(minbearing/2)), 0,
               (scm.border()+(minbearing/2)), 0);

    return rc;
}

void NodeDelegate::drawText(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    KisNodeViewColorScheme scm;
    const QRect rc = textRect(option, index).adjusted(scm.textMargin(), 0,
                                                      -scm.textMargin(), 0);

    QPen oldPen = p->pen();
    const qreal oldOpacity = p->opacity(); // remember previous opacity

    p->setPen(option.palette.color(QPalette::Active,QPalette::Text ));

    if (!(option.state & QStyle::State_Enabled)) {
        p->setOpacity(0.55);
    }

    QString text = index.data(Qt::DisplayRole).toString();
    // being renamed: its editor shows the name
    if (d->edit && d->edit->isVisible() && d->view->indexWidget(index) == d->edit) text.clear();
    if (d->view->property("rowControls").toBool()) {
        /**
         * The name, and by the icons what is unusual about the layer's blend
         * (its mode, its opacity: where the selected layer has them to change)
         */
        const QString info = holdsControls(d->view, index) ? QString() : index.data(KisNodeModel::InfoTextRole).toString();
        const int infoWidth = info.isEmpty() ? 0 : p->fontMetrics().horizontalAdvance(info) + scm.textMargin() * 2;
        const QString name = p->fontMetrics().elidedText(text, Qt::ElideRight, qMax(0, rc.width() - infoWidth));
        const Qt::Alignment start = option.direction == Qt::RightToLeft ? Qt::AlignRight : Qt::AlignLeft;
        const Qt::Alignment end = option.direction == Qt::RightToLeft ? Qt::AlignLeft : Qt::AlignRight;
        p->drawText(rc, start | Qt::AlignVCenter, name);
        if (!info.isEmpty()) {
            p->setOpacity(p->opacity() * 0.55);
            p->drawText(rc, end | Qt::AlignVCenter, p->fontMetrics().elidedText(info, Qt::ElideLeft, infoWidth));
        }
        p->setPen(oldPen);
        p->setOpacity(oldOpacity);
        return;
    }
    const QString elided = p->fontMetrics().elidedText(text, Qt::ElideRight, rc.width());
    KisConfig cfg(true);
    if (cfg.layerInfoTextStyle() == KisConfig::LayerInfoTextStyle::INFOTEXT_NONE) {
        p->drawText(rc, Qt::AlignLeft | Qt::AlignVCenter, elided);
    }
    else {
        const QString infoText = index.data(KisNodeModel::InfoTextRole).toString();
        if (infoText.isEmpty()) {
            p->drawText(rc, Qt::AlignLeft | Qt::AlignVCenter, elided);
        } else {
            bool useOneLine = cfg.useInlineLayerInfoText();
            if (!useOneLine) {
                // check whether there is enough space for two lines
                const int textHeight = p->fontMetrics().height();
                useOneLine = rc.height() < textHeight*2;
            }

            const int rectCenter = rc.height()/2;
            const int nameWidth = p->fontMetrics().horizontalAdvance(elided);
            // draw the layer name
            if (!useOneLine) {
                // enforce Qt::TextSingleLine because we are adding a line below it
                p->drawText(rc.adjusted(0, 0, 0, -rectCenter), Qt::AlignLeft | Qt::AlignBottom | Qt::TextSingleLine, elided);
            }
            else {
                p->drawText(rc.adjusted(0, 0, 0, 0), Qt::AlignLeft | Qt::AlignVCenter, elided);
            }
            // draw the info-text
            p->save();
            QFont layerInfoTextFont = p->font();
            layerInfoTextFont.setBold(false);
            p->setFont(layerInfoTextFont);
            if (option.state & QStyle::State_Enabled) {
                p->setOpacity(qreal(cfg.layerInfoTextOpacity())/100);
            }
            if (!useOneLine) {
                const QString infoTextElided = p->fontMetrics().elidedText(infoText, Qt::ElideRight, rc.width());
                p->drawText(rc.adjusted(0, rectCenter, 0, 0), Qt::AlignLeft | Qt::AlignTop, infoTextElided);
            }
            else {
                const QString infoTextElided = p->fontMetrics().elidedText(" "+infoText, Qt::ElideRight, rc.width()-nameWidth);
                p->drawText(rc.adjusted(nameWidth, 0, 0, 0), Qt::AlignLeft | Qt::AlignVCenter, infoTextElided);
            }
            p->restore();
        }
    }

    p->setPen(oldPen); // restore pen settings
    p->setOpacity(oldOpacity);
}

QList<OptionalProperty> NodeDelegate::Private::rightmostProperties(const KisBaseNode::PropertyList &props) const
{
    QList<OptionalProperty> list;
    QList<OptionalProperty> prependList;
    list << OptionalProperty(0);
    list << OptionalProperty(0);
    list << OptionalProperty(0);

    KisBaseNode::PropertyList::const_iterator it = props.constBegin();
    KisBaseNode::PropertyList::const_iterator end = props.constEnd();
    for (; it != end; ++it) {
        if (!it->isMutable &&
                it->id != KisLayerPropertiesIcons::layerError.id() &&
                it->id != KisLayerPropertiesIcons::layerColorSpaceMismatch.id() &&
                it->id != KisLayerPropertiesIcons::colorOverlay.id()) continue;

        if (it->id == KisLayerPropertiesIcons::visible.id()) {
            // noop...
        } else if (it->id == KisLayerPropertiesIcons::locked.id()) {
            list[0] = OptionalProperty(&(*it));
        } else if (it->id == KisLayerPropertiesIcons::inheritAlpha.id()) {
            list[1] = OptionalProperty(&(*it));
        } else if (it->id == KisLayerPropertiesIcons::alphaLocked.id()) {
            list[2] = OptionalProperty(&(*it));
        } else {
            prependList.prepend(OptionalProperty(&(*it)));
        }
    }

    auto putToTheLeft = [] (QList<OptionalProperty> &list, const QString &id) {
        auto it = std::find_if(list.begin(), list.end(), kismpl::mem_equal_to(&KisBaseNode::Property::id, id));
        if (it != list.end()) {
            std::rotate(list.begin(), it, std::next(it));
        }
    };

    putToTheLeft(prependList, KisLayerPropertiesIcons::colorOverlay.id());
    putToTheLeft(prependList, KisLayerPropertiesIcons::layerColorSpaceMismatch.id());
    putToTheLeft(prependList, KisLayerPropertiesIcons::layerError.id());

    {
        QMutableListIterator<OptionalProperty> i(prependList);
        i.toBack();
        while (i.hasPrevious()) {
            OptionalProperty val = i.previous();

            int emptyIndex = list.lastIndexOf(nullptr);
            if (emptyIndex < 0) break;

            list[emptyIndex] = val;
            i.remove();
        }
    }

    return prependList + list;
}

int NodeDelegate::Private::numProperties(const QModelIndex &index) const
{
    KisBaseNode::PropertyList props = index.data(KisNodeModel::PropertiesRole).value<KisBaseNode::PropertyList>();
    QList<OptionalProperty> realProps = rightmostProperties(props);
    return realProps.size();
}

OptionalProperty NodeDelegate::Private::findProperty(KisBaseNode::PropertyList &props, const OptionalProperty &refProp) const
{
    KisBaseNode::PropertyList::iterator it = props.begin();
    KisBaseNode::PropertyList::iterator end = props.end();
    for (; it != end; ++it) {
        if (it->id == refProp->id) {
            return &(*it);
        }
    }

    return 0;
}

OptionalProperty NodeDelegate::Private::findVisibilityProperty(KisBaseNode::PropertyList &props) const
{
    KisBaseNode::PropertyList::iterator it = props.begin();
    KisBaseNode::PropertyList::iterator end = props.end();
    for (; it != end; ++it) {
        if (it->id == KisLayerPropertiesIcons::visible.id()) {
            return &(*it);
        }
    }

    return 0;
}

void NodeDelegate::Private::toggleProperty(KisBaseNode::PropertyList &props, const OptionalProperty clickedProperty, const Qt::KeyboardModifiers modifier, const QModelIndex &index)
{
    QModelIndex root(view->rootIndex());

    if (clickedProperty->id == KisLayerPropertiesIcons::colorOverlay.id()) {
        // Open the properties dialog for the layer's fast color overlay mask.
        view->model()->setData(index, QVariant() /* unused */, KisNodeModel::LayerColorOverlayPropertiesRole);

    } else if ((modifier & Qt::ShiftModifier) == Qt::ShiftModifier && clickedProperty->canHaveStasis) {
        bool mode = true;

        OptionalProperty prop = findProperty(props, clickedProperty);

        // XXX: Change to use NodeProperty
        int position = shiftClickedIndexes.indexOf(index);

        StasisOperation record = (!prop->isInStasis)? StasisOperation::Record :
                      (position < 0) ? StasisOperation::Review : StasisOperation::Restore;

        shiftClickedIndexes.clear();
        shiftClickedIndexes.push_back(index);

        QList<QModelIndex> items;
        if (modifier == (Qt::ControlModifier | Qt::ShiftModifier)) {
            mode = false; // inverted mode
            items.insert(0, index); // important!
            getSiblingsIndex(items, index);
        } else {
            getParentsIndex(items, index);
            getChildrenIndex(items, index);
        }
        togglePropertyRecursive(root, clickedProperty, items, record, mode);

    } else {
        // If we have properties in stasis, we need to cancel stasis to avoid overriding
        // values in stasis.
        // IMPORTANT -- we also need to check the first row of nodes to determine
        // if a stasis is currently active in some cases.
        const bool hasPropInStasis = (shiftClickedIndexes.count() > 0 || checkImmediateStasis(root, clickedProperty));
        if (clickedProperty->canHaveStasis && hasPropInStasis) {
            shiftClickedIndexes.clear();

            restorePropertyInStasisRecursive(root, clickedProperty);
        } else {
            shiftClickedIndexes.clear();

            resetPropertyStateRecursive(root, clickedProperty);

            OptionalProperty prop = findProperty(props, clickedProperty);
            prop->state = !prop->state.toBool();
            prop->isInStasis = false;
            view->model()->setData(index, QVariant::fromValue(props), KisNodeModel::PropertiesRole);
        }
    }
}

void NodeDelegate::Private::togglePropertyRecursive(const QModelIndex &root, const OptionalProperty &clickedProperty, const QList<QModelIndex> &items, StasisOperation record, bool mode)
{
    int rowCount = view->model()->rowCount(root);

    for (int i = 0; i < rowCount; i++) {
        QModelIndex idx = view->model()->index(i, 0, root);

        // The entire property list has to be altered because model->setData cannot set individual properties
        KisBaseNode::PropertyList props = idx.data(KisNodeModel::PropertiesRole).value<KisBaseNode::PropertyList>();
        OptionalProperty prop = findProperty(props, clickedProperty);

        if (!prop) continue;

        if (record == StasisOperation::Record) {
             prop->stateInStasis = prop->state.toBool();
        }
        if (record == StasisOperation::Review || record ==  StasisOperation::Record) {
            prop->isInStasis = true;
            if(mode) { //include mode
                prop->state = (items.contains(idx)) ? QVariant(true) : QVariant(false);
            } else { // exclude
                prop->state = (!items.contains(idx))? prop->state :
                              (items.at(0) == idx)? QVariant(true) : QVariant(false);
            }
        } else { // restore
            prop->state = QVariant(prop->stateInStasis);
            prop->isInStasis = false;
        }

        view->model()->setData(idx, QVariant::fromValue(props), KisNodeModel::PropertiesRole);

        togglePropertyRecursive(idx,clickedProperty, items, record, mode);
    }
}

bool NodeDelegate::Private::stasisIsDirty(const QModelIndex &root, const OptionalProperty &clickedProperty, bool on, bool off)
{

    int rowCount = view->model()->rowCount(root);
    bool result = false;

    for (int i = 0; i < rowCount; i++) {
        if (result) break; // return on first find
        QModelIndex idx = view->model()->index(i, 0, root);
        // The entire property list has to be altered because model->setData cannot set individual properties
        KisBaseNode::PropertyList props = idx.data(KisNodeModel::PropertiesRole).value<KisBaseNode::PropertyList>();
        OptionalProperty prop = findProperty(props, clickedProperty);

        if (!prop) continue;
        if (prop->isInStasis) {
            on = true;
        } else {
            off = true;
        }
        // stop if both states exist
        if (on && off) {
            return true;
        }

        result = stasisIsDirty(idx,clickedProperty, on, off);
    }
    return result;
}

void NodeDelegate::Private::resetPropertyStateRecursive(const QModelIndex &root, const OptionalProperty &clickedProperty)
{
    if (!clickedProperty->canHaveStasis) return;
    int rowCount = view->model()->rowCount(root);

    for (int i = 0; i < rowCount; i++) {
        QModelIndex idx = view->model()->index(i, 0, root);
        // The entire property list has to be altered because model->setData cannot set individual properties
        KisBaseNode::PropertyList props = idx.data(KisNodeModel::PropertiesRole).value<KisBaseNode::PropertyList>();
        OptionalProperty prop = findProperty(props, clickedProperty);

        if (!prop) continue;
        prop->isInStasis = false;
        view->model()->setData(idx, QVariant::fromValue(props), KisNodeModel::PropertiesRole);

        resetPropertyStateRecursive(idx,clickedProperty);
    }
}

void NodeDelegate::Private::restorePropertyInStasisRecursive(const QModelIndex &root, const OptionalProperty &clickedProperty)
{
    if (!clickedProperty->canHaveStasis) return;
    int rowCount = view->model()->rowCount(root);

    for (int i = 0; i < rowCount; i++) {
        QModelIndex idx = view->model()->index(i, 0, root);
        KisBaseNode::PropertyList props = idx.data(KisNodeModel::PropertiesRole).value<KisBaseNode::PropertyList>();
        OptionalProperty prop = findProperty(props, clickedProperty);

        if (prop->isInStasis) {
            prop->isInStasis = false;
            prop->state = QVariant(prop->stateInStasis);
        }

        view->model()->setData(idx, QVariant::fromValue(props), KisNodeModel::PropertiesRole);

        restorePropertyInStasisRecursive(idx, clickedProperty);
    }
}

bool NodeDelegate::Private::checkImmediateStasis(const QModelIndex &root, const OptionalProperty &clickedProperty)
{
    if (!clickedProperty->canHaveStasis) return false;

    const int rowCount = view->model()->rowCount(root);
    for (int i = 0; i < rowCount; i++){
        QModelIndex idx = view->model()->index(i, 0, root);
        KisBaseNode::PropertyList props = idx.data(KisNodeModel::PropertiesRole).value<KisBaseNode::PropertyList>();
        OptionalProperty prop = findProperty(props, clickedProperty);

        if (prop->isInStasis) {
            return true;
        }
    }

    return false;
}

void NodeDelegate::Private::getParentsIndex(QList<QModelIndex> &items, const QModelIndex &index)
{
    if (!index.isValid()) return;
    items.append(index);
    getParentsIndex(items, index.parent());
}

void NodeDelegate::Private::getChildrenIndex(QList<QModelIndex> &items, const QModelIndex &index)
{
    qint32 childs = view->model()->rowCount(index);
    QModelIndex child;
    // STEP 1: Go.
    for (quint16 i = 0; i < childs; ++i) {
        child = view->model()->index(i, 0, index);
        items.append(child);
        getChildrenIndex(items, child);
    }
}

void NodeDelegate::Private::getSiblingsIndex(QList<QModelIndex> &items, const QModelIndex &index)
{
    qint32 numberOfLeaves = view->model()->rowCount(index.parent());
    QModelIndex item;
    // STEP 1: Go.
    for (quint16 i = 0; i < numberOfLeaves; ++i) {
        item = view->model()->index(i, 0, index.parent());
        if (item != index) {
            items.append(item);
        }
    }
}


void NodeDelegate::drawIcons(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    KisNodeViewColorScheme scm;
    const QRect rc = iconsRect(option, index);

    QTransform oldTransform = p->transform();
    QPen oldPen = p->pen();
    p->setTransform(QTransform::fromTranslate(rc.x(), rc.y()));
    p->setPen(scm.gridColor(option, d->view));

    int x = 0;
    const int y = (d->rowHeight - scm.border() - scm.iconSize()) / 2;
    KisBaseNode::PropertyList props = index.data(KisNodeModel::PropertiesRole).value<KisBaseNode::PropertyList>();
    QList<OptionalProperty> realProps = d->rightmostProperties(props);

    if (option.direction == Qt::RightToLeft) {
        std::reverse(realProps.begin(), realProps.end());
    }

    // a row says what is unusual about its layer; the rest shows while it is pointed at or selected
    const bool pointedAt = option.state & QStyle::State_MouseOver;
    const bool revealAll = option.state & (QStyle::State_MouseOver | QStyle::State_Selected);
    // a hidden layer is its closed eye: nothing else of it matters until it is pointed at
    KisBaseNode::PropertyList allProps = props;
    const OptionalProperty visibility = d->findVisibilityProperty(allProps);
    const bool hidden = visibility && !visibility->state.toBool();
    // the icon under the pointer, lit, on a backplate of its own
    const QPoint pointer = d->view->viewport()->mapFromGlobal(QCursor::pos()) - rc.topLeft();
    const int slot = scm.iconSize() + 2 * scm.iconMargin() + scm.border();
    const int under = pointedAt && pointer.y() >= 0 && pointer.y() < rc.height() && pointer.x() >= 0 ? pointer.x() / slot : -1;
    int slotAt = 0;

    Q_FOREACH (OptionalProperty prop, realProps) {
        const bool lit = slotAt++ == under;
        x += scm.iconMargin();
        if (prop && (prop->state.toBool() || revealAll) && (!hidden || pointedAt)) {
            QIcon icon = prop->state.toBool() ? prop->onIcon : prop->offIcon;
            bool fullColor = prop->state.toBool() && option.state & QStyle::State_Enabled;

            const qreal oldOpacity = p->opacity(); // remember previous opacity
            // on is a toggle pressed in: a plate under it at full ink; off is faint, bare; pointed at, a lighter plate
            const bool on = prop->state.toBool();
            if (on || lit) {
                QColor plate = option.palette.color(QPalette::Text);
                plate.setAlphaF((on ? 0.2 : 0.0) + (lit ? 0.1 : 0.0));
                p->save();
                p->setRenderHint(QPainter::Antialiasing, true);
                p->setPen(Qt::NoPen);
                p->setBrush(plate);
                p->drawPath(KisModernStyle::squircle(QRectF(x - scm.iconMargin() + 1, y - scm.iconMargin() + 1, scm.iconSize() + 2 * scm.iconMargin() - 2,
                                                            scm.iconSize() + 2 * scm.iconMargin() - 2), 4));
                p->restore();
            }
            if (fullColor || lit) {
                p->setOpacity(1.0);
            } else {
                p->setOpacity(on ? 0.6 : 0.3);
            }

            if (prop->id == KisLayerPropertiesIcons::colorOverlay.id()) {
                // Parent layer can show its color overlay mask color here.
                QRect colorRect(x, y, scm.iconSize(), scm.iconSize());
                colorRect = colorRect.marginsRemoved(QMargins(2, 1, 2, 1));
                p->fillRect(colorRect, index.data(KisNodeModel::LayerColorOverlayColorRole).value<QColor>());
                p->drawRect(colorRect);
            } else {
                p->drawPixmap(x, y, icon.pixmap(scm.iconSize(), QIcon::Normal));
            }
            p->setOpacity(oldOpacity); // restore old opacity
        }
        x += scm.iconSize() + scm.iconMargin();

        x += scm.border();
    }

    // Draw a color preview "icon" for some types of filter masks,
    // but especially for KisFilterFastColorOverlay.

    if (index.data(KisNodeModel::FilterMaskColorRole).isNull() == false) {
        if (!(option.state & QStyle::State_Enabled)) {
            p->setOpacity(0.35);
        } else {
            p->setOpacity(1.0);
        }

        const QRect colorRect = filterColorClickRect(option, index).translated(-rc.x(), -rc.y());
        p->fillRect(colorRect, index.data(KisNodeModel::FilterMaskColorRole).value<QColor>());
        p->drawRect(colorRect);
    }

    p->setTransform(oldTransform);
    p->setPen(oldPen);
}

QRect NodeDelegate::visibilityClickRect(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    Q_UNUSED(index);
    KisNodeViewColorScheme scm;

    QRect rc = scm.relVisibilityRect();
    rc.setHeight(d->rowHeight);

    // Move to current index
    rc.moveCenter(option.rect.center());
    // Move to correct location.
    if (option.direction == Qt::RightToLeft) {
        rc.moveRight(option.rect.right());
    } else {
        rc.moveLeft(option.rect.left());
    }

    return rc;
}

QRect NodeDelegate::decorationClickRect(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    Q_UNUSED(index);
    KisNodeViewColorScheme scm;

    QRect rc = scm.relDecorationRect();

    // Move to current index
    rc.moveTop(option.rect.topLeft().y());
    rc.setHeight(d->rowHeight);
    // Move to correct location.
    if (option.direction == Qt::RightToLeft) {
        rc.moveRight(option.rect.right() - d->thumbnailGeometry.width());
    } else {
        rc.moveLeft(option.rect.left() + d->thumbnailGeometry.width());
    }

    return rc;
}

void NodeDelegate::drawVisibilityIcon(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    KisNodeViewColorScheme scm;

    KisBaseNode::PropertyList props = index.data(KisNodeModel::PropertiesRole).value<KisBaseNode::PropertyList>();
    OptionalProperty prop = d->findVisibilityProperty(props);
    if (!prop) return;

    QRect fitRect = visibilityClickRect(option, index);
    // Shrink to icon rect
    fitRect = kisGrowRect(fitRect, -(scm.visibilityMargin() + scm.border()));

    QIcon icon = prop->state.toBool() ? prop->onIcon : prop->offIcon;

    // visible is the normal case and needs no mark; a hidden layer shows its closed eye
    const bool revealAll = option.state & (QStyle::State_MouseOver | QStyle::State_Selected);
    if (prop->state.toBool() && !revealAll && !prop->isInStasis) {
        return;
    }
    const qreal oldOpacity = p->opacity();

    if (!prop->state.toBool()) {
        p->setOpacity(0.7);
    } else {
        p->setOpacity(0.55);
    }

    QPixmap pixmapIcon(icon.pixmap(scm.visibilitySize(), QIcon::Active));
    p->drawPixmap(fitRect.x(), fitRect.center().y() - scm.visibilitySize() / 2, pixmapIcon);

    if (prop->isInStasis) {
        QPainter::CompositionMode prevComposition = p->compositionMode();
        p->setCompositionMode(QPainter::CompositionMode_HardLight);
        pixmapIcon = icon.pixmap(scm.visibilitySize(), QIcon::Active);
        QBitmap mask = pixmapIcon.mask();
        pixmapIcon.fill(d->view->palette().color(QPalette::Highlight));
        pixmapIcon.setMask(mask);
        p->drawPixmap(fitRect.x(), fitRect.center().y() - scm.visibilitySize() / 2, pixmapIcon);
        p->setCompositionMode(prevComposition);
    }

    p->setOpacity(oldOpacity);

    //// For debugging purposes only
// //     // p->save();
// //     // p->setPen(Qt::blue);
// //     // KritaUtils::renderExactRect(p, visibilityClickRect(option, index));
// //     // p->restore();
}

bool NodeDelegate::showsDecoration(const QModelIndex &index) const
{
    // a paint layer is what a layer is unless it says otherwise: its icon would only repeat that on every row
    const QIcon icon = index.data(Qt::DecorationRole).value<QIcon>();
    static const qint64 paintLayer = KisIconUtils::loadIcon("paintLayer").cacheKey();
    return !icon.isNull() && icon.cacheKey() != paintLayer;
}

void NodeDelegate::drawDecoration(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    KisNodeViewColorScheme scm;
    QIcon icon = index.data(Qt::DecorationRole).value<QIcon>();

    if (showsDecoration(index)) {
        QPixmap pixmap = icon.pixmap(scm.decorationSize(),
                                     (option.state & QStyle::State_Enabled) ?
                                     QIcon::Normal : QIcon::Disabled);

        QRect rc = decorationClickRect(option, index);

        // Shrink to icon rect
        rc = kisGrowRect(rc, -(scm.decorationMargin()+scm.border()));

        const qreal oldOpacity = p->opacity(); // remember previous opacity

        if (!(option.state & QStyle::State_Enabled)) {
            p->setOpacity(0.35);
        }

        p->drawPixmap(QPoint(rc.left(), rc.center().y() - pixmap.height() / pixmap.devicePixelRatio() / 2), pixmap);

        p->setOpacity(oldOpacity); // restore old opacity
    }
}

void NodeDelegate::drawExpandButton(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    Q_UNUSED(index);
    KisNodeViewColorScheme scm;

    QRect rc = decorationClickRect(option, index);

    // Move to current index
//     rc.moveTop(option.rect.topLeft().y());
    // Shrink to icon rect
    rc = kisGrowRect(rc, -(scm.decorationMargin()+scm.border()));

    if (!(option.state & QStyle::State_Children)) return;

    QString iconName = option.state & QStyle::State_Open ?
        "arrow-down" : ((option.direction == Qt::RightToLeft) ? "arrow-left" : "arrow-right");
    QIcon icon = KisIconUtils::loadIcon(iconName);
    QPixmap pixmap = icon.pixmap(rc.width(),
                                 (option.state & QStyle::State_Enabled) ?
                                 QIcon::Normal : QIcon::Disabled);
    p->drawPixmap(rc.bottomLeft()-QPoint(0, scm.decorationSize()-1), pixmap);
}

void NodeDelegate::drawAnimatedDecoration(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const {

    KisNodeViewColorScheme scm;
    QRect rc = decorationClickRect(option, index);

    QIcon animatedIndicatorIcon = KisIconUtils::loadIcon("layer-animated");
    const bool isAnimated = index.data(KisNodeModel::IsAnimatedRole).toBool();

    rc = kisGrowRect(rc, -(scm.decorationMargin()+scm.border()));

    if (!isAnimated) return;

    if ((option.state & QStyle::State_Children)) return;

    const qreal oldOpacity = p->opacity(); // remember previous opacity

    if (!(option.state & QStyle::State_Enabled)) {
        p->setOpacity(0.35);
    }

    int decorationSize = scm.decorationSize();

    QPixmap animPixmap = animatedIndicatorIcon.pixmap(decorationSize,
                                 (option.state & QStyle::State_Enabled) ?
                                 QIcon::Normal : QIcon::Disabled);

    p->drawPixmap(rc.bottomLeft()-QPoint(0, scm.decorationSize()-1), animPixmap);

    p->setOpacity(oldOpacity);
}

void NodeDelegate::drawSelectedButton(QPainter *p, const QStyleOptionViewItem &option,
                                      const QModelIndex &index, QStyle *style) const
{
    QStyleOptionButton buttonOption;

    KisNodeViewColorScheme scm;
    QRect rect = option.rect;

    // adjust the icon to not touch the borders
    rect = kisGrowRect(rect, -(scm.thumbnailMargin() + scm.border()));
    // Make the rect a square so the check mark is not distorted. also center
    // it horizontally and vertically with respect to the whole area rect
    constexpr qint32 maximumAllowedSideLength = 48;
    const qint32 minimumSideLength = qMin(rect.width(), rect.height());
    const qint32 sideLength = qMin(minimumSideLength, maximumAllowedSideLength);
    rect =
        QRect(rect.left() + static_cast<qint32>(qRound(static_cast<qreal>(rect.width() - sideLength) / 2.0)),
              rect.top() + static_cast<qint32>(qRound(static_cast<qreal>(rect.height() - sideLength) / 2.0)),
              sideLength, sideLength);

    buttonOption.rect = rect;

    // Update palette colors to make the check box more readable over the base
    // color
    const QColor prevBaseColor = buttonOption.palette.base().color();
    const qint32 windowLightness = buttonOption.palette.window().color().lightness();
    const qint32 baseLightness = prevBaseColor.lightness();
    const QColor newBaseColor =
        baseLightness > windowLightness ? prevBaseColor.lighter(120) : prevBaseColor.darker(120);
    buttonOption.palette.setColor(QPalette::Window, prevBaseColor);
    buttonOption.palette.setColor(QPalette::Base, newBaseColor);

    // check if the current index exists in the selected rows.
    buttonOption.state.setFlag((d->view->selectionModel()->isRowSelected(index.row(), index.parent())
                                    ? QStyle::State_On
                                    : QStyle::State_Off));
    style->drawPrimitive(QStyle::PE_IndicatorCheckBox, &buttonOption, p);
}

boost::optional<KisBaseNode::Property>
NodeDelegate::Private::propForMousePos(const QModelIndex &index, const QPoint &mousePos, const QStyleOptionViewItem &option)
{
    KisNodeViewColorScheme scm;

    const QRect iconsRect = q->iconsRect(option, index);

    const bool iconsClicked = iconsRect.isValid() &&
        iconsRect.contains(mousePos);

    if (!iconsClicked) return boost::none;

    KisBaseNode::PropertyList props = index.data(KisNodeModel::PropertiesRole).value<KisBaseNode::PropertyList>();
    QList<OptionalProperty> realProps = this->rightmostProperties(props);
    if (option.direction == Qt::RightToLeft) {
        std::reverse(realProps.begin(), realProps.end());
    }
    const int numProps = realProps.size();

    const int iconWidth = scm.iconSize() + 2 * scm.iconMargin() + scm.border();
    const int xPos = mousePos.x() - iconsRect.left();
    const int clickedIcon = xPos / iconWidth;
    const int distToBorder = qMin(xPos % iconWidth, iconWidth - xPos % iconWidth);


    if (clickedIcon >= 0 &&
        clickedIcon < numProps &&
        realProps[clickedIcon] &&
        distToBorder > scm.iconMargin()) {

        return *realProps[clickedIcon];
    }

    return boost::none;
}

bool NodeDelegate::editorEvent(QEvent *event, QAbstractItemModel *model, const QStyleOptionViewItem &option, const QModelIndex &index)
{
    //Explicitly handle mouse release events
    if (event->type() == QEvent::MouseButtonRelease &&index.flags() & Qt::ItemIsEnabled)  {
        //For some reason qt5 doesn't give us which button was released, From my testing i couldn't get a release event on rmb, so we assume that it's lmb
        QMouseEvent *mouseEvent = static_cast<QMouseEvent*>(event);
        auto clickedProperty = d->propForMousePos(index, mouseEvent->pos(), option);
        if (!clickedProperty && mouseEvent->modifiers() == Qt::ControlModifier) {
            changeSelectionAndCurrentIndex(index);
            return true;
        }

        //We only need this event for ctrl clicks, so we just return here
        return false;
    }

    if ((event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonDblClick)
        && (index.flags() & Qt::ItemIsEnabled))
    {
        QMouseEvent *mouseEvent = static_cast<QMouseEvent*>(event);

        const bool leftButton = mouseEvent->buttons() & Qt::LeftButton;
        const bool altButton = mouseEvent->modifiers() & Qt::AltModifier;
        // marks that are off only show on hover or selection; a pen or a finger
        // has no hover, so its tap on another row can't hit an unseen mark
        const bool marksShown = mouseEvent->source() == Qt::MouseEventNotSynthesized
            || d->view->selectionModel()->isRowSelected(index.row(), index.parent());

        if (index.column() == NodeView::VISIBILITY_COL) {

            const QRect visibilityRect = visibilityClickRect(option, index);
            const bool visibilityClicked = visibilityRect.isValid() && visibilityRect.contains(mouseEvent->pos());
            if (leftButton && visibilityClicked) {
                KisBaseNode::PropertyList props = index.data(KisNodeModel::PropertiesRole).value<KisBaseNode::PropertyList>();
                OptionalProperty clickedProperty = d->findVisibilityProperty(props);
                if (!clickedProperty) return false;
                if (!marksShown && clickedProperty->state.toBool()) return false;

                d->toggleProperty(props, clickedProperty, mouseEvent->modifiers(), index);

                return true;
            }
            return false;
        } else if (index.column() == NodeView::SELECTED_COL) {
            if (leftButton && option.rect.contains(mouseEvent->pos())) {
                changeSelectionAndCurrentIndex(index);
                return true;
            }
        }

        const QRect thumbnailRect = thumbnailClickRect(option, index);
        const bool thumbnailClicked = thumbnailRect.isValid() &&
            thumbnailRect.contains(mouseEvent->pos());

        const QRect decorationRect = decorationClickRect(option, index);
        const bool decorationClicked = decorationRect.isValid() &&
            decorationRect.contains(mouseEvent->pos());

        const QRect filterRect = filterColorClickRect(option, index);
        const bool filterColorClicked =
            (index.data(KisNodeModel::FilterMaskColorRole).isNull() == false) &&
            filterRect.isValid() &&
            filterRect.contains(mouseEvent->pos());

        if (leftButton) {
            if (decorationClicked) {
                bool isExpandable = model->hasChildren(index);
                if (isExpandable) {
                    bool isExpanded = d->view->isExpanded(index);
                    d->view->setExpanded(index, !isExpanded);
                }
                return true;

            } else if (thumbnailClicked) {
                bool hasCorrectModifier = false;
                SelectionAction action = SELECTION_REPLACE;

                if (mouseEvent->modifiers() == Qt::ControlModifier) {
                    action = SELECTION_REPLACE;
                    hasCorrectModifier = true;
                } else if (mouseEvent->modifiers() == (Qt::ControlModifier | Qt::ShiftModifier)) {
                    action = SELECTION_ADD;
                    hasCorrectModifier = true;
                } else if (mouseEvent->modifiers() == (Qt::ControlModifier | Qt::AltModifier)) {
                    action = SELECTION_SUBTRACT;
                    hasCorrectModifier = true;
                } else if (mouseEvent->modifiers() == (Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier)) {
                    action = SELECTION_INTERSECT;
                    hasCorrectModifier = true;
                }

                if (hasCorrectModifier) {
                    model->setData(index, QVariant(int(action)), KisNodeModel::SelectOpaqueRole);
                } else {
                    d->view->setCurrentIndex(index);
                }
                return hasCorrectModifier; //If not here then the item is !expanded when reaching return false;

            } else if (filterColorClicked) {
                model->setData(index, QVariant() /* unused */, KisNodeModel::FilterMaskPropertiesRole);
                return true;

            } else {
                auto clickedProperty = d->propForMousePos(index, mouseEvent->pos(), option);

                if (!clickedProperty) {
                    if (altButton) {
                        d->view->setCurrentIndex(index);
                        model->setData(index, true, KisNodeModel::AlternateActiveRole);

                        return true;
                    }
                    return false;
                }

                if (!marksShown && !clickedProperty->state.toBool()) {
                    return false;
                }
                KisBaseNode::PropertyList props = index.data(KisNodeModel::PropertiesRole).value<KisBaseNode::PropertyList>();
                d->toggleProperty(props, &(*clickedProperty), mouseEvent->modifiers(), index);
                return true;
            }
        }
    }
    else if (event->type() == QEvent::ToolTip) {
        if (!KisConfig(true).hidePopups()) {
            QHelpEvent *helpEvent = static_cast<QHelpEvent*>(event);

            auto hoveredProperty = d->propForMousePos(index, helpEvent->pos(), option);
            if (hoveredProperty &&
                    (hoveredProperty->id == KisLayerPropertiesIcons::layerError.id() ||
                     hoveredProperty->id == KisLayerPropertiesIcons::layerColorSpaceMismatch.id())) {
                QToolTip::showText(helpEvent->globalPos(), hoveredProperty->state.toString(), d->view);
            } else {
                d->tip.showTip(helpEvent->pos(), option, index);
            }
        }
        return true;
    }

    return false;
}

void NodeDelegate::changeSelectionAndCurrentIndex(const QModelIndex &index)
{
    QItemSelectionModel *selectionModel = d->view->selectionModel();
    const bool wasSelected = selectionModel->isRowSelected(index.row(), index.parent());

    // if only one item is selected and that too is us then in that case we don't do anything to
    // the selection.
    if (selectionModel->selectedIndexes().size() == 1
        && selectionModel->isRowSelected(index.row(), index.parent())) {
        selectionModel->select(index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    } else {
        selectionModel->select(index, QItemSelectionModel::Toggle | QItemSelectionModel::Rows);
    }

    const auto belongToSameRow = [](const QModelIndex &a, const QModelIndex &b) {
        return a.row() == b.row() && a.parent() == b.parent();
    };

    // in this condition we move the current index to the best guessed previous one.
    if (wasSelected && belongToSameRow(selectionModel->currentIndex(), index)) {
        selectionModel->setCurrentIndex(selectionModel->selectedRows().last(), QItemSelectionModel::NoUpdate);
    }
}

QWidget *NodeDelegate::createEditor(QWidget *parent, const QStyleOptionViewItem&, const QModelIndex &index) const
{
    // #400357 do not override QAbstractItemDelegate::setEditorData to update editor's text
    // because replacing the text while user type is confusing
    const QString &text = index.data(Qt::DisplayRole).toString();
    QLineEdit *edit = new QLineEdit(text, parent);
    d->edit = edit;
    d->edit->setFocusPolicy(Qt::StrongFocus);
    // the name typed over where it is written: its font, no box beyond the quiet field's, the text where the name's is
    edit->setFrame(false);
    edit->setProperty(KisModernStyle::quietFieldProperty, true);
    edit->setTextMargins(0, 0, 0, 0);
    edit->setFont(parent->font());
    edit->selectAll();
    d->edit->installEventFilter(const_cast<NodeDelegate*>(this)); //hack?
    return d->edit;
}

void NodeDelegate::setModelData(QWidget *widget, QAbstractItemModel *model, const QModelIndex &index) const
{
    QLineEdit *edit = qobject_cast<QLineEdit*>(widget);
    Q_ASSERT(edit);

    model->setData(index, edit->text(), Qt::DisplayRole);
}

void NodeDelegate::updateEditorGeometry(QWidget *widget, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    // over the name only (not the thumbnail, the blend, the icons): its text where the name's is
    KisNodeViewColorScheme scm;
    QStyleOptionViewItem row = option;
    row.rect.setHeight(d->rowHeight);
    QRect rc = textRect(row, index).adjusted(scm.textMargin(), 0, -scm.textMargin(), 0);
    const int height = qMin(rc.height(), widget->sizeHint().height() + 2);
    rc = QRect(rc.left(), rc.top() + (rc.height() - height) / 2, rc.width(), height);
    // the line edit's own inset before its text
    const int inset = 2;
    widget->setGeometry(rc.adjusted(-inset, 0, 0, 0));
}

void NodeDelegate::toggleSolo(const QModelIndex &index) {
    KisBaseNode::PropertyList props = index.data(KisNodeModel::PropertiesRole).value<KisBaseNode::PropertyList>();
    OptionalProperty visibilityProperty = d->findVisibilityProperty(props);
    d->toggleProperty(props, visibilityProperty, Qt::ShiftModifier, index);
}


// PROTECTED


bool NodeDelegate::eventFilter(QObject *object, QEvent *event)
{
    switch (event->type()) {
    case QEvent::MouseButtonPress: {
        if (d->edit) {
            QMouseEvent *me = static_cast<QMouseEvent*>(event);
            if (!QRect(d->edit->mapToGlobal(QPoint()), d->edit->size()).contains(me->globalPos())) {
                Q_EMIT commitData(d->edit);
                Q_EMIT closeEditor(d->edit);
            }
        }
    } break;
    case QEvent::KeyPress: {
        QLineEdit *edit = qobject_cast<QLineEdit*>(object);
        if (edit && edit == d->edit) {
            QKeyEvent *ke = static_cast<QKeyEvent*>(event);
            switch (ke->key()) {
            case Qt::Key_Escape:
                Q_EMIT closeEditor(edit);
                return true;
            case Qt::Key_Tab:
                Q_EMIT commitData(edit);
                Q_EMIT closeEditor(edit, EditNextItem);
                return true;
            case Qt::Key_Backtab:
                Q_EMIT commitData(edit);
                Q_EMIT closeEditor(edit, EditPreviousItem);
                return true;
            case Qt::Key_Return:
            case Qt::Key_Enter:
                Q_EMIT commitData(edit);
                Q_EMIT closeEditor(edit);
                return true;
            default: break;
            }
        }
    } break;
    case QEvent::ShortcutOverride : {
        QLineEdit *edit = qobject_cast<QLineEdit*>(object);
        if (edit && edit == d->edit){
            auto* key = static_cast<QKeyEvent*>(event);
            if (key->modifiers() == Qt::NoModifier){
                switch (key->key()){
                case Qt::Key_Escape:
                case Qt::Key_Tab:
                case Qt::Key_Backtab:
                case Qt::Key_Return:
                case Qt::Key_Enter:
                    event->accept();
                    return true;
                default: break;
                }
            }
        }

    } break;
    case QEvent::FocusOut : {
        QLineEdit *edit = qobject_cast<QLineEdit*>(object);
        if (edit && edit == d->edit) {
            Q_EMIT commitData(edit);
            Q_EMIT closeEditor(edit);
        }
    }
    default: break;
    }

    return QAbstractItemDelegate::eventFilter(object, event);
}


// PRIVATE


QStyleOptionViewItem NodeDelegate::getOptions(const QStyleOptionViewItem &o, const QModelIndex &index)
{
    QStyleOptionViewItem option = o;
    QVariant v = index.data(Qt::FontRole);
    if (v.isValid()) {
        option.font = v.value<QFont>();
        option.fontMetrics = QFontMetrics(option.font);
    }
    v = index.data(Qt::TextAlignmentRole);
    if (v.isValid())
        option.displayAlignment = QFlag(v.toInt());
    v = index.data(Qt::ForegroundRole);
    if (v.isValid())
        option.palette.setColor(QPalette::Text, v.value<QColor>());
    v = index.data(Qt::BackgroundRole);
    if (v.isValid())
        option.palette.setColor(QPalette::Window, v.value<QColor>());

   return option;
}

void NodeDelegate::drawProgressBar(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    QVariant value = index.data(KisNodeModel::ProgressRole);
    if (!value.isNull() && (value.toInt() >= 0 && value.toInt() <= 100)) {

        /// The progress bar will display under the layer name area. The bars have accurate data, so we
        /// probably don't need to also show the actual number for % complete

        KisNodeViewColorScheme scm;

        const QRect thumbnailRect = thumbnailClickRect(option, index);
        const QRect iconsRectR    = iconsRect(option, index);
        const int height = 5;
        const QRect rc = QRect(
            ((option.direction == Qt::RightToLeft) ?
              iconsRectR.bottomRight() :
              thumbnailRect.bottomRight()) - QPoint(0, height),
            ((option.direction == Qt::RightToLeft) ?
              thumbnailRect.bottomLeft() :
              iconsRectR.bottomLeft()));

        p->save();
        {
            p->setClipRect(rc);
            QStyle* style = QApplication::style();
            QStyleOptionProgressBar opt;

            opt.rect = rc;
            opt.minimum = 0;
            opt.maximum = 100;
            opt.progress = value.toInt();
            opt.textVisible = false;
            opt.textAlignment = Qt::AlignHCenter;
            opt.text = i18n("%1 %", opt.progress);
            opt.state = option.state;
            style->drawControl(QStyle::CE_ProgressBar, &opt, p, 0);
        }
        p->restore();
    }
}

void NodeDelegate::slotConfigChanged()
{
    KisConfig cfg(true);
    const int oldHeight = d->rowHeight;
    // cache values that require a config lookup and get used frequently
    d->thumbnailSize = KisNodeViewColorScheme::instance()->thumbnailSize();
    d->thumbnailGeometry = KisNodeViewColorScheme::instance()->relThumbnailRect();
    d->rowHeight = KisNodeViewColorScheme::instance()->rowHeight();

    const QColor newCheckersColor1 = cfg.checkersColor1();
    const QColor newCheckersColor2 = cfg.checkersColor2();

    // generate the checker backdrop for thumbnails
    const int step = d->thumbnailSize / 6;
    if ((d->checkers.width() != 2 * step) ||
        (d->checkersColor1 != newCheckersColor1) ||
        (d->checkersColor2 != newCheckersColor2)) {

        d->checkersColor1 = newCheckersColor1;
        d->checkersColor2 = newCheckersColor2;
        d->checkers = QImage(2 * step, 2 * step, QImage::Format_ARGB32);

        QPainter gc(&d->checkers);
        gc.fillRect(QRect(0, 0, step, step), newCheckersColor1);
        gc.fillRect(QRect(step, 0, step, step), newCheckersColor2);
        gc.fillRect(QRect(step, step, step, step), newCheckersColor1);
        gc.fillRect(QRect(0, step, step, step), newCheckersColor2);
    }

    if (d->rowHeight != oldHeight) {
        // QAbstractItemView/QTreeView don't even look at the index and redo the whole layout...
        Q_EMIT sizeHintChanged(QModelIndex());
    }
}

void NodeDelegate::slotUpdateIcon()
{
   KisLayerPropertiesIcons::instance()->updateIcons();
}

void NodeDelegate::slotResetState(){

    NodeView *view = d->view;
    QModelIndex root = view->rootIndex();
    int childs = view->model()->rowCount(root);
    if (childs > 0){
        QModelIndex firstChild = view->model()->index(0, 0, root);
        KisBaseNode::PropertyList props = firstChild.data(KisNodeModel::PropertiesRole).value<KisBaseNode::PropertyList>();

        OptionalProperty visibilityProperty = d->findVisibilityProperty(props);
        if(d->stasisIsDirty(root, visibilityProperty)){ // clean inStasis if mixed!
            d->resetPropertyStateRecursive(root, visibilityProperty);
        }
    }
}

QRect NodeDelegate::filterColorClickRect(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    KisNodeViewColorScheme scm;
    const QRect icons = iconsRect(option, index);

    // Filter masks have two unused properties/icons,
    // and we can use that space to draw the filter's selected color.

    QRect rc;
    const int dx = scm.iconSize() + scm.iconMargin();
    if (option.direction == Qt::RightToLeft) {
        // The free space is at the beginning on the left.
        rc.setRect(0, 0, icons.width() - dx, icons.height());
    } else {
        // The free space is at the end on the right.
        rc.setRect(dx, 0, icons.width() - dx, icons.height());
    }
    rc = rc.marginsRemoved(QMargins(8, 10, 8, 10));

    rc.translate(icons.x(), icons.y());

    return rc;
}
