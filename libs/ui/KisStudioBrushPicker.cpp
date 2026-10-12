/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioBrushPicker.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QFuture>
#include <QMutex>
#include <QMutexLocker>
#include <QPainter>
#include <QRegularExpression>
#include <QPainterPath>
#include <QMouseEvent>
#include <QThread>
#include <QTimer>
#include <QtConcurrent>
#include <QtMath>

#include <atomic>

#include <KisGlobalResourcesInterface.h>
#include <KisResourceModel.h>
#include <KisResourceModelProvider.h>
#include <KisTagResourceModel.h>
#include <KisResourceTypes.h>
#include <KisTagFilterResourceProxyModel.h>
#include <KisTagModel.h>
#include <KoCanvasResourceProvider.h>
#include <KoColorSpaceRegistry.h>
#include <KoToolManager.h>
#include <klocalizedstring.h>

#include "KisMainWindow.h"
#include "KisResourceServerProvider.h"
#include "KisViewManager.h"
#include "kis_canvas_resource_provider.h"
#include "kis_config.h"
#include "kis_image.h"
#include "kis_paint_device.h"
#include "kis_paint_layer.h"
#include "kis_paintop_preset.h"
#include "kis_paintop_settings.h"
#include "kis_painter.h"
#include "brushengine/kis_paint_information.h"
#include "kis_distance_information.h"
#include "KisRunnableStrokeJobData.h"
#include "brushengine/kis_paintop.h"
#include <KisModernStyle.h>

/**
 * The tiles' strokes: painted in a worker thread, one brush after another.
 * The GUI thread loads each brush and hands the worker a copy that needs
 * nothing from the GUI thread; the worker hands back the stroke, and the
 * copy, to be let go of in the GUI thread.
 */
struct KisStudioBrushStrokes {
    struct Brush {
        QString name;
        QString md5;
        QString title;
        QString engine;
        QStringList tags;
        QImage icon;
        QString kind;
    };

    struct Catalog {
        QStringList ids;                ///< by title
        QHash<QString, Brush> brushes;
    };

    struct Job {
        QString id;
        KisPaintOpPresetSP preset;      ///< a copy with its resources, at a size that fits
        KisImageSP image;
        KisPaintLayerSP layer;
        KoColor paint;
        QString compositeOp;
        qreal opacity {1.0};
        QString kind;
        QSize size;
    };

    std::shared_ptr<Catalog> catalog;   ///< all the brushes there are, as the picker opens
    QColor paper;                       ///< behind the strokes, set before the worker starts
    QMutex mutex;
    QList<Job> queue;                   ///< for the worker, under the mutex
    QHash<QString, QImage> made;        ///< by the worker, under the mutex
    QList<Job> spent;                   ///< painted, to be let go of in the GUI thread, under the mutex
    std::atomic<bool> cancelled {false};
    std::atomic<bool> allQueued {false};
    std::atomic<bool> finished {false};
    QHash<QString, QImage> shown;       ///< the ones collected, used in the GUI thread only
    /// What each stroke changed, as alpha (over a background: the difference it made): for the map
    QHash<QString, QImage> madeMasks;   ///< by the worker, under the mutex
    QHash<QString, QImage> masks;       ///< collected, GUI thread only
};

