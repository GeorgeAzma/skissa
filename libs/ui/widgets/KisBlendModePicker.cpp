/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisBlendModePicker.h"

#include "KisPopupCloser.h"
#include "KisStudioMotion.h"
#include "kis_config.h"

#include <QAbstractButton>
#include <QApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QVBoxLayout>

#include <KSharedConfig>
#include <kconfiggroup.h>

#include <KoColorSpace.h>
#include <KoColorSpaceRegistry.h>
#include <KoCompositeOp.h>
#include <KoCompositeOpRegistry.h>
#include <kfts_fuzzy_match.h>
#include <klocalizedstring.h>

#include <algorithm>
#include <cmath>

namespace {

constexpr int shadowMargin = 16;
constexpr int columnWidth = 196;
constexpr int rowHeight = 32;
constexpr int headerHeight = 26;
constexpr int columns = 3;

const QString pinnedKey = QStringLiteral("StudioBlendModes");
/// Before the list could be ordered, starred modes came first
const QString oldFavoritesKey = QStringLiteral("StudioFavoriteBlendModes");

struct Group {
    QString title;
    QStringList ids;
};

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

/// "Soft Light (Photoshop)" -> "Soft Light", in your list, where the variant rarely matters
QString withoutQualifier(QString name)
{
    static const QRegularExpression qualifier(QStringLiteral("\\s*\\([^)]*\\)$"));
    return name.remove(qualifier);
}

QRectF starRect(const QRect &row)
{
    return QRectF(row.right() - 20, row.center().y() - 6.5, 13, 13);
}

QPolygonF starShape(const QRectF &r)
{
    QPolygonF star;
    for (int i = 0; i < 10; i++) {
        const qreal radius = (i % 2 ? 0.42 : 1.0) * r.width() / 2;
        const qreal angle = M_PI / 2 + i * M_PI / 5;
        star << r.center() + QPointF(radius * std::cos(angle), -radius * std::sin(angle));
    }
    return star;
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

struct Item {
    QString id;
    QString name;
    QRect rect;
    int order {-1}; ///< the place in your modes, -1 for the others
};

struct Header {
    QString title;
    QRect rect;
};

} // namespace

struct KisBlendModePicker::Private
{
    QString original;
    QSet<QString> available;
    QStringList pinned;
    std::function<void()> relayout;
    bool showAll {false};
    bool done {false};
    int highlighted {-1};

    QLineEdit *search {nullptr};
    QWidget *grid {nullptr};
    QScrollArea *scroll {nullptr};
    QAbstractButton *allButton {nullptr};
    QLabel *hint {nullptr};

    QVector<Item> items;
    QVector<Header> headers;
    /// Where your modes are, a drop there puts a mode among them; empty while searching
    QRect pinnedArea;

    bool searching() const { return !search->text().trimmed().isEmpty(); }

