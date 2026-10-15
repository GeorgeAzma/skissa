/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioStartPage.h"

#include <QAbstractButton>
#include <QApplication>
#include <QClipboard>
#include <QDragEnterEvent>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QScreen>
#include <QStandardItemModel>
#include <QToolButton>
#include <QVBoxLayout>

#include <klocalizedstring.h>

#include <KisModernStyle.h>
#include <KoColor.h>
#include <KoColorSpaceRegistry.h>
#include <kis_icon_utils.h>

#include "KisDocument.h"
#include "KisMainWindow.h"
#include "KisPart.h"
#include "dialogs/KisDlgCreateNewDocument.h"
#include "kis_clipboard.h"
#include "kis_config.h"
#include "utils/KisRecentDocumentsModelWrapper.h"

namespace
{

QColor withAlpha(QColor color, qreal alpha)
{
    color.setAlphaF(alpha);
    return color;
}

/// A size for a new image, drawn as its shape; or Custom, or the image on the clipboard
class NewTile : public QAbstractButton
{
public:
    static constexpr int tileWidth = 124;
    static constexpr int tileHeight = 150;

    NewTile(const QString &name, const QSize &pixels, QWidget *parent)
        : QAbstractButton(parent)
        , m_pixels(pixels)
    {
        setText(name);
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_Hover);
        setFixedSize(tileWidth, tileHeight);
    }

    /// Shows \p picture in place of a shape (the clipboard's image)
    void setPicture(const QImage &picture)
    {
        m_picture = picture;
        update();
    }

    QSize pixels() const { return m_pixels; }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QColor ink = palette().color(QPalette::WindowText);
        painter.setPen(Qt::NoPen);
        painter.setBrush(withAlpha(ink, isDown() ? 0.14 : underMouse() ? 0.1 : 0.05));
        painter.drawPath(KisModernStyle::squircle(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), KisModernStyle::tileRadius));

        // the shape, in a box over the name
        const QRectF box(KisModernStyle::space3, KisModernStyle::space3, width() - 2 * KisModernStyle::space3, 84);
        if (!m_picture.isNull()) {
            const QSizeF fitted = QSizeF(m_picture.size()).scaled(box.size(), Qt::KeepAspectRatio);
            const QRectF at(box.center() - QPointF(fitted.width(), fitted.height()) / 2, fitted);
            painter.save();
            painter.setClipPath(KisModernStyle::squircle(at, 3));
            painter.drawImage(at, m_picture);
            painter.restore();
        } else if (m_pixels.isValid()) {
            const QSizeF fitted = QSizeF(m_pixels).scaled(box.size() * 0.86, Qt::KeepAspectRatio);
            const QRectF at(box.center() - QPointF(fitted.width(), fitted.height()) / 2, fitted);
            KisConfig cfg(true);
            QColor paper = cfg.defaultBackgroundColor();
            paper.setAlpha(qMax(40, int(cfg.defaultBackgroundOpacity())));
            painter.setBrush(paper);
            painter.setPen(QPen(withAlpha(ink, 0.25), 1));
            painter.drawPath(KisModernStyle::squircle(at, 3));
        } else {
            // Custom: an outline to fill in
            QPen pen(withAlpha(ink, underMouse() ? 0.6 : 0.4), 1.5, Qt::DashLine);
            painter.setPen(pen);
            painter.setBrush(Qt::NoBrush);
            const QRectF at = QRectF(box.center() - QPointF(30, 30), QSizeF(60, 60));
            painter.drawPath(KisModernStyle::squircle(at, 4));
            painter.setPen(QPen(withAlpha(ink, 0.7), 1.5, Qt::SolidLine, Qt::RoundCap));
            painter.drawLine(at.center() - QPointF(8, 0), at.center() + QPointF(8, 0));
            painter.drawLine(at.center() - QPointF(0, 8), at.center() + QPointF(0, 8));
        }

        // the name, then the size quietly
        painter.setPen(ink);
        painter.setFont(font());
        const QRect nameRect(KisModernStyle::space2, int(box.bottom()) + KisModernStyle::space2, width() - 2 * KisModernStyle::space2, fontMetrics().height());
        painter.drawText(nameRect, Qt::AlignHCenter | Qt::AlignVCenter, fontMetrics().elidedText(text(), Qt::ElideRight, nameRect.width()));
        if (m_pixels.isValid()) {
            const QFont small = KisModernStyle::textFont(font(), 0.88);
            painter.setFont(small);
            painter.setPen(KisModernStyle::secondaryInk(ink));
            const QRect sizeRect = nameRect.translated(0, nameRect.height() + 1);
            painter.drawText(sizeRect, Qt::AlignHCenter | Qt::AlignVCenter,
                             i18nc("an image's size in pixels: width × height", "%1 × %2", m_pixels.width(), m_pixels.height()));
        }
    }