namespace {

using Group = KisStudioPicker::Group;
using Job = KisStudioBrushStrokes::Job;
using Brush = KisStudioBrushStrokes::Brush;
using Catalog = KisStudioBrushStrokes::Catalog;

const QString pinnedKey = QStringLiteral("StudioBrushes");
const QString recentKey = QStringLiteral("StudioRecentBrushes");
constexpr int recentKept = 8;
/// The size of a tile's picture, see KisStudioPicker
const QSize strokeSize(112, 64);

/// The groups of "All brushes": by what a brush is for, in this order
QVector<QPair<QString, QString>> kindTitles()
{
    return {
        {QStringLiteral("sketch"), i18nc("brush group", "Sketch and dry media")},
        {QStringLiteral("ink"), i18nc("brush group", "Ink and lines")},
        {QStringLiteral("paint"), i18nc("brush group", "Paint")},
        {QStringLiteral("soft"), i18nc("brush group", "Airbrush and glow")},
        {QStringLiteral("blend"), i18nc("brush group", "Blend and smudge")},
        {QStringLiteral("texture"), i18nc("brush group", "Texture and scatter")},
        {QStringLiteral("pixel"), i18nc("brush group", "Pixel")},
        {QStringLiteral("erase"), i18nc("brush group", "Erasers")},
        {QStringLiteral("effect"), i18nc("brush group", "Effects, clone and deform")},
        {QStringLiteral("other"), i18nc("brush group", "Other")},
    };
}

/// What a brush is for: from the words in its name, then from its engine
QString kindOf(const QString &name, const QString &engine)
{
    const QString n = name.toLower();
    auto has = [&n](std::initializer_list<const char *> words) {
        for (const char *word : words) {
            if (n.contains(QLatin1String(word))) return true;
        }
        return false;
    };
    if (has({"eras"})) return QStringLiteral("erase");
    if (engine == QLatin1String("filter") || engine == QLatin1String("deformbrush") || engine == QLatin1String("duplicate")
        || has({"filter", "deform", "liquify", "clone", "dodge", "burn", "sharpen", "smart patch"})) {
        return QStringLiteral("effect");
    }
    if (has({"blend", "smudge", "smear", "blur", "mix"})) return QStringLiteral("blend");
    if (has({"airbrush", "glow", "fog", "mist", "cloud", "haze"})) return QStringLiteral("soft");
    if (has({"pencil", "graphite", "charcoal", "chalk", "crayon", "pastel", "sketch", "conte", "lead"})) return QStringLiteral("sketch");
    // wet media first: "WaterC Basic Lines" is paint, not ink
    if (has({"water", "wet", "oil", "acryl", "gouache", "bristle", "impasto"})) return QStringLiteral("paint");
    if (has({"ink", "liner", "pen", "marker", "calligraph", "sumi", "line"})) return QStringLiteral("ink");
    if (has({"pixel"})) return QStringLiteral("pixel");
    if (has({"texture", "grain", "splat", "spread", "rock", "hair", "fur", "grass", "foliage", "leaf", "leaves", "pattern",
             "stamp", "noise", "hatch", "shape", "scatter", "star", "spray", "speckle", "sparkle", "bokeh"})) {
        return QStringLiteral("texture");
    }
    if (engine == QLatin1String("spraybrush") || engine == QLatin1String("particlebrush") || engine == QLatin1String("hatchingbrush")
        || engine == QLatin1String("gridbrush") || engine == QLatin1String("sketchbrush") || engine == QLatin1String("curvebrush")) {
        return QStringLiteral("texture");
    }
    if (has({"paint", "dry", "basic", "brush", "round", "flat", "glaze", "rough", "knife", "fan"})
        || engine == QLatin1String("colorsmudge") || engine == QLatin1String("hairybrush") || engine == QLatin1String("mypaintbrush")) {
        return QStringLiteral("paint");
    }
    // the quick brush: a hard round tip, mostly for lines
    if (engine == QLatin1String("roundmarker")) return QStringLiteral("ink");
    return QStringLiteral("other");
}

std::shared_ptr<Catalog> loadCatalog()
{
    auto catalog = std::make_shared<Catalog>();
    KisResourceModel model(ResourceType::PaintOpPresets);
    for (int row = 0; row < model.rowCount(); row++) {
        const QModelIndex index = model.index(row, 0);
        Brush brush;
        brush.name = index.data(Qt::UserRole + KisAbstractResourceModel::Name).toString();
        if (brush.name.isEmpty() || catalog->brushes.contains(brush.name)) {
            continue;
        }
        brush.md5 = index.data(Qt::UserRole + KisAbstractResourceModel::MD5).toString();
        brush.title = KisStudioBrushPicker::title(brush.name).replace(QLatin1Char('_'), QLatin1Char(' '));
        brush.tags = index.data(Qt::UserRole + KisAbstractResourceModel::Tags).toStringList();
        brush.engine = index.data(Qt::UserRole + KisAbstractResourceModel::MetaData).toMap().value(QStringLiteral("paintopid")).toString();
        brush.icon = index.data(Qt::UserRole + KisAbstractResourceModel::Thumbnail).value<QImage>();
        brush.kind = kindOf(brush.name, brush.engine);
        catalog->ids << brush.name;
        catalog->brushes.insert(brush.name, brush);
    }
    std::sort(catalog->ids.begin(), catalog->ids.end(), [catalog](const QString &a, const QString &b) {
        return catalog->brushes.value(a).title.localeAwareCompare(catalog->brushes.value(b).title) < 0;
    });
    return catalog;
}

/// The catalog, made again only once the brushes or their tags changed (a ring, a scroll on the brush chip, need it at once)
std::shared_ptr<Catalog> catalog()
{
    static std::shared_ptr<Catalog> cached;
    static bool watching = false;
    if (!watching) {
        watching = true;
        const auto drop = []() { cached.reset(); };
        const QList<QAbstractItemModel *> models{KisResourceModelProvider::resourceModel(ResourceType::PaintOpPresets),
                                                 KisResourceModelProvider::tagResourceModel(ResourceType::PaintOpPresets)};
        Q_FOREACH (QAbstractItemModel *model, models) {
            if (!model) continue;
            QObject::connect(model, &QAbstractItemModel::modelReset, qApp, drop);
            QObject::connect(model, &QAbstractItemModel::layoutChanged, qApp, drop);
            QObject::connect(model, &QAbstractItemModel::rowsInserted, qApp, drop);
            QObject::connect(model, &QAbstractItemModel::rowsRemoved, qApp, drop);
            QObject::connect(model, &QAbstractItemModel::dataChanged, qApp, drop);
        }
    }
    if (!cached) cached = loadCatalog();
    return cached;
}

/// Yours until you change them: the pop-up palette's favorites, or a few for every kind of work
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

/// Light paper, or dark paper for a light color
QColor paperFor(const QColor &paint)
{
    return paint.lightnessF() > 0.8 ? QColor(52, 52, 56) : QColor(244, 241, 234);
}

std::shared_ptr<KisStudioBrushStrokes> makeStrokes()
{
    auto strokes = std::make_shared<KisStudioBrushStrokes>();
    strokes->catalog = catalog();
    return strokes;
}

/// @param strokes the tiles' strokes, or none when the source is not for showing
KisStudioPicker::Source brushSource(std::shared_ptr<Catalog> catalog, std::shared_ptr<KisStudioBrushStrokes> strokes)
{
    KisStudioPicker::Source source;
    source.pinnedKey = pinnedKey;
    source.initialPinned = initialBrushes;
    source.allIds = [catalog]() { return catalog->ids; };
    source.otherGroups = [catalog](const QStringList &pinned) {
        QVector<Group> groups;
        Q_FOREACH (const auto &title, kindTitles()) {
            Group group{title.second, {}};
            Q_FOREACH (const QString &id, catalog->ids) {
                if (!pinned.contains(id) && catalog->brushes.value(id).kind == title.first) group.ids << id;
            }
            if (!group.ids.isEmpty()) groups << group;
        }
        return groups;
    };
    source.name = [catalog](const QString &id) { return catalog->brushes.value(id).title; };
    source.aliases = [catalog](const QString &id) {
        const Brush brush = catalog->brushes.value(id);
        QStringList aliases = brush.tags;
        Q_FOREACH (const auto &title, kindTitles()) {
            if (title.first == brush.kind) aliases << title.second;
        }
        aliases << brush.engine;
        return aliases;
    };
    if (strokes) {
        source.paintPreview = [catalog, strokes](QPainter &painter, const QRect &rect, const QString &id) {
            painter.fillRect(rect, strokes->paper);
            const QImage stroke = strokes->shown.value(id);
            if (!stroke.isNull()) {
                painter.drawImage(rect, stroke);
                return;
            }
            // not painted (yet): the brush's own picture, faint
            const QImage icon = catalog->brushes.value(id).icon;
            if (!icon.isNull()) {
                const int side = rect.height() - 8;
                painter.save();
                painter.setOpacity(painter.opacity() * 0.45);
                painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
                painter.drawImage(QRect(rect.center().x() - side / 2, rect.top() + 4, side, side), icon);
                painter.restore();
            }
        };
    }
    source.recent = []() { return KisConfig(true).readEntry<QStringList>(recentKey, QStringList()); };
    source.tiles = true;
    source.yoursTitle = i18nc("brush group", "Your brushes");
    source.searchPlaceholder = i18nc("@info:placeholder", "Search %1 brushes", catalog->ids.size());
    // the number goes in later: %1 passes through
    source.allLabel = i18nc("@action:button %1 is a number", "All %1 brushes", QStringLiteral("%1"));
    return source;
}

/// The brush's stroke: a wave that starts and ends light, heaviest in the middle
QImage paintStroke(const Job &job, QImage *mask)
{
    KisPaintDeviceSP device = job.layer->paintDevice();
    const int w = job.size.width();
    const int h = job.size.height();
    const QRect bounds(0, 0, w, h);

    if (job.kind == QLatin1String("blend") || job.kind == QLatin1String("effect")) {
        // something to blend or change: bands of the color over the paper
        KoColor band = job.paint;
        for (int i = 0; i < 4; i += 2) {
            device->fill(QRect(w * i / 4, 0, w / 4 + 1, h), band);
        }
        KoColor other(device->colorSpace());
        other.fromQColor(job.paint.toQColor().lighter(160));
        device->fill(QRect(w / 4, 0, w / 4 + 1, h), other);
    } else if (job.kind == QLatin1String("erase")) {
        // something to erase: a wash of the color
        KoColor wash = job.paint;
        wash.setOpacity(0.55);
        device->fill(bounds, wash);
    }
    const bool overBackground = job.kind == QLatin1String("blend") || job.kind == QLatin1String("effect")
        || job.kind == QLatin1String("erase");
    const QImage before = overBackground ? device->convertToQImage(nullptr, 0, 0, w, h).convertToFormat(QImage::Format_ARGB32) : QImage();

    KisPainter painter(device);
    painter.setPaintColor(job.paint);
    if (!job.compositeOp.isEmpty()) painter.setCompositeOpId(job.compositeOp);
    painter.setOpacityF(job.opacity);
    painter.setPaintOpPreset(job.preset, job.layer, job.image);
    if (!painter.paintOp()) {
        return QImage();
    }

    const qreal radius = job.preset->settings()->paintOpSize() / 2;
    const qreal margin = qMax<qreal>(radius, 3) + 3;
    const qreal amplitude = qMax<qreal>(h / 2.0 - margin, 0) * 0.8;
    constexpr int steps = 28;
    KisDistanceInformation distance;
    KisPaintInformation previous;
    for (int i = 0; i <= steps; i++) {
        const qreal t = qreal(i) / steps;
        const QPointF pos(margin + t * (w - 2 * margin), h / 2.0 - amplitude * qSin(2 * M_PI * t));
        const qreal pressure = qMax(0.05, qPow(qSin(M_PI * t), 0.6));
        KisPaintInformation info(pos, pressure);
        info.setCurrentTime(i * 12);
        if (i > 0) {
            painter.paintLine(previous, info, &distance);
        }
        previous = info;
    }
    // most engines only queue their dabs while painting, for the stroke to render them in its
    // own time; there is no stroke here: render them all now (the painter runs the jobs it makes)
    for (int round = 0; round < 256; round++) {
        QVector<KisRunnableStrokeJobData *> jobs;
        const bool more = painter.paintOp()->doAsynchronousUpdate(jobs).second;
        Q_FOREACH (KisRunnableStrokeJobData *data, jobs) {
            data->run();
            delete data;
        }
        if (!more) break;
    }
    const QImage after = device->convertToQImage(nullptr, 0, 0, w, h).convertToFormat(QImage::Format_ARGB32);
    if (mask) {
        if (overBackground) {
            // only what the stroke changed: the larger difference of any channel, as alpha
            QImage changed(w, h, QImage::Format_ARGB32);
            for (int y = 0; y < h; y++) {
                const QRgb *a = reinterpret_cast<const QRgb *>(before.constScanLine(y));
                const QRgb *b = reinterpret_cast<const QRgb *>(after.constScanLine(y));
                QRgb *out = reinterpret_cast<QRgb *>(changed.scanLine(y));
                for (int x = 0; x < w; x++) {
                    const int d = qMax(qMax(qAbs(qRed(a[x]) - qRed(b[x])), qAbs(qGreen(a[x]) - qGreen(b[x]))),
                                       qMax(qAbs(qBlue(a[x]) - qBlue(b[x])), qAbs(qAlpha(a[x]) - qAlpha(b[x]))));
                    out[x] = qRgba(0, 0, 0, qMin(255, d * 2));
                }
            }
            *mask = changed;
        } else {
            *mask = after;
        }
    }
    return after;
}

/// Strokes already painted, by brush, for the color and size they were painted at
QHash<QString, QImage> &strokeCache()
{
    static QHash<QString, QImage> cache;
    return cache;
}

/// The last stroke painted for each brush, whatever the color: for showing a brush elsewhere (the canvas ring)
QHash<QString, QImage> &latestStrokes()
{
    static QHash<QString, QImage> latest;
    return latest;
}

/// What each cached stroke changed (see KisStudioBrushStrokes::masks), by the same keys
QHash<QString, QImage> &maskCache()
{
    static QHash<QString, QImage> cache;
    return cache;
}

QString &strokeCacheStamp()
{
    static QString stamp;
    return stamp;
}

} // namespace

