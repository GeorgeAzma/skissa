/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisBlendModePicker.h"

#include "kis_config.h"

#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QWidget>

#include <memory>

#include <KoColorSpace.h>
#include <KoColorSpaceRegistry.h>
#include <KoCompositeOp.h>
#include <KoCompositeOpRegistry.h>
#include <klocalizedstring.h>

#include "kis_group_layer.h"
#include "kis_image.h"
#include "kis_layer.h"
#include "kis_node.h"
#include "kis_paint_device.h"

#include <algorithm>

namespace {

using Group = KisStudioPicker::Group;

/// Before the list could be ordered, starred modes came first
const QString oldFavoritesKey = QStringLiteral("StudioFavoriteBlendModes");

/**
 * The list "your modes" starts as: the common modes by intent, the order
 * scrolling stepped through before it could be changed. The ids are the
 * ones of KoCompositeOpRegistry.
 */
QStringList defaultPinned()
{
    return {
        QStringLiteral("normal"),
        // darken
        QStringLiteral("multiply"), QStringLiteral("burn"), QStringLiteral("linear_burn"), QStringLiteral("darken"),
        QStringLiteral("darker color"),
        // lighten
        QStringLiteral("screen"), QStringLiteral("linear_dodge"), QStringLiteral("dodge"), QStringLiteral("lighten"),
        QStringLiteral("lighter color"),
        // contrast
        QStringLiteral("overlay"), QStringLiteral("soft_light"), QStringLiteral("hard_light"), QStringLiteral("vivid_light"),
        QStringLiteral("linear light"), QStringLiteral("pin_light"), QStringLiteral("hard_mix_photoshop"),
        // compare
        QStringLiteral("diff"), QStringLiteral("exclusion"), QStringLiteral("subtract"), QStringLiteral("divide"),
        // color
        QStringLiteral("hue"), QStringLiteral("saturation"), QStringLiteral("color"), QStringLiteral("luminize"),
        // transparency
        QStringLiteral("erase"), QStringLiteral("behind"), QStringLiteral("dissolve"),
    };
}

/**
 * What the mode does, made with its real composite op: a solid warm color
 * blended over a gray ramp (top) and a hue ramp (bottom), so lightness
 * effects and color effects both show. Shown over a checkerboard, and for
 * the transparency modes the backdrop fades out to the right: on an opaque
 * backdrop Behind would look like nothing and Erase like a hole.
 */
QImage swatchFor(const QString &id)
{
    static QHash<QString, QImage> cache;
    auto it = cache.constFind(id);
    if (it != cache.constEnd()) {
        return *it;
    }

    const int w = 30;
    const int h = 20;
    const bool transparencyMode = id == QLatin1String("behind") || id == QLatin1String("erase")
                                  || id == QLatin1String("dissolve");
    QImage dst(w, h, QImage::Format_ARGB32);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const qreal t = qreal(x) / (w - 1);
            QColor c = y < h / 2 ? QColor::fromRgbF(0.08 + 0.86 * t, 0.08 + 0.86 * t, 0.08 + 0.86 * t)
                                 : QColor::fromHsvF(0.55 + 0.45 * t, 0.65, 0.85);
            if (transparencyMode) {
                c.setAlphaF(qBound(0.0, 1.4 - 1.6 * t, 1.0));
            }
            dst.setPixel(x, y, c.rgba());
        }
    }
    QImage src(w, h, QImage::Format_ARGB32);
    src.fill(QColor(200, 85, 61));

    // Krita's 8-bit RGB is BGRA in memory, the same as ARGB32 on little endian
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    const KoCompositeOp *op = cs ? cs->compositeOp(id) : nullptr;
    if (op && op->id() == id) {
        KoCompositeOp::ParameterInfo params;
        params.dstRowStart = dst.bits();
        params.dstRowStride = dst.bytesPerLine();
        params.srcRowStart = src.constBits();
        params.srcRowStride = src.bytesPerLine();
        params.rows = h;
        params.cols = w;
        // dissolve at full opacity is just Normal
        params.opacity = id == QLatin1String("dissolve") ? 0.5f : 1.0f;
        params.flow = 1.0f;
        params.channelFlags = QBitArray();
        op->composite(params);
    }

    QImage swatch(w, h, QImage::Format_ARGB32_Premultiplied);
    QPainter painter(&swatch);
    const int cell = 5;
    for (int y = 0; y < h; y += cell) {
        for (int x = 0; x < w; x += cell) {
            painter.fillRect(x, y, cell, cell, ((x + y) / cell) % 2 ? QColor(150, 150, 150) : QColor(105, 105, 105));
        }
    }
    painter.drawImage(0, 0, dst);
    painter.end();

    cache.insert(id, swatch);
    return swatch;
}