    void setPinned(const QStringList &ids)
    {
        pinned = ids;
        KisBlendModePicker::setPinnedModes(ids);
        if (relayout) relayout();
    }
};

namespace {

/**
 * The painted grid of modes inside the picker. A press and a release picks,
 * a press and a drag moves the mode: among your modes to reorder them, into
 * them to add it, out of them to take it off.
 */
class Grid : public QWidget
{
public:
    Grid(KisBlendModePicker::Private *d, std::function<void(int)> hover, std::function<void(int)> pick, QWidget *parent)
        : QWidget(parent), m_d(d), m_hover(std::move(hover)), m_pick(std::move(pick))
    {
        setMouseTracking(true);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QPalette &pal = palette();
        QColor dim = pal.color(QPalette::WindowText);
        dim.setAlphaF(0.5);

        QFont headerFont = font();
        headerFont.setPointSizeF(headerFont.pointSizeF() * 0.8);
        headerFont.setBold(true);
        headerFont.setCapitalization(QFont::AllUppercase);
        headerFont.setLetterSpacing(QFont::PercentageSpacing, 106);
        painter.setFont(headerFont);
        painter.setPen(dim);
        Q_FOREACH (const Header &header, m_d->headers) {
            painter.drawText(header.rect.adjusted(8, 0, 0, -2), Qt::AlignLeft | Qt::AlignBottom, header.title);
        }

        QFont numberFont = font();
        numberFont.setPointSizeF(numberFont.pointSizeF() * 0.8);
        QColor numberInk = pal.color(QPalette::WindowText);
        numberInk.setAlphaF(0.35);

        for (int i = 0; i < m_d->items.size(); i++) {
            const Item &item = m_d->items[i];
            const bool current = item.id == m_d->original;
            const bool highlighted = i == m_d->highlighted && m_dragging < 0;
            const bool enabled = m_d->available.contains(item.id);
            painter.setOpacity(i == m_dragging ? 0.3 : 1.0);

            if (highlighted || current) {
                const QColor base = pal.color(QPalette::Window);
                const QColor select = pal.color(QPalette::Highlight);
                const qreal t = highlighted ? 0.55 : 0.28;
                painter.setPen(Qt::NoPen);
                painter.setBrush(QColor::fromRgbF(base.redF() + (select.redF() - base.redF()) * t,
                                                  base.greenF() + (select.greenF() - base.greenF()) * t,
                                                  base.blueF() + (select.blueF() - base.blueF()) * t));
                painter.drawRoundedRect(QRectF(item.rect).adjusted(2, 1, -2, -1), 6, 6);
            }

            // your modes are numbered: the order scrolling goes in
            const int left = item.order >= 0 ? 22 : 8;
            if (item.order >= 0) {
                painter.setFont(numberFont);
                painter.setPen(numberInk);
                painter.drawText(QRect(item.rect.left() + 2, item.rect.top(), 18, item.rect.height()),
                                 Qt::AlignRight | Qt::AlignVCenter, QString::number(item.order + 1));
            }

            const QRect swatchRect(item.rect.left() + left, item.rect.center().y() - 10, 30, 20);
            QPainterPath clip;
            clip.addRoundedRect(QRectF(swatchRect), 4, 4);
            painter.save();
            painter.setClipPath(clip);
            painter.setOpacity(painter.opacity() * (enabled ? 1.0 : 0.35));
            painter.drawImage(swatchRect, swatchFor(item.id));
            painter.restore();

            painter.setFont(font());
            painter.setPen(enabled ? pal.color(QPalette::WindowText) : dim);
            const int textLeft = left + 38;
            painter.drawText(item.rect.adjusted(textLeft, 0, -24, 0), Qt::AlignVCenter | Qt::AlignLeft,
                             painter.fontMetrics().elidedText(item.name, Qt::ElideRight, item.rect.width() - textLeft - 26));

            // the star of the row pointed at: full for your modes, empty for the others; among
            // search matches your modes always show theirs, as there is no other way to tell
            const bool pinned = item.order >= 0 || m_d->pinned.contains(item.id);
            if (highlighted || (pinned && item.order < 0)) {
                QColor ink = pinned ? pal.color(QPalette::Link) : pal.color(QPalette::WindowText);
                ink.setAlphaF(pinned ? 0.9 : 0.45);
                painter.setPen(QPen(ink, 1.1));
                painter.setBrush(pinned ? ink : Qt::NoBrush);
                painter.drawPolygon(starShape(starRect(item.rect)));
            }
        }
        painter.setOpacity(1.0);

        // where a dragged mode would go
        if (m_dragging >= 0) {
            if (!m_dropLine.isNull()) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(pal.color(QPalette::Highlight));
                painter.drawRoundedRect(QRectF(m_dropLine), 1.5, 1.5);
            } else if (m_dropUnpins) {
                QColor wash = pal.color(QPalette::WindowText);
                wash.setAlphaF(0.06);
                painter.setPen(Qt::NoPen);
                painter.setBrush(wash);
                painter.drawRoundedRect(QRectF(0, m_d->pinnedArea.bottom() + 4, width(), height() - m_d->pinnedArea.bottom() - 4), 8, 8);
            }
            // the mode itself, beside the pointer, where it does not hide the line
            const Item &dragged = m_d->items[m_dragging];
            const QRect ghost(m_pointer + QPoint(14, 10), QSize(columnWidth - 8, rowHeight - 2));
            QColor card = pal.color(QPalette::Window).lighter(125);
            painter.setPen(QPen(pal.color(QPalette::Highlight), 1));
            painter.setBrush(card);
            painter.drawRoundedRect(QRectF(ghost), 6, 6);
            const QRect swatchRect(ghost.left() + 8, ghost.center().y() - 10, 30, 20);
            painter.drawImage(swatchRect, swatchFor(dragged.id));
            painter.setPen(pal.color(QPalette::WindowText));
            painter.drawText(ghost.adjusted(46, 0, -6, 0), Qt::AlignVCenter | Qt::AlignLeft,
                             painter.fontMetrics().elidedText(dragged.name, Qt::ElideRight, ghost.width() - 52));
        }
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        m_press = event->button() == Qt::LeftButton ? itemAt(event->pos()) : -1;
        m_pressPos = event->pos();
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (m_press >= 0 && (event->buttons() & Qt::LeftButton)) {
            if (m_dragging < 0 && (event->pos() - m_pressPos).manhattanLength() >= QApplication::startDragDistance()
                && !m_d->searching()) {
                m_dragging = m_press;
                m_hover(-1);
            }
            if (m_dragging >= 0) {
                m_pointer = event->pos();
                updateDrop();
                update();
                return;
            }
        }
        m_hover(itemAt(event->pos()));
    }

