/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioFind.h"
#include "KisStudioGlass.h"
#include <QPainterPath>

#include "widgets/KisPopupCloser.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QDateTime>
#include <QDockWidget>
#include <QContextMenuEvent>
#include <QDrag>
#include <QMimeData>
#include <QScopeGuard>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QMenuBar>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QStandardItemModel>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QToolTip>
#include <QVBoxLayout>
#include <QWidgetAction>
#include <QAbstractButton>
#include <QAbstractSpinBox>
#include <QComboBox>
#include <QScrollBar>
#include <QTabBar>
#include <QTextDocument>
#include <QToolButton>

#include <KSharedConfig>
#include <kconfiggroup.h>

#include <cmath>

#include <KoCompositeOpRegistry.h>
#include <KoToolFactoryBase.h>
#include <KoToolManager.h>
#include <KoToolRegistry.h>
#include <kactioncollection.h>
#include <kfts_fuzzy_match.h>
#include <klocalizedstring.h>
#include <kxmlguiclient.h>
#include <kxmlguifactory.h>
#include <KisResourceModel.h>

#include "KisMainWindow.h"
#include "KisStudioMotion.h"
#include "kis_floating_message.h"
#include "KisTouchMode.h"
#include "KisStudioItemCard.h"
#include "KisStudioToolRail.h"
#include "KisNodeActivationActionCreatorVisitor.h"
#include "KisResourceServerProvider.h"
#include "KisView.h"
#include "KisViewManager.h"
#include "dialogs/kis_dlg_preferences.h"
#include "kis_canvas_resource_provider.h"
#include "kis_config.h"
#include "kis_filter_registry.h"
#include "kis_icon_utils.h"
#include "kis_image.h"
#include "kis_node_manager.h"
#include "kis_paintop_preset.h"
#include "kis_workspace_resource.h"
#include "filter/kis_filter.h"
#include <KisModernStyle.h>

namespace {

/**
 * How well \p query names \p title, or a negative value if it doesn't.
 * Predictable matches only, best first: the start of the title, the start
 * of a word, the initials ("gb" for Gaussian Blur), every query word
 * starting a title word ("gaus bl"), and last a substring inside a word,
 * which short queries hit by accident ("gb" in RGBA) and so don't get.
 */
qreal strictMatch(const QString &query, const QString &title)
{
    const qreal lengthPenalty = title.size() * 0.3;
    qreal best = -1.0;

    for (int at = title.indexOf(query, 0, Qt::CaseInsensitive); at >= 0;
         at = title.indexOf(query, at + 1, Qt::CaseInsensitive)) {
        if (at == 0) {
            best = qMax(best, 130.0 - lengthPenalty);
        } else if (!title.at(at - 1).isLetterOrNumber()) {
            best = qMax(best, 115.0 - lengthPenalty);
        } else if (query.size() >= 3) {
            best = qMax(best, 50.0 - lengthPenalty);
        }
    }
    if (best >= 100.0) {
        return best;
    }

    static const QRegularExpression nonWord(QStringLiteral("[^\\w]+"));
    const QStringList titleWords = title.split(nonWord, Qt::SkipEmptyParts);
    const QStringList queryWords = query.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (queryWords.size() > 1) {
        bool all = true;
        Q_FOREACH (const QString &word, queryWords) {
            bool found = false;
            Q_FOREACH (const QString &titleWord, titleWords) {
                if (titleWord.startsWith(word, Qt::CaseInsensitive)) {
                    found = true;
                    break;
                }
            }
            all = all && found;
        }
        if (all) {
            best = qMax(best, 90.0 - lengthPenalty);
        }
    }

    if (query.size() >= 2 && !query.contains(QLatin1Char(' ')) && titleWords.size() >= query.size()) {
        QString initials;
        Q_FOREACH (const QString &word, titleWords) {
            initials += word.at(0);
        }
        if (initials.startsWith(query, Qt::CaseInsensitive)) {
            best = qMax(best, initials.size() == query.size() ? 100.0 : 92.0);
        }
    }
    return best;
}

enum class Kind {
    Tool,
    Command,
    Effect,
    Brush,
    BlendMode,
    Panel,
    Layer,
    Setting,
    Workspace,
};

QString kindTitle(Kind kind)
{
    switch (kind) {
    case Kind::Tool: return i18nc("@title find results", "Tools");
    case Kind::Command: return i18nc("@title find results", "Commands");
    case Kind::Effect: return i18nc("@title find results", "Effects");
    case Kind::Brush: return i18nc("@title find results", "Brushes");
    case Kind::BlendMode: return i18nc("@title find results", "Blend Modes");
    case Kind::Panel: return i18nc("@title find results", "Panels");
    case Kind::Layer: return i18nc("@title find results", "Layers");
    case Kind::Setting: return i18nc("@title find results", "Settings");
    case Kind::Workspace: return i18nc("@title find results", "Workspaces");
    }
    return QString();
}

/// Kinds that are usually what you look for get a small head start
int kindBias(Kind kind)
{
    switch (kind) {
    case Kind::Tool: return 6;
    case Kind::Effect: return 5;
    case Kind::Command: return 4;
    case Kind::Brush: return 2;
    case Kind::Panel: return 2;
    case Kind::Layer: return 1;
    case Kind::Setting: return 0;
    case Kind::Workspace: return 0;
    case Kind::BlendMode: return -4; // 138 of them, they shouldn't drown everything else
    }
    return 0;
}

/// "&Gaussian Blur..." -> "Gaussian Blur"
QString cleanText(QString text)
{
    text.remove(QLatin1Char('&'));
    text = text.trimmed();
    while (text.endsWith(QLatin1Char('.')) || text.endsWith(QChar(0x2026))) {
        text.chop(1);
    }
    return text.trimmed();
}

enum Roles {
    HeaderRole = Qt::UserRole + 1,
    DetailRole,
    ShortcutRole,
    EntryRole,
    MessageRole, ///< a line of text instead of results
    PinnableRole, ///< it can go on the tools panel
    PinnedRole,   ///< it is on the tools panel
    ShortcutEditableRole, ///< its shortcut can be changed here (a click on its key cap)
    RecordingRole,        ///< its key cap waits for the new shortcut
};

/// The pin of a result row, at its right end: puts the result on the tools panel, or takes it off
QRect pinRect(const QRect &row)
{
    return QRect(row.right() - 38, row.center().y() - 12, 24, 24);
}

/// The key cap of a result's shortcut (or the "+" of one without), left of the pin
QRectF shortcutCapRect(const QModelIndex &index, const QRect &row, const QFont &font, KisModernStyle::KeyCap state)
{
    const int right = index.data(PinnableRole).toBool() ? pinRect(row).left() - 8 : row.right() - 12;
    return KisModernStyle::keyCapRect(QRectF(row.left(), row.top(), right - row.left(), row.height()),
                                      KisModernStyle::keyCapFont(font),
                                      KisModernStyle::keyCapText(index.data(ShortcutRole).toString()), state);
}

/// A push pin, upright, in \p rect: filled when pinned
void paintPin(QPainter *painter, const QRectF &rect, const QColor &ink, bool pinned)
{
    painter->save();
    painter->translate(rect.center());
    painter->rotate(45);
    QPen pen(ink, 1.3);
    pen.setJoinStyle(Qt::RoundJoin);
    pen.setCapStyle(Qt::RoundCap);
    painter->setPen(pen);
    painter->setBrush(pinned ? ink : Qt::NoBrush);
    // the head, the collar and the needle
    QPainterPath head;
    head.addPath(KisModernStyle::squircle(QRectF(-3.2, -7.0, 6.4, 6.0), 1.2));
    painter->drawPath(head);
    painter->drawLine(QPointF(-5.5, -1.0), QPointF(5.5, -1.0));
    painter->drawLine(QPointF(0, -1.0), QPointF(0, 6.5));
    painter->restore();
}

struct Entry {
    QString key;       ///< stable id for the usage statistics
    Kind kind;
    QString title;
    QString detail;    ///< where it lives or what it does
    QString keywords;  ///< extra words that should find it
    QString shortcut;
    mutable QIcon icon; ///< see shownIcon()
    std::function<void()> run;
    QStringList aliases; ///< what other applications call it
    QPointer<QAction> action; ///< what has its shortcut, if anything does
    /// Makes the icon when it is first shown, if it is costly to make (a brush's thumbnail, read and decoded)
    mutable std::function<QIcon()> makeIcon;

