/* This file is part of the KDE project
 * SPDX-FileCopyrightText: 1999 Carsten Pfeiffer (pfeiffer@kde.org)
 * SPDX-FileCopyrightText: 2002 Igor Jansen (rm@kde.org)
 * SPDX-FileCopyrightText: 2018 Boudewijn Rempt <boud@valdyas.org>
 *
 * SPDX-License-Identifier: LGPL-2.0-or-later
*/

#ifndef KISICONTOOLTIP_H
#define KISICONTOOLTIP_H

#include "KoItemToolTip.h"

#include "kritaresourcewidgets_export.h"

#include <functional>
#include <QImage>

class KoCheckerBoardPainter;

class KRITARESOURCEWIDGETS_EXPORT KisIconToolTip: public KoItemToolTip
{

public:
    KisIconToolTip(QWidget *parent);
    ~KisIconToolTip() override;

    void setFixedToolTipThumbnailSize(const QSize &size);
    void setToolTipShouldRenderCheckers(bool value);

    /**
     * A picture of a resource other than its thumbnail (a brush's stroke), or a null image. Given it, a
     * resource's tip is a card: the picture, its name, its tags as pills, and quietly where it is from.
     */
    using PictureProvider = std::function<QImage(const QModelIndex &index, const QColor &ink)>;
    static void setPictureProvider(const PictureProvider &provider);

protected:
    QTextDocument *createDocument( const QModelIndex &index ) override;

private:
    /// The card, or a null image when no picture provider says anything of \p index
    QImage card(const QModelIndex &index);

private:
    QSize m_fixedToolTipThumbnailSize;
    QScopedPointer<KoCheckerBoardPainter> m_checkersPainter;
};

#endif // KISICONTOOLTIP_H