private:
    QSize m_pixels;
    QImage m_picture;
};

/// The recent images as large thumbnails, wrapping to the width; a click opens one
class RecentGrid : public QWidget
{
public:
    static constexpr int cell = 196;
    static constexpr int gap = KisModernStyle::space3;

    std::function<void(const QString &path)> opened;
    std::function<void(const QString &path)> forgotten;

    explicit RecentGrid(QWidget *parent)
        : QWidget(parent)
    {
        setMouseTracking(true);
        setAttribute(Qt::WA_Hover);
        QSizePolicy policy(QSizePolicy::Preferred, QSizePolicy::Preferred);
        policy.setHeightForWidth(true);
        setSizePolicy(policy);
        QStandardItemModel *model = &KisRecentDocumentsModelWrapper::instance()->model();
        m_model = model;
        auto refresh = [this]() {
            updateGeometry();
            update();
        };
        connect(model, &QAbstractItemModel::rowsInserted, this, refresh);
        connect(model, &QAbstractItemModel::rowsRemoved, this, refresh);
        connect(model, &QAbstractItemModel::modelReset, this, refresh);
        connect(model, &QAbstractItemModel::dataChanged, this, [this]() { update(); });
    }

    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override
    {
        const int count = m_model ? m_model->rowCount() : 0;
        if (!count) return fontMetrics().height() * 3;
        const int columns = qMax(1, (width + gap) / (cell + gap));
        const int rows = (count + columns - 1) / columns;
        return rows * (cell + nameHeight()) + (rows - 1) * gap;
    }
    QSize sizeHint() const override { return QSize(4 * cell + 3 * gap, heightForWidth(4 * cell + 3 * gap)); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        const QColor ink = palette().color(QPalette::WindowText);
        const int count = m_model ? m_model->rowCount() : 0;
        if (!count) {
            painter.setPen(KisModernStyle::secondaryInk(ink));
            painter.drawText(rect(), Qt::AlignLeft | Qt::AlignTop, i18nc("@info the start page's recent images, when there are none", "Images you open show here"));
            return;
        }
        for (int i = 0; i < count; i++) {
            const QRect at = cellRect(i);
            const QModelIndex index = m_model->index(i, 0);
            const QRectF card = QRectF(at.left(), at.top(), cell, cell).adjusted(0.5, 0.5, -0.5, -0.5);
            painter.setPen(Qt::NoPen);
            painter.setBrush(withAlpha(ink, i == m_hovered ? 0.1 : 0.05));
            painter.drawPath(KisModernStyle::squircle(card, KisModernStyle::cardRadius));
            const QIcon icon = index.data(Qt::DecorationRole).value<QIcon>();
            if (!icon.isNull()) {
                const QRect inside = card.adjusted(KisModernStyle::space2, KisModernStyle::space2, -KisModernStyle::space2, -KisModernStyle::space2).toRect();
                const QPixmap pixmap = icon.pixmap(inside.size() * devicePixelRatioF());
                QSizeF fitted = QSizeF(pixmap.size()) / devicePixelRatioF();
                fitted = fitted.scaled(inside.size(), Qt::KeepAspectRatio);
                const QRectF picture(QRectF(inside).center() - QPointF(fitted.width(), fitted.height()) / 2, fitted);
                painter.save();
                painter.setClipPath(KisModernStyle::squircle(picture, 4));
                painter.drawPixmap(picture, pixmap, QRectF(pixmap.rect()));
                painter.restore();
            }
            painter.setPen(ink);
            const QRect nameRect(at.left() + 2, at.top() + cell + 2, cell - 4, nameHeight() - 2);
            painter.drawText(nameRect, Qt::AlignHCenter | Qt::AlignVCenter,
                             fontMetrics().elidedText(index.data(Qt::DisplayRole).toString(), Qt::ElideMiddle, nameRect.width()));
        }
    }