    void leaveEvent(QEvent *) override
    {
        if (m_dragging < 0) m_hover(-1);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        const int pressed = m_press;
        m_press = -1;
        if (m_dragging >= 0) {
            drop();
            return;
        }
        const int index = itemAt(event->pos());
        if (index < 0 || index != pressed || event->button() != Qt::LeftButton) {
            return;
        }
        // the star keeps a mode in your list or takes it off, the rest of the row picks
        const Item &item = m_d->items[index];
        if (starRect(item.rect).adjusted(-4, -6, 4, 6).contains(event->pos())) {
            QStringList pinned = m_d->pinned;
            if (pinned.contains(item.id)) pinned.removeAll(item.id);
            else pinned << item.id;
            m_d->setPinned(pinned);
            return;
        }
        m_pick(index);
    }

private:
    int itemAt(const QPoint &pos) const
    {
        for (int i = 0; i < m_d->items.size(); i++) {
            if (m_d->items[i].rect.contains(pos)) return i;
        }
        return -1;
    }

    /// Where the dragged mode goes: a place among your modes, or out of them
    void updateDrop()
    {
        m_dropAt = -1;
        m_dropLine = QRect();
        m_dropUnpins = false;
        const QRect area = m_d->pinnedArea.adjusted(0, 0, 0, rowHeight / 2);
        if (!area.contains(m_pointer)) {
            m_dropUnpins = m_d->items[m_dragging].order >= 0;
            return;
        }
        // before the row under the pointer, or after it in its lower half
        const int column = qBound(0, m_pointer.x() / columnWidth, columns - 1);
        int lastInColumn = -1;
        for (int i = 0; i < m_d->items.size(); i++) {
            const Item &item = m_d->items[i];
            if (item.order < 0 || item.rect.left() != column * columnWidth) continue;
            lastInColumn = i;
            if (m_pointer.y() < item.rect.center().y()) {
                m_dropAt = item.order;
                m_dropLine = QRect(item.rect.left() + 6, item.rect.top() - 2, columnWidth - 12, 3);
                return;
            }
        }
        if (lastInColumn >= 0) {
            const Item &last = m_d->items[lastInColumn];
            m_dropAt = last.order + 1;
            m_dropLine = QRect(last.rect.left() + 6, last.rect.bottom() - 1, columnWidth - 12, 3);
        } else {
            m_dropAt = m_d->pinned.size();
        }
    }

    void drop()
    {
        const QString id = m_d->items[m_dragging].id;
        const int from = m_d->pinned.indexOf(id);
        QStringList pinned = m_d->pinned;
        if (m_dropAt >= 0) {
            int to = m_dropAt;
            if (from >= 0) {
                pinned.removeAt(from);
                if (to > from) to--;
            }
            pinned.insert(qBound(0, to, pinned.size()), id);
        } else if (m_dropUnpins) {
            pinned.removeAll(id);
        }
        m_dragging = -1;
        m_dropLine = QRect();
        m_dropUnpins = false;
        if (pinned != m_d->pinned) {
            m_d->setPinned(pinned);
        } else {
            update();
        }
    }

    KisBlendModePicker::Private *m_d;
    std::function<void(int)> m_hover;
    std::function<void(int)> m_pick;

