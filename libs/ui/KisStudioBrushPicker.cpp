/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioBrushPicker.h"

#include <QApplication>
#include <QElapsedTimer>
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
 * Sample strokes for the tiles, painted in a worker thread. The GUI thread
 * loads each brush and gives the worker a self-contained copy; the worker
 * returns the stroke along with the copy, which is released in the GUI thread.
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
        QStringList ids;                ///< sorted by title
        QHash<QString, Brush> brushes;
    };

    struct Job {
        QString id;
        KisPaintOpPresetSP preset;      ///< a copy with its resources, resized to fit
        KisImageSP image;
        KisPaintLayerSP layer;
        KoColor paint;
        QString compositeOp;
        qreal opacity {1.0};
        QString kind;
        QSize size;
    };

    std::shared_ptr<Catalog> catalog;
    QColor paper;                       ///< background color; set before the worker starts
    QMutex mutex;
    QList<Job> queue;                   ///< pending jobs, guarded by the mutex
    QHash<QString, QImage> made;        ///< painted by the worker, guarded by the mutex
    QList<Job> spent;                   ///< finished jobs to release in the GUI thread, guarded by the mutex
    std::atomic<bool> cancelled {false};
    std::atomic<bool> allQueued {false};
    std::atomic<bool> finished {false};
    QHash<QString, QImage> shown;       ///< collected strokes, GUI thread only
    /// Alpha masks of what each stroke changed, used to place brushes on the map
    QHash<QString, QImage> madeMasks;   ///< painted by the worker, guarded by the mutex
    QHash<QString, QImage> masks;       ///< collected masks, GUI thread only
};

namespace {

using Group = KisStudioPicker::Group;
using Job = KisStudioBrushStrokes::Job;
using Brush = KisStudioBrushStrokes::Brush;
using Catalog = KisStudioBrushStrokes::Catalog;

const QString pinnedKey = QStringLiteral("StudioBrushes");
const QString recentKey = QStringLiteral("StudioRecentBrushes");
constexpr int recentKept = 8;
/// Size of a default tile's preview, see KisStudioPicker
const QSize strokeSize(112, 64);

/// Brush groups by purpose, in display order
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

/// Guesses a brush's purpose from words in its name, then from its engine
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
    // Check wet media before ink: "WaterC Basic Lines" is paint, not ink
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
    // The Quick Brush engine: a hard round tip, mostly used for lines
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

/// Cached until the presets or their tags change, since the canvas ring and the brush wheel need it instantly
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

/// Light background, or a dark one when the paint color is light
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

/// @param strokes the tiles' strokes; null for a source that is never displayed
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
            // Not painted yet: show the preset's icon, faded
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
    // The count is filled in later, so keep %1 as a placeholder
    source.allLabel = i18nc("@action:button %1 is a number", "All %1 brushes", QStringLiteral("%1"));
    return source;
}