    bool event(QEvent *event) override
    {
        if (event->type() == QEvent::ToolTip) {
            const int at = indexAt(static_cast<QHelpEvent *>(event)->pos());
            if (at >= 0) setToolTip(pathOf(at));
            else setToolTip(QString());
        }
        return QWidget::event(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        const int hovered = indexAt(event->pos());
        if (hovered != m_hovered) {
            m_hovered = hovered;
            setCursor(hovered >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
            update();
        }
    }

    void leaveEvent(QEvent *) override
    {
        m_hovered = -1;
        update();
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        const int at = indexAt(event->pos());
        if (event->button() == Qt::LeftButton && at >= 0 && opened) opened(pathOf(at));
    }

    void contextMenuEvent(QContextMenuEvent *event) override
    {
        const int at = indexAt(event->pos());
        if (at < 0) return;
        const QString path = pathOf(at);
        QMenu menu;
        QAction *forget = menu.addAction(i18nc("@action:inmenu take an image off the recent images", "Forget"));
        if (menu.exec(event->globalPos()) == forget && forgotten) forgotten(path);
    }

private:
    int nameHeight() const { return fontMetrics().height() + KisModernStyle::space2; }

    int columns() const { return qMax(1, (width() + gap) / (cell + gap)); }

    QRect cellRect(int i) const
    {
        const int column = i % columns();
        const int row = i / columns();
        return QRect(column * (cell + gap), row * (cell + nameHeight() + gap), cell, cell + nameHeight());
    }

    int indexAt(const QPoint &pos) const
    {
        const int count = m_model ? m_model->rowCount() : 0;
        for (int i = 0; i < count; i++) {
            if (cellRect(i).contains(pos)) return i;
        }
        return -1;
    }

    QString pathOf(int i) const { return m_model->index(i, 0).data(Qt::ToolTipRole).toString(); }

    QPointer<QStandardItemModel> m_model;
    int m_hovered {-1};
};

QLabel *heading(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setFont(KisModernStyle::textFont(label->font(), 1.35, QFont::DemiBold));
    return label;
}

} // namespace

struct KisStudioStartPage::Private
{
    QPointer<KisMainWindow> mainWindow;
    QPointer<NewTile> clipboardTile;
    bool dropping {false};

