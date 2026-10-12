/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISBLENDMODEPICKER_H
#define KISBLENDMODEPICKER_H

#include <KoID.h>

#include <QImage>

#include <functional>

#include "kis_types.h"

#include "KisStudioPicker.h"
#include "kritaui_export.h"

/// What the picker's tiles show a mode doing: \p source in that mode over \p backdrop, both the size of a tile's picture
struct KisBlendModePictures {
    QImage backdrop;
    QImage source;
    qreal sourceOpacity {1.0};
    bool isValid() const { return !backdrop.isNull() && backdrop.size() == source.size(); }
};

/**
 * Picks a blend mode from your own list of them (see KisStudioPicker).
 *
 * The list starts with about thirty common modes, ordered by what they do
 * (darken, lighten, contrast, compare, color); scrolling over a blend mode
 * box steps through it in that order, then on into all the others.
 *
 * Given Pictures, every mode is a tile with what it really does to them:
 * the layer in that mode over the layers below it, or a stroke of the
 * current color over the image; a blank canvas gets a sample picture.
 * Without, every mode shows a small swatch made with its composite op.
 */
class KRITAUI_EXPORT KisBlendModePicker : public KisStudioPicker
{
    Q_OBJECT
public:
    using Pictures = KisBlendModePictures;

    /**
     * @param available the ids that can be used in the current color space
     */
    KisBlendModePicker(const QString &current, const QSet<QString> &available, QWidget *parent = nullptr,
                       const Pictures &pictures = Pictures());

    /// The size of a tile's picture, in pixels, for a widget's screen
    static QSize pictureSize(const QWidget *widget);
    /// A layer over everything below it; \p color makes a sample when the layer is empty
    static Pictures layerPictures(KisNodeSP node, const QSize &size, const QColor &color);
    /// A stroke of \p color over the image
    static Pictures brushPictures(KisImageSP image, const QSize &size, const QColor &color);

    /// What scrolling steps through: your modes, then all the others as the picker shows them
    static QStringList scrollOrder();

Q_SIGNALS:
    /// The highlighted mode changed; an empty id when the preview should end
    void previewed(const QString &id);
    void picked(const KoID &op);

private:
    static Source source(const Pictures &pictures = Pictures());
};

#endif // KISBLENDMODEPICKER_H
