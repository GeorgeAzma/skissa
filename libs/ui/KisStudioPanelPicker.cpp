/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioPanelPicker.h"

#include <QDateTime>
#include <QDir>
#include <QDockWidget>
#include <QHash>
#include <QPainter>
#include <QPointer>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QtConcurrent>

#include <klocalizedstring.h>

#include "KisMainWindow.h"

namespace {

/// Portrait, like most panels
const QSize tileSize(156, 176);
/// Wide enough to stay sharp in a tile on a 2x screen
constexpr int keptWidth = 300;

/// Crops away the empty space around the content, since most panels are mostly empty
QImage trimmed(const QImage &image)
{
    const QImage img = image.convertToFormat(QImage::Format_ARGB32);
    if (img.isNull()) {
        return img;
    }
    // take the panel's background colour from a bottom corner
    const QRgb background = img.pixel(img.width() - 1, img.height() - 1);
    auto differs = [background](QRgb c) {
        return qAbs(qRed(c) - qRed(background)) + qAbs(qGreen(c) - qGreen(background)) + qAbs(qBlue(c) - qBlue(background)) > 30;
    };
    int left = img.width(), right = -1, top = img.height(), bottom = -1;
    for (int y = 0; y < img.height(); y++) {
        const QRgb *line = reinterpret_cast<const QRgb *>(img.constScanLine(y));
        for (int x = 0; x < img.width(); x++) {
            if (!differs(line[x])) continue;
            left = qMin(left, x);
            right = qMax(right, x);
            top = qMin(top, y);
            bottom = qMax(bottom, y);
        }
    }
    if (right < 0) {
        return img;
    }
    const int margin = qRound(6 * image.devicePixelRatio());
    return img.copy(QRect(QPoint(left, top), QPoint(right, bottom)).adjusted(-margin, -margin, margin, margin) & img.rect());
}

/// Panel snapshots by panel id, cached in memory and on disk
struct Looks {
    QHash<QString, QImage> images;
    QHash<QString, qint64> taken;

    static Looks &instance()
    {
        static Looks looks;
        return looks;
    }

    static QString folder()
    {
        return QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/studio-panels");
    }

    static QString fileOf(const QString &id)
    {
        QString name = id;
        name.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_.-]")), QStringLiteral("_"));
        return folder() + QLatin1Char('/') + name + QStringLiteral(".png");
    }

    QImage look(const QString &id)
    {
        auto it = images.constFind(id);
        if (it != images.constEnd()) {
            return *it;
        }
        const QImage image = trimmed(QImage(fileOf(id)));
        images.insert(id, image);
        return image;
    }

    void keep(const QString &id, const QImage &image)
    {
        if (image.isNull()) {
            return;
        }
        const QImage content = trimmed(image);
        const QImage small = content.width() > keptWidth ? content.scaledToWidth(keptWidth, Qt::SmoothTransformation) : content;
        images.insert(id, small);
        taken.insert(id, QDateTime::currentMSecsSinceEpoch());
        // save off the GUI thread, since this runs while a panel hides
        const QString file = fileOf(id);
        QtConcurrent::run([small, file]() {
            if (QDir().mkpath(folder())) small.save(file, "PNG");
        });
    }
};

/// \p look with a solid background made transparent, so it sits on the glass
QImage withoutBackground(const QImage &look)
{
    static QHash<qint64, QImage> made;
    auto it = made.constFind(look.cacheKey());
    if (it != made.constEnd()) {
        return *it;
    }
    QImage image = look.convertToFormat(QImage::Format_ARGB32);
    const QRgb background = image.pixel(image.width() - 1, image.height() - 1);
    if (qAlpha(background) == 255) {
        auto close = [background](QRgb c) {
            return qAbs(qRed(c) - qRed(background)) <= 2 && qAbs(qGreen(c) - qGreen(background)) <= 2
                && qAbs(qBlue(c) - qBlue(background)) <= 2;
        };
        for (int y = 0; y < image.height(); y++) {
            QRgb *line = reinterpret_cast<QRgb *>(image.scanLine(y));
            for (int x = 0; x < image.width(); x++) {
                if (close(line[x])) line[x] = 0;
            }
        }
    }
    if (made.size() > 64) made.clear();
    made.insert(look.cacheKey(), image);
    return image;
}

