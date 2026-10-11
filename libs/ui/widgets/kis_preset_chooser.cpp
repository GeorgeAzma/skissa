/*
 *  SPDX-FileCopyrightText: 2002 Patrick Julien <freak@codepimps.org>
 *  SPDX-FileCopyrightText: 2009 Sven Langkamp <sven.langkamp@gmail.com>
 *  SPDX-FileCopyrightText: 2011 Silvio Heinrich <plassy@web.de>
 *  SPDX-FileCopyrightText: 2011 Srikanth Tiyyagura <srikanth.tulasiram@gmail.com>
 *  SPDX-FileCopyrightText: 2011 José Luis Vergara <pentalis@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "kis_preset_chooser.h"

#include <QVBoxLayout>
#include <QPainter>
#include <QAbstractItemDelegate>
#include <QStyleOptionViewItem>
#include <QSortFilterProxyModel>
#include <KisResourceModel.h>
#include <QApplication>

#include <kis_config.h>
#include <klocalizedstring.h>
#include <KisKineticScroller.h>

#include <KoIcon.h>
#include <KisResourceItemChooser.h>
#include <KisResourceItemChooserSync.h>
#include <KisResourceItemListView.h>
#include <KisResourceLocator.h>
#include <KisResourceTypes.h>

#include <brushengine/kis_paintop_settings.h>
#include <brushengine/kis_paintop_preset.h>
#include "kis_config_notifier.h"
#include <kis_icon.h>
#include <KisResourceModelProvider.h>
#include <KisTagFilterResourceProxyModel.h>
#include <KisResourceThumbnailCache.h>
#include <KisResourceMetaDataModel.h>
#include <KisModernStyle.h>
#include "KisPresetStrokeThumbnails.h"


/// The resource item delegate for rendering the resource preview
class KisPresetDelegate : public QAbstractItemDelegate
{
public:
    KisPresetDelegate(QObject * parent = 0)
        : QAbstractItemDelegate(parent)
        , m_showText(false)
        , m_useDirtyPresets(false) {}

    ~KisPresetDelegate() override {}

    /// reimplemented
    void paint(QPainter *, const QStyleOptionViewItem &, const QModelIndex &) const override;

    /// reimplemented
    QSize sizeHint(const QStyleOptionViewItem & option, const QModelIndex &) const override {
        return option.decorationSize;
    }

    void setShowText(bool showText) {
        m_showText = showText;
    }

    void setUseDirtyPresets(bool value) {
        m_useDirtyPresets = value;
    }

    /// In rows, what each brush paints rather than its picture
    void setStrokes(bool strokes) {
        m_strokes = strokes;
    }

    bool strokes() const {
        return m_strokes;
    }

private:
    bool m_strokes {false};
    bool m_showText;
    bool m_useDirtyPresets;
};

void KisPresetDelegate::paint(QPainter * painter, const QStyleOptionViewItem & option, const QModelIndex & index) const
{
    if (!index.isValid()) {
        return;
    }
    painter->save();
    painter->setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter->setRenderHint(QPainter::Antialiasing, true);
    if (!(option.state & QStyle::State_Enabled)) {
        painter->setOpacity(0.2);
    }

    const bool dirty = m_useDirtyPresets && index.data(Qt::UserRole + KisAbstractResourceModel::Dirty).toBool();
    const bool selected = option.state & QStyle::State_Selected;
    const bool hovered = option.state & QStyle::State_MouseOver;
    const qreal devicePixelRatioF = painter->device()->devicePixelRatioF();
    const QColor accent = option.palette.color(QPalette::Highlight);
    const QColor ink = option.palette.color(QPalette::Text);

    /**
     * Each brush a rounded tile with room around it, the chosen one ringed
     * in the accent color (a fill over the picture would change its colors).
     */
    auto ring = [&](const QRectF &tile, qreal radius) {
        if (selected) {
            // in the text's color (white on a dark panel): seen at a glance whatever the picture's colors
            painter->setPen(QPen(ink, 2.25));
            painter->setBrush(Qt::NoBrush);
            painter->drawPath(KisModernStyle::squircle(tile.adjusted(-2.5, -2.5, 2.5, 2.5), radius + 2.5));
        } else if (hovered) {
            QColor line = ink;
            line.setAlphaF(0.3);
            painter->setPen(QPen(line, 1));
            painter->setBrush(Qt::NoBrush);
            painter->drawPath(KisModernStyle::squircle(tile.adjusted(-1.5, -1.5, 1.5, 1.5), radius + 1.5));
        }
    };
    auto picture = [&](const QRectF &tile, qreal radius) {
        const QSize pixels = (tile.size() * devicePixelRatioF).toSize();
        QImage preview = KisResourceThumbnailCache::instance()->getImage(index, pixels, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        painter->save();
        painter->setClipPath(KisModernStyle::squircle(tile, radius));
        if (preview.isNull()) {
            QColor empty = ink;
            empty.setAlphaF(0.08);
            painter->fillRect(tile, empty);
        } else {
            preview.setDevicePixelRatio(devicePixelRatioF);
            painter->drawImage(tile.topLeft(), preview);
        }
        painter->restore();
        // a hairline, so that a light picture keeps its edge on a light panel
        QColor edge = ink;
        edge.setAlphaF(0.12);
        painter->setPen(QPen(edge, 1));
        painter->setBrush(Qt::NoBrush);
        painter->drawPath(KisModernStyle::squircle(tile.adjusted(0.5, 0.5, -0.5, -0.5), radius - 0.5));
    };
    auto marks = [&](const QRectF &tile) {
        // changed and not saved: a dot in the corner
        if (dirty) {
            const QPointF dot = tile.topRight() + QPointF(-6, 6);
            painter->setPen(QPen(option.palette.color(QPalette::Base), 1.5));
            painter->setBrush(accent);
            painter->drawEllipse(dot, 3.5, 3.5);
        }
        if (index.data(Qt::UserRole + KisAbstractResourceModel::BrokenStatus).toBool()) {
            const QIcon icon = KisIconUtils::loadIcon("broken-preset");
            const int size = qMin(20, int(tile.height() / 2));
            icon.paint(painter, QRect(int(tile.right()) - size - 2, int(tile.bottom()) - size - 2, size, size));
        }
    };

    if (!m_showText) {
        const QRectF tile = QRectF(option.rect).adjusted(3, 3, -3, -3);
        const qreal radius = qMax<qreal>(4.0, tile.width() * 0.16);
        picture(tile, radius);
        ring(tile, radius);
        marks(tile);
        painter->restore();
        return;
    }

    // a row: the name, and what the brush paints (or its picture)
    const QRectF row = QRectF(option.rect).adjusted(3, 2, -3, -2);
    const qreal radius = 8;
    if (selected || hovered) {
        QColor fill = selected ? accent : ink;
        fill.setAlphaF(selected ? 0.22 : 0.06);
        painter->setPen(Qt::NoPen);
        painter->setBrush(fill);
        painter->drawPath(KisModernStyle::squircle(row, radius));
    }

    const QRectF art = row.adjusted(row.width() * 0.42, 2, -4, -2);
    QImage stroke;
    if (m_strokes) {
        QColor strokeInk = ink;
        strokeInk.setAlpha(255);
        stroke = KisPresetStrokeThumbnails::instance()->stroke(index.data(Qt::UserRole + KisAbstractResourceModel::Id).toInt(),
                                                               index.data(Qt::UserRole + KisAbstractResourceModel::MD5).toString(),
                                                               strokeInk);
    }
    if (!stroke.isNull()) {
        const QSizeF fitted = QSizeF(stroke.size()).scaled(art.size(), Qt::KeepAspectRatio);
        const QRectF target(art.center() - QPointF(fitted.width(), fitted.height()) / 2, fitted);
        painter->drawImage(target, stroke);
        if (dirty) {
            marks(QRectF(row.right() - 12, row.top(), 12, 12));
        }
    } else {
        const qreal side = qMin(art.height(), row.height() - 8);
        const QRectF tile(art.right() - side, row.center().y() - side / 2, side, side);
        picture(tile, qMax<qreal>(4.0, side * 0.18));
        marks(tile);
    }

    const QString name = index.data(Qt::UserRole + KisAbstractResourceModel::Name).toString().replace("_", " ");
    painter->setFont(option.font);
    QColor text = ink;
    text.setAlphaF(selected ? 1.0 : 0.82);
    painter->setPen(text);
    const QRectF textRect = QRectF(row.left() + 8, row.top(), row.width() * 0.42 - 10, row.height());
    painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextWordWrap, name);
    painter->restore();
}

