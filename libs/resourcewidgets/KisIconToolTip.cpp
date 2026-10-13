/* This file is part of the KDE project
 * SPDX-FileCopyrightText: 1999 Carsten Pfeiffer (pfeiffer@kde.org)
 * SPDX-FileCopyrightText: 2002 Igor Jansen (rm@kde.org)
 * SPDX-FileCopyrightText: 2018 Boudewijn Rempt <boud@valdyas.org>
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
*/

#include "KisIconToolTip.h"

#include <QTextDocument>
#include <QUrl>
#include <QModelIndex>
#include <QPainter>
#include <QRegularExpression>
#include <QtMath>

#include <KisResourceModel.h>
#include <KisResourceThumbnailCache.h>
#include <klocalizedstring.h>

#include "KoCheckerBoardPainter.h"

KisIconToolTip::KisIconToolTip(QWidget *parent)
    : KoItemToolTip(parent)
{
}

KisIconToolTip::~KisIconToolTip()
{
}

void KisIconToolTip::setFixedToolTipThumbnailSize(const QSize &size)
{
    m_fixedToolTipThumbnailSize = size;
}

void KisIconToolTip::setToolTipShouldRenderCheckers(bool value)
{
    if (value) {
        m_checkersPainter.reset(new KoCheckerBoardPainter(4));
    } else {
        m_checkersPainter.reset();
    }
}

namespace
{
KisIconToolTip::PictureProvider &pictureProvider()
{
    static KisIconToolTip::PictureProvider provider;
    return provider;
}
} // namespace

void KisIconToolTip::setPictureProvider(const PictureProvider &provider)
{
    pictureProvider() = provider;
}

QImage KisIconToolTip::card(const QModelIndex &index)
{
    if (!pictureProvider()) return QImage();
    const QColor ink = palette().color(QPalette::ToolTipText);
    const QImage picture = pictureProvider()(index, ink);
    if (picture.isNull()) return QImage();

    const qreal ratio = devicePixelRatioF();
    QString name = index.data(Qt::UserRole + KisAbstractResourceModel::Name).toString().replace(QLatin1Char('_'), QLatin1Char(' '));
    if (name.isEmpty()) name = index.data(Qt::DisplayRole).toString();
    // the sorting prefix of Krita's own brushes ("b) ") is not part of the name
    name.remove(QRegularExpression(QStringLiteral("^[a-z]\\)\\s+")));
    const QStringList tags = index.data(Qt::UserRole + KisAbstractResourceModel::Tags).toStringList();
    QString location = index.data(Qt::UserRole + KisAbstractResourceModel::Location).toString();

    const int width = 240;
    const int pad = 2;
    QFont titleFont = font();
    titleFont.setWeight(QFont::DemiBold);
    titleFont.setPointSizeF(titleFont.pointSizeF() * 1.1);
    QFont smallFont = font();
    smallFont.setPointSizeF(smallFont.pointSizeF() * 0.88);
    const QFontMetrics titleMetrics(titleFont);
    const QFontMetrics smallMetrics(smallFont);

    // the picture as large as fits the width, at most 120 tall
    QSizeF pictureSize = QSizeF(picture.size()) / picture.devicePixelRatio();
    pictureSize.scale(QSizeF(width, 120), Qt::KeepAspectRatio);

    // the tags, wrapped into rows of pills
    const int pillHeight = smallMetrics.height() + 6;
    QVector<QRectF> pills;
    qreal x = 0;
    qreal y = 0;
    Q_FOREACH (const QString &tag, tags) {
        const qreal w = smallMetrics.horizontalAdvance(tag) + 14;
        if (x > 0 && x + w > width) {
            x = 0;
            y += pillHeight + 4;
        }
        pills << QRectF(x, y, w, pillHeight);
        x += w + 4;
    }
    const qreal tagsHeight = pills.isEmpty() ? 0 : y + pillHeight;
    const QString where = smallMetrics.elidedText(location, Qt::ElideMiddle, width);

    const qreal height = pad + pictureSize.height() + 10 + titleMetrics.height() + (pills.isEmpty() ? 0 : 8 + tagsHeight)
        + (where.isEmpty() ? 0 : 6 + smallMetrics.height()) + pad;
    QImage card(QSize(width + 2 * pad, qCeil(height)) * ratio, QImage::Format_ARGB32_Premultiplied);
    card.setDevicePixelRatio(ratio);
    card.fill(Qt::transparent);
    QPainter painter(&card);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    qreal top = pad;
    painter.drawImage(QRectF(pad + (width - pictureSize.width()) / 2, top, pictureSize.width(), pictureSize.height()), picture);
    top += pictureSize.height() + 10;
    painter.setFont(titleFont);
    painter.setPen(ink);
    painter.drawText(QRectF(pad, top, width, titleMetrics.height()), Qt::AlignLeft | Qt::AlignVCenter,
                     titleMetrics.elidedText(name, Qt::ElideRight, width));
    top += titleMetrics.height();
    painter.setFont(smallFont);
    if (!pills.isEmpty()) {
        top += 8;
        QColor fill = ink;
        fill.setAlphaF(0.12);
        for (int i = 0; i < pills.size(); i++) {
            const QRectF pill = pills[i].translated(pad, top);
            painter.setPen(Qt::NoPen);
            painter.setBrush(fill);
            painter.drawRoundedRect(pill, pill.height() / 2, pill.height() / 2);
            painter.setPen(ink);
            painter.drawText(pill, Qt::AlignCenter, tags[i]);
        }
        top += tagsHeight;
    }
    if (!where.isEmpty()) {
        top += 6;
        QColor quiet = ink;
        quiet.setAlphaF(0.5);
        painter.setPen(quiet);
        painter.drawText(QRectF(pad, top, width, smallMetrics.height()), Qt::AlignLeft | Qt::AlignVCenter, where);
    }
    painter.end();
    return card;
}