    const QIcon &shownIcon() const
    {
        if (makeIcon) {
            icon = makeIcon();
            makeIcon = nullptr;
        }
        return icon;
    }
};

/**
 * The words people bring from other painting applications, so that Find
 * understands "lasso" as well as "Freehand Selection". Keyed like the
 * usage statistics; every value is one comma separated translatable list.
 */
QHash<QString, QStringList> aliasTable()
{
    const QVector<QPair<const char *, QString>> table = {
        {"tool:KisToolSelectContiguous", i18nc("@info find aliases, comma separated", "magic wand, wand")},
        {"tool:KisToolSelectOutline", i18nc("@info find aliases, comma separated", "lasso")},
        {"tool:KisToolSelectPolygonal", i18nc("@info find aliases, comma separated", "polygonal lasso")},
        {"tool:KisToolSelectMagnetic", i18nc("@info find aliases, comma separated", "magnetic lasso")},
        {"tool:KisToolSelectSimilar", i18nc("@info find aliases, comma separated", "color range, select color range")},
        {"tool:KisToolSelectRectangular", i18nc("@info find aliases, comma separated", "marquee, rectangular marquee")},
        {"tool:KisToolSelectElliptical", i18nc("@info find aliases, comma separated", "elliptical marquee")},
        {"tool:KritaSelected/KisToolColorSampler", i18nc("@info find aliases, comma separated", "eyedropper, color picker, pipette, picker")},
        {"tool:KritaFill/KisToolFill", i18nc("@info find aliases, comma separated", "paint bucket, bucket")},
        {"tool:KisToolTransform", i18nc("@info find aliases, comma separated", "free transform, scale, rotate, warp, liquify, perspective, distort")},
        {"tool:KritaShape/KisToolSmartPatch", i18nc("@info find aliases, comma separated", "healing brush, spot healing, content aware fill")},
        {"tool:KritaShape/KisToolBrush", i18nc("@info find aliases, comma separated", "paintbrush, pencil, pen")},
        {"tool:KritaShape/KisToolLazyBrush", i18nc("@info find aliases, comma separated", "flatting, flats")},
        {"tool:KisToolPath", i18nc("@info find aliases, comma separated", "pen tool, vector path")},
        {"tool:SvgTextTool", i18nc("@info find aliases, comma separated", "type")},
        {"tool:ToolReferenceImages", i18nc("@info find aliases, comma separated", "references")},
        {"tool:KisAssistantTool", i18nc("@info find aliases, comma separated", "perspective grid, vanishing point, ruler")},
        {"effect:hsvadjustment", i18nc("@info find aliases, comma separated", "hue saturation, hue/saturation, saturation")},
        {"effect:perchannel", i18nc("@info find aliases, comma separated", "curves")},
        {"effect:brightnesscontrast", i18nc("@info find aliases, comma separated", "brightness contrast, contrast")},
        {"effect:desaturate", i18nc("@info find aliases, comma separated", "black and white, grayscale, greyscale")},
        {"effect:invert", i18nc("@info find aliases, comma separated", "negative")},
        {"effect:unsharp", i18nc("@info find aliases, comma separated", "sharpen")},
        {"effect:colorbalance", i18nc("@info find aliases, comma separated", "color grading, tint")},
        {"effect:gradientmap", i18nc("@info find aliases, comma separated", "gradient map, colorize")},
        {"effect:pixelize", i18nc("@info find aliases, comma separated", "mosaic, pixelate")},
        {"action:invert_selection", i18nc("@info find aliases, comma separated", "inverse selection")},
        {"action:mirror_canvas", i18nc("@info find aliases, comma separated", "flip canvas, flip view, mirror view")},
        {"action:merge_layer", i18nc("@info find aliases, comma separated", "merge down")},
        {"action:flatten_image", i18nc("@info find aliases, comma separated", "flatten")},
        {"action:imagesize", i18nc("@info find aliases, comma separated", "image size, resize image, resample")},
        {"action:canvassize", i18nc("@info find aliases, comma separated", "canvas size, resize canvas")},
        {"action:view_show_canvas_only", i18nc("@info find aliases, comma separated", "full screen, focus mode, hide interface")},
        {"action:zoom_to_100pct", i18nc("@info find aliases, comma separated", "actual pixels, actual size")},
    };
    QHash<QString, QStringList> aliases;
    for (const auto &entry : table) {
        QStringList words = entry.second.split(QLatin1Char(','), Qt::SkipEmptyParts);
        for (QString &word : words) {
            word = word.trimmed();
        }
        aliases.insert(QLatin1String(entry.first), words);
    }
    return aliases;
}

struct Usage {
    int count {0};
    qint64 lastUsed {0};
};

/**
 * Draws result rows: an icon tile, the title, a dimmed detail and the
 * shortcut on the right; and section headers in small caps.
 */
class FindDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &index) const override
    {
        return QSize(100, index.data(MessageRole).toBool() ? 44 : index.data(HeaderRole).toBool() ? 28 : 40);
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);

        const QPalette &pal = option.palette;
        const QColor text = pal.color(QPalette::Text);
        const QColor dim = KisModernStyle::secondaryInk(text);
        const QRect r = option.rect;

        if (index.data(MessageRole).toBool()) {
            painter->setPen(dim);
            painter->drawText(r.adjusted(12, 0, -12, 0), Qt::AlignLeft | Qt::AlignVCenter, index.data(Qt::DisplayRole).toString());
            painter->restore();
            return;
        }

        if (index.data(HeaderRole).toBool()) {
            QFont font = option.font;
            font.setPointSizeF(font.pointSizeF() * 0.82);
            font.setBold(true);
            font.setCapitalization(QFont::AllUppercase);
            font.setLetterSpacing(QFont::PercentageSpacing, 106);
            painter->setFont(font);
            painter->setPen(dim);
            painter->drawText(r.adjusted(12, 6, -12, 0), Qt::AlignLeft | Qt::AlignVCenter, index.data(Qt::DisplayRole).toString());
            painter->restore();
            return;
        }

        if (option.state.testFlag(QStyle::State_Selected)) {
            const QColor tint = QColor::fromRgbF(
                pal.color(QPalette::Base).redF() * 0.45 + pal.color(QPalette::Highlight).redF() * 0.55,
                pal.color(QPalette::Base).greenF() * 0.45 + pal.color(QPalette::Highlight).greenF() * 0.55,
                pal.color(QPalette::Base).blueF() * 0.45 + pal.color(QPalette::Highlight).blueF() * 0.55);
            painter->setPen(Qt::NoPen);
            painter->setBrush(tint);
            painter->drawPath(KisModernStyle::squircle(QRectF(r).adjusted(4, 1, -4, -1), 7));
        }

        // icon tile
        const QRectF tile(r.left() + 12, r.top() + 6, 28, 28);
        painter->setPen(Qt::NoPen);
        painter->setBrush(pal.color(QPalette::Button));
        painter->drawPath(KisModernStyle::squircle(tile, 6));
        const QIcon icon = index.data(Qt::DecorationRole).value<QIcon>();
        if (!icon.isNull()) {
            icon.paint(painter, tile.adjusted(5, 5, -5, -5).toRect());
        }

        int right = r.right() - 14;
        // the pin: on the pointed at row, always when pinned, and on every row for a finger
        if (index.data(PinnableRole).toBool()) {
            const bool pinned = index.data(PinnedRole).toBool();
            const bool pointed = option.state.testFlag(QStyle::State_Selected) || option.state.testFlag(QStyle::State_MouseOver);
            if (pinned || pointed || KisTouchMode::isActive()) {
                QColor ink = pinned ? pal.color(QPalette::Link) : text;
                ink.setAlphaF(pinned ? 0.95 : 0.5);
                paintPin(painter, pinRect(r), ink, pinned);
            }
            right = pinRect(r).left() - 8;
        }

        // shortcut on the right: seen every time, so it gets learned; a key cap, as in menus, and changed by a click
        using KeyCap = KisModernStyle::KeyCap;
        const QString shortcut = KisModernStyle::keyCapText(index.data(ShortcutRole).toString());
        const bool pointed = option.state.testFlag(QStyle::State_Selected) || option.state.testFlag(QStyle::State_MouseOver);
        const bool editable = index.data(ShortcutEditableRole).toBool();
        KeyCap state = KeyCap::Shown;
        bool shown = !shortcut.isEmpty();
        if (index.data(RecordingRole).toBool()) {
            state = KeyCap::Recording;
            shown = true;
        } else if (!shortcut.isEmpty()) {
            state = pointed && editable ? KeyCap::Pointed : KeyCap::Shown;
        } else if (pointed && editable) {
            state = KeyCap::Add;
            shown = true;
        }
        if (shown) {
            const QRectF cap = shortcutCapRect(index, r, option.font, state);
            KisModernStyle::drawKeyCap(painter, cap, KisModernStyle::keyCapFont(option.font), shortcut, pal, state);
            right = int(cap.left()) - 12;
        }

        // title and detail on one line
        painter->setFont(option.font);
        painter->setPen(text);
        const QString title = index.data(Qt::DisplayRole).toString();
        const int left = int(tile.right()) + 12;
        const QFontMetrics titleMetrics(option.font);
        const QString elidedTitle = titleMetrics.elidedText(title, Qt::ElideRight, right - left);
        painter->drawText(QRect(left, r.top(), right - left, r.height()), Qt::AlignVCenter, elidedTitle);

        const QString detail = index.data(DetailRole).toString();
        const int detailLeft = left + titleMetrics.horizontalAdvance(elidedTitle) + 10;
        if (!detail.isEmpty() && detailLeft < right - 30) {
            QFont font = option.font;
            font.setPointSizeF(font.pointSizeF() * 0.92);
            painter->setFont(font);
            painter->setPen(dim);
            const QString elided = QFontMetrics(font).elidedText(detail, Qt::ElideRight, right - detailLeft);
            painter->drawText(QRect(detailLeft, r.top(), right - detailLeft, r.height()), Qt::AlignVCenter, elided);
        }

        painter->restore();
    }
};

constexpr int glassShadowMargin = 18;