    int m_press {-1};
    QPoint m_pressPos;
    int m_dragging {-1};
    QPoint m_pointer;
    int m_dropAt {-1};       ///< the place among your modes it would take
    bool m_dropUnpins {false};
    QRect m_dropLine;
};

/// A text button that looks like a link
class LinkButton : public QAbstractButton
{
public:
    using QAbstractButton::QAbstractButton;
    QSize sizeHint() const override
    {
        return QSize(fontMetrics().horizontalAdvance(QString(text()).remove(QLatin1Char('&'))) + 12, 24);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        QColor color = palette().color(QPalette::Link);
        if (!underMouse()) color.setAlphaF(0.85);
        painter.setPen(color);
        painter.drawText(rect(), Qt::AlignVCenter | Qt::AlignRight | Qt::TextHideMnemonic, text());
    }
};

} // namespace

KisBlendModePicker::KisBlendModePicker(const QString &current, const QSet<QString> &available, QWidget *parent)
    // not Qt::Popup: translucent popups render a black background on Windows
    : QFrame(parent, Qt::Tool | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint)
    , d(new Private)
{
    d->original = current;
    d->available = available;
    d->pinned = pinnedModes();
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_DeleteOnClose);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(shadowMargin + 10, shadowMargin + 10, shadowMargin + 10, shadowMargin + 8);
    layout->setSpacing(4);

    d->search = new QLineEdit(this);
    d->search->setPlaceholderText(i18nc("@info:placeholder", "Search all %1 blend modes", allOps().size()));
    d->search->setMinimumHeight(32);
    d->search->installEventFilter(this);
    layout->addWidget(d->search);

    auto hover = [this](int index) {
        if (index == d->highlighted) return;
        d->highlighted = index;
        d->grid->update();
        if (index >= 0 && d->available.contains(d->items[index].id)) {
            Q_EMIT previewed(d->items[index].id);
        }
    };
    auto pick = [this](int index) {
        const Item &item = d->items[index];
        if (!d->available.contains(item.id)) return;
        d->done = true;
        Q_EMIT previewed(QString());
        Q_EMIT picked(KoCompositeOpRegistry::instance().getKoID(item.id));
        close();
    };

    d->grid = new Grid(d.get(), hover, pick, nullptr);
    d->scroll = new QScrollArea(this);
    d->scroll->setWidget(d->grid);
    d->scroll->setFrameShape(QFrame::NoFrame);
    d->scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    d->scroll->setWidgetResizable(false);
    d->scroll->viewport()->setAutoFillBackground(false);
    d->grid->setAutoFillBackground(false);
    layout->addWidget(d->scroll);

    auto *footer = new QHBoxLayout();
    footer->setContentsMargins(8, 2, 4, 0);
    d->hint = new QLabel(this);
    d->hint->setText(i18nc("@info", "Drag to reorder, the star adds or removes"));
    QColor hintInk = d->hint->palette().color(QPalette::WindowText);
    hintInk.setAlphaF(0.45);
    QPalette hintPalette = d->hint->palette();
    hintPalette.setColor(QPalette::WindowText, hintInk);
    d->hint->setPalette(hintPalette);
    footer->addWidget(d->hint, 1);
    auto *all = new LinkButton(this);
    all->setCursor(Qt::PointingHandCursor);
    d->allButton = all;
    footer->addWidget(all);
    layout->addLayout(footer);

    // your modes first, numbered, in three columns read downwards; then the others by category, or the matches
    auto relayout = [this]() {
        d->items.clear();
        d->headers.clear();
        d->pinnedArea = QRect();
        const QString query = d->search->text().trimmed();

        auto placeGroups = [this](const QVector<Group> &groups, int top, bool balanceOne) {
            QVector<QVector<Group>> cols(columns);
            QVector<int> heights(columns, 0);
            if (groups.size() == 1 && balanceOne) {
                const Group &only = groups.first();
                const int perColumn = (only.ids.size() + columns - 1) / columns;
                for (int c = 0; c < columns; c++) {
                    Group part{c == 0 ? only.title : QString(), only.ids.mid(c * perColumn, perColumn)};
                    if (!part.ids.isEmpty()) cols[c] << part;
                }
            } else {
                Q_FOREACH (const Group &group, groups) {
                    const int c = int(std::min_element(heights.begin(), heights.end()) - heights.begin());
                    cols[c] << group;
                    heights[c] += headerHeight + group.ids.size() * rowHeight;
                }
            }
            int bottom = top;
            for (int c = 0; c < columns; c++) {
                int y = top;
                Q_FOREACH (const Group &group, cols[c]) {
                    if (!group.title.isEmpty() || y > top) {
                        d->headers << Header{group.title, QRect(c * columnWidth, y, columnWidth, headerHeight)};
                        y += headerHeight;
                    }
                    Q_FOREACH (const QString &id, group.ids) {
                        d->items << Item{id, nameOf(id), QRect(c * columnWidth, y, columnWidth, rowHeight), -1};
                        y += rowHeight;
                    }
                }
                bottom = std::max(bottom, y);
            }
            return bottom;
        };

        int height = 0;
        if (!query.isEmpty()) {
            QVector<QPair<int, QString>> scored;
            Q_FOREACH (const KoID &id, allOps()) {
                const int at = id.name().indexOf(query, 0, Qt::CaseInsensitive);
                if (at >= 0) {
                    const bool wordStart = at == 0 || !id.name().at(at - 1).isLetterOrNumber();
                    scored.append(qMakePair(1000 - (wordStart ? 0 : 200) - at, id.id()));
                }
            }
            if (scored.size() < 6) {
                Q_FOREACH (const KoID &id, allOps()) {
                    int score = 0;
                    const bool have = std::any_of(scored.begin(), scored.end(), [&id](const auto &s) { return s.second == id.id(); });
                    if (!have && kfts::fuzzy_match(query, id.name(), score)) {
                        scored.append(qMakePair(score, id.id()));
                    }
                }
            }
            std::stable_sort(scored.begin(), scored.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
            Group matches{i18nc("blend mode group", "Matches"), {}};
            for (const auto &s : scored) matches.ids << s.second;
            height = placeGroups({matches}, 0, true);
        } else {
            // read downwards, column by column, like a list
            const int count = d->pinned.size();
            const int perColumn = std::max(1, (count + columns - 1) / columns);
            d->headers << Header{i18nc("blend mode group", "Your modes"), QRect(0, 0, columnWidth, headerHeight)};
            for (int i = 0; i < count; i++) {
                const QString &id = d->pinned[i];
                const QRect rect((i / perColumn) * columnWidth, headerHeight + (i % perColumn) * rowHeight, columnWidth, rowHeight);
                d->items << Item{id, withoutQualifier(nameOf(id)), rect, i};
            }
            height = headerHeight + std::max(1, std::min(count, perColumn)) * rowHeight;
            d->pinnedArea = QRect(0, 0, columns * columnWidth, height);
            if (d->showAll) {
                height = placeGroups(otherGroups(d->pinned), height + 8, false);
            }
        }

        d->grid->setFixedSize(columns * columnWidth, std::max(height, rowHeight));
        const int maxHeight = 460;
        d->scroll->setFixedSize(columns * columnWidth + (height > maxHeight ? d->scroll->verticalScrollBar()->sizeHint().width() : 0),
                                std::min(height, maxHeight) + 2);
        d->scroll->setVerticalScrollBarPolicy(height > maxHeight ? Qt::ScrollBarAsNeeded : Qt::ScrollBarAlwaysOff);
        d->allButton->setText(d->showAll ? i18nc("@action:button", "Only yours")
                                         : i18nc("@action:button", "All %1 modes", allOps().size()));
        d->allButton->setVisible(query.isEmpty());
        d->hint->setVisible(query.isEmpty());

        d->highlighted = -1;
        for (int i = 0; i < d->items.size(); i++) {
            if (d->items[i].id == d->original) d->highlighted = i;
        }
        if (!query.isEmpty()) d->highlighted = d->items.isEmpty() ? -1 : 0;
        d->grid->update();
        adjustSize();
    };

    d->relayout = relayout;
    connect(d->search, &QLineEdit::textChanged, this, relayout);
    connect(all, &QAbstractButton::clicked, this, [this, relayout]() {
        d->showAll = !d->showAll;
        relayout();
    });
    // a current mode that is not one of yours: show it among the others
    d->showAll = !d->pinned.contains(current);
    relayout();
}