KisPresetChooser::KisPresetChooser(QWidget *parent)
    : QWidget(parent)
{
    setObjectName("KisPresetChooser");

    QVBoxLayout * layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    m_chooser = new KisResourceItemChooser(ResourceType::PaintOpPresets, false, this);
    m_chooser->setRowHeight(50);
    m_delegate = new KisPresetDelegate(this);
    m_chooser->setItemDelegate(m_delegate);
    m_chooser->setSynced(true);
    m_chooser->showImportExportBtns(false);
    layout->addWidget(m_chooser);

    connect(m_chooser, SIGNAL(resourceSelected(KoResourceSP )),
            this, SLOT(slotResourceWasSelected(KoResourceSP )));

    connect(m_chooser, SIGNAL(resourceSelected(KoResourceSP )),
            this, SIGNAL(resourceSelected(KoResourceSP )));
    connect(m_chooser, SIGNAL(resourceClicked(KoResourceSP )),
            this, SIGNAL(resourceClicked(KoResourceSP )));

    connect(m_chooser, &KisResourceItemChooser::listViewModeChanged, this, &KisPresetChooser::showHideBrushNames);

    m_mode = ViewMode::THUMBNAIL;

    // a stroke made later: shown when it is ready
    connect(KisPresetStrokeThumbnails::instance(), &KisPresetStrokeThumbnails::changed, this, [this]() {
        if (m_delegate->strokes()) m_chooser->itemView()->viewport()->update();
    });

    connect(KisConfigNotifier::instance(), SIGNAL(configChanged()),
            SLOT(notifyConfigChanged()));

    // the size the thumbnails were zoomed to (Ctrl+wheel), kept as it changes: for the next run too
    setIconSize(KisConfig(true).presetIconSize());
    connect(KisResourceItemChooserSync::instance(), &KisResourceItemChooserSync::baseLengthChanged, this, [this]() {
        saveIconSize();
    });

    notifyConfigChanged();
}

