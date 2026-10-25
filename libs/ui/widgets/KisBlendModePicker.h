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

/// Images for the picker's tiles: each tile shows \p source blended over \p backdrop in its mode; both are tile-sized
struct KisBlendModePictures {
    QImage backdrop;
    QImage source;
    qreal sourceOpacity {1.0};
    bool isValid() const { return !backdrop.isNull() && backdrop.size() == source.size(); }
};

/**
 * Blend mode picker built on KisStudioPicker. The user's list starts with
 * the common modes, grouped by effect.
 *
 * With Pictures, each mode is a tile previewing its effect (the layer over
 * the layers below, or a brush stroke over the image); otherwise each mode
 * gets a small swatch rendered with its composite op.
 */
class KRITAUI_EXPORT KisBlendModePicker : public KisStudioPicker
{
    Q_OBJECT
public:
    using Pictures = KisBlendModePictures;

    /// @param available the ids usable in the current color space
    /// @param forBrush leave out the modes only layers have
    KisBlendModePicker(const QString &current, const QSet<QString> &available, QWidget *parent = nullptr,
                       const Pictures &pictures = Pictures(), bool forBrush = false);

    /// Size of a tile's picture in device pixels for \p widget's screen
    static QSize pictureSize(const QWidget *widget);
    /// The layer over everything below it; an empty layer is replaced by a sample shape in \p color
    static Pictures layerPictures(KisNodeSP node, const QSize &size, const QColor &color);
    /// A brush stroke in \p color over the image
    static Pictures brushPictures(KisImageSP image, const QSize &size, const QColor &color);
    /// \p pictures' source over their backdrop in mode \p id
    static QImage modePicture(const Pictures &pictures, const QString &id);

    /// Order used when scrolling through modes: the user's modes and groups, then the rest as the picker lists them
    static QStringList scrollOrder(bool forBrush = false);
    /// The modes the user's list starts with
    static QStringList defaultModes();
    /// The groups after it the user starts with
    static QVector<Group> defaultGroups();

Q_SIGNALS:
    /// The highlighted mode changed; an empty id ends the preview
    void previewed(const QString &id);
    void picked(const KoID &op);

private:
    static Source source(const Pictures &pictures = Pictures(), bool forBrush = false);
};

#endif // KISBLENDMODEPICKER_H