KisBlendModePicker::~KisBlendModePicker() = default;

QStringList KisBlendModePicker::pinnedModes()
{
    KisConfig cfg(true);
    QStringList pinned;
    // an empty list is a list too: the key says whether you made one
    if (KSharedConfig::openConfig()->group(QString()).hasKey(pinnedKey)) {
        pinned = cfg.readEntry<QStringList>(pinnedKey, QStringList());
    } else {
        // first time: the stars of before, then the common modes
        pinned = cfg.readEntry<QStringList>(oldFavoritesKey, QStringList());
        Q_FOREACH (const QString &id, defaultPinned()) {
            if (!pinned.contains(id)) pinned << id;
        }
    }
    // modes this version does not have
    QSet<QString> known;
    Q_FOREACH (const KoID &op, allOps()) known.insert(op.id());
    pinned.erase(std::remove_if(pinned.begin(), pinned.end(), [&known](const QString &id) { return !known.contains(id); }), pinned.end());
    pinned.removeDuplicates();
    return pinned;
}

void KisBlendModePicker::setPinnedModes(const QStringList &ids)
{
    KisConfig(false).writeEntry(pinnedKey, ids);
}

QStringList KisBlendModePicker::scrollOrder()
{
    QStringList order = pinnedModes();
    Q_FOREACH (const Group &group, otherGroups(order)) {
        order << group.ids;
    }
    return order;
}