KisPresetChooser::~KisPresetChooser()
{
}

void KisPresetChooser::setViewMode(KisPresetChooser::ViewMode mode)
{
    m_mode = mode;
    updateViewSettings();
}

void KisPresetChooser::setViewModeToThumbnail()
{
    setViewMode(KisPresetChooser::ViewMode::THUMBNAIL);
}

void KisPresetChooser::setViewModeToDetail()
{
    setViewMode(KisPresetChooser::ViewMode::DETAIL);
}

void KisPresetChooser::notifyConfigChanged()
{
    KisConfig cfg(true);
    m_delegate->setUseDirtyPresets(cfg.useDirtyPresets());
    if (m_delegate->strokes() != KisPresetStrokeThumbnails::isEnabled()) {
        updateViewSettings();
    }
}

void KisPresetChooser::slotResourceWasSelected(KoResourceSP resource)
{
    m_currentPresetConnections.clear();
    if (!resource) return;

    KisPaintOpPresetSP preset = resource.dynamicCast<KisPaintOpPreset>();
    KIS_SAFE_ASSERT_RECOVER_RETURN(preset);

    m_currentPresetConnections.addUniqueConnection(
        preset->updateProxy(), SIGNAL(sigSettingsChanged()),
        this, SLOT(slotCurrentPresetChanged()));
}

void KisPresetChooser::slotCurrentPresetChanged()
{
    KoResourceSP currentResource = m_chooser->currentResource();
    if (!currentResource) return;

    QModelIndex index = m_chooser->tagFilterModel()->indexForResource(currentResource);
    Q_EMIT m_chooser->tagFilterModel()->dataChanged(index,
                                               index,
                                               {Qt::UserRole + KisAbstractResourceModel::Thumbnail});
}

void KisPresetChooser::updateViewSettings()
{
    // brushes as strokes: in rows, the stroke beside the name
    const bool strokes = KisPresetStrokeThumbnails::isEnabled();
    m_delegate->setStrokes(strokes);
    if (strokes) {
        m_chooser->setListViewMode(ListViewMode::Detail);
        m_delegate->setShowText(true);
        return;
    }
    switch (m_mode) {
    case ViewMode::THUMBNAIL: {
        m_chooser->setListViewMode(ListViewMode::IconGrid);
        m_delegate->setShowText(false);
        break;
    }
    case ViewMode::DETAIL: {
        m_chooser->setListViewMode(ListViewMode::Detail);
        m_delegate->setShowText(true);
        break;
    }
    }
}

void KisPresetChooser::setCurrentResource(KoResourceSP resource)
{
    m_chooser->setCurrentResource(resource);
}

KoResourceSP KisPresetChooser::currentResource() const
{
    return m_chooser->currentResource();
}

void KisPresetChooser::showTaggingBar(bool show)
{
    m_chooser->showTaggingBar(show);
}

KisResourceItemChooser *KisPresetChooser::itemChooser()
{
    return m_chooser;
}

void KisPresetChooser::setPresetFilter(const QString& paintOpId)
{
    QMap<QString, QVariant> metaDataFilter;
    if (!paintOpId.isEmpty()) { // empty means "all"
        metaDataFilter["paintopid"] = paintOpId;
    }
    m_chooser->tagFilterModel()->setMetaDataFilter(metaDataFilter);
    updateViewSettings();
}

void KisPresetChooser::setIconSize(int newSize)
{
    KisResourceItemChooserSync* chooserSync = KisResourceItemChooserSync::instance();
    chooserSync->setBaseLength(newSize);
}

int KisPresetChooser::iconSize()
{
    KisResourceItemChooserSync* chooserSync = KisResourceItemChooserSync::instance();
    return chooserSync->baseLength();
}

void KisPresetChooser::saveIconSize()
{
    // save icon size
    if (KisConfig(true).presetIconSize() != iconSize()) {
        KisConfig(false).setPresetIconSize(iconSize());
    }
}

void KisPresetChooser::showHideBrushNames(ListViewMode newViewMode)
{
    switch (newViewMode) {
    case ListViewMode::Detail: {
        m_delegate->setShowText(true);
        break;
    }
    default: {
        m_delegate->setShowText(false);
    }
    }
}