/// \p source in mode \p id over \p backdrop, with the mode's real composite op
QImage compositeWith(const QImage &backdrop, const QImage &source, qreal opacity, const QString &id)
{
    // Krita's 8-bit RGB is BGRA in memory, the same as ARGB32 on little endian
    QImage dst = backdrop.convertToFormat(QImage::Format_ARGB32);
    const QImage src = source.convertToFormat(QImage::Format_ARGB32);
    const KoColorSpace *cs = KoColorSpaceRegistry::instance()->rgb8();
    const KoCompositeOp *op = cs ? cs->compositeOp(id) : nullptr;
    if (op && op->id() == id && dst.size() == src.size()) {
        KoCompositeOp::ParameterInfo params;
        params.dstRowStart = dst.bits();
        params.dstRowStride = dst.bytesPerLine();
        params.srcRowStart = src.constBits();
        params.srcRowStride = src.bytesPerLine();
        params.rows = dst.height();
        params.cols = dst.width();
        // dissolve at full opacity is just Normal
        params.opacity = float(opacity) * (id == QLatin1String("dissolve") ? 0.5f : 1.0f);
        params.flow = 1.0f;
        params.channelFlags = QBitArray();
        op->composite(params);
    }
    return dst;
}

/// A soft round shape of the color: something to blend when the layer is empty
QImage sampleShape(const QSize &size, const QColor &color)
{
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QRectF r(QPointF(0, 0), QSizeF(size));
    QRadialGradient fill(QPointF(r.width() * 0.42, r.height() * 0.5), r.height() * 0.62);
    fill.setColorAt(0.0, color);
    fill.setColorAt(0.75, color);
    QColor edge = color;
    edge.setAlpha(0);
    fill.setColorAt(1.0, edge);
    painter.setPen(Qt::NoPen);
    painter.setBrush(fill);
    painter.drawEllipse(QPointF(r.width() * 0.42, r.height() * 0.5), r.height() * 0.62, r.height() * 0.62);
    painter.end();
    return image.convertToFormat(QImage::Format_ARGB32);
}

/// A thick stroke of the color across the picture, as a brush would paint it
QImage strokeShape(const QSize &size, const QColor &color)
{
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const qreal w = size.width();
    const qreal h = size.height();
    QPainterPath wave;
    wave.moveTo(w * 0.1, h * 0.62);
    wave.cubicTo(w * 0.3, h * 0.05, w * 0.55, h * 1.0, w * 0.9, h * 0.36);
    QPen pen(color, h * 0.3);
    pen.setCapStyle(Qt::RoundCap);
    painter.setPen(pen);
    painter.drawPath(wave);
    painter.end();
    return image.convertToFormat(QImage::Format_ARGB32);
}