    /// A new image of \p pixels at \p pixelsPerPoint, otherwise as the last image made
    void create(const QSize &pixels, qreal pixelsPerPoint)
    {
        KisConfig cfg(true);
        const KoColorSpace *cs = KoColorSpaceRegistry::instance()->colorSpace(cfg.defColorModel(), cfg.defaultColorDepth(), cfg.defColorProfile());
        if (!cs) cs = KoColorSpaceRegistry::instance()->rgb8();
        QColor background = cfg.defaultBackgroundColor();
        background.setAlpha(cfg.defaultBackgroundOpacity());
        KisDocument *doc = KisPart::instance()->createDocument();
        if (!doc->newImage(i18n("Unnamed"), pixels.width(), pixels.height(), cs, KoColor(background, cs), cfg.defaultBackgroundStyle(),
                           cfg.numDefaultLayers(), QString(), pixelsPerPoint)) {
            delete doc;
            return;
        }
        doc->setModified(false);
        KisPart::instance()->startCustomDocument(doc);
    }
};

KisStudioStartPage::KisStudioStartPage(KisMainWindow *mainWindow)
    : QWidget(nullptr)
    , d(new Private)
{
    d->mainWindow = mainWindow;
    setObjectName(QStringLiteral("studioStartPage"));
    setAcceptDrops(true);
    setAutoFillBackground(false);

    auto *outer = new QHBoxLayout(this);
    outer->setContentsMargins(KisModernStyle::space4 * 3, KisModernStyle::space4 * 4, KisModernStyle::space4 * 3, KisModernStyle::space4 * 2);
    outer->addStretch(1);
    auto *column = new QVBoxLayout();
    column->setSpacing(KisModernStyle::space3);
    outer->addLayout(column, 8);
    outer->addStretch(1);

    // New, and Open beside it
    auto *newRow = new QHBoxLayout();
    newRow->addWidget(heading(i18nc("@title the start page's new images", "New"), this));
    newRow->addStretch(1);
    auto *open = new QToolButton(this);
    open->setText(i18nc("@action:button open an image", "Open"));
    open->setIcon(KisIconUtils::loadIcon(QStringLiteral("document-open")));
    open->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    open->setAutoRaise(true);
    open->setCursor(Qt::PointingHandCursor);
    connect(open, &QToolButton::clicked, this, [this]() {
        if (d->mainWindow) d->mainWindow->slotFileOpen();
    });
    newRow->addWidget(open);
    column->addLayout(newRow);

    auto *tiles = new QHBoxLayout();
    tiles->setSpacing(KisModernStyle::space3);
    KisConfig cfg(true);
    const qreal pixelsPerPoint = cfg.defImageResolution();
    QScreen *screen = mainWindow && mainWindow->screen() ? mainWindow->screen() : QGuiApplication::primaryScreen();
    const QSize screenPixels = screen ? screen->size() * screen->devicePixelRatio() : QSize(1920, 1080);
    struct Preset {
        QString name;
        QSize pixels;
        qreal pixelsPerPoint;
    };
    QList<Preset> presets = {
        {i18nc("a new image's size: as big as the screen", "Screen"), screenPixels, pixelsPerPoint},
        {i18nc("a new image's size", "Square"), QSize(2048, 2048), pixelsPerPoint},
        {i18nc("a new image's size: the paper size, at 300 dpi", "A4"), QSize(2480, 3508), 300.0 / 72.0},
        {i18nc("a new image's size", "4K"), QSize(3840, 2160), pixelsPerPoint},
        {i18nc("a new image's size: a phone's screen, upright", "Portrait"), QSize(1080, 1920), pixelsPerPoint},
    };
    // the size made last, when it's none of these
    const QSize last(cfg.defImageWidth(), cfg.defImageHeight());
    bool known = false;
    for (const Preset &preset : presets) known = known || preset.pixels == last;
    if (!known && last.isValid()) presets.prepend({i18nc("a new image's size: the one made last", "Last"), last, pixelsPerPoint});
    for (const Preset &preset : presets) {
        auto *tile = new NewTile(preset.name, preset.pixels, this);
        const QSize pixels = preset.pixels;
        const qreal ppp = preset.pixelsPerPoint;
        connect(tile, &QAbstractButton::clicked, this, [this, pixels, ppp]() { d->create(pixels, ppp); });
        tiles->addWidget(tile);
    }
    auto *clipboard = new NewTile(i18nc("a new image: from the image on the clipboard", "Clipboard"), QSize(), this);
    connect(clipboard, &QAbstractButton::clicked, this, [this]() {
        auto *dialog = new KisDlgCreateNewDocument(this);
        dialog->SelectPage(KisDlgCreateNewDocument::Page::CreateFromClipboard);
        dialog->exec();
        dialog->deleteLater();
    });
    d->clipboardTile = clipboard;
    auto updateClipboard = [this]() {
        if (!d->clipboardTile) return;
        const QImage image = QGuiApplication::clipboard() ? QGuiApplication::clipboard()->image() : QImage();
        d->clipboardTile->setVisible(!image.isNull());
        if (!image.isNull()) d->clipboardTile->setPicture(image.scaled(240, 240, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    };
    connect(KisClipboard::instance(), &KisClipboard::clipChanged, this, updateClipboard);
    updateClipboard();
    tiles->addWidget(clipboard);
    auto *custom = new NewTile(i18nc("a new image: made to measure in the full dialog", "Custom"), QSize(), this);
    custom->setToolTip(i18nc("@info:tooltip", "Size, resolution, color, templates"));
    connect(custom, &QAbstractButton::clicked, this, [this]() {
        if (d->mainWindow) d->mainWindow->slotFileNew();
    });
    tiles->addWidget(custom);
    tiles->addStretch(1);
    column->addLayout(tiles);

    column->addSpacing(KisModernStyle::space4 * 2);
    column->addWidget(heading(i18nc("@title the start page's recent images", "Recent"), this));
    auto *recent = new RecentGrid(this);
    recent->opened = [this](const QString &path) {
        if (d->mainWindow) d->mainWindow->openDocument(path, KisMainWindow::None);
    };
    recent->forgotten = [this](const QString &path) {
        if (d->mainWindow) d->mainWindow->removeRecentFile(path);
    };
    column->addWidget(recent);
    column->addStretch(1);
}

KisStudioStartPage::~KisStudioStartPage()
{
    delete d;
}

void KisStudioStartPage::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.fillRect(rect(), palette().color(QPalette::Window));
    if (d->dropping) {
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(QPen(palette().color(QPalette::Highlight), 2));
        painter.setBrush(withAlpha(palette().color(QPalette::Highlight), 0.06));
        painter.drawPath(KisModernStyle::squircle(QRectF(rect()).adjusted(KisModernStyle::space3, KisModernStyle::space3, -KisModernStyle::space3, -KisModernStyle::space3),
                                                  KisModernStyle::panelRadius));
    }
}

void KisStudioStartPage::dragEnterEvent(QDragEnterEvent *event)
{
    if (!event->mimeData()->hasUrls()) {
        event->ignore();
        return;
    }
    event->acceptProposedAction();
    d->dropping = true;
    update();
}

void KisStudioStartPage::dragLeaveEvent(QDragLeaveEvent *)
{
    d->dropping = false;
    update();
}

void KisStudioStartPage::dropEvent(QDropEvent *event)
{
    d->dropping = false;
    update();
    if (!d->mainWindow) return;
    Q_FOREACH (const QUrl &url, event->mimeData()->urls()) {
        if (!url.isLocalFile()) continue;
        const QString path = url.toLocalFile();
        if (path.endsWith(QLatin1String(".bundle"), Qt::CaseInsensitive)) {
            d->mainWindow->installBundle(path);
        } else {
            d->mainWindow->openDocument(path, KisMainWindow::None);
        }
    }
    event->acceptProposedAction();
}
