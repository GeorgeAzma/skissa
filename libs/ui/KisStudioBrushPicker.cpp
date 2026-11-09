/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioBrushPicker.h"
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
constexpr int recentKept = 8;

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

    Q_FOREACH (const QString &id, catalog.ids) {
        auto first = firstGroup.constFind(id);
        if (first != firstGroup.constEnd()) {
            catalog.tagGroups[*first].ids << id;
        } else {
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
        catalog->ids << brush.name;
        catalog->brushes.insert(brush.name, brush);
    }
    std::sort(catalog->ids.begin(), catalog->ids.end(), [catalog](const QString &a, const QString &b) {
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
            const QImage stroke = KisPresetStrokeThumbnails::isEnabled() ? strokeOf(*catalog, id, strokes->ink) : QImage();
            if (!stroke.isNull()) {
                drawStroke(painter, QRectF(rect).adjusted(KisModernStyle::space1, KisModernStyle::space1, -KisModernStyle::space1, -KisModernStyle::space1), stroke);
                return;
            }
            // not painted yet, or painting nothing alone: the brush's own picture
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
    source.recent = []() { return KisConfig(true).readEntry<QStringList>(recentKey, QStringList()); };
    source.tiles = true;
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

/**
 * All brushes laid out by how their strokes look: soft to hard from left to
 * right, smooth to textured from top to bottom. Pinned brushes are placed
 * first; the others take the nearest free cell.
 */
class BrushMap : public QWidget
{
public:
    std::function<void(const QString &)> picked;

    BrushMap(std::shared_ptr<KisStudioBrushStrokes> strokes, const QStringList &yours, const QString &current, QWidget *parent)
        : QWidget(parent)
        , m_strokes(strokes)
        , m_yours(yours)
        , m_current(current)
    {
        setMouseTracking(true);
        // Fits 160 brushes; extra rows are added for more
        const int rows = (int(strokes->catalog->ids.size()) + columns - 1) / columns;
        setFixedSize(columns * cellWidth + 6, 512 + qMax(0, rows - 16) * cellHeight);
    }

protected:
    struct Look {
        qreal soft {0};
        qreal texture {0};
    };

    /// Softness is the share of semi-transparent pixels; texture is the average alpha change between neighbours
    static Look measure(const QImage &image)
    {
        const QImage img = image.convertToFormat(QImage::Format_ARGB32);
        qint64 covered = 0, mid = 0, roughCount = 0;
        qreal rough = 0;
        for (int y = 0; y < img.height(); y++) {
            const QRgb *line = reinterpret_cast<const QRgb *>(img.constScanLine(y));
            for (int x = 0; x < img.width(); x++) {
                const int a = qAlpha(line[x]);
                if (a > 10) {
                    covered++;
                    if (a > 25 && a < 215) mid++;
                }
                if (x + 1 < img.width()) {
                    const int b = qAlpha(line[x + 1]);
                    if (a > 10 || b > 10) {
                        rough += qAbs(a - b);
                        roughCount++;
                    }
                }
            }
        }
        Look look;
        look.soft = covered ? qreal(mid) / covered : 0;
        look.texture = roughCount ? rough / roughCount / 255.0 : 0;
        return look;
    }

    void place()
    {
        // each brush is measured once its stroke is painted; asking for a stroke paints it
        Q_FOREACH (const QString &id, m_strokes->catalog->ids) {
            if (m_shown.contains(id)) continue;
            const QImage stroke = strokeOf(*m_strokes->catalog, id, m_strokes->ink);
            if (stroke.isNull()) continue;
            m_shown.insert(id, stroke);
            m_looks.insert(id, measure(stroke));
        }
        if (m_looks.size() == m_placedFor) return;
        m_placedFor = m_looks.size();

        // Use rank on each axis so brushes spread over the whole map
        QStringList ids = m_looks.keys();
        QStringList bySoft = ids, byTexture = ids;
        std::sort(bySoft.begin(), bySoft.end(), [this](const QString &a, const QString &b) { return m_looks[a].soft > m_looks[b].soft; });
        std::sort(byTexture.begin(), byTexture.end(), [this](const QString &a, const QString &b) { return m_looks[a].texture < m_looks[b].texture; });
        const int rows = (height() - 22) / cellHeight;
        QVector<QVector<QString>> taken(rows, QVector<QString>(columns));
        // While hovered, keep existing cells in place and fit new brushes around them
        const bool keep = underMouse() && !m_cells.isEmpty();
        if (keep) {
            for (auto it = m_cells.constBegin(); it != m_cells.constEnd(); ++it) {
                const int x = it->x() / cellWidth, y = (it->y() - 18) / cellHeight;
                if (x >= 0 && y >= 0 && x < columns && y < rows) taken[y][x] = it.key();
            }
        } else {
            m_cells.clear();
        }

        // Pinned brushes first, so they get the most accurate positions
        QStringList order;
        Q_FOREACH (const QString &id, m_yours) if (m_looks.contains(id)) order << id;
        std::sort(ids.begin(), ids.end());
        Q_FOREACH (const QString &id, ids) if (!order.contains(id)) order << id;

        const qreal n = qMax(1, ids.size() - 1);
        Q_FOREACH (const QString &id, order) {
            if (keep && m_cells.contains(id)) continue;
            const int cx = qRound(bySoft.indexOf(id) / n * (columns - 1));
            const int cy = qRound(byTexture.indexOf(id) / n * (rows - 1));
            bool done = false;
            for (int r = 0; r < qMax(columns, rows) && !done; r++) {
                for (int dy = -r; dy <= r && !done; dy++) {
                    for (int dx = -r; dx <= r && !done; dx++) {
                        if (qMax(qAbs(dx), qAbs(dy)) != r) continue;
                        const int x = cx + dx, y = cy + dy;
                        if (x < 0 || y < 0 || x >= columns || y >= rows || !taken[y][x].isEmpty()) continue;
                        taken[y][x] = id;
                        m_cells.insert(id, QRect(x * cellWidth, 18 + y * cellHeight, cellWidth, cellHeight));
                        done = true;
                    }
                }
            }
        }
    }

    void paintEvent(QPaintEvent *) override
    {
        place();
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        const QColor dim = KisModernStyle::secondaryInk(palette().color(QPalette::WindowText));

        QFont small = font();
        small.setPointSizeF(small.pointSizeF() * 0.85);
        painter.setFont(small);
        painter.setPen(dim);
        painter.drawText(QRect(2, 0, width() / 2, 16), Qt::AlignLeft | Qt::AlignVCenter, i18nc("brush map: the left side", "← Soft"));
        painter.drawText(QRect(width() / 2, 0, width() / 2 - 2, 16), Qt::AlignRight | Qt::AlignVCenter, i18nc("brush map: the right side", "Hard →"));
        painter.drawText(QRect(0, height() - 18, width() / 2, 18), Qt::AlignLeft | Qt::AlignVCenter, i18nc("brush map: the bottom", "↓ Textured"));

        for (auto it = m_cells.constBegin(); it != m_cells.constEnd(); ++it) {
            if (it.key() == m_hover) continue;
            paintCell(painter, it.value().adjusted(2, 2, -2, -2), it.key(), false);
        }
        // Draw the hovered brush enlarged, with its name
        if (m_cells.contains(m_hover)) {
            const QRect cell = m_cells.value(m_hover);
            QRect big(0, 0, cellWidth * 2.2, cellHeight * 2.2);
            big.moveCenter(cell.center());
            big.moveLeft(qBound(0, big.left(), width() - big.width()));
            big.moveTop(qBound(18, big.top(), height() - big.height() - 20));
            paintCell(painter, big, m_hover, true);
            const QString name = KisStudioBrushPicker::title(m_hover);
            QFont bold = font();
            bold.setBold(true);
            painter.setFont(bold);
            const QFontMetrics metrics(bold);
            QRectF pill(0, 0, metrics.horizontalAdvance(name) + 20, metrics.height() + 8);
            pill.moveCenter(QPointF(big.center().x(), big.bottom() + pill.height() / 2 + 4));
            pill.moveLeft(qBound(0.0, pill.left(), width() - pill.width()));
            painter.setPen(Qt::NoPen);
            painter.setBrush(palette().color(QPalette::Highlight));
            painter.drawPath(KisModernStyle::squircle(pill, pill.height() / 2));
            painter.setPen(palette().color(QPalette::HighlightedText));
            painter.drawText(pill, Qt::AlignCenter, name);
        }
    }

    void paintCell(QPainter &painter, const QRect &rect, const QString &id, bool big)
    {
        if (big) {
            // over the cells around it
            painter.fillPath(KisModernStyle::squircle(QRectF(rect), 8), palette().color(QPalette::Window));
        }
        drawStroke(painter, QRectF(rect).adjusted(2, 2, -2, -2), m_shown.value(id));
        const bool yours = m_yours.contains(id);
        const bool current = id == m_current;
        if (big || yours || current) {
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(big ? palette().color(QPalette::Highlight) : current ? palette().color(QPalette::WindowText)
                                                                              : palette().color(QPalette::Link),
                                big ? 2 : 1.2));
            painter.drawPath(KisModernStyle::squircle(QRectF(rect).adjusted(0.5, 0.5, -0.5, -0.5), big ? 8 : 4));
        }
    }

    QString idAt(const QPoint &pos) const
    {
        for (auto it = m_cells.constBegin(); it != m_cells.constEnd(); ++it) {
            if (it.value().contains(pos)) return it.key();
        }
        return QString();
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        const QString hover = idAt(event->pos());
        if (hover != m_hover) {
            m_hover = hover;
            setCursor(hover.isEmpty() ? Qt::ArrowCursor : Qt::PointingHandCursor);
            update();
        }
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        const QString id = idAt(event->pos());
        if (event->button() == Qt::LeftButton && !id.isEmpty() && picked) picked(id);
    }

    void leaveEvent(QEvent *) override
    {
        m_hover.clear();
        // Re-place brushes that were held in place while hovering
        m_placedFor = -1;
        update();
    }

private:
    static constexpr int columns = 10;
    static constexpr int cellWidth = 49;
    static constexpr int cellHeight = 30;
    std::shared_ptr<KisStudioBrushStrokes> m_strokes;
    QStringList m_yours;
    QString m_current;
    /// the strokes measured, kept: the brush lists' cache may let them go
    QHash<QString, QImage> m_shown;
    QHash<QString, Look> m_looks;
    QHash<QString, QRect> m_cells;
    int m_placedFor {-1};
    QString m_hover;
};

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

    // brushes as their pictures: nothing to paint, and no map of how strokes look
    if (!KisPresetStrokeThumbnails::isEnabled()) return;

    // once for the strokes finished within a moment of each other: they arrive one by one
    auto *strokesShown = new QTimer(this);
    strokesShown->setSingleShot(true);
    strokesShown->setInterval(50);
    connect(strokesShown, &QTimer::timeout, this, &KisStudioPicker::updatePreviews);
    connect(KisPresetStrokeThumbnails::instance(), &KisPresetStrokeThumbnails::changed, strokesShown, [strokesShown]() {
        if (!strokesShown->isActive()) strokesShown->start();
    });

    auto *map = new BrushMap(strokes, pinnedIds(pickerSource()), currentPresetName(mainWindow), this);
    map->picked = [this](const QString &id) { pickId(id); };
    addAlternateView(map, i18nc("@action:button the brushes by how they look", "Map"), QStringLiteral("StudioBrushesAsMap"));
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