KisStudioPicker::Source panelsSource(KisMainWindow *mainWindow, const KisStudioPanelPicker::Rail &rail)
{
    QPointer<KisMainWindow> window = mainWindow;
    auto docks = [window]() {
        QHash<QString, QDockWidget *> result;
        if (!window) return result;
        Q_FOREACH (QDockWidget *dock, window->dockWidgets()) {
            if (!dock->objectName().isEmpty() && !KisStudioPanelPicker::isRetired(dock->objectName()) && !dock->windowTitle().isEmpty()) {
                result.insert(dock->objectName(), dock);
            }
        }
        return result;
    };

    KisStudioPicker::Source source;
    source.pinnedKey = rail.key;
    source.initialPinned = rail.defaults;
    source.groupsKey = rail.key + QStringLiteral("Groups");
    source.allIds = [docks]() {
        const QHash<QString, QDockWidget *> all = docks();
        QStringList ids = all.keys();
        std::sort(ids.begin(), ids.end(), [&all](const QString &a, const QString &b) {
            return all.value(a)->windowTitle().localeAwareCompare(all.value(b)->windowTitle()) < 0;
        });
        return ids;
    };
    source.otherGroups = [allIds = source.allIds](const QStringList &pinned) {
        KisStudioPicker::Group group{i18nc("panels group: the ones not on the rail", "More panels"), {}};
        Q_FOREACH (const QString &id, allIds()) {
            if (!pinned.contains(id)) group.ids << id;
        }
        return QVector<KisStudioPicker::Group>{group};
    };
    source.name = [docks](const QString &id) {
        QDockWidget *dock = docks().value(id);
        return dock ? dock->windowTitle() : id;
    };
    source.aliases = [](const QString &id) { return QStringList{id}; };
    source.paintPreview = [icon = rail.icon, docks](QPainter &painter, const QRect &rect, const QString &id) {
        const QImage look = Looks::instance().look(id);
        painter.save();
        if (!look.isNull()) {
            const QImage onGlass = withoutBackground(look);
            const QSize fitted = onGlass.size().scaled(rect.size() - QSize(8, 8), Qt::KeepAspectRatio);
            QRect target(QPoint(0, 0), fitted);
            target.moveCenter(rect.center());
            painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
            painter.drawImage(target, onGlass);
        } else {
            // not shown yet; laying the panel out off screen would disturb it
            painter.fillRect(rect, QColor(128, 128, 128, 40));
            if (QDockWidget *dock = docks().value(id)) {
                QRect iconRect(0, 0, 32, 32);
                iconRect.moveCenter(rect.center());
                painter.setOpacity(painter.opacity() * 0.6);
                (icon ? icon(dock) : dock->windowIcon()).paint(&painter, iconRect);
            }
        }
        painter.restore();
    };
    source.tiles = true;
    source.tileSize = tileSize;
    source.yoursTitle = i18nc("panels group: the ones on the rail", "On the rail");
    source.searchPlaceholder = i18nc("@info:placeholder", "Search panels");
    return source;
}

} // namespace

bool KisStudioPanelPicker::isRetired(const QString &panelId)
{
    // the panels the Studio covers otherwise are not built; the toolbox stays for the classic layout
    return panelId == QLatin1String("ToolBox");
}

KisStudioPanelPicker::KisStudioPanelPicker(KisMainWindow *mainWindow, const Rail &rail)
    : KisStudioPicker(panelsSource(mainWindow, rail), QString(), QSet<QString>(), mainWindow)
{
    Q_FOREACH (QDockWidget *dock, mainWindow->dockWidgets()) {
        if (dock->isVisible()) rememberLook(dock);
    }
}

void KisStudioPanelPicker::rememberLook(QDockWidget *dock)
{
    QWidget *content = dock ? dock->widget() : nullptr;
    // only while laid out on screen, not while the window closes
    if (!content || !dock->window() || !dock->window()->isVisible() || content->width() < 40 || content->height() < 40) {
        return;
    }
    Looks &looks = Looks::instance();
    // at most once a second per panel
    if (QDateTime::currentMSecsSinceEpoch() - looks.taken.value(dock->objectName(), 0) < 1000) {
        return;
    }
    looks.keep(dock->objectName(), content->grab().toImage());
}
