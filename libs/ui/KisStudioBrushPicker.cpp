/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioBrushPicker.h"
#include <climits>
#include <KisResourceOrder.h>
#include "KisStudioTools.h"

#include <QApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>

#include <algorithm>

#include <KisResourceModel.h>
#include <KisResourceModelProvider.h>
#include <KisResourceTypes.h>
#include <KisTagFilterResourceProxyModel.h>
#include <KisTagModel.h>
#include <KisTagResourceModel.h>
#include <klocalizedstring.h>

#include "KisMainWindow.h"
#include "KisSettingsRegistry.h"
#include "KisResourceServerProvider.h"
#include "KisViewManager.h"
#include "kis_paintop_box.h"
#include "kis_canvas_resource_provider.h"
#include "kis_config.h"
#include "kis_paintop_preset.h"
#include "widgets/KisPresetStrokeThumbnails.h"
#include <KisModernStyle.h>

/**
 * The brushes the tiles show, and the ink their strokes are painted in.
 */
struct KisStudioBrushStrokes {
    struct Brush {
        QString name;
        QString md5;
        int resourceId {-1};
        QString title;
        QString engine;
        /// For the order the brushes were dragged into
        QString file;
        QStringList tags;
        QPersistentModelIndex index;    ///< in the catalog's model
    };

    struct Catalog {
        QStringList ids;                ///< sorted by title
        QHash<QString, Brush> brushes;
        /// Krita's tags in the order the brush panel lists them, each with the brushes it is the first tag of
        QVector<KisStudioPicker::Group> tagGroups;
        /// the brushes without a tag, sorted by title
        QStringList untagged;
        /// the presets, kept for their icons: decoding them all took most of the loading
        std::shared_ptr<KisResourceModel> model;
        QHash<QString, QImage> icons;

        QImage icon(const QString &id)
        {
            auto it = icons.constFind(id);
            if (it != icons.constEnd()) return *it;
            const QModelIndex index = brushes.value(id).index;
            const QImage image = index.isValid() ? index.data(Qt::UserRole + KisAbstractResourceModel::Thumbnail).value<QImage>() : QImage();
            icons.insert(id, image);
            return image;
        }
    };

    std::shared_ptr<Catalog> catalog;
    /// the text's color, as the brush lists paint strokes in: they read on the panel on any theme
    QColor ink;
};