/// Around the panel, for its shadow: none on a solid window (see KisStudioGlass::seeThroughWindows)
int shadowMarginOf(const QWidget *window)
{
    return window->testAttribute(Qt::WA_TranslucentBackground) ? glassShadowMargin : 0;
}
constexpr int maxResults = 40;

} // namespace

struct KisStudioFind::Private
{
    KisMainWindow *mainWindow {nullptr};
    std::function<void(QDockWidget *)> openPanel;

    QLineEdit *edit {nullptr};
    QListView *list {nullptr};
    QStandardItemModel *model {nullptr};
    QPoint dragStart;
    QPersistentModelIndex dragIndex;
    /// Set while Find picks something for the caller instead of running it
    std::function<void(const QString &key)> picker;
    QString pickPrefix; ///< while picking, only items whose key starts with it
    std::function<bool(const QString &key)> isOnTools;
    std::function<void(const QString &key, bool on)> setOnTools;
    /// The pin pressed, released on it to toggle
    QPersistentModelIndex pinPress;
    /// The result whose key cap waits for its new shortcut
    QPersistentModelIndex recording;
    bool capPress {false};
    /// How many GUI clients the index saw: commands only exist once the GUI is built
    int indexedClients {-1};

    int clientCount() const
    {
        return mainWindow->guiFactory() ? mainWindow->guiFactory()->clients().size() : 0;
    }
    /// The index is out of date once the GUI has more clients, or the window more commands of its own
    bool isStale() const
    {
        return entries.isEmpty() || indexedClients != clientCount() || indexedActions != mainWindow->actionCollection()->actions().size();
    }
    int indexedActions {-1};
    QString placeholder;

    QVector<Entry> entries;
    QHash<QString, Usage> usage;
    /// A button that is not an action of its own, by its key: what Find runs, and what holds its shortcut
    struct Button {
        QPointer<QAbstractButton> button;
        QPointer<QAction> action;
        QString title;
        QString where;
    };
    QHash<QString, Button> buttons;
    QScopedPointer<KisKActionCollection> layerActions;

    void loadUsage()
    {
        usage.clear();
        const QStringList stored = KisConfig(true).readEntry<QStringList>(QStringLiteral("StudioFindUsage"));
        Q_FOREACH (const QString &line, stored) {
            const QStringList parts = line.split(QLatin1Char('|'));
            if (parts.size() == 3) {
                usage.insert(parts[0], {parts[1].toInt(), parts[2].toLongLong()});
            }
        }
    }

    void recordUse(const QString &key)
    {
        Usage &u = usage[key];
        u.count++;
        u.lastUsed = QDateTime::currentSecsSinceEpoch();

        // keep the 200 most recent ones
        QVector<QPair<QString, Usage>> sorted;
        for (auto it = usage.begin(); it != usage.end(); ++it) {
            sorted.append(qMakePair(it.key(), it.value()));
        }
        std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) {
            return a.second.lastUsed > b.second.lastUsed;
        });
        QStringList stored;
        for (int i = 0; i < sorted.size() && i < 200; i++) {
            stored << QStringLiteral("%1|%2|%3").arg(sorted[i].first).arg(sorted[i].second.count).arg(sorted[i].second.lastUsed);
        }
        KisConfig(false).writeEntry(QStringLiteral("StudioFindUsage"), stored);
    }

    /// Frequently and recently used results rank higher; recency decays over a few days
    qreal frecency(const QString &key) const
    {
        const auto it = usage.constFind(key);
        if (it == usage.constEnd()) {
            return 0.0;
        }
        const qreal days = qMax<qint64>(0, QDateTime::currentSecsSinceEpoch() - it->lastUsed) / 86400.0;
        return 6.0 * std::log2(1.0 + it->count) + 14.0 * std::exp(-days / 3.0);
    }
};

KisStudioFind::KisStudioFind(KisMainWindow *mainWindow)
    /**
     * On Windows not Qt::Popup: a translucent popup renders a black background
     * there (see KisPopupPalette). A frameless tool window with a
     * KisPopupCloser behaves the same (see KisStudioGlass::popupWindowType).
     */
    : QFrame(mainWindow, KisStudioGlass::popupWindowType() | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint)
    , d(new Private)
{
    d->mainWindow = mainWindow;
    setAttribute(Qt::WA_TranslucentBackground, KisStudioGlass::seeThroughWindows());
    new KisPopupCloser(this);
    setObjectName(QStringLiteral("studioFind"));

    auto *layout = new QVBoxLayout(this);
    const int shadowMargin = shadowMarginOf(this);
    layout->setContentsMargins(shadowMargin + 8, shadowMargin + 8, shadowMargin + 8, shadowMargin + 8);
    layout->setSpacing(6);

    d->edit = new QLineEdit(this);
    d->edit->setPlaceholderText(i18nc("@info:placeholder", "Find anything, or > for commands"));
    d->edit->setClearButtonEnabled(false);
    d->edit->setMinimumHeight(36);
    QFont editFont = d->edit->font();
    editFont.setPointSizeF(editFont.pointSizeF() * 1.12);
    d->edit->setFont(editFont);
    d->edit->setTextMargins(6, 0, 6, 0);
    d->edit->installEventFilter(this);
    layout->addWidget(d->edit);

    d->model = new QStandardItemModel(this);
    d->list = new QListView(this);
    d->list->setModel(d->model);
    d->list->setItemDelegate(new FindDelegate(d->list));
    d->list->setFrameShape(QFrame::NoFrame);
    d->list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    d->list->viewport()->installEventFilter(this);
    d->list->setSelectionMode(QAbstractItemView::SingleSelection);
    d->list->setMouseTracking(true);
    d->list->setFocusPolicy(Qt::NoFocus);
    d->list->viewport()->setAutoFillBackground(false);
    d->list->setAutoFillBackground(false);
    layout->addWidget(d->list);

    connect(d->edit, &QLineEdit::textChanged, this, &KisStudioFind::updateResults);
    connect(d->list, &QListView::clicked, this, [this](const QModelIndex &index) { runRow(index.row()); });
    connect(d->list, &QListView::entered, this, [this](const QModelIndex &index) {
        if (!index.data(HeaderRole).toBool()) {
            d->list->setCurrentIndex(index);
        }
    });
}

KisStudioFind::~KisStudioFind() = default;

void KisStudioFind::setPanelOpener(std::function<void(QDockWidget *)> opener)
{
    d->openPanel = std::move(opener);
}

void KisStudioFind::setTools(std::function<bool(const QString &key)> isOnTools, std::function<void(const QString &key, bool on)> setOnTools)
{
    d->isOnTools = std::move(isOnTools);
    d->setOnTools = std::move(setOnTools);
}

void KisStudioFind::pick(const QString &placeholder, std::function<void(const QString &key)> picked, const QString &keyPrefix)
{
    open();
    d->picker = std::move(picked);
    d->pickPrefix = keyPrefix;
    d->edit->setPlaceholderText(placeholder);
    updateResults();
}

void KisStudioFind::open(const QString &text)
{
    d->picker = nullptr;
    d->pickPrefix.clear();
    if (d->placeholder.isNull()) {
        d->placeholder = d->edit->placeholderText();
    }
    d->edit->setPlaceholderText(d->placeholder);
    d->loadUsage();
    rebuildIndex();
    d->edit->setText(text);
    updateResults();
    adjustGeometry();
    KisStudioMotion::showFaded(this);
    raise();
    activateWindow();
    d->edit->setFocus();
}