/// Paints a wave with light pressure at the ends and full in the middle; \p mask receives what the stroke changed
QImage paintStroke(const Job &job, QImage *mask)
{
    KisPaintDeviceSP device = job.layer->paintDevice();
    const int w = job.size.width();
    const int h = job.size.height();
    const QRect bounds(0, 0, w, h);

    if (job.kind == QLatin1String("blend") || job.kind == QLatin1String("effect")) {
        // Color bands give blending and effect brushes something to work on
        KoColor band = job.paint;
        for (int i = 0; i < 4; i += 2) {
            device->fill(QRect(w * i / 4, 0, w / 4 + 1, h), band);
        }
        KoColor other(device->colorSpace());
        other.fromQColor(job.paint.toQColor().lighter(160));
        device->fill(QRect(w / 4, 0, w / 4 + 1, h), other);
    } else if (job.kind == QLatin1String("erase")) {
        // A wash for erasers to remove
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
    // Most engines only queue their dabs for a stroke to render; with no stroke, run those jobs here
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
            // Alpha is the largest difference across channels
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

/// Painted strokes, kept for the next time the picker opens
struct StrokeCache {
    QString stamp;                      ///< the color and size they were painted with
    QHash<QString, QImage> strokes;     ///< by md5 + name
    QHash<QString, QImage> masks;       ///< by md5 + name
    QHash<QString, QImage> latest;      ///< by name, in any color; used by the canvas ring
};

StrokeCache &strokeCache()
{
    static StrokeCache cache;
    return cache;
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
        for (auto it = m_strokes->shown.constBegin(); it != m_strokes->shown.constEnd(); ++it) {
            if (m_looks.contains(it.key())) continue;
            const QImage mask = m_strokes->masks.value(it.key());
            m_looks.insert(it.key(), measure(mask.isNull() ? it.value() : mask));
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

    /// Queues the next brushes for the worker, a few milliseconds at a time
    void feedSome();
    /// Collects finished strokes from the worker; returns false if there were none
    bool collectSome();
};

KisStudioBrushPicker::KisStudioBrushPicker(KisMainWindow *mainWindow)
    : KisStudioBrushPicker(mainWindow, makeStrokes())
{
}

KisStudioBrushPicker::KisStudioBrushPicker(KisMainWindow *mainWindow, std::shared_ptr<KisStudioBrushStrokes> strokes)
    : KisStudioPicker(brushSource(strokes->catalog, strokes), currentPresetName(mainWindow), QSet<QString>(), mainWindow)
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

    const Source &source = pickerSource();
    auto *map = new BrushMap(strokes, pinnedIds(source), currentPresetName(mainWindow), this);
    map->picked = [this](const QString &id) { pickId(id); };
    addAlternateView(map, i18nc("@action:button the brushes by how they look", "Map"), QStringLiteral("StudioBrushesAsMap"));

    if (!resources || !resources->resourceManager()) {
        return;
    }

    StrokeCache &cache = strokeCache();
    const QString stamp = d->paint.toQColor().name() + QLatin1Char('@') + QString::number(d->size.width());
    if (cache.stamp != stamp) {
        cache.strokes.clear();
        cache.masks.clear();
        cache.stamp = stamp;
    }

    // Paint the brushes that are seen first before the rest
    QStringList order = source.recent() + pinnedIds(source) + d->catalog->ids;
    order.removeDuplicates();
    Q_FOREACH (const QString &id, order) {
        const Brush brush = d->catalog->brushes.value(id);
        const QImage cached = cache.strokes.value(brush.md5 + id);
        if (!cached.isNull()) {
            strokes->shown.insert(id, cached);
            const QImage cachedMask = cache.masks.value(brush.md5 + id);
            if (!cachedMask.isNull()) strokes->masks.insert(id, cachedMask);
        } else if (!brush.name.isEmpty()) {
            d->toQueue << id;
        }
    }
    if (d->toQueue.isEmpty()) {
        return;
    }

    d->image = new KisImage(nullptr, d->size.width(), d->size.height(), rgb, QStringLiteral("brush strokes"));

    (void)QtConcurrent::run([strokes]() {
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
            job = Job();
        }
        strokes->finished = true;
        // Release the jobs' presets and images in the GUI thread, even if the picker is already gone
        QMetaObject::invokeMethod(qApp, [strokes]() {
            QMutexLocker locker(&strokes->mutex);
            strokes->queue.clear();
            strokes->spent.clear();
        }, Qt::QueuedConnection);
    });

    d->feed.setInterval(0);
    connect(&d->feed, &QTimer::timeout, this, [this]() { d->feedSome(); });
    d->feed.start();

    d->collect.setInterval(80);
    connect(&d->collect, &QTimer::timeout, this, [this]() {
        if (d->collectSome()) updatePreviews();
    });
    d->collect.start();
}

bool KisStudioBrushPicker::Private::collectSome()
{
    // Read before collecting, so a finished worker's last strokes are still collected below
    const bool done = strokes->finished;
    QHash<QString, QImage> madeStrokes;
    QHash<QString, QImage> madeMasks;
    QList<Job> spent;
    {
        QMutexLocker locker(&strokes->mutex);
        madeStrokes.swap(strokes->made);
        madeMasks.swap(strokes->madeMasks);
        spent.swap(strokes->spent);
    }
    // Release the preset copies here, in the GUI thread
    spent.clear();
    StrokeCache &cache = strokeCache();
    for (auto it = madeMasks.constBegin(); it != madeMasks.constEnd(); ++it) {
        strokes->masks.insert(it.key(), it.value());
        cache.masks.insert(catalog->brushes.value(it.key()).md5 + it.key(), it.value());
    }
    for (auto it = madeStrokes.constBegin(); it != madeStrokes.constEnd(); ++it) {
        strokes->shown.insert(it.key(), it.value());
        cache.strokes.insert(catalog->brushes.value(it.key()).md5 + it.key(), it.value());
        cache.latest.insert(it.key(), it.value());
    }
    if (done) collect.stop();
    return !madeStrokes.isEmpty();
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
        // The clone brush copies from elsewhere in the image, so it paints nothing here
        if (!preset || !preset->valid() || !preset->settings() || brush.engine == QLatin1String("duplicate")) {
            continue;
        }
        KisPaintOpPresetSP copy = preset->cloneWithResourcesSnapshot(KisGlobalResourcesInterface::instance(),
                                                                     resources->resourceManager()->canvasResourcesInterface(), nullptr);
        if (!copy || !copy->settings()) {
            continue;
        }
        // Big enough to show the tip, small enough to fit the tile
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
    // Not waited for: the worker stops after its current brush
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
    if (preset) {
        resources->setPaintOpPreset(preset);
    }
}

QImage KisStudioBrushPicker::strokePicture(const QString &presetName)
{
    return strokeCache().latest.value(presetName);
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