namespace {

using Group = KisStudioPicker::Group;
using Brush = KisStudioBrushStrokes::Brush;
using Catalog = KisStudioBrushStrokes::Catalog;

const QString pinnedKey = QStringLiteral("StudioBrushes");
const QString recentKey = QStringLiteral("StudioRecentBrushes");
constexpr int recentKept = 24;

/// Puts each brush under the first of its tags, the tags in the order the brush panel's tag chooser lists them
void groupByTags(Catalog &catalog, const QHash<int, QString> &nameOfId)
{
    QHash<int, int> groupOfTag;
    KisTagModel tags(ResourceType::PaintOpPresets);
    for (int row = 0; row < tags.rowCount(); row++) {
        const QModelIndex index = tags.index(row, 0);
        const int id = index.data(Qt::UserRole + KisAllTagsModel::Id).toInt();
        // All and All untagged
        if (id < 0) continue;
        groupOfTag.insert(id, catalog.tagGroups.size());
        catalog.tagGroups << Group{index.data(Qt::UserRole + KisAllTagsModel::Name).toString(), {},
                                   index.data(Qt::UserRole + KisAllTagsModel::Url).toString()};
    }

    QHash<QString, int> firstGroup;
    KisTagResourceModel tagged(ResourceType::PaintOpPresets);
    for (int row = 0; row < tagged.rowCount(); row++) {
        const QModelIndex index = tagged.index(row, 0);
        const auto group = groupOfTag.constFind(index.data(Qt::UserRole + KisAllTagResourceModel::TagId).toInt());
        const QString name = nameOfId.value(index.data(Qt::UserRole + KisAllTagResourceModel::ResourceId).toInt());
        if (group == groupOfTag.constEnd() || name.isEmpty()) continue;
        auto first = firstGroup.find(name);
        if (first == firstGroup.end()) {
            firstGroup.insert(name, *group);
        } else {
            *first = qMin(*first, *group);
        }
    }

    // the brushes put away from the list stay out of Other too; search still finds them
    const QStringList awayFiles = KisResourceOrder::putAway(ResourceType::PaintOpPresets);
    const QSet<QString> away(awayFiles.begin(), awayFiles.end());
    Q_FOREACH (const QString &id, catalog.ids) {
        auto first = firstGroup.constFind(id);
        if (first != firstGroup.constEnd()) {
            catalog.tagGroups[*first].ids << id;
        } else if (!away.contains(catalog.brushes.value(id).file)) {
            catalog.untagged << id;
        }
    }
    catalog.tagGroups.erase(std::remove_if(catalog.tagGroups.begin(), catalog.tagGroups.end(),
                                           [](const Group &group) { return group.ids.isEmpty(); }),
                            catalog.tagGroups.end());
}

std::shared_ptr<Catalog> loadCatalog()
{
    auto catalog = std::make_shared<Catalog>();
    catalog->model = std::make_shared<KisResourceModel>(ResourceType::PaintOpPresets);
    const KisResourceModel &model = *catalog->model;
    // every preset's name, those of a name already taken too: their tags count for the brush of that name
    QHash<int, QString> nameOfId;
    for (int row = 0; row < model.rowCount(); row++) {
        const QModelIndex index = model.index(row, 0);
        Brush brush;
        brush.name = index.data(Qt::UserRole + KisAbstractResourceModel::Name).toString();
        brush.resourceId = index.data(Qt::UserRole + KisAbstractResourceModel::Id).toInt();
        if (!brush.name.isEmpty()) nameOfId.insert(brush.resourceId, brush.name);
        if (brush.name.isEmpty() || catalog->brushes.contains(brush.name)) {
            continue;
        }
        brush.md5 = index.data(Qt::UserRole + KisAbstractResourceModel::MD5).toString();
        brush.title = KisStudioBrushPicker::title(brush.name).replace(QLatin1Char('_'), QLatin1Char(' '));
        brush.tags = index.data(Qt::UserRole + KisAbstractResourceModel::Tags).toStringList();
        brush.engine = index.data(Qt::UserRole + KisAbstractResourceModel::MetaData).toMap().value(QStringLiteral("paintopid")).toString();
        brush.index = index;
        brush.file = index.data(Qt::UserRole + KisAbstractResourceModel::Filename).toString();
        catalog->ids << brush.name;
        catalog->brushes.insert(brush.name, brush);
    }
    // in the order the brushes were dragged into in the list (All's), the rest after by name
    QHash<QString, int> order;
    const QStringList ordered = KisResourceOrder::inTag(ResourceType::PaintOpPresets, KisAllTagsModel::urlAll());
    for (int i = 0; i < ordered.size(); i++) order.insert(ordered[i], i);
    std::sort(catalog->ids.begin(), catalog->ids.end(), [catalog, &order](const QString &a, const QString &b) {
        const int left = order.value(catalog->brushes.value(a).file, INT_MAX), right = order.value(catalog->brushes.value(b).file, INT_MAX);
        if (left != right) return left < right;
        return catalog->brushes.value(a).title.localeAwareCompare(catalog->brushes.value(b).title) < 0;
    });
    groupByTags(*catalog, nameOfId);
    return catalog;
}

/// Whether the presets from \p top to \p bottom (in all the presets) are in \p catalog as they are now
bool unchangedIn(const Catalog &catalog, const QModelIndex &top, const QModelIndex &bottom)
{
    for (int row = top.row(); row <= bottom.row(); row++) {
        const QModelIndex index = top.sibling(row, 0);
        auto it = catalog.brushes.constFind(index.data(Qt::UserRole + KisAbstractResourceModel::Name).toString());
        if (it == catalog.brushes.constEnd() || !catalog.model || it->index != catalog.model->mapFromSource(index)
            || it->md5 != index.data(Qt::UserRole + KisAbstractResourceModel::MD5).toString()
            || it->tags != index.data(Qt::UserRole + KisAbstractResourceModel::Tags).toStringList()
            || it->engine != index.data(Qt::UserRole + KisAbstractResourceModel::MetaData).toMap().value(QStringLiteral("paintopid")).toString()) {
            return false;
        }
    }
    return true;
}

/// Cached until the presets or their tags change, since the canvas ring and the brush wheel need it instantly
std::shared_ptr<Catalog> catalog()
{
    static std::shared_ptr<Catalog> cached;
    static bool watching = false;
    if (!watching) {
        watching = true;
        const auto drop = []() { cached.reset(); };
        QAbstractItemModel *presets = KisResourceModelProvider::resourceModel(ResourceType::PaintOpPresets);
        const QList<QAbstractItemModel *> models{presets, KisResourceModelProvider::tagResourceModel(ResourceType::PaintOpPresets),
                                                 KisResourceModelProvider::tagModel(ResourceType::PaintOpPresets)};
        Q_FOREACH (QAbstractItemModel *model, models) {
            if (!model) continue;
            QObject::connect(model, &QAbstractItemModel::modelReset, qApp, drop);
            QObject::connect(model, &QAbstractItemModel::layoutChanged, qApp, drop);
            QObject::connect(model, &QAbstractItemModel::rowsInserted, qApp, drop);
            QObject::connect(model, &QAbstractItemModel::rowsRemoved, qApp, drop);
            if (model != presets) QObject::connect(model, &QAbstractItemModel::dataChanged, qApp, drop);
        }
        QObject::connect(KisResourceOrder::instance(), &KisResourceOrder::changed, qApp, drop);
        if (presets) {
            // a brush read again from its file (switching brushes, Reload) leaves it as the catalog has it
            QObject::connect(presets, &QAbstractItemModel::dataChanged, qApp,
                             [](const QModelIndex &top, const QModelIndex &bottom, const QVector<int> &roles) {
                                 if (cached && roles.isEmpty() && top.isValid() && bottom.isValid() && unchangedIn(*cached, top, bottom)) return;
                                 cached.reset();
                             });
        }
    }
    if (!cached) cached = loadCatalog();
    return cached;
}

/// The pop-up palette's favorites, or a default set covering each kind of work
QStringList initialBrushes()
{
    QStringList names;
    const QString url = KisConfig(true).readEntry<QString>(QStringLiteral("favoritePresetsTag"), QStringLiteral("★ My Favorites"));
    KisTagModel tags(ResourceType::PaintOpPresets);
    KisTagSP tag = tags.tagForUrl(url);
    if (tag) {
        KisTagFilterResourceProxyModel tagged(ResourceType::PaintOpPresets);
        tagged.setTagFilter(tag);
        tagged.sort(KisAbstractResourceModel::Name);
        for (int row = 0; row < tagged.rowCount() && names.size() < 16; row++) {
            names << tagged.index(row, 0).data(Qt::UserRole + KisAbstractResourceModel::Name).toString();
        }
    }
    if (names.isEmpty()) {
        names = QStringList{
            QStringLiteral("c) Pencil-2"), QStringLiteral("d) Ink-2 Fineliner"), QStringLiteral("b) Basic-5 Size Opacity"),
            QStringLiteral("f) Bristles-2 Flat Rough"), QStringLiteral("b) Airbrush Soft"), QStringLiteral("k) Blender Basic"),
            QStringLiteral("m) Texture Big"), QStringLiteral("a) Eraser Soft"),
        };
    }
    return names;
}

std::shared_ptr<KisStudioBrushStrokes> makeStrokes()
{
    auto strokes = std::make_shared<KisStudioBrushStrokes>();
    strokes->catalog = catalog();
    return strokes;
}

/// The stroke the brush lists show for \p id in \p ink: null while it is painted, and for a brush that paints nothing alone
QImage strokeOf(const Catalog &catalog, const QString &id, const QColor &ink)
{
    const auto brush = catalog.brushes.constFind(id);
    if (brush == catalog.brushes.constEnd()) return QImage();
    return KisPresetStrokeThumbnails::instance()->stroke(brush->resourceId, brush->md5, ink);
}

/// \p stroke's painted part at \p size, kept for the tiles and cells on screen
QImage strokeAt(const QImage &stroke, const QSize &size)
{
    if (size.isEmpty()) return QImage();
    static QHash<QString, QImage> made;
    // the one used longest ago first
    static QList<QString> used;
    static qint64 bytes = 0;
    const QString key = QStringLiteral("%1 %2x%3").arg(stroke.cacheKey()).arg(size.width()).arg(size.height());
    auto it = made.constFind(key);
    if (it != made.constEnd()) {
        if (used.last() != key) {
            used.removeOne(key);
            used.append(key);
        }
        return *it;
    }
    const QImage scaled = stroke.copy(KisPresetStrokeThumbnails::paintedPart(stroke)).scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    while (bytes > (16 << 20) && !used.isEmpty()) bytes -= made.take(used.takeFirst()).sizeInBytes();
    made.insert(key, scaled);
    used.append(key);
    bytes += scaled.sizeInBytes();
    return scaled;
}

/// \p stroke as the brush lists show it: what it painted, as large as \p area lets it be, in its middle
void drawStroke(QPainter &painter, const QRectF &area, const QImage &stroke)
{
    const QSizeF fitted = QSizeF(KisPresetStrokeThumbnails::paintedPart(stroke).size()).scaled(area.size(), Qt::KeepAspectRatio);
    const QRectF place(area.center() - QPointF(fitted.width(), fitted.height()) / 2, fitted);
    painter.drawImage(place, strokeAt(stroke, (place.size() * painter.device()->devicePixelRatioF()).toSize()));
}

/// @param strokes the tiles' strokes; null for a source that is never displayed
KisStudioPicker::Source brushSource(std::shared_ptr<Catalog> catalog, std::shared_ptr<KisStudioBrushStrokes> strokes)
{
    KisStudioPicker::Source source;
    source.pinnedKey = pinnedKey;
    source.initialPinned = initialBrushes;
    source.groupsKey = QStringLiteral("StudioBrushGroups");
    source.dragKey = QStringLiteral("brush:");
    // Krita's tags, as groups of the user's own
    source.initialGroups = [catalog]() { return catalog->tagGroups; };
    source.allIds = [catalog]() { return catalog->ids; };
    source.otherGroups = [catalog](const QStringList &pinned) {
        const QSet<QString> skipped(pinned.begin(), pinned.end());
        const auto notSkipped = [&skipped](QStringList ids) {
            ids.erase(std::remove_if(ids.begin(), ids.end(), [&skipped](const QString &id) { return skipped.contains(id); }), ids.end());
            return ids;
        };
        QVector<Group> groups;
        Q_FOREACH (const Group &tag, catalog->tagGroups) {
            const QStringList ids = notSkipped(tag.ids);
            if (!ids.isEmpty()) groups << Group{tag.title, ids, tag.key};
        }
        const QStringList untagged = notSkipped(catalog->untagged);
        if (!untagged.isEmpty()) groups << Group{i18nc("picker group of what is in no other", "Other"), untagged, QString()};
        return groups;
    };
    source.name = [catalog](const QString &id) { return catalog->brushes.value(id).title; };
    source.aliases = [catalog](const QString &id) {
        const Brush brush = catalog->brushes.value(id);
        QStringList aliases = brush.tags;
        aliases << brush.engine;
        return aliases;
    };
    if (strokes) {
        source.paintPreview = [catalog, strokes](QPainter &painter, const QRect &rect, const QString &id) {
            Q_UNUSED(strokes);
            // the brush's own picture: a small square says which brush at a glance, where a stroke needs a wide
            // tile and left most of the picker empty
            const QImage icon = catalog->icon(id);
            if (icon.isNull()) return;
            const int side = rect.height() - 2 * KisModernStyle::space1;
            const QRectF tile(rect.center().x() - side / 2, rect.top() + KisModernStyle::space1, side, side);
            painter.save();
            painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
            painter.setClipPath(KisModernStyle::squircle(tile, qMax<qreal>(4.0, side * 0.16)), Qt::IntersectClip);
            painter.drawImage(tile, icon);
            painter.restore();
        };
    }
    source.tiles = true;
    // a picture and its name under it
    source.tileSize = QSize(76, 92);
    source.yoursTitle = i18nc("brush group", "Favorites");
    source.searchPlaceholder = i18nc("@info:placeholder", "Search %1 brushes", catalog->ids.size());
    return source;
}

QString currentPresetName(KisMainWindow *mainWindow)
{
    KisViewManager *view = mainWindow->viewManager();
    KisCanvasResourceProvider *resources = view ? view->canvasResourceProvider() : nullptr;
    return resources && resources->currentPreset() ? resources->currentPreset()->name() : QString();
}

} // namespace