void KisStudioFind::rebuildIndex()
{
    d->entries.clear();
    d->indexedClients = d->clientCount();
    d->indexedActions = d->mainWindow->actionCollection()->actions().size();
    // the aliases go on at the end, whatever the order of the sources
    auto attachAliases = qScopeGuard([this]() {
        static const QHash<QString, QStringList> aliases = aliasTable();
        static const QRegularExpression blending(QStringLiteral("blend|smudge|smear"), QRegularExpression::CaseInsensitiveOption);
        const QStringList blendWords = i18nc("@info find aliases, comma separated", "smudge, smear, blend").split(QLatin1Char(','));
        for (Entry &entry : d->entries) {
            entry.aliases = aliases.value(entry.key);
            if (entry.kind == Kind::Brush && blending.match(entry.title).hasMatch()) {
                for (const QString &word : blendWords) {
                    entry.aliases << word.trimmed();
                }
            }
        }
    });
    KisMainWindow *mainWindow = d->mainWindow;
    KisViewManager *viewManager = mainWindow->viewManager();

    // tools
    QSet<QString> toolIds;
    Q_FOREACH (KoToolFactoryBase *factory, KoToolRegistry::instance()->values()) {
        toolIds.insert(factory->id());
        QString shortcut;
        if (QAction *action = mainWindow->actionCollection()->action(factory->id())) {
            shortcut = action->shortcut().toString(QKeySequence::NativeText);
        }
        const QString id = factory->id();
        d->entries.append({QStringLiteral("tool:") + id, Kind::Tool,
                           KisStudioToolRail::toolName(factory), QString(),
                           factory->section(), shortcut, KisIconUtils::loadIcon(factory->iconName()),
                           [id]() { KoToolManager::instance()->switchToolRequested(id); }});
        d->entries.last().action = mainWindow->actionCollection()->action(factory->id());
    }

    // where every command lives in the menus, shown as its context
    QHash<QAction *, QString> menuPaths;
    std::function<void(QMenu *, const QString &)> walk = [&](QMenu *menu, const QString &path) {
        Q_FOREACH (QAction *action, menu->actions()) {
            if (action->menu()) {
                walk(action->menu(), path.isEmpty() ? cleanText(action->text()) : path + QStringLiteral(" › ") + cleanText(action->text()));
            } else if (!menuPaths.contains(action)) {
                menuPaths.insert(action, path);
            }
        }
    };
    Q_FOREACH (QAction *top, mainWindow->menuBar()->actions()) {
        if (top->menu()) {
            walk(top->menu(), cleanText(top->text()));
        }
    }

    // effects, from the filter registry so that they get their category
    QSet<QString> filterActions;
    Q_FOREACH (const QString &filterId, KisFilterRegistry::instance()->keys()) {
        KisFilterSP filter = KisFilterRegistry::instance()->value(filterId);
        const QString actionName = QStringLiteral("krita_filter_") + filterId;
        QAction *action = mainWindow->actionCollection()->action(actionName);
        if (!action) {
            Q_FOREACH (KisKXMLGUIClient *client, mainWindow->guiFactory()->clients()) {
                if (client && client->actionCollection() && (action = client->actionCollection()->action(actionName))) {
                    break;
                }
            }
        }
        if (!filter || !action) {
            continue;
        }
        filterActions.insert(actionName);
        QPointer<QAction> target = action;
        d->entries.append({QStringLiteral("effect:") + filterId, Kind::Effect, cleanText(filter->name()),
                           filter->menuCategory().name(), QStringLiteral("filter"),
                           action->shortcut().toString(QKeySequence::NativeText),
                           action->icon().isNull() ? KisIconUtils::loadIcon(QStringLiteral("studio_effects")) : action->icon(),
                           [target]() { if (target && target->isEnabled()) target->trigger(); }});
        d->entries.last().action = action;
    }

    // every other command, from all the action collections
    QSet<QAction *> seen;
    Q_FOREACH (KisKXMLGUIClient *client, mainWindow->guiFactory()->clients()) {
        if (!client || !client->actionCollection()) {
            continue;
        }
        Q_FOREACH (QAction *action, client->actionCollection()->actions()) {
            const QString name = action->objectName();
            const QString text = cleanText(action->text());
            // every KisAction is a QWidgetAction: only the ones that are a widget (the toolbar's sliders, spacers) are left out
            auto *widgetAction = qobject_cast<QWidgetAction *>(action);
            if (seen.contains(action) || text.isEmpty() || action->isSeparator() || action->menu()
                || (widgetAction && widgetAction->defaultWidget()) || toolIds.contains(name) || filterActions.contains(name)) {
                continue;
            }
            seen.insert(action);
            QPointer<QAction> target = action;
            d->entries.append({QStringLiteral("action:") + name, Kind::Command, text, menuPaths.value(action),
                               cleanText(action->toolTip()), action->shortcut().toString(QKeySequence::NativeText),
                               action->icon(),
                               [target]() { if (target && target->isEnabled()) target->trigger(); }});
            d->entries.last().action = action;
        }
    }

    // every other button: findable, pinnable, and given a shortcut from its card
    registerButtons();
    for (auto it = d->buttons.constBegin(); it != d->buttons.constEnd(); ++it) {
        if (!it->button || !it->action) continue;
        const QString key = it.key();
        QPointer<QAction> action = it->action;
        QIcon icon = it->button->icon();
        if (icon.isNull()) icon = KisIconUtils::loadIcon(QStringLiteral("hamburger_menu_dots"));
        d->entries.append({key, Kind::Command, it->title, it->where, QStringLiteral("button"),
                           it->action->shortcut().toString(QKeySequence::NativeText), icon,
                           [action]() { if (action) action->trigger(); }});
        d->entries.last().action = it->action;
    }

    // panels
    Q_FOREACH (QDockWidget *dock, mainWindow->dockWidgets()) {
        QPointer<QDockWidget> target = dock;
        d->entries.append({QStringLiteral("panel:") + dock->objectName(), Kind::Panel, dock->windowTitle(),
                           QString(), QStringLiteral("docker panel"), QString(), dock->windowIcon(),
                           [this, target]() {
                               if (!target) return;
                               if (d->openPanel) {
                                   d->openPanel(target);
                               } else {
                                   target->show();
                                   target->raise();
                               }
                           }});
        d->entries.last().action = dock->toggleViewAction();
    }

    // brushes, from the model so that only the chosen preset gets loaded; their thumbnails when shown (reading
    // and decoding all of them was a good part of startup)
    if (KisPaintOpPresetResourceServer *server = KisResourceServerProvider::instance()->paintOpPresetServer()) {
        KisResourceModel *model = server->resourceModel();
        for (int row = 0; row < model->rowCount(); row++) {
            const QModelIndex index = model->index(row, 0);
            const QString name = index.data(Qt::UserRole + KisAbstractResourceModel::Name).toString();
            const QString md5 = index.data(Qt::UserRole + KisAbstractResourceModel::MD5).toString();
            const int resourceId = index.data(Qt::UserRole + KisAbstractResourceModel::Id).toInt();
            // the bundled presets sort themselves with "b) ", which is noise here
            static const QRegularExpression sortPrefix(QStringLiteral("^[a-z]\\)\\s+"));
            const QString title = QString(name).remove(sortPrefix);
            d->entries.append({QStringLiteral("brush:") + name, Kind::Brush, title, QString(),
                               index.data(Qt::UserRole + KisAbstractResourceModel::Tags).toStringList().join(QLatin1Char(' ')),
                               QString(), QIcon(),
                               [viewManager, md5, name]() {
                                   KisPaintOpPresetSP preset = KisResourceServerProvider::instance()->paintOpPresetServer()->resource(md5, QString(), name);
                                   if (preset && viewManager && viewManager->canvasResourceProvider()) {
                                       viewManager->canvasResourceProvider()->setPaintOpPreset(preset);
                                   }
                               }});
            d->entries.last().makeIcon = [resourceId]() {
                KisPaintOpPresetResourceServer *server = KisResourceServerProvider::instance()->paintOpPresetServer();
                if (!server) return QIcon();
                // by its id, not a kept index: the model may have been reset since
                const QModelIndex index = server->resourceModel()->indexForResourceId(resourceId);
                return QIcon(QPixmap::fromImage(index.data(Qt::UserRole + KisAbstractResourceModel::Thumbnail).value<QImage>()));
            };
        }
    }

    // blend modes of the brush
    const auto ops = KoCompositeOpRegistry::instance().getCompositeOps();
    for (auto it = ops.constBegin(); it != ops.constEnd(); ++it) {
        const QString opId = it.value().id();
        d->entries.append({QStringLiteral("blend:") + opId, Kind::BlendMode, it.value().name(),
                           i18nc("find result detail", "Brush blend mode · %1", it.key().name()),
                           QStringLiteral("blending mode composite"), QString(),
                           KisIconUtils::loadIcon(QStringLiteral("hamburger_menu_dots")),
                           [viewManager, opId]() {
                               if (viewManager && viewManager->canvasResourceProvider()) {
                                   viewManager->canvasResourceProvider()->setCurrentCompositeOp(opId);
                               }
                           }});
    }

    // layers of the current image
    d->layerActions.reset();
    if (viewManager && mainWindow->activeView() && mainWindow->activeView()->image()) {
        d->layerActions.reset(new KisKActionCollection(nullptr, QStringLiteral("studiofindlayers")));
        KisNodeActivationActionCreatorVisitor visitor(d->layerActions.data(), viewManager->nodeManager());
        mainWindow->activeView()->image()->rootLayer()->accept(visitor);
        Q_FOREACH (QAction *action, d->layerActions->actions()) {
            QPointer<QAction> target = action;
            d->entries.append({QString(), Kind::Layer, cleanText(action->text()), QString(), QStringLiteral("layer"),
                               QString(), KisIconUtils::loadIcon(QStringLiteral("all-layers")),
                               [target]() { if (target) target->trigger(); }});
        }
    }

    // settings pages
    struct SettingPage {
        QString title;
        QString keywords;
        KisDlgPreferences::Page page;
        int tab;
    };
    const QVector<SettingPage> pages = {
        {i18nc("settings page", "Autosave and Backups"), QStringLiteral("file handling compression export"), KisDlgPreferences::General, KisDlgPreferences::File},
        {i18nc("settings page", "Pasting"), QStringLiteral("clipboard"), KisDlgPreferences::General, KisDlgPreferences::Pasting},
        {i18nc("settings page", "Window and Interface"), QStringLiteral("tabs subwindows font hidpi scaling"), KisDlgPreferences::General, KisDlgPreferences::Window},
        {i18nc("settings page", "Brush Cursor"), QStringLiteral("outline pointer crosshair"), KisDlgPreferences::General, KisDlgPreferences::Cursor},
        {i18nc("settings page", "Tool Behavior"), QStringLiteral("tool options zoom steps touch painting selection actions bar"), KisDlgPreferences::General, KisDlgPreferences::Tools},
        {i18nc("settings page", "Animation"), QStringLiteral("timeline playback"), KisDlgPreferences::General, KisDlgPreferences::Animation},
        {i18nc("settings page", "Resource Folder"), QStringLiteral("resources bundles location"), KisDlgPreferences::General, KisDlgPreferences::Resources},
        {i18nc("settings page", "Startup, Undo and Miscellaneous"), QStringLiteral("session undo stack layer naming"), KisDlgPreferences::General, KisDlgPreferences::MiscellaneousGeneral},
        {i18nc("settings page", "Keys and Shortcuts"), QStringLiteral("keyboard hotkeys keybindings canvas input mouse pan zoom rotate gestures held"), KisDlgPreferences::Shortucts, 0},
        {i18nc("settings page", "Color Management"), QStringLiteral("profile icc monitor display soft proofing"), KisDlgPreferences::Color, 0},
        {i18nc("settings page", "Memory and Performance"), QStringLiteral("ram swap threads cache instant preview"), KisDlgPreferences::Performance, 0},
        {i18nc("settings page", "Canvas Display and Acceleration"), QStringLiteral("opengl renderer checkerboard border grid hdr"), KisDlgPreferences::Display, 0},
        {i18nc("settings page", "Tablet and Pressure"), QStringLiteral("wintab windows ink pen pressure curve"), KisDlgPreferences::Tablet, 0},
    };
    Q_FOREACH (const SettingPage &page, pages) {
        QPointer<KisMainWindow> window = mainWindow;
        const int pageId = page.page;
        const int tab = page.tab;
        d->entries.append({QStringLiteral("setting:%1:%2").arg(pageId).arg(tab), Kind::Setting, page.title,
                           i18nc("find result detail", "Settings"), page.keywords + QStringLiteral(" preferences configure"),
                           QString(), KisIconUtils::loadIcon(QStringLiteral("configure-thicker")),
                           [window, pageId, tab]() {
                               if (!window) return;
                               if (QAction *configure = window->actionCollection()->action(QStringLiteral("options_configure"))) {
                                   configure->setData(QVariantList{pageId, tab});
                                   configure->trigger();
                               }
                           }});
    }

    // what canvas-only mode hides, each on or off here (no page of its own in the settings)
    struct CanvasOnly {
        QString title;
        bool (KisConfig::*get)(bool) const;
        void (KisConfig::*set)(bool) const;
    };
    const QVector<CanvasOnly> canvasOnly = {
        {i18nc("canvas-only mode setting", "Canvas-only: hide panels"), &KisConfig::hideDockersFullscreen, &KisConfig::setHideDockersFullscreen},
        {i18nc("canvas-only mode setting", "Canvas-only: hide menu"), &KisConfig::hideMenuFullscreen, &KisConfig::setHideMenuFullscreen},
        {i18nc("canvas-only mode setting", "Canvas-only: hide scrollbars"), &KisConfig::hideScrollbarsFullscreen, &KisConfig::setHideScrollbarsFullscreen},
        {i18nc("canvas-only mode setting", "Canvas-only: hide status bar"), &KisConfig::hideStatusbarFullscreen, &KisConfig::setHideStatusbarFullscreen},
        {i18nc("canvas-only mode setting", "Canvas-only: hide title bar"), &KisConfig::hideTitlebarFullscreen, &KisConfig::setHideTitlebarFullscreen},
        {i18nc("canvas-only mode setting", "Canvas-only: hide toolbars"), &KisConfig::hideToolbarFullscreen, &KisConfig::setHideToolbarFullscreen},
    };
    for (int i = 0; i < canvasOnly.size(); i++) {
        const CanvasOnly setting = canvasOnly[i];
        const bool on = (KisConfig(true).*setting.get)(false);
        d->entries.append({QStringLiteral("canvasonly:%1").arg(i), Kind::Setting, setting.title,
                           on ? i18nc("find result detail: a setting that is on", "On") : i18nc("find result detail: a setting that is off", "Off"),
                           QStringLiteral("fullscreen canvas only mode hide settings"), QString(),
                           KisIconUtils::loadIcon(QStringLiteral("config-canvas-only")),
                           [setting]() {
                               KisConfig cfg(false);
                               (cfg.*setting.set)(!(cfg.*setting.get)(false));
                           }});
    }

    // workspaces
    if (KoResourceServer<KisWorkspaceResource> *server = KisResourceServerProvider::instance()->workspaceServer()) {
        KisResourceModel *model = server->resourceModel();
        for (int row = 0; row < model->rowCount(); row++) {
            const QModelIndex index = model->index(row, 0);
            const QString name = index.data(Qt::UserRole + KisAbstractResourceModel::Name).toString();
            const QString md5 = index.data(Qt::UserRole + KisAbstractResourceModel::MD5).toString();
            QPointer<KisMainWindow> window = mainWindow;
            d->entries.append({QStringLiteral("workspace:") + name, Kind::Workspace, name, QString(),
                               QStringLiteral("layout"), QString(), QIcon(),
                               [window, md5, name]() {
                                   KisWorkspaceResourceSP workspace = KisResourceServerProvider::instance()->workspaceServer()->resource(md5, QString(), name);
                                   if (window && workspace) {
                                       window->restoreWorkspace(workspace);
                                   }
                               }});
        }
    }
}

