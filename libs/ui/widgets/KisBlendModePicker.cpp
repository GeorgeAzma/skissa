/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisBlendModePicker.h"

#include "KisPopupCloser.h"
#include "kis_config.h"

#include <QAbstractButton>
#include <QApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QVBoxLayout>

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
constexpr int columnWidth = 182;
constexpr int rowHeight = 32;
constexpr int headerHeight = 26;
constexpr int columns = 3;

struct Group {
    QString title;
    QStringList ids;
};

/**
 * The essential modes, by intent, in the order scrolling steps through them.
 * The ids are the ones of KoCompositeOpRegistry.
 */
QVector<Group> essentialGroups()
{
    return {
        {i18nc("blend mode group", "Normal"), {QStringLiteral("normal")}},
        {i18nc("blend mode group", "Darken"), {QStringLiteral("multiply"), QStringLiteral("burn"), QStringLiteral("linear_burn"),
                                               QStringLiteral("darken"), QStringLiteral("darker color")}},
        {i18nc("blend mode group", "Lighten"), {QStringLiteral("screen"), QStringLiteral("linear_dodge"), QStringLiteral("dodge"),
                                                QStringLiteral("lighten"), QStringLiteral("lighter color")}},
        {i18nc("blend mode group", "Contrast"), {QStringLiteral("overlay"), QStringLiteral("soft_light"), QStringLiteral("hard_light"),
                                                 QStringLiteral("vivid_light"), QStringLiteral("linear light"), QStringLiteral("pin_light"),
                                                 QStringLiteral("hard_mix_photoshop")}},
        {i18nc("blend mode group", "Compare"), {QStringLiteral("diff"), QStringLiteral("exclusion"), QStringLiteral("subtract"),
                                                QStringLiteral("divide")}},
        {i18nc("blend mode group", "Color"), {QStringLiteral("hue"), QStringLiteral("saturation"), QStringLiteral("color"),
                                              QStringLiteral("luminize")}},
        {i18nc("blend mode group", "Transparency"), {QStringLiteral("erase"), QStringLiteral("behind"), QStringLiteral("dissolve")}},
    };
}