KisStudioBrushPicker::KisStudioBrushPicker(KisMainWindow *mainWindow)
    : KisStudioBrushPicker(mainWindow, makeStrokes())
{
}

KisStudioBrushPicker::KisStudioBrushPicker(KisMainWindow *mainWindow, std::shared_ptr<KisStudioBrushStrokes> strokes)
    : KisStudioPicker(brushSource(strokes->catalog, strokes), currentPresetName(mainWindow), QSet<QString>(), mainWindow)
{
    // the brush lists' ink, on the panel as theirs is
    strokes->ink = palette().color(QPalette::Text);
    strokes->ink.setAlpha(255);

    connect(this, &KisStudioPicker::sigPicked, this, [mainWindow](const QString &id) {
        recordUse(id);
        use(mainWindow->viewManager(), id);
    });

    // brushes as their pictures: no strokes to wait for
    if (!KisPresetStrokeThumbnails::isEnabled()) return;

    // once for the strokes finished within a moment of each other: they arrive one by one
    auto *strokesShown = new QTimer(this);
    strokesShown->setSingleShot(true);
    strokesShown->setInterval(50);
    connect(strokesShown, &QTimer::timeout, this, &KisStudioPicker::updatePreviews);
    connect(KisPresetStrokeThumbnails::instance(), &KisPresetStrokeThumbnails::changed, strokesShown, [strokesShown]() {
        if (!strokesShown->isActive()) strokesShown->start();
    });

}