bool KisStudioFind::isCommandKind(int kind)
{
    switch (Kind(kind)) {
    case Kind::Tool:
    case Kind::Command:
    case Kind::Effect:
    case Kind::Panel:
    case Kind::Setting:
        return true;
    default:
        return false;
    }
}

void KisStudioFind::appendResult(int index, const QString &detail)
{
    const Entry &entry = d->entries[index];
    auto *item = new QStandardItem(entry.shownIcon(), entry.title);
    item->setData(detail, DetailRole);
    item->setData(entry.shortcut, ShortcutRole);
    item->setData(entry.action && !entry.action->objectName().isEmpty(), ShortcutEditableRole);
    item->setData(index, EntryRole);
    // brushes, effects, commands, tools and panels can go on the tools panel
    const bool pinnable = !d->picker && d->isOnTools && !entry.key.isEmpty()
        && (entry.kind == Kind::Tool || entry.kind == Kind::Command || entry.kind == Kind::Effect
            || entry.kind == Kind::Panel || entry.kind == Kind::Brush);
    item->setData(pinnable, PinnableRole);
    item->setData(pinnable && d->isOnTools(entry.key), PinnedRole);
    item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    d->model->appendRow(item);
}

void KisStudioFind::appendHeader(const QString &title)
{
    auto *header = new QStandardItem(title);
    header->setData(true, HeaderRole);
    header->setFlags(Qt::NoItemFlags);
    d->model->appendRow(header);
}

/**
 * "> ..." lists only what can be done, as one list ranked by how well it
 * matches (loosely: a few letters of each word are enough) and how often
 * it was used; with nothing after the ">", the recent ones, the most used
 * ones, then all of them.
 */
