/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioImagePage.h"

#include <QAction>
#include <QFileInfo>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMdiArea>
#include <QMdiSubWindow>
#include <QPainter>
#include <QPointer>
#include <QSet>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <KisDocument.h>
#include <KisMainWindow.h>
#include <KisModernStyle.h>
#include <KisRecentFilesManager.h>
#include <KisView.h>
#include <KisViewManager.h>
#include <kactioncollection.h>
#include <kis_icon_utils.h>
#include <kis_image.h>
#include <kis_paint_device.h>
#include <kis_selection.h>
#include <klocalizedstring.h>

#include "utils/KisRecentFileIconCache.h"
#include "widgets/KisStudioPopup.h"

#include <functional>

namespace
{
const int picture = 64;
const int mostPictures = 6;

/// \p image fitted in the middle of a square, so tiles line up whatever its shape
QIcon pictureIcon(const QImage &image, qreal dpr)
{
    const int side = qRound(picture * dpr);
    QPixmap square(side, side);
    square.fill(Qt::transparent);
    if (!image.isNull()) {
        const QImage fitted = image.scaled(side, side, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        QPainter p(&square);
        p.drawImage((side - fitted.width()) / 2, (side - fitted.height()) / 2, fitted);
    }
    square.setDevicePixelRatio(dpr);
    return QIcon(square);
}

QToolButton *tile(QWidget *parent, const QIcon &icon, const QString &text, const QString &tip = QString())
{
    auto *button = new QToolButton(parent);
    button->setAutoRaise(true);
    button->setFocusPolicy(Qt::NoFocus);
    button->setIcon(icon);
    button->setIconSize(QSize(picture, picture));
    button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    const int width = picture + 2 * KisModernStyle::space2;
    QString shown = button->fontMetrics().elidedText(text, Qt::ElideMiddle, width - KisModernStyle::space1);
    button->setText(shown.replace(QLatin1Char('&'), QLatin1String("&&")));
    button->setToolTip(tip.isEmpty() && shown != text ? text : tip);
    button->setFixedWidth(width);
    return button;
}

/// A small ✕ in \p tile's corner while the pointer is over it
QToolButton *closer(QToolButton *tile)
{
    auto *x = new QToolButton(tile);
    x->setAutoRaise(true);
    x->setFocusPolicy(Qt::NoFocus);
    x->setIcon(KisIconUtils::loadIcon(QStringLiteral("studio_cancel")));
    x->setIconSize(QSize(12, 12));
    x->setFixedSize(20, 20);
    x->setToolTip(i18nc("@action:button close the image", "Close"));
    x->move(tile->width() - x->width() - KisModernStyle::hairline, KisModernStyle::hairline);
    x->hide();
    struct Hover : QObject {
        QToolButton *x;
        bool eventFilter(QObject *, QEvent *event) override
        {
            if (event->type() == QEvent::Enter || event->type() == QEvent::HoverEnter) x->show();
            if (event->type() == QEvent::Leave || event->type() == QEvent::HoverLeave) x->hide();
            return false;
        }
    };
    auto *hover = new Hover;
    hover->setParent(tile);
    hover->x = x;
    tile->installEventFilter(hover);
    return x;
}

QToolButton *word(QWidget *parent, const QString &text)
{
    auto *button = new QToolButton(parent);
    button->setAutoRaise(true);
    button->setFocusPolicy(Qt::NoFocus);
    button->setToolButtonStyle(Qt::ToolButtonTextOnly);
    button->setText(text);
    return button;
}

QString nameOf(KisDocument *document)
{
    const QString path = document->path();
    return path.isEmpty() ? i18nc("a document that was never saved", "Untitled") : QFileInfo(path).fileName();
}

QHBoxLayout *row(QVBoxLayout *column, int spacing)
{
    auto *line = new QHBoxLayout();
    line->setContentsMargins(0, 0, 0, 0);
    line->setSpacing(spacing);
    column->addLayout(line);
    return line;
}
} // namespace

void KisStudioImagePage::open(KisMainWindow *window, QMdiArea *area, QWidget *anchor)
{
    auto *popup = new KisStudioPopup(window);
    popup->setAttribute(Qt::WA_DeleteOnClose);
    auto *column = new QVBoxLayout(popup);
    popup->fitLayout(column, KisModernStyle::space3);
    column->setSpacing(KisModernStyle::space3);
    const qreal dpr = window->devicePixelRatioF();
    KisKActionCollection *actions = window->actionCollection();
    QPointer<KisMainWindow> owner = window;

    // the page goes away first, so what the click starts (a dialog, another image) has the window to itself
    auto later = [popup](std::function<void()> run) {
        popup->close();
        QTimer::singleShot(0, run);
    };
    auto trigger = [owner, later](const QString &name) {
        later([owner, name]() {
            QAction *action = owner ? owner->actionCollection()->action(name) : nullptr;
            if (action && action->isEnabled()) action->trigger();
        });
    };
    auto command = [&](QHBoxLayout *line, const char *name, QToolButton *button) {
        QAction *action = actions->action(QLatin1String(name));
        if (!action || !action->isEnabled()) {
            delete button;
            return;
        }
        QObject::connect(button, &QAbstractButton::clicked, popup, [trigger, name]() { trigger(QLatin1String(name)); });
        line->addWidget(button);
    };

    // images: New, Open, the open ones, then the recent ones
    QHBoxLayout *images = row(column, KisModernStyle::space1);
    command(images, "file_new", tile(popup, KisIconUtils::loadIcon(QStringLiteral("studio_add")), i18nc("@action:button a new image", "New")));
    command(images, "file_open", tile(popup, KisIconUtils::loadIcon(QStringLiteral("document-open")), i18nc("@action:button open an image", "Open")));
    int pictures = 0;
    QSet<QString> shownPaths;
    QMdiSubWindow *current = area ? area->currentSubWindow() : nullptr;
    const QList<QMdiSubWindow *> windows = area ? area->subWindowList() : QList<QMdiSubWindow *>();
    for (QMdiSubWindow *sub : windows) {
        KisView *view = qobject_cast<KisView *>(sub->widget());
        KisDocument *document = view ? view->document() : nullptr;
        if (!document || pictures >= mostPictures) continue;
        shownPaths << document->path();
        QImage thumbnail;
        if (document->image()) {
            thumbnail = document->image()->projection()->createThumbnail(qRound(picture * dpr), qRound(picture * dpr), Qt::KeepAspectRatio);
        }
        QString name = nameOf(document);
        if (document->isModified()) name += QStringLiteral(" •");
        QToolButton *button = tile(popup, pictureIcon(thumbnail, dpr), name);
        button->setCheckable(true);
        button->setChecked(sub == current);
        QPointer<QMdiSubWindow> target = sub;
        QObject::connect(button, &QAbstractButton::clicked, popup, [later, area, target]() {
            later([area, target]() {
                if (area && target) area->setActiveSubWindow(target);
            });
        });
        QToolButton *x = closer(button);
        QObject::connect(x, &QAbstractButton::clicked, popup, [later, owner, area, target]() {
            later([owner, area, target]() {
                if (!owner || !area || !target) return;
                area->setActiveSubWindow(target);
                QAction *close = owner->actionCollection()->action(QStringLiteral("file_close"));
                if (close && close->isEnabled()) close->trigger();
            });
        });
        images->addWidget(button);
        pictures++;
    }
    Q_FOREACH (const QUrl &url, KisRecentFilesManager::instance()->recentUrlsLatestFirst()) {
        if (pictures >= mostPictures) break;
        const QString path = url.toLocalFile();
        if (shownPaths.contains(path) || (url.isLocalFile() && !QFileInfo::exists(path))) continue;
        QToolButton *button = tile(popup, KisRecentFileIconCache::instance()->getOrQueueFileIcon(url), url.fileName(), path);
        QObject::connect(button, &QAbstractButton::clicked, popup, [later, owner, url]() {
            later([owner, url]() {
                if (owner) owner->slotFileOpenRecent(url);
            });
        });
        images->addWidget(button);
        pictures++;
    }
    images->addStretch(1);

    // this image: its name and size, keeping it, and changes to all of it shown on the picture
    KisView *view = window->activeView();
    KisImageSP image = view ? view->image() : nullptr;
    if (view && view->document() && image) {
        QHBoxLayout *keep = row(column, KisModernStyle::space1);
        auto *name = new QLabel(nameOf(view->document()), popup);
        QFont bold = name->font();
        bold.setBold(true);
        name->setFont(bold);
        keep->addWidget(name);
        QToolButton *size = word(popup, i18nc("image width × height in pixels", "%1 × %2", image->width(), image->height()));
        size->setToolTip(i18nc("@info:tooltip", "Image size"));
        command(keep, "imagesize", size);
        keep->addStretch(1);

        // keeping it, then changing all of it, shown on the picture
        QHBoxLayout *change = row(column, KisModernStyle::space1);
        command(change, "file_save", tile(popup, KisIconUtils::loadIcon(QStringLiteral("studio_save")), i18nc("@action:button", "Save")));
        command(change, "file_save_as", tile(popup, KisIconUtils::loadIcon(QStringLiteral("studio_save_as")), i18nc("@action:button", "Save as")));
        command(change, "file_export_file", tile(popup, KisIconUtils::loadIcon(QStringLiteral("studio_export")), i18nc("@action:button", "Export")));
        change->addSpacing(KisModernStyle::space4);
        const QImage shown = image->projection()->createThumbnail(qRound(picture * dpr), qRound(picture * dpr), Qt::KeepAspectRatio);
        command(change, "mirrorImageHorizontal", tile(popup, pictureIcon(shown.mirrored(true, false), dpr), i18nc("@action:button flip the image", "Flip")));
        command(change, "rotateImageCW90", tile(popup, pictureIcon(shown.transformed(QTransform().rotate(90)), dpr), i18nc("@action:button turn the image a quarter", "Turn")));
        command(change, "canvassize", tile(popup, KisIconUtils::loadIcon(QStringLiteral("studio_crop")), i18nc("@action:button", "Canvas size")));
        KisSelectionSP selection = window->viewManager() ? window->viewManager()->selection() : nullptr;
        const QRect bounds = selection ? selection->selectedExactRect() & image->bounds() : QRect();
        if (!bounds.isEmpty()) {
            const qreal scale = qMin(1.0, picture * dpr / qMax(bounds.width(), bounds.height()));
            const QImage cropped = image->projection()->createThumbnailUncached(qMax(1, qRound(bounds.width() * scale)), qMax(1, qRound(bounds.height() * scale)), bounds);
            command(change, "resizeimagetoselection", tile(popup, pictureIcon(cropped, dpr), i18nc("@action:button crop the image to the selection", "Crop")));
        }
        change->addStretch(1);
    }

    popup->popupBeside(anchor, Qt::BottomEdge);
}