QStringList KisStudioBrushPicker::recentBrushes()
{
    return KisConfig(true).readEntry<QStringList>(recentKey, QStringList());
}

void KisStudioBrushPicker::recordUse(const QString &presetName)
{
    QStringList recent = KisConfig(true).readEntry<QStringList>(recentKey, QStringList());
    if (!recent.isEmpty() && recent.first() == presetName) {
        return;
    }
    recent.removeAll(presetName);
    recent.prepend(presetName);
    // saved with the next sync: every brush switch comes here, often many in a row
    KisSettingsRegistry::writeLater(recentKey, recent.mid(0, recentKept));
}

QString KisStudioBrushPicker::neighbour(KisMainWindow *, const QString &current, int step)
{
    const QStringList yours = pinnedIds(brushSource(catalog(), nullptr));
    if (yours.isEmpty()) {
        return QString();
    }
    const int index = yours.indexOf(current);
    if (index < 0) {
        // Not pinned: enter the list from its start or end
        return step > 0 ? yours.first() : yours.last();
    }
    const int to = index + step;
    return to >= 0 && to < yours.size() ? yours[to] : QString();
}

void KisStudioBrushPicker::use(KisViewManager *view, const QString &presetName)
{
    KisCanvasResourceProvider *resources = view ? view->canvasResourceProvider() : nullptr;
    if (!resources) {
        return;
    }
    KisPaintOpPresetSP preset = KisResourceServerProvider::instance()->paintOpPresetServer()->resource(QString(), QString(), presetName);
    if (!preset) return;
    // through the paintop box, as Krita's brush panel does: set only as the brush in use, one of another engine
    // kept the last engine's settings in the editors
    if (KisPaintopBox *box = view->paintOpBox()) {
        box->resourceSelected(preset);
    } else {
        resources->setPaintOpPreset(preset);
    }
}

QImage KisStudioBrushPicker::strokePicture(const QString &presetName)
{
    // in black, for the light paper the canvas ring lays it on
    return KisPresetStrokeThumbnails::isEnabled() ? strokeOf(*catalog(), presetName, Qt::black) : QImage();
}

QString KisStudioBrushPicker::title(const QString &presetName)
{
    static const QRegularExpression sortPrefix(QStringLiteral("^[a-z]\\)\\s+"));
    return QString(presetName).remove(sortPrefix);
}

bool KisStudioBrushPicker::paintToolActive()
{
    static const QStringList tools = {
        QStringLiteral("KritaShape/KisToolBrush"), QStringLiteral("KritaShape/KisToolDyna"),
        QStringLiteral("KritaShape/KisToolMultiBrush"), QStringLiteral("KritaShape/KisToolLine"),
    };
    return tools.contains(KisStudioTools::activeToolId());
}

QStringList KisStudioBrushPicker::yourBrushes()
{
    return pinnedIds(brushSource(catalog(), nullptr));
}

void KisStudioBrushPicker::setYourBrushes(const QStringList &presetNames)
{
    setPinnedIds(brushSource(catalog(), nullptr), presetNames);
}