void KisStudioFind::updateCommands(const QString &query)
{
    auto detailOf = [this](int index) {
        const Entry &entry = d->entries[index];
        return entry.detail.isEmpty() || entry.kind != Kind::Command ? kindTitle(entry.kind) : entry.detail;
    };
    QVector<int> commands;
    for (int i = 0; i < d->entries.size(); i++) {
        if (isCommandKind(int(d->entries[i].kind)) && !d->entries[i].key.isEmpty()) commands << i;
    }

    if (query.isEmpty()) {
        QVector<int> recent = commands;
        recent.erase(std::remove_if(recent.begin(), recent.end(), [this](int i) { return !d->usage.contains(d->entries[i].key); }),
                     recent.end());
        std::sort(recent.begin(), recent.end(), [this](int a, int b) {
            return d->usage.value(d->entries[a].key).lastUsed > d->usage.value(d->entries[b].key).lastUsed;
        });
        QVector<int> often = recent;
        recent.resize(qMin(recent.size(), 6));
        often.erase(std::remove_if(often.begin(), often.end(), [&recent](int i) { return recent.contains(i); }), often.end());
        std::sort(often.begin(), often.end(), [this](int a, int b) {
            return d->usage.value(d->entries[a].key).count > d->usage.value(d->entries[b].key).count;
        });
        often.resize(qMin(often.size(), 6));
        if (!recent.isEmpty()) {
            appendHeader(i18nc("@title find results", "Recent"));
            Q_FOREACH (int index, recent) appendResult(index, detailOf(index));
        }
        if (!often.isEmpty()) {
            appendHeader(i18nc("@title find results", "Most used"));
            Q_FOREACH (int index, often) appendResult(index, detailOf(index));
        }
        std::sort(commands.begin(), commands.end(), [this](int a, int b) {
            return d->entries[a].title.localeAwareCompare(d->entries[b].title) < 0;
        });
        appendHeader(i18nc("@title find results", "All"));
        Q_FOREACH (int index, commands) appendResult(index, detailOf(index));
    } else {
        QVector<QPair<qreal, int>> scored;
        Q_FOREACH (int i, commands) {
            const Entry &entry = d->entries[i];
            // the menu it is in counts too: "new layer" finds Layer › New › Paint Layer
            QStringList path = entry.kind == Kind::Command ? entry.detail.split(QStringLiteral(" › "), Qt::SkipEmptyParts) : QStringList();
            const QString inMenu = path.isEmpty() ? QString() : path.last() + QLatin1Char(' ') + entry.title;
            const QString fullPath = path.isEmpty() ? QString() : path.join(QLatin1Char(' ')) + QLatin1Char(' ') + entry.title;
            qreal score = strictMatch(query, entry.title);
            Q_FOREACH (const QString &alias, entry.aliases) {
                const qreal viaAlias = strictMatch(query, alias);
                if (viaAlias >= 0.0) score = qMax(score, viaAlias - 8.0);
            }
            if (!inMenu.isEmpty()) {
                const qreal viaMenu = qMax(strictMatch(query, inMenu), strictMatch(query, fullPath) - 4.0);
                if (viaMenu >= 0.0) score = qMax(score, viaMenu - 4.0);
            }
            // a few letters of each word, in order
            bool loose = false;
            if (score < 0.0) {
                int fuzzy = 0;
                if (kfts::fuzzy_match(query, entry.title, fuzzy)) {
                    loose = true;
                    score = fuzzy * 0.3 - 10;
                } else if (!inMenu.isEmpty() && (kfts::fuzzy_match(query, inMenu, fuzzy) || kfts::fuzzy_match(query, fullPath, fuzzy))) {
                    loose = true;
                    score = fuzzy * 0.3 - 14;
                }
            }
            if (score >= 0.0 || loose) {
                scored.append({score + d->frecency(entry.key) + kindBias(entry.kind), i});
            }
        }
        std::stable_sort(scored.begin(), scored.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
        if (scored.isEmpty()) {
            auto *nothing = new QStandardItem(i18nc("@info find results", "No command “%1”", query));
            nothing->setData(true, HeaderRole);
            nothing->setData(true, MessageRole);
            nothing->setFlags(Qt::NoItemFlags);
            d->model->appendRow(nothing);
        }
        for (int i = 0; i < scored.size() && i < 60; i++) {
            appendResult(scored[i].second, detailOf(scored[i].second));
        }
    }
    for (int row = 0; row < d->model->rowCount(); row++) {
        if (!d->model->item(row)->data(HeaderRole).toBool()) {
            d->list->setCurrentIndex(d->model->index(row, 0));
            break;
        }
    }
    adjustGeometry();
}

/**
 * The buttons of the window that are no action of their own (the panels'
 * buttons, the top bar's, switches and options), each with a hidden action
 * on the window that clicks it: Find shows them like commands, and their
 * card gives them a shortcut, saved as any shortcut is. Their key is where
 * they are: the panel, the named widgets on the way, the button.
 */
void KisStudioFind::registerButtons()
{
    KisMainWindow *mainWindow = d->mainWindow;
    // the actions Krita already has: those buttons are found as commands
    QSet<QAction *> known;
    Q_FOREACH (QAction *action, mainWindow->actionCollection()->actions()) known.insert(action);
    if (mainWindow->guiFactory()) {
        Q_FOREACH (KisKXMLGUIClient *client, mainWindow->guiFactory()->clients()) {
            if (client && client->actionCollection()) {
                Q_FOREACH (QAction *action, client->actionCollection()->actions()) known.insert(action);
            }
        }
    }
    static const QRegularExpression tags(QStringLiteral("<[^>]*>"));
    QHash<QString, int> seen;
    QSet<QString> registered;
    const KConfigGroup shortcuts(KSharedConfig::openConfig(QStringLiteral("kritashortcutsrc")), QStringLiteral("Shortcuts"));

    Q_FOREACH (QAbstractButton *button, mainWindow->findChildren<QAbstractButton *>()) {
        auto *tool = qobject_cast<QToolButton *>(button);
        if (tool && tool->defaultAction() && known.contains(tool->defaultAction())) continue;
        // parts of other widgets, Find itself, the tools panel (its items are Find's already)
        bool skip = false;
        QDockWidget *dock = nullptr;
        QStringList path;
        for (QWidget *w = button->parentWidget(); w && w != mainWindow; w = w->parentWidget()) {
            if (qobject_cast<QAbstractSpinBox *>(w) || qobject_cast<QComboBox *>(w) || qobject_cast<QLineEdit *>(w)
                || qobject_cast<QTabBar *>(w) || qobject_cast<QScrollBar *>(w) || qobject_cast<QMenu *>(w)
                || w == this || qobject_cast<KisStudioToolRail *>(w) || w->inherits("KisStudioPicker")) {
                skip = true;
                break;
            }
            if (auto *asDock = qobject_cast<QDockWidget *>(w)) {
                dock = asDock;
                path.prepend(asDock->objectName());
                break;
            }
            if (!w->objectName().isEmpty() && !w->objectName().startsWith(QLatin1String("qt_"))) path.prepend(w->objectName());
        }
        // hidden on purpose (replaced, or not for this mode): not a button there is
        if (skip || !button->isVisibleTo(dock ? static_cast<QWidget *>(dock) : static_cast<QWidget *>(mainWindow))) continue;

        // what it is called: its text, else the first line of its tooltip
        QString title = cleanText(button->text());
        if (title.isEmpty()) {
            QString tip = button->toolTip();
            tip.remove(tags);
            tip = tip.section(QLatin1Char('\n'), 0, 0).trimmed();
            // "Drag to move, click to fold" is no name: only the part before a colon or comma if long
            if (tip.size() > 48) tip = tip.section(QRegularExpression(QStringLiteral("[:,.]")), 0, 0);
            title = cleanText(tip);
        }
        if (title.isEmpty()) title = cleanText(button->accessibleName());
        if (title.isEmpty() || title.size() > 60) continue;

        QString key = QStringLiteral("button:") + path.join(QLatin1Char('/')) + QLatin1Char('/')
            + (button->objectName().isEmpty() ? title : button->objectName());
        const int count = seen.value(key, 0);
        seen.insert(key, count + 1);
        if (count > 0) key += QLatin1Char('#') + QString::number(count);
        registered.insert(key);

        Private::Button &entry = d->buttons[key];
        entry.button = button;
        entry.title = title;
        entry.where = dock ? dock->windowTitle() : i18nc("find result detail: a button of the window, outside the panels", "Window");
        if (!entry.action) {
            // the hidden action that holds its shortcut, named after where the button is
            QString name = key;
            name.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_]")), QStringLiteral("_"));
            auto *action = new QAction(title, mainWindow);
            action->setObjectName(QStringLiteral("studio_button_") + name);
            const QString saved = shortcuts.readEntry(action->objectName(), QString());
            if (!saved.isEmpty() && saved != QLatin1String("none")) {
                action->setShortcuts(QKeySequence::listFromString(saved));
            }
            connect(action, &QAction::triggered, this, [this, key]() {
                const Private::Button entry = d->buttons.value(key);
                QAbstractButton *button = entry.button;
                if (!button || !button->isEnabled()) return;
                if (button->isVisible()) {
                    button->click();
                    return;
                }
                // in a panel that is closed: opened, then clicked
                QDockWidget *dock = nullptr;
                for (QWidget *w = button->parentWidget(); w && !dock; w = w->parentWidget()) dock = qobject_cast<QDockWidget *>(w);
                if (dock && d->openPanel) {
                    d->openPanel(dock);
                    QPointer<QAbstractButton> later = button;
                    QTimer::singleShot(0, this, [later]() {
                        if (later && later->isVisible() && later->isEnabled()) later->click();
                    });
                }
            });
            mainWindow->addAction(action);
            entry.action = action;
        }
        entry.action->setText(title);
    }
    // a button whose text changed is one button, not one more under each of its texts
    QSet<QAbstractButton *> current;
    Q_FOREACH (const QString &key, registered) current.insert(d->buttons.value(key).button);
    for (auto it = d->buttons.begin(); it != d->buttons.end();) {
        if (!registered.contains(it.key()) && it->button && current.contains(it->button)) {
            delete it->action.data();
            it = d->buttons.erase(it);
        } else {
            ++it;
        }
    }
}