void KisBlendModePicker::popupBelow(QWidget *anchor)
{
    adjustSize();
    QPoint pos = anchor->mapToGlobal(QPoint(0, anchor->height())) - QPoint(shadowMargin, shadowMargin - 4);
    if (QScreen *screen = anchor->screen()) {
        const QRect available = screen->availableGeometry();
        pos.setX(qBound(available.left(), pos.x(), available.right() - width()));
        pos.setY(qBound(available.top(), pos.y(), available.bottom() - height()));
    }
    move(pos);
    new KisPopupCloser(this, anchor);
    KisStudioMotion::showFaded(this);
    raise();
    activateWindow();
    d->search->setFocus();
    // the current mode in view, also when it is far down among all the others
    if (d->highlighted >= 0) {
        const QRect rect = d->items[d->highlighted].rect;
        d->scroll->ensureVisible(rect.center().x(), rect.center().y(), 0, rowHeight * 2);
    }
}

bool KisBlendModePicker::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == d->search && event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        const int count = d->items.size();
        auto move = [this, count](int delta) {
            if (!count) return;
            int index = d->highlighted < 0 ? 0 : qBound(0, d->highlighted + delta, count - 1);
            d->highlighted = index;
            d->grid->update();
            d->scroll->ensureVisible(d->items[index].rect.center().x(), d->items[index].rect.center().y(), 0, rowHeight);
            if (d->available.contains(d->items[index].id)) {
                Q_EMIT previewed(d->items[index].id);
            }
        };
        switch (key->key()) {
        case Qt::Key_Down: move(1); return true;
        case Qt::Key_Up: move(-1); return true;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            if (d->highlighted >= 0 && d->available.contains(d->items[d->highlighted].id)) {
                d->done = true;
                Q_EMIT previewed(QString());
                Q_EMIT picked(KoCompositeOpRegistry::instance().getKoID(d->items[d->highlighted].id));
                close();
            }
            return true;
        case Qt::Key_Escape:
            close();
            return true;
        default:
            break;
        }
    }
    return QFrame::eventFilter(watched, event);
}

void KisBlendModePicker::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QRectF panel = QRectF(rect()).adjusted(shadowMargin, shadowMargin, -shadowMargin, -shadowMargin);
    const qreal radius = 12;
    for (int i = shadowMargin; i > 0; i -= 2) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0, 0, 0, int(34.0 * (1.0 - qreal(i) / shadowMargin) + 2)));
        painter.drawRoundedRect(panel.adjusted(-i, -i + 4, i, i + 4), radius + i, radius + i);
    }
    QColor border = palette().color(QPalette::WindowText);
    border.setAlphaF(0.08);
    painter.setPen(QPen(border, 1));
    painter.setBrush(palette().color(QPalette::Window));
    painter.drawRoundedRect(panel.adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
}

void KisBlendModePicker::hideEvent(QHideEvent *event)
{
    // closed without picking: back to the original mode
    if (!d->done) {
        d->done = true;
        Q_EMIT previewed(QString());
    }
    QFrame::hideEvent(event);
}