/// The same groups in three columns of about equal height
QVector<QVector<Group>> essentialColumns()
{
    const QVector<Group> g = essentialGroups();
    return {{g[0], g[1], g[6]}, {g[2], g[4]}, {g[3], g[5]}};
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

/// "Soft Light (Photoshop)" -> "Soft Light", where only one of the variants is shown
QString withoutQualifier(QString name)
{
    static const QRegularExpression qualifier(QStringLiteral("\\s*\\([^)]*\\)$"));
    return name.remove(qualifier);
}

const QString favoritesKey = QStringLiteral("StudioFavoriteBlendModes");

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

struct Item {
    QString id;
    QString name;
    QRect rect;
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
    QStringList favorites;
    std::function<void()> relayout;
    bool showAll {false};
    bool done {false};
    int highlighted {-1};

    QLineEdit *search {nullptr};
    QWidget *grid {nullptr};
    QScrollArea *scroll {nullptr};
    QAbstractButton *allButton {nullptr};

    QVector<Item> items;
    QVector<Header> headers;

    QString nameOf(const QString &id) const
    {
        return KoCompositeOpRegistry::instance().getKoID(id).name();
    }
};

namespace {

/// The painted grid of modes inside the picker
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

        painter.setFont(font());
        for (int i = 0; i < m_d->items.size(); i++) {
            const Item &item = m_d->items[i];
            const bool current = item.id == m_d->original;
            const bool highlighted = i == m_d->highlighted;
            const bool enabled = m_d->available.contains(item.id);

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

            const QRect swatchRect(item.rect.left() + 8, item.rect.center().y() - 10, 30, 20);
            QPainterPath clip;
            clip.addRoundedRect(QRectF(swatchRect), 4, 4);
            painter.save();
            painter.setClipPath(clip);
            painter.setOpacity(enabled ? 1.0 : 0.35);
            painter.drawImage(swatchRect, swatchFor(item.id));
            painter.restore();

            painter.setPen(enabled ? pal.color(QPalette::WindowText) : dim);
            painter.drawText(item.rect.adjusted(46, 0, -24, 0), Qt::AlignVCenter | Qt::AlignLeft,
                             painter.fontMetrics().elidedText(item.name, Qt::ElideRight, item.rect.width() - 72));

            const bool favorite = m_d->favorites.contains(item.id);
            if (favorite || highlighted) {
                QColor ink = favorite ? pal.color(QPalette::Link) : pal.color(QPalette::WindowText);
                ink.setAlphaF(favorite ? 0.9 : 0.45);
                painter.setPen(QPen(ink, 1.1));
                painter.setBrush(favorite ? ink : Qt::NoBrush);
                painter.drawPolygon(starShape(starRect(item.rect)));
            }
        }
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        m_hover(itemAt(event->pos()));
    }

    void leaveEvent(QEvent *) override
    {
        m_hover(-1);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        const int index = itemAt(event->pos());
        if (index >= 0 && event->button() == Qt::LeftButton) {
            // the star makes a favorite, the rest of the row picks
            if (starRect(m_d->items[index].rect).adjusted(-4, -6, 4, 6).contains(event->pos())) {
                KisBlendModePicker::setFavorite(m_d->items[index].id, !m_d->favorites.contains(m_d->items[index].id));
                m_d->favorites = KisBlendModePicker::favoriteModes();
                if (m_d->relayout) m_d->relayout();
                return;
            }
            m_pick(index);
        }
    }

private:
    int itemAt(const QPoint &pos) const
    {
        for (int i = 0; i < m_d->items.size(); i++) {
            if (m_d->items[i].rect.contains(pos)) return i;
        }
        return -1;
    }

    KisBlendModePicker::Private *m_d;
    std::function<void(int)> m_hover;
    std::function<void(int)> m_pick;
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
    d->favorites = favoriteModes();
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
    auto *hint = new QWidget(this);
    hint->setFixedHeight(24);
    footer->addWidget(hint, 1);
    auto *all = new LinkButton(this);
    all->setText(i18nc("@action:button", "All %1 modes", allOps().size()));
    all->setCursor(Qt::PointingHandCursor);
    d->allButton = all;
    footer->addWidget(all);
    layout->addLayout(footer);

    // lays the modes out in columns; the essentials by intent, the rest by category
    auto relayout = [this]() {
        d->items.clear();
        d->headers.clear();
        const QString query = d->search->text().trimmed();

        QVector<QVector<Group>> cols;
        const bool essentials = query.isEmpty() && !d->showAll;
        if (!essentials) {
            QVector<Group> groups;
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
                groups << matches;
            } else {
                const auto ops = KoCompositeOpRegistry::instance().getCompositeOps();
                Q_FOREACH (const KoID &category, KoCompositeOpRegistry::instance().getCategories()) {
                    Group group{category.name(), {}};
                    Q_FOREACH (const KoID &op, ops.values(category)) group.ids << op.id();
                    std::sort(group.ids.begin(), group.ids.end(), [this](const QString &a, const QString &b) {
                        return d->nameOf(a).localeAwareCompare(d->nameOf(b)) < 0;
                    });
                    if (!group.ids.isEmpty()) groups << group;
                }
            }
            // balance the groups over the columns by height
            cols.resize(columns);
            QVector<int> heights(columns, 0);
            if (groups.size() == 1) {
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
        } else {
            cols = essentialColumns();
            // favorites open the first column, and come first when scrolling through modes
            if (!d->favorites.isEmpty()) {
                cols[0].prepend(Group{i18nc("blend mode group", "Favorites"), d->favorites});
            }
        }

        int height = 0;
        for (int c = 0; c < cols.size(); c++) {
            int y = 0;
            Q_FOREACH (const Group &group, cols[c]) {
                if (!group.title.isEmpty() || y > 0) {
                    d->headers << Header{group.title, QRect(c * columnWidth, y, columnWidth, headerHeight)};
                    y += headerHeight;
                }
                Q_FOREACH (const QString &id, group.ids) {
                    const QString name = essentials ? withoutQualifier(d->nameOf(id)) : d->nameOf(id);
                    d->items << Item{id, name, QRect(c * columnWidth, y, columnWidth, rowHeight)};
                    y += rowHeight;
                }
            }
            height = std::max(height, y);
        }

        d->grid->setFixedSize(columns * columnWidth, std::max(height, rowHeight));
        const int maxHeight = 460;
        d->scroll->setFixedSize(columns * columnWidth + (height > maxHeight ? d->scroll->verticalScrollBar()->sizeHint().width() : 0),
                                std::min(height, maxHeight) + 2);
        d->scroll->setVerticalScrollBarPolicy(height > maxHeight ? Qt::ScrollBarAsNeeded : Qt::ScrollBarAlwaysOff);
        d->allButton->setText(d->showAll ? i18nc("@action:button", "Common modes")
                                         : i18nc("@action:button", "All %1 modes", allOps().size()));
        d->allButton->setVisible(query.isEmpty());

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
    relayout();
}

KisBlendModePicker::~KisBlendModePicker() = default;

QStringList KisBlendModePicker::favoriteModes()
{
    return KisConfig(true).readEntry<QStringList>(favoritesKey, QStringList());
}

QStringList KisBlendModePicker::essentialModes()
{
    // your favorites first, then the groups in order of intent
    QStringList order = favoriteModes();
    Q_FOREACH (const Group &group, essentialGroups()) {
        Q_FOREACH (const QString &id, group.ids) {
            if (!order.contains(id)) order << id;
        }
    }
    return order;
}

void KisBlendModePicker::setFavorite(const QString &id, bool favorite)
{
    QStringList favorites = favoriteModes();
    favorites.removeAll(id);
    if (favorite) favorites << id;
    KisConfig(false).writeEntry(favoritesKey, favorites);
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
    show();
    raise();
    activateWindow();
    d->search->setFocus();
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