void KisStudioFind::updateResults()
{
    d->model->clear();
    const QString query = d->edit->text().trimmed();
    if (!d->picker && query.startsWith(QLatin1Char('>'))) {
        updateCommands(query.mid(1).trimmed());
        return;
    }

    struct Scored {
        int entry;
        qreal score;
    };
    QVector<Scored> scored;

    for (int i = 0; i < d->entries.size(); i++) {
        const Entry &entry = d->entries[i];
        const qreal frecency = entry.key.isEmpty() ? 0.0 : d->frecency(entry.key);

        if (d->picker && !d->pickPrefix.isEmpty() && !entry.key.startsWith(d->pickPrefix)) {
            continue;
        }
        if (query.isEmpty() && d->picker) {
            const bool browsable = d->pickPrefix.isEmpty() ? (entry.kind == Kind::Tool || entry.kind == Kind::Effect) : true;
            if (browsable && !entry.key.isEmpty()) {
                // tools before effects, each alphabetically (the sort below is stable)
                scored.append({i, entry.kind == Kind::Tool ? 2.0 : 1.0});
            }
            continue;
        }
        if (query.isEmpty()) {
            // nothing typed yet: what was used recently
            if (frecency > 0.0) {
                scored.append({i, frecency});
            }
            continue;
        }

        qreal total = strictMatch(query, entry.title);
        Q_FOREACH (const QString &alias, entry.aliases) {
            // a little below the real name, which is what the result shows and teaches
            const qreal viaAlias = strictMatch(query, alias);
            if (viaAlias >= 0.0) {
                total = qMax(total, viaAlias - 8.0);
            }
        }
        if (total < 0.0) {
            // keywords only count as whole words that start with the query,
            // fuzzy matching across words finds nonsense ("blur" in "tablet ... pressure")
            bool keywordMatch = false;
            const QString text = entry.keywords + QLatin1Char(' ') + entry.detail;
            const QStringList words = text.split(QLatin1Char(' '), Qt::SkipEmptyParts);
            Q_FOREACH (const QString &word, words) {
                if (query.size() >= 2 && word.startsWith(query, Qt::CaseInsensitive)) {
                    keywordMatch = true;
                    break;
                }
            }
            if (!keywordMatch) {
                continue;
            }
            total = 30;
        }
        scored.append({i, total + frecency + kindBias(entry.kind)});
    }

    // too few predictable matches: allow loose fuzzy ones, which tolerate typos;
    // not for one or two letters, which fuzzily match half of everything
    if (query.size() >= 3 && scored.size() < 5) {
        QSet<int> have;
        Q_FOREACH (const Scored &s, scored) have.insert(s.entry);
        for (int i = 0; i < d->entries.size(); i++) {
            int score = 0;
            if (d->picker && !d->pickPrefix.isEmpty() && !d->entries[i].key.startsWith(d->pickPrefix)) {
                continue;
            }
            if (!have.contains(i) && kfts::fuzzy_match(query, d->entries[i].title, score)) {
                scored.append({i, score * 0.3 - 20 + kindBias(d->entries[i].kind)});
            }
        }
    }

    const bool browsing = query.isEmpty() && d->picker;

    /**
     * Nothing typed: what was used recently, then every effect and every
     * tool, so that a finger can find Gaussian Blur by scrolling, without a
     * keyboard.
     */
    if (query.isEmpty() && !d->picker) {
        std::sort(scored.begin(), scored.end(), [](const Scored &a, const Scored &b) { return a.score > b.score; });
        if (scored.size() > 8) {
            scored.resize(8);
        }
        auto addHeader = [this](const QString &title) { appendHeader(title); };
        auto addEntry = [this](int index, const QString &detail) { appendResult(index, detail); };
        if (!scored.isEmpty()) {
            addHeader(i18nc("@title find results", "Recent"));
            Q_FOREACH (const Scored &s, scored) {
                addEntry(s.entry, kindTitle(d->entries[s.entry].kind));
            }
        }
        // what the main menu had: the document's commands
        QVector<int> document;
        for (const char *name : {"file_new", "file_open", "file_save", "file_save_as", "file_export_file",
                                 "imagesize", "canvassize", "options_configure"}) {
            const QString key = QStringLiteral("action:") + QLatin1String(name);
            for (int i = 0; i < d->entries.size(); i++) {
                if (d->entries[i].key == key) {
                    document << i;
                    break;
                }
            }
        }
        if (!document.isEmpty()) {
            addHeader(i18nc("@title find results", "Document"));
            Q_FOREACH (int index, document) {
                addEntry(index, QString());
            }
        }
        for (Kind kind : {Kind::Effect, Kind::Tool}) {
            QVector<int> all;
            for (int i = 0; i < d->entries.size(); i++) {
                if (d->entries[i].kind == kind && !d->entries[i].key.isEmpty()) all << i;
            }
            std::sort(all.begin(), all.end(), [this](int a, int b) {
                return d->entries[a].title.localeAwareCompare(d->entries[b].title) < 0;
            });
            addHeader(kindTitle(kind));
            Q_FOREACH (int index, all) {
                addEntry(index, d->entries[index].detail);
            }
        }
        for (int row = 0; row < d->model->rowCount(); row++) {
            if (!d->model->item(row)->data(HeaderRole).toBool()) {
                d->list->setCurrentIndex(d->model->index(row, 0));
                break;
            }
        }
        adjustGeometry();
        return;
    }
    if (browsing) {
        std::sort(scored.begin(), scored.end(), [this](const Scored &a, const Scored &b) {
            if (a.score != b.score) return a.score > b.score;
            return d->entries[a.entry].title.localeAwareCompare(d->entries[b.entry].title) < 0;
        });
    } else {
        std::sort(scored.begin(), scored.end(), [](const Scored &a, const Scored &b) { return a.score > b.score; });
        if (scored.size() > maxResults) {
            scored.resize(maxResults);
        }
    }

    // group by kind, the groups ordered by their best result
    QVector<Kind> kindOrder;
    QHash<int, QVector<int>> groups;
    Q_FOREACH (const Scored &s, scored) {
        const Kind kind = d->entries[s.entry].kind;
        if (!groups.contains(int(kind))) {
            kindOrder.append(kind);
        }
        groups[int(kind)].append(s.entry);
    }

    if (!query.isEmpty() && scored.isEmpty()) {
        auto *nothing = new QStandardItem(i18nc("@info find results", "No results for “%1”", query));
        nothing->setData(true, HeaderRole);
        nothing->setData(true, MessageRole);
        nothing->setFlags(Qt::NoItemFlags);
        d->model->appendRow(nothing);
    }

    if (query.isEmpty() && !browsing && !scored.isEmpty()) {
        auto *header = new QStandardItem(i18nc("@title find results", "Recent"));
        header->setData(true, HeaderRole);
        header->setFlags(Qt::NoItemFlags);
        d->model->appendRow(header);
    }

    Q_FOREACH (Kind kind, kindOrder) {
        if (!query.isEmpty() || browsing) {
            auto *header = new QStandardItem(kindTitle(kind));
            header->setData(true, HeaderRole);
            header->setFlags(Qt::NoItemFlags);
            d->model->appendRow(header);
        }
        Q_FOREACH (int index, groups[int(kind)]) {
            const Entry &entry = d->entries[index];
            appendResult(index, query.isEmpty() ? kindTitle(entry.kind) + (entry.detail.isEmpty() ? QString() : QStringLiteral(" · ") + entry.detail)
                                                : entry.detail);
        }
    }

    // select the first result
    for (int row = 0; row < d->model->rowCount(); row++) {
        if (!d->model->item(row)->data(HeaderRole).toBool()) {
            d->list->setCurrentIndex(d->model->index(row, 0));
            break;
        }
    }

    adjustGeometry();
}

void KisStudioFind::moveSelection(int delta)
{
    const int rows = d->model->rowCount();
    if (!rows) {
        return;
    }
    int row = d->list->currentIndex().isValid() ? d->list->currentIndex().row() : -1;
    for (int step = 0; step < rows; step++) {
        row = qBound(0, row + delta, rows - 1);
        if (!d->model->item(row)->data(HeaderRole).toBool()) {
            break;
        }
        if (row == 0 || row == rows - 1) {
            delta = -delta; // only headers left in this direction
        }
    }
    d->list->setCurrentIndex(d->model->index(row, 0));
}

bool KisStudioFind::item(const QString &key, Item *item)
{
    if (d->isStale()) {
        rebuildIndex();
    }
    Q_FOREACH (const Entry &entry, d->entries) {
        if (entry.key == key) {
            if (item) {
                *item = {entry.key, entry.title, entry.shownIcon(), entry.action, entry.aliases};
            }
            return true;
        }
    }
    return false;
}

bool KisStudioFind::run(const QString &key)
{
    if (d->isStale()) {
        rebuildIndex();
    }
    Q_FOREACH (const Entry &entry, d->entries) {
        if (entry.key == key && entry.run) {
            d->recordUse(key);
            entry.run();
            return true;
        }
    }
    return false;
}

void KisStudioFind::runSelected()
{
    if (d->list->currentIndex().isValid()) {
        runRow(d->list->currentIndex().row());
    }
}

void KisStudioFind::runRow(int row)
{
    QStandardItem *item = d->model->item(row);
    if (!item || item->data(HeaderRole).toBool()) {
        return;
    }
    const int index = item->data(EntryRole).toInt();
    if (index < 0 || index >= d->entries.size()) {
        return;
    }
    const Entry entry = d->entries[index];
    if (d->picker) {
        auto picked = d->picker;
        d->picker = nullptr;
        hide();
        if (!entry.key.isEmpty()) {
            picked(entry.key);
        }
        return;
    }
    if (!entry.key.isEmpty()) {
        d->recordUse(entry.key);
    }
    hide();
    // after the popup is gone, so that dialogs get the focus
    QTimer::singleShot(0, this, [run = entry.run]() { if (run) run(); });
    // it has a quicker way: say so, the first few times
    if (!entry.shortcut.isEmpty() && d->usage.value(entry.key).count <= 3 && d->mainWindow->viewManager()) {
        d->mainWindow->viewManager()->showFloatingMessage(i18nc("@info a shortcut Find teaches; %1 the command, %2 its keys", "%1  ·  %2", entry.title, entry.shortcut),
                                                          entry.shownIcon(), 2600, KisFloatingMessage::Low);
    }
}