/// A picture of the image's shape, scaled to cover \p size and cropped to it in the middle
QImage cropTo(QImage image, const QSize &full, const QSize &size)
{
    if (image.isNull()) {
        return QImage();
    }
    image = image.convertToFormat(QImage::Format_ARGB32);
    if (image.size() != full) {
        image = image.scaled(full, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }
    return image.copy((full.width() - size.width()) / 2, (full.height() - size.height()) / 2, size.width(), size.height());
}

/// "Soft Light (Photoshop)" -> "Soft Light", in your list, where the variant rarely matters
QString withoutQualifier(QString name)
{
    static const QRegularExpression qualifier(QStringLiteral("\\s*\\([^)]*\\)$"));
    return name.remove(qualifier);
}

QString nameOf(const QString &id)
{
    return KoCompositeOpRegistry::instance().getKoID(id).name();
}

/// Every registered mode, once
QList<KoID> allOps()
{
    QList<KoID> ops;
    QSet<QString> seen;
    const auto map = KoCompositeOpRegistry::instance().getCompositeOps();
    for (auto it = map.constBegin(); it != map.constEnd(); ++it) {
        if (!seen.contains(it.value().id())) {
            seen.insert(it.value().id());
            ops << it.value();
        }
    }
    return ops;
}

/// The modes that are not yours, by category, each once, in the order the picker shows them
QVector<Group> otherGroups(const QStringList &pinned)
{
    QVector<Group> groups;
    QSet<QString> seen(pinned.begin(), pinned.end());
    const auto ops = KoCompositeOpRegistry::instance().getCompositeOps();
    Q_FOREACH (const KoID &category, KoCompositeOpRegistry::instance().getCategories()) {
        Group group{category.name(), {}};
        Q_FOREACH (const KoID &op, ops.values(category)) {
            if (!seen.contains(op.id())) {
                seen.insert(op.id());
                group.ids << op.id();
            }
        }
        std::sort(group.ids.begin(), group.ids.end(), [](const QString &a, const QString &b) {
            return nameOf(a).localeAwareCompare(nameOf(b)) < 0;
        });
        if (!group.ids.isEmpty()) groups << group;
    }
    return groups;
}


} // namespace

KisStudioPicker::Source KisBlendModePicker::source(const Pictures &pictures)
{
    Source source;
    source.pinnedKey = QStringLiteral("StudioBlendModes");
    source.initialPinned = []() {
        // the stars of before, then the common modes
        QStringList pinned = KisConfig(true).readEntry<QStringList>(oldFavoritesKey, QStringList());
        Q_FOREACH (const QString &id, defaultPinned()) {
            if (!pinned.contains(id)) pinned << id;
        }
        return pinned;
    };
    source.allIds = []() {
        QStringList ids;
        Q_FOREACH (const KoID &op, allOps()) ids << op.id();
        return ids;
    };
    source.otherGroups = otherGroups;
    source.name = nameOf;
    source.pinnedName = [](const QString &id) { return withoutQualifier(nameOf(id)); };
    if (pictures.isValid()) {
        // what the mode really does to them, made the first time a tile shows
        auto made = std::make_shared<QHash<QString, QImage>>();
        source.tiles = true;
        source.paintPreview = [pictures, made](QPainter &painter, const QRect &rect, const QString &id) {
            if (!made->contains(id)) {
                made->insert(id, compositeWith(pictures.backdrop, pictures.source, pictures.sourceOpacity, id));
            }
            const QImage picture = made->value(id);
            // a checkerboard where it leaves the picture transparent
            const int cell = 8;
            for (int y = rect.top(); y < rect.bottom(); y += cell) {
                for (int x = rect.left(); x < rect.right(); x += cell) {
                    painter.fillRect(QRect(x, y, cell, cell) & rect,
                                     ((x - rect.left() + y - rect.top()) / cell) % 2 ? QColor(150, 150, 150) : QColor(110, 110, 110));
                }
            }
            painter.drawImage(rect, picture);
        };
    } else {
        source.paintPreview = [](QPainter &painter, const QRect &rect, const QString &id) {
            painter.drawImage(rect, swatchFor(id));
        };
    }
    source.yoursTitle = i18nc("blend mode group", "Your modes");
    source.searchPlaceholder = i18nc("@info:placeholder", "Search all %1 blend modes", allOps().size());
    // the number goes in later: %1 passes through
    source.allLabel = i18nc("@action:button %1 is a number", "All %1 modes", QStringLiteral("%1"));
    return source;
}

KisBlendModePicker::KisBlendModePicker(const QString &current, const QSet<QString> &available, QWidget *parent,
                                       const Pictures &pictures)
    : KisStudioPicker(source(pictures), current, available, parent)
{
    connect(this, &KisStudioPicker::sigPreview, this, &KisBlendModePicker::previewed);
    connect(this, &KisStudioPicker::sigPicked, this, [this](const QString &id) {
        Q_EMIT picked(KoCompositeOpRegistry::instance().getKoID(id));
    });
}