/**
 * All the brushes on one field, placed by how their strokes look: soft to
 * hard from left to right, smooth to textured from top to bottom, so that
 * brushes that paint alike are neighbours. Measured from the strokes as they
 * are painted; yours get their places first, the rest the nearest free one.
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
        setFixedSize(496, 512);
    }

protected:
    struct Look {
        qreal soft {0};
        qreal texture {0};
    };

    /// How much of the stroke is half transparent (soft), and how much it changes from pixel to pixel (texture)
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
        // the ones painted since: measured once
        for (auto it = m_strokes->shown.constBegin(); it != m_strokes->shown.constEnd(); ++it) {
            if (m_looks.contains(it.key())) continue;
            // what the stroke itself changed, where there is a background under it
            const QImage mask = m_strokes->masks.value(it.key());
            m_looks.insert(it.key(), measure(mask.isNull() ? it.value() : mask));
        }
        if (m_looks.size() == m_placedFor) return;
        m_placedFor = m_looks.size();

        // by rank on each axis: spread over the whole field
        QStringList ids = m_looks.keys();
        QStringList bySoft = ids, byTexture = ids;
        std::sort(bySoft.begin(), bySoft.end(), [this](const QString &a, const QString &b) { return m_looks[a].soft > m_looks[b].soft; });
        std::sort(byTexture.begin(), byTexture.end(), [this](const QString &a, const QString &b) { return m_looks[a].texture < m_looks[b].texture; });
        const int columns = width() / cellWidth;
        const int rows = (height() - 22) / cellHeight;
        QVector<QVector<QString>> taken(rows, QVector<QString>(columns));
        // pointed at: none moves under the pointer (a click would take another), the new ones go where there is room
        const bool keep = underMouse() && !m_cells.isEmpty();
        if (keep) {
            for (auto it = m_cells.constBegin(); it != m_cells.constEnd(); ++it) {
                const int x = it->x() / cellWidth, y = (it->y() - 18) / cellHeight;
                if (x >= 0 && y >= 0 && x < columns && y < rows) taken[y][x] = it.key();
            }
        } else {
            m_cells.clear();
        }

        // yours first, in your order: their places are the most exact
        QStringList order;
        Q_FOREACH (const QString &id, m_yours) if (m_looks.contains(id)) order << id;
        std::sort(ids.begin(), ids.end());
        Q_FOREACH (const QString &id, ids) if (!order.contains(id)) order << id;

        const qreal n = qMax(1, ids.size() - 1);
        Q_FOREACH (const QString &id, order) {
            if (keep && m_cells.contains(id)) continue;
            const int cx = qRound(bySoft.indexOf(id) / n * (columns - 1));
            const int cy = qRound(byTexture.indexOf(id) / n * (rows - 1));
            // the nearest free cell, ring by ring
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

        // which way is what
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
        // the one pointed at, bigger, with its name
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
        QPainterPath clip;
        clip.addPath(KisModernStyle::squircle(QRectF(rect), big ? 8 : 4));
        painter.save();
        painter.setClipPath(clip);
        painter.fillRect(rect, m_strokes->paper);
        painter.drawImage(rect, m_strokes->shown.value(id));
        painter.restore();
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
        // placed again, each where it belongs among all of them
        m_placedFor = -1;
        update();
    }

private:
    static constexpr int cellWidth = 49;
    static constexpr int cellHeight = 30;
    std::shared_ptr<KisStudioBrushStrokes> m_strokes;
    QStringList m_yours;
    QString m_current;
    QHash<QString, Look> m_looks;
    QHash<QString, QRect> m_cells;
    int m_placedFor {-1};
    QString m_hover;
};

struct KisStudioBrushPicker::Private
{
    KisMainWindow *mainWindow {nullptr};
    std::shared_ptr<Catalog> catalog;
    std::shared_ptr<KisStudioBrushStrokes> strokes;
    KisImageSP image;
    KoColor paint;
    QSize size;
    QStringList toQueue;
    int next {0};
    QTimer feed;
    QTimer collect;
    QFuture<void> worker;

    /// The next brushes for the worker, for a few ms at a time: loading them takes a moment
    void feedSome();
};

KisStudioBrushPicker::KisStudioBrushPicker(KisMainWindow *mainWindow)
    : KisStudioBrushPicker(mainWindow, makeStrokes())
{
}

KisStudioBrushPicker::KisStudioBrushPicker(KisMainWindow *mainWindow, std::shared_ptr<KisStudioBrushStrokes> strokes)
    : KisStudioPicker(brushSource(strokes->catalog, strokes),
                      mainWindow->viewManager() && mainWindow->viewManager()->canvasResourceProvider()
                              && mainWindow->viewManager()->canvasResourceProvider()->currentPreset()
                          ? mainWindow->viewManager()->canvasResourceProvider()->currentPreset()->name()
                          : QString(),
                      QSet<QString>(), mainWindow)
    , d(new Private)
{
    d->mainWindow = mainWindow;
    d->strokes = strokes;
    d->catalog = strokes->catalog;

    KisViewManager *view = mainWindow->viewManager();
    KisCanvasResourceProvider *resources = view ? view->canvasResourceProvider() : nullptr;
    const KoColorSpace *rgb = KoColorSpaceRegistry::instance()->rgb8();
    d->paint = resources ? resources->fgColor().convertedTo(rgb) : KoColor(Qt::black, rgb);
    d->paint.setOpacity(1.0);
    strokes->paper = paperFor(d->paint.toQColor());
    d->size = strokeSize * mainWindow->devicePixelRatioF();

    connect(this, &KisStudioPicker::sigPicked, this, [mainWindow](const QString &id) {
        recordUse(id);
        use(mainWindow->viewManager(), id);
    });

    // all of them by how they look
    auto *map = new BrushMap(strokes, pinnedIds(brushSource(strokes->catalog, nullptr)),
                             mainWindow->viewManager() && mainWindow->viewManager()->canvasResourceProvider()
                                     && mainWindow->viewManager()->canvasResourceProvider()->currentPreset()
                                 ? mainWindow->viewManager()->canvasResourceProvider()->currentPreset()->name()
                                 : QString(),
                             this);
    map->picked = [this](const QString &id) { pickId(id); };
    addAlternateView(map, i18nc("@action:button the brushes by how they look", "Map"), QStringLiteral("StudioBrushesAsMap"));

    if (!resources || !resources->resourceManager()) {
        strokes->finished = true;
        return;
    }

    // painted before for this color: shown at once
    const QString stamp = d->paint.toQColor().name() + QLatin1Char('@') + QString::number(d->size.width());
    if (strokeCacheStamp() != stamp) {
        strokeCache().clear();
        maskCache().clear();
        strokeCacheStamp() = stamp;
    }

    // the recent ones and yours first, then the rest: the ones seen first are painted first
    const Source source = brushSource(d->catalog, nullptr);
    QStringList order = source.recent() + pinnedIds(source) + d->catalog->ids;
    order.removeDuplicates();
    Q_FOREACH (const QString &id, order) {
        const Brush brush = d->catalog->brushes.value(id);
        const QImage cached = strokeCache().value(brush.md5 + id);
        if (!cached.isNull()) {
            strokes->shown.insert(id, cached);
            const QImage cachedMask = maskCache().value(brush.md5 + id);
            if (!cachedMask.isNull()) strokes->masks.insert(id, cachedMask);
        } else if (!brush.name.isEmpty()) {
            d->toQueue << id;
        }
    }

    d->image = new KisImage(nullptr, d->size.width(), d->size.height(), rgb, QStringLiteral("brush strokes"));

    d->worker = QtConcurrent::run([strokes]() {
        while (!strokes->cancelled) {
            Job job;
            bool have = false;
            {
                QMutexLocker locker(&strokes->mutex);
                if (!strokes->queue.isEmpty()) {
                    job = strokes->queue.takeFirst();
                    have = true;
                } else if (strokes->allQueued) {
                    break;
                }
            }
            if (!have) {
                QThread::msleep(4);
                continue;
            }
            QImage mask;
            const QImage stroke = paintStroke(job, &mask);
            QMutexLocker locker(&strokes->mutex);
            if (!stroke.isNull()) strokes->made.insert(job.id, stroke);
            if (!mask.isNull()) strokes->madeMasks.insert(job.id, mask);
            strokes->spent << job;
        }
        strokes->finished = true;
        // what the jobs hold (presets, an image) let go of in the GUI thread, also once the picker is gone
        QMetaObject::invokeMethod(qApp, [strokes]() {
            QMutexLocker locker(&strokes->mutex);
            strokes->queue.clear();
            strokes->spent.clear();
        }, Qt::QueuedConnection);
    });

    d->feed.setInterval(0);
    connect(&d->feed, &QTimer::timeout, this, [this]() { d->feedSome(); });
    d->feed.start();

    // the finished ones, a few times a second, until all are
    d->collect.setInterval(80);
    connect(&d->collect, &QTimer::timeout, this, [this]() {
        bool any = false;
        QList<Job> spent;
        {
            QMutexLocker locker(&d->strokes->mutex);
            for (auto it = d->strokes->madeMasks.constBegin(); it != d->strokes->madeMasks.constEnd(); ++it) {
                d->strokes->masks.insert(it.key(), it.value());
                maskCache().insert(d->catalog->brushes.value(it.key()).md5 + it.key(), it.value());
            }
            d->strokes->madeMasks.clear();
            for (auto it = d->strokes->made.constBegin(); it != d->strokes->made.constEnd(); ++it) {
                d->strokes->shown.insert(it.key(), it.value());
                strokeCache().insert(d->catalog->brushes.value(it.key()).md5 + it.key(), it.value());
                latestStrokes().insert(it.key(), it.value());
                any = true;
            }
            d->strokes->made.clear();
            spent.swap(d->strokes->spent);
        }
        // the copies go here, in the GUI thread
        spent.clear();
        if (any) updatePreviews();
        if (d->strokes->finished && !any) d->collect.stop();
    });
    d->collect.start();
}

void KisStudioBrushPicker::Private::feedSome()
{
    KisViewManager *view = mainWindow->viewManager();
    KisCanvasResourceProvider *resources = view ? view->canvasResourceProvider() : nullptr;
    if (!resources || !resources->resourceManager() || next >= toQueue.size()) {
        feed.stop();
        strokes->allQueued = true;
        return;
    }
    KisPaintOpPresetResourceServer *server = KisResourceServerProvider::instance()->paintOpPresetServer();
    const KoColorSpace *rgb = KoColorSpaceRegistry::instance()->rgb8();
    QElapsedTimer spent;
    spent.start();
    QList<Job> jobs;
    while (next < toQueue.size() && spent.elapsed() < 6) {
        const Brush brush = catalog->brushes.value(toQueue[next++]);
        KisPaintOpPresetSP preset = server->resource(brush.md5, QString(), brush.name);
        // the clone brush paints what is elsewhere in the image: nothing to show
        if (!preset || !preset->valid() || !preset->settings() || brush.engine == QLatin1String("duplicate")) {
            continue;
        }
        KisPaintOpPresetSP copy = preset->cloneWithResourcesSnapshot(KisGlobalResourcesInterface::instance(),
                                                                     resources->resourceManager()->canvasResourcesInterface(), nullptr);
        if (!copy || !copy->settings()) {
            continue;
        }
        // big enough to see the tip, small enough to fit the tile
        const qreal dpr = qreal(size.height()) / strokeSize.height();
        const qreal largest = size.height() * 0.32;
        copy->settings()->setPaintOpSize(qBound(2.5 * dpr, copy->settings()->paintOpSize() * dpr, largest));
        Job job;
        job.id = brush.name;
        job.preset = copy;
        job.image = image;
        job.layer = new KisPaintLayer(image, QStringLiteral("stroke"), OPACITY_OPAQUE_U8, rgb);
        job.paint = paint;
        job.compositeOp = copy->settings()->paintOpCompositeOp();
        job.opacity = copy->settings()->paintOpOpacity();
        job.kind = brush.kind;
        job.size = size;
        jobs << job;
    }
    QMutexLocker locker(&strokes->mutex);
    strokes->queue << jobs;
    if (next >= toQueue.size()) {
        feed.stop();
        strokes->allQueued = true;
    }
}

KisStudioBrushPicker::~KisStudioBrushPicker()
{
    // the worker stops after the brush it is painting, not waited for: it hands what it holds back to the GUI thread
    d->strokes->cancelled = true;
    QMutexLocker locker(&d->strokes->mutex);
    d->strokes->queue.clear();
    d->strokes->spent.clear();
}

void KisStudioBrushPicker::recordUse(const QString &presetName)
{
    QStringList recent = KisConfig(true).readEntry<QStringList>(recentKey, QStringList());
    if (!recent.isEmpty() && recent.first() == presetName) {
        return;
    }
    recent.removeAll(presetName);
    recent.prepend(presetName);
    KisConfig(false).writeEntry(recentKey, recent.mid(0, recentKept));
}

QString KisStudioBrushPicker::neighbour(KisMainWindow *, const QString &current, int step)
{
    const QStringList yours = pinnedIds(brushSource(catalog(), nullptr));
    if (yours.isEmpty()) {
        return QString();
    }
    const int index = yours.indexOf(current);
    if (index < 0) {
        // not one of yours: into your list from its start or end
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
    if (preset) {
        resources->setPaintOpPreset(preset);
    }
}

QImage KisStudioBrushPicker::strokePicture(const QString &presetName)
{
    return latestStrokes().value(presetName);
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
    return tools.contains(KoToolManager::instance()->activeToolId());
}

QStringList KisStudioBrushPicker::yourBrushes()
{
    return pinnedIds(brushSource(catalog(), nullptr));
}

void KisStudioBrushPicker::setYourBrushes(const QStringList &presetNames)
{
    setPinnedIds(brushSource(catalog(), nullptr), presetNames);
}