void KisStudioFind::adjustGeometry()
{
    const int width = 640 + 2 * shadowMarginOf(this);
    int listHeight = 0;
    for (int row = 0; row < d->model->rowCount(); row++) {
        listHeight += d->list->sizeHintForRow(row);
    }
    listHeight = qMin(listHeight + 4, 440);
    d->list->setFixedHeight(qMax(listHeight, 0));
    d->list->setVisible(listHeight > 4);
    // the minimum size would only follow on the next layout pass and keep the window tall
    layout()->activate();

    const int height = d->edit->sizeHint().height() + 36 + (d->list->isVisible() ? listHeight + 6 : 0) + 2 * shadowMarginOf(this);
    const QRect window = d->mainWindow->geometry();
    setGeometry(window.center().x() - width / 2, window.top() + int(window.height() * 0.12), width, height);
}

bool KisStudioFind::showCard(QAbstractButton *button)
{
    QString key;
    QString title;
    QPointer<QAction> action;
    auto *tool = qobject_cast<QToolButton *>(button);
    if (tool && tool->defaultAction() && !tool->defaultAction()->objectName().isEmpty()
        && d->mainWindow->actionCollection()->action(tool->defaultAction()->objectName()) == tool->defaultAction()) {
        // an action of the window's: its own shortcut
        action = tool->defaultAction();
        key = QStringLiteral("action:") + action->objectName();
        title = cleanText(action->text());
    } else {
        registerButtons();
        for (auto it = d->buttons.constBegin(); it != d->buttons.constEnd(); ++it) {
            if (it->button == button && it->action) {
                key = it.key();
                title = it->title;
                action = it->action;
                break;
            }
        }
    }
    if (!action) {
        return false;
    }
    QIcon icon = button->icon();
    if (icon.isNull()) icon = action->icon();
    auto *card = new KisStudioItemCard(d->mainWindow, icon, title, action);
    if (d->setOnTools && d->isOnTools && !d->isOnTools(key)) {
        auto set = d->setOnTools;
        card->addCommand({KisIconUtils::loadIcon(QStringLiteral("list-add")), i18nc("@info:tooltip", "Pin to tools"),
                          [set, key]() { set(key, true); }});
    }
    card->popupBeside(button);
    return true;
}

bool KisStudioFind::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == d->list->viewport()) {
        auto *mouse = static_cast<QMouseEvent *>(event);
        if (event->type() == QEvent::ContextMenu) {
            // a result's card (right click, or a finger or pen held on it): its shortcut, and a place on the tools panel
            const QModelIndex at = d->list->indexAt(static_cast<QContextMenuEvent *>(event)->pos());
            const int index = at.data(EntryRole).isValid() ? at.data(EntryRole).toInt() : -1;
            if (index >= 0 && index < d->entries.size() && !d->entries[index].key.isEmpty()) {
                const Entry entry = d->entries[index];
                auto *card = new KisStudioItemCard(d->mainWindow, entry.shownIcon(), entry.title, entry.action);
                if (d->setOnTools && d->isOnTools && !d->isOnTools(entry.key)) {
                    auto set = d->setOnTools;
                    card->addCommand({KisIconUtils::loadIcon(QStringLiteral("list-add")),
                                      i18nc("@info:tooltip", "Pin to tools"),
                                      [set, key = entry.key]() { set(key, true); }});
                }
                QWidget *anchor = new QWidget(d->list->viewport());
                anchor->setGeometry(d->list->visualRect(at));
                card->popupBeside(anchor);
                anchor->deleteLater();
            }
            return true;
        }
        if ((event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonDblClick)
            && mouse->button() == Qt::LeftButton) {
            const QModelIndex at = d->list->indexAt(mouse->pos());
            if (at.data(PinnableRole).toBool() && pinRect(d->list->visualRect(at)).adjusted(-4, -4, 4, 4).contains(mouse->pos())) {
                d->pinPress = at;
                return true;
            }
            stopRecording();
            const KisModernStyle::KeyCap state = at.data(ShortcutRole).toString().isEmpty() ? KisModernStyle::KeyCap::Add : KisModernStyle::KeyCap::Pointed;
            if (at.data(ShortcutEditableRole).toBool()
                && shortcutCapRect(at, d->list->visualRect(at), d->list->font(), state).adjusted(-4, -4, 4, 4).contains(mouse->pos())) {
                d->recording = at;
                d->model->setData(at, true, RecordingRole);
                d->pinPress = QModelIndex();
                d->capPress = true;
                return true;
            }
        }
        if (event->type() == QEvent::MouseButtonRelease && d->capPress) {
            d->capPress = false;
            return true;
        }
        if (event->type() == QEvent::MouseButtonRelease && d->pinPress.isValid()) {
            const QModelIndex at = d->pinPress;
            d->pinPress = QModelIndex();
            if (d->list->indexAt(mouse->pos()) == at && d->setOnTools) {
                const int index = at.data(EntryRole).toInt();
                const bool pinned = !at.data(PinnedRole).toBool();
                d->setOnTools(d->entries[index].key, pinned);
                d->model->setData(at, pinned, PinnedRole);
            }
            return true;
        }
        if (event->type() == QEvent::MouseButtonPress && mouse->button() == Qt::LeftButton) {
            d->dragStart = mouse->pos();
            d->dragIndex = d->list->indexAt(mouse->pos());
        } else if (event->type() == QEvent::MouseMove && (mouse->buttons() & Qt::LeftButton) && d->dragIndex.isValid()
                   && (mouse->pos() - d->dragStart).manhattanLength() > QApplication::startDragDistance()) {
            const int index = d->dragIndex.data(EntryRole).toInt();
            d->dragIndex = QModelIndex();
            if (index >= 0 && index < d->entries.size() && !d->entries[index].key.isEmpty()) {
                // take it to the rail: Find gets out of the way of the drop
                const Entry entry = d->entries[index];
                auto *drag = new QDrag(this);
                auto *mime = new QMimeData();
                mime->setData(KisStudioShortcuts::itemMimeType(), entry.key.toUtf8());
                drag->setMimeData(mime);
                drag->setPixmap(entry.shownIcon().pixmap(32, 32));
                hide();
                drag->exec(Qt::CopyAction);
                return true;
            }
        }
    }
    if (watched == d->edit && d->recording.isValid()) {
        if (event->type() == QEvent::ShortcutOverride) {
            event->accept();
            return true;
        }
        if (event->type() == QEvent::KeyPress) {
            recordShortcut(static_cast<QKeyEvent *>(event));
            return true;
        }
    }
    if (watched == d->edit && event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        switch (key->key()) {
        case Qt::Key_Down: moveSelection(1); return true;
        case Qt::Key_Up: moveSelection(-1); return true;
        case Qt::Key_PageDown: moveSelection(6); return true;
        case Qt::Key_PageUp: moveSelection(-6); return true;
        case Qt::Key_Return:
        case Qt::Key_Enter: runSelected(); return true;
        case Qt::Key_Escape: hide(); return true;
        default: break;
        }
    }
    return QFrame::eventFilter(watched, event);
}

void KisStudioFind::stopRecording()
{
    if (d->recording.isValid()) {
        d->model->setData(d->recording, false, RecordingRole);
    }
    d->recording = QModelIndex();
}

void KisStudioFind::recordShortcut(QKeyEvent *event)
{
    const int key = event->key();
    if (key == Qt::Key_Shift || key == Qt::Key_Control || key == Qt::Key_Alt || key == Qt::Key_Meta
        || key == Qt::Key_AltGr || key == 0 || key == Qt::Key_unknown) {
        return; // a modifier alone is not a shortcut yet
    }
    const QPersistentModelIndex at = d->recording;
    stopRecording();
    const int index = at.data(EntryRole).isValid() ? at.data(EntryRole).toInt() : -1;
    if (key == Qt::Key_Escape || index < 0 || index >= d->entries.size() || !d->entries[index].action) {
        return;
    }
    QKeySequence sequence;
    if (!((key == Qt::Key_Backspace || key == Qt::Key_Delete) && event->modifiers() == Qt::NoModifier)) {
        const Qt::KeyboardModifiers modifiers = event->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
        sequence = QKeySequence(int(modifiers) | key);
    }
    const QString taken = KisStudioShortcuts::assign(d->mainWindow, d->entries[index].action, sequence);
    d->entries[index].shortcut = sequence.toString(QKeySequence::NativeText);
    d->model->setData(at, d->entries[index].shortcut, ShortcutRole);
    if (!taken.isEmpty()) {
        const QRect row = d->list->visualRect(at);
        QToolTip::showText(d->list->viewport()->mapToGlobal(QPoint(row.right() - 60, row.bottom())),
                           i18nc("@info", "Taken from “%1”", taken), d->list);
    }
}

void KisStudioFind::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const int shadowMargin = shadowMarginOf(this);
    const QRectF panel = QRectF(rect()).adjusted(shadowMargin, shadowMargin, -shadowMargin, -shadowMargin);
    const qreal radius = 12;

    if (shadowMargin) KisStudioGlass::paintShadow(painter, panel, radius, shadowMargin, 36, 4);

    // glass: what it is over still shows through
    const QPainterPath shape = KisStudioGlass::paint(painter, this, panel.adjusted(0.5, 0.5, -0.5, -0.5), radius);
    QColor border = palette().color(QPalette::WindowText);
    border.setAlphaF(0.08);
    KisStudioGlass::paintBorder(painter, this, shape, border);
}