QStringList KisBlendModePicker::pinnedModes()
{
    return pinnedIds(source());
}

void KisBlendModePicker::setPinnedModes(const QStringList &ids)
{
    setPinnedIds(source(), ids);
}

QStringList KisBlendModePicker::scrollOrder()
{
    return KisStudioPicker::scrollOrder(source());
}

QSize KisBlendModePicker::pictureSize(const QWidget *widget)
{
    // the tiles' picture, see KisStudioPicker
    return QSize(112, 64) * (widget ? widget->devicePixelRatioF() : 1.0);
}

KisBlendModePicker::Pictures KisBlendModePicker::layerPictures(KisNodeSP node, const QSize &size, const QColor &color)
{
    // a mask blends nothing itself: its layer does
    while (node && !node->inherits("KisLayer")) {
        node = node->parent();
    }
    KisImageSP image = node ? KisImageSP(node->image()) : KisImageSP();
    if (!node || !image || !node->parent() || size.isEmpty()) {
        return Pictures();
    }
    const QSize full = image->bounds().size().scaled(size, Qt::KeepAspectRatioByExpanding);

    /**
     * Everything below the layer, bottom up, each in its own mode: groups
     * are looked into rather than blended as one, which is close enough
     * for a picture this small.
     */
    // what is under all the layers: the image's own background color (white, often)
    QImage backdrop(size, QImage::Format_ARGB32);
    backdrop.fill(image->defaultProjectionColor().toQColor());
    bool reached = false;
    std::function<void(KisNodeSP)> below = [&](KisNodeSP parent) {
        for (KisNodeSP child = parent->firstChild(); child && !reached; child = child->nextSibling()) {
            if (child == node) {
                reached = true;
                return;
            }
            if (!child->inherits("KisLayer") || child->inherits("KisAdjustmentLayer")) {
                continue;
            }
            if (child->inherits("KisGroupLayer") && child->childCount() > 0) {
                below(child);
                continue;
            }
            if (!child->visible(true)) {
                continue;
            }
            const QImage picture = child->projection()
                ? cropTo(child->projection()->createThumbnailUncached(full.width(), full.height(), image->bounds()), full, size)
                : QImage();
            if (!picture.isNull()) {
                backdrop = compositeWith(backdrop, picture, child->opacity() / 255.0, child->compositeOpId());
            }
        }
    };
    below(image->root());

    Pictures pictures;
    pictures.backdrop = KisStudioPicker::isFlat(backdrop) ? KisStudioPicker::samplePicture(size).convertToFormat(QImage::Format_ARGB32) : backdrop;
    // the whole image's area, so that it lines up with the layers below (a cached thumbnail is only the painted part)
    const QImage layer = node->projection()
        ? cropTo(node->projection()->createThumbnailUncached(full.width(), full.height(), image->bounds()), full, size)
        : QImage();
    pictures.source = KisStudioPicker::isFlat(layer) ? sampleShape(size, color) : layer;
    pictures.sourceOpacity = node->opacity() / 255.0;
    return pictures;
}

KisBlendModePicker::Pictures KisBlendModePicker::brushPictures(KisImageSP image, const QSize &size, const QColor &color)
{
    if (size.isEmpty()) {
        return Pictures();
    }
    QImage backdrop;
    if (image && image->projection()) {
        const QSize full = image->bounds().size().scaled(size, Qt::KeepAspectRatioByExpanding);
        backdrop = QImage(size, QImage::Format_ARGB32);
        backdrop.fill(image->defaultProjectionColor().toQColor());
        const QImage projection = cropTo(image->projection()->createThumbnailUncached(full.width(), full.height(), image->bounds()), full, size);
        backdrop = compositeWith(backdrop, projection, 1.0, QStringLiteral("normal"));
    }
    Pictures pictures;
    pictures.backdrop = KisStudioPicker::isFlat(backdrop) ? KisStudioPicker::samplePicture(size).convertToFormat(QImage::Format_ARGB32) : backdrop;
    pictures.source = strokeShape(size, color);
    return pictures;
}