QTextDocument *KisIconToolTip::createDocument(const QModelIndex &index)
{
    QTextDocument *doc = new QTextDocument(this);

    const QImage cardImage = card(index);
    if (!cardImage.isNull()) {
        // named after the picture: another picture (a stroke made meanwhile) is another tip, shown again
        const QString url = QStringLiteral("data:card%1").arg(cardImage.cacheKey());
        doc->addResource(QTextDocument::ImageResource, QUrl(url), cardImage);
        doc->setHtml(QStringLiteral("<html><body><img src=\"%1\"></body></html>").arg(url));
        doc->setDocumentMargin(10);
        doc->setTextWidth(cardImage.width() / cardImage.devicePixelRatio() + 20);
        return doc;
    }

    QImage thumb = index.data(Qt::DecorationRole).value<QImage>();
    if (thumb.isNull()) {
        thumb = index.data(Qt::UserRole + KisAbstractResourceModel::Thumbnail).value<QImage>();
    }

    if (!m_fixedToolTipThumbnailSize.isEmpty() && !thumb.isNull()) {
        int pixelSize = 48; //  // let's say, 48x48?
        if (!thumb.isNull()) {
            // affects mostly gradients, which are very long but only 1px tall
            Qt::AspectRatioMode aspectRatioMode = thumb.height() == 1 ? Qt::IgnoreAspectRatio : Qt::KeepAspectRatio;
            // this allows the pixel patterns to be displayed correctly,
            // while the presets (which have 200x200 thumbnails) will still be pretty
            // Fast Transformation == Nearest Neighbour
            Qt::TransformationMode transformationMode = (thumb.width() < pixelSize || thumb.height() < pixelSize) ? Qt::FastTransformation : Qt::SmoothTransformation;

                thumb = KisResourceThumbnailCache::instance()->getImage(index,
                                                                        m_fixedToolTipThumbnailSize
                                                                            * devicePixelRatioF(),
                                                                        aspectRatioMode,
                                                                        transformationMode);
        }
    }

    if (m_checkersPainter) {
        QImage image(thumb.size(), QImage::Format_ARGB32);

        {
            QPainter gc(&image);
            m_checkersPainter->paint(gc, thumb.rect());
            gc.drawImage(QPoint(), thumb);
        }

        thumb = image;
    }

    thumb.setDevicePixelRatio(devicePixelRatioF());
    doc->addResource(QTextDocument::ImageResource, QUrl("data:thumbnail"), thumb);

    QString name = index.data(Qt::DisplayRole).toString();
    QString presetDisplayName = index.data(Qt::UserRole + KisAbstractResourceModel::Name).toString().replace("_", " ");
    //This is to ensure that the other uses of this class don't get an empty string, while resource management should get a nice string.
    if (!presetDisplayName.isEmpty()) {
        name = presetDisplayName;
    }

    QString translatedName = index.data(Qt::UserRole + KisAbstractResourceModel::Tooltip).toString().replace("_", " ");

    QString tagsRow;
    QString tagsData = index.data(Qt::UserRole + KisAbstractResourceModel::Tags).toStringList().join(", ");
    if (!tagsData.isEmpty()) {
        const QString list = QString("<ul style=\"list-style-type: none; margin: 0px;\">%1</ul> ").arg(tagsData);
        tagsRow = QString("<tr><td>%1:</td><td style=\"text-align: right;\">%2</td></tr>").arg(i18n("Tags"), list);
    }

    const QString brokenReason = index.data(Qt::UserRole + KisAbstractResourceModel::BrokenStatusMessage).toString();
    const QString brokenRow = 
        !brokenReason.isEmpty() 
        ? QString("<tr><td colspan=\"2\"><b>%1</b></td></tr>"
                  "<tr><td colspan=\"2\">%2</td></tr>")
                  .arg(i18n("Resource is broken!"), brokenReason)
        : QString();

    QString location = index.data(Qt::UserRole + KisAbstractResourceModel::Location).toString();
    if (location.isEmpty()) {
        location = i18nc("a placeholder name for the default storage of resources", "resource folder");
    }

    const QString locationRow = QString("<tr><td>%1:</td><td style=\"text-align: right;\">%2</td></tr>").arg(i18n("Location"), location);

    const QString footerTable = QString("<p><table>%1%2%3</table></p>").arg(brokenRow).arg(tagsRow).arg(locationRow);

    const QString image = QString("<center><img src=\"data:thumbnail\"></center>");
    QString body = QString("<h3 align=\"center\">%1</h3>%2%3").arg(name, image, footerTable);
    if (translatedName != name) {
        body = QString("<h3 align=\"center\">%1</h3><h4 align=\"center\">%2</h4>%3%4").arg(name, translatedName, image, footerTable);
    }
    const QString html = QString("<html><body>%1</body></html>").arg(body);

    doc->setHtml(html);

    const int margin = 16;
    doc->setTextWidth(qMin(doc->size().width() + 2 * margin, qreal(500.0)));
    doc->setDocumentMargin(margin);
    doc->setUseDesignMetrics(true);

    return doc;
}
