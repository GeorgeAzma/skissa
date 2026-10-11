/*
 *  SPDX-FileCopyrightText: 2010 Dmitry Kazakov <dimula73@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KIS_TEXTURE_TILE_UPDATE_INFO_H_
#define KIS_TEXTURE_TILE_UPDATE_INFO_H_

#include <QMessageBox>
#include <QtGlobal>
#include <cmath>
#include <QThreadStorage>
#include <QScopedArrayPointer>

#include "kis_config.h"
#include "kis_image.h"
#include "kis_paint_device.h"
#include "kis_texture_tile_info_pool.h"
#include <KoChannelInfo.h>
#include <KoColorConversionTransformation.h>
#include <KoColorModelStandardIds.h>
#include <KoColorSpace.h>
#include <KoLabColorSpaceMaths.h>
#include <kis_lod_transform.h>
#include <KisPortingUtils.h>
#include <KisDisplayConfig.h>

class KisTextureTileUpdateInfo;
typedef QSharedPointer<KisTextureTileUpdateInfo> KisTextureTileUpdateInfoSP;
typedef QVector<KisTextureTileUpdateInfoSP> KisTextureTileUpdateInfoSPList;

/**
 * A buffer object for temporary data needed during the update process.
 *
 * - the buffer is allocated from the common pool to avoid memory
 *   fragmentation
 *
 * - the buffer's lifetime defines the lifetime of the allocated chunk
 *   of memory, so you don't have to thing about freeing the memory
 */

class DataBuffer
{
public:
    DataBuffer(KisTextureTileInfoPoolSP pool)
        : m_data(0),
          m_pixelSize(0),
          m_pool(pool)
    {
    }

    DataBuffer(int pixelSize, KisTextureTileInfoPoolSP pool)
        : m_data(0),
          m_pixelSize(0),
          m_pool(pool)
    {
        allocate(pixelSize);
    }

    DataBuffer(DataBuffer &&rhs)
        : m_data(rhs.m_data),
          m_pixelSize(rhs.m_pixelSize),
          m_pool(rhs.m_pool)
    {
        rhs.m_data = 0;
    }

    DataBuffer& operator=(DataBuffer &&rhs) {
        swap(rhs);
        return *this;
    }

    ~DataBuffer() {
        if (m_data) {
            m_pool->free(m_data, m_pixelSize);
        }
    }

    void allocate(int pixelSize) {
        Q_ASSERT(!m_data);

        m_pixelSize = pixelSize;
        m_data = m_pool->malloc(m_pixelSize);
    }

    inline quint8* data() const {
        return m_data;
    }

    void swap(DataBuffer &other) {
        std::swap(other.m_pixelSize, m_pixelSize);
        std::swap(other.m_data, m_data);
        std::swap(other.m_pool, m_pool);
    }

    int size() const {
        return m_data ? m_pool->chunkSize(m_pixelSize) : 0;
    }

    KisTextureTileInfoPoolSP pool() const {
        return m_pool;
    }

    int pixelSize() const {
        return m_pixelSize;
    }

private:
    Q_DISABLE_COPY(DataBuffer)

    quint8 *m_data {nullptr};
    int m_pixelSize;
    KisTextureTileInfoPoolSP m_pool;
};

class KisTextureTileUpdateInfo
{
public:
    KisTextureTileUpdateInfo(KisTextureTileInfoPoolSP pool)
        : m_patchPixels(pool),
          m_pool(pool)
    {
    }

    KisTextureTileUpdateInfo(qint32 col, qint32 row,
                             const QRect &tileRect, const QRect &updateRect, const QRect &currentImageRect,
                             int levelOfDetail,
                             KisTextureTileInfoPoolSP pool)
        : m_patchPixels(pool),
          m_pool(pool)
    {
        m_tileCol = col;
        m_tileRow = row;
        m_tileRect = tileRect;
        m_originalTileRect = m_tileRect;
        m_patchRect = m_tileRect & updateRect;
        m_originalPatchRect = m_patchRect;
        m_currentImageRect = currentImageRect;

        m_patchLevelOfDetail = levelOfDetail;

        if (m_patchLevelOfDetail) {
            // TODO: check if isBottommost() works correctly when m_originalPatchRect gets aligned
            //       and m_currentImageRect has non-aligned size
            m_originalPatchRect = KisLodTransform::alignedRect(m_originalPatchRect, m_patchLevelOfDetail);
            m_patchRect = KisLodTransform::scaledRect(m_originalPatchRect, m_patchLevelOfDetail);
            m_tileRect = KisLodTransform::scaledRect(m_originalTileRect, m_patchLevelOfDetail);
        }
    }

    ~KisTextureTileUpdateInfo() {
    }

    /**
     * The CIE L* of the gray as light as a color of 16 bit Lab (D50) looks,
     * by OKLab's lightness: Lab to XYZ, adapted to D65 (Bradford), OKLab's
     * L of it; a gray of that L has the luminance L cubed
     */
    static qreal oklabGrayLightness(quint16 l16, quint16 a16, quint16 b16)
    {
        const qreal L = l16 / 65535.0 * 100.0;
        const qreal a = (a16 - 32896.0) / 257.0;
        const qreal b = (b16 - 32896.0) / 257.0;
        auto finv = [](qreal t) { return t > 6.0 / 29.0 ? t * t * t : 3.0 * (6.0 / 29.0) * (6.0 / 29.0) * (t - 4.0 / 29.0); };
        const qreal fy = (L + 16.0) / 116.0;
        // XYZ, D50 white
        const qreal x50 = 0.9642 * finv(fy + a / 500.0);
        const qreal y50 = finv(fy);
        const qreal z50 = 0.8249 * finv(fy - b / 200.0);
        // to D65
        const qreal x = 0.9555766 * x50 - 0.0230393 * y50 + 0.0631636 * z50;
        const qreal y = -0.0282895 * x50 + 1.0099416 * y50 + 0.0210077 * z50;
        const qreal z = 0.0122982 * x50 - 0.0204830 * y50 + 1.3299098 * z50;
        // OKLab: XYZ to cone responses, their cube roots, the lightness of them
        const qreal l = std::cbrt(qMax(0.0, 0.8189330101 * x + 0.3618667424 * y - 0.1288597137 * z));
        const qreal m = std::cbrt(qMax(0.0, 0.0329845436 * x + 0.9293118715 * y + 0.0361456387 * z));
        const qreal s = std::cbrt(qMax(0.0, 0.0482003018 * x + 0.2643662691 * y + 0.6338517070 * z));
        const qreal lightness = 0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s;
        // the gray of that lightness: its luminance, and that as L*
        const qreal grayY = lightness * lightness * lightness;
        const qreal f = grayY > 216.0 / 24389.0 ? std::cbrt(grayY) : (24389.0 / 27.0 * grayY + 16.0) / 116.0;
        return 116.0 * f - 16.0;
    }

    /// \p grayscale: each color as a gray as light as it looks (the value view, see KisOpenGLUpdateInfoBuilder::setValueView)
    void retrieveData(KisPaintDeviceSP projectionDevice, const QBitArray &channelFlags, bool onlyOneChannelSelected, int selectedChannelIndex,
                      bool grayscale = false)
    {
        m_patchColorSpace = projectionDevice->colorSpace();
        m_patchPixels.allocate(m_patchColorSpace->pixelSize());

        projectionDevice->readBytes(m_patchPixels.data(),
                                       m_patchRect.x(), m_patchRect.y(),
                                       m_patchRect.width(), m_patchRect.height());

        // XXX: if the paint colorspace is rgb, we should do the channel swizzling in
        //      the display shader
        if (!channelFlags.isEmpty() && selectedChannelIndex >= 0 && selectedChannelIndex < m_patchColorSpace->channelCount()) {
            DataBuffer conversionCache(m_patchColorSpace->pixelSize(), m_pool);

            quint32 numPixels = m_patchRect.width() * m_patchRect.height();

            KisConfig cfg(true);

            if (onlyOneChannelSelected && !cfg.showSingleChannelAsColor()) {
                m_patchColorSpace->convertChannelToVisualRepresentation(m_patchPixels.data(), conversionCache.data(), numPixels, selectedChannelIndex);
            } else {
                m_patchColorSpace->convertChannelToVisualRepresentation(m_patchPixels.data(), conversionCache.data(), numPixels, channelFlags);
            }

            conversionCache.swap(m_patchPixels);
        }

        /**
         * The value view: the colors made neutral in the image's own color
         * space, each as light as it looks, before they are made colors of
         * the display. A neutral stays one through whatever comes after (the
         * monitor's profile, proofing, OCIO), so the grays are right for any
         * image and any screen, as a gray made of the display's values would
         * not be.
         *
         * As light as it looks is OKLab's lightness, the one the color panel
         * picks by: two colors of one OKLCH lightness are one gray. (CIE L*
         * follows luminance alone, and makes saturated blues and purples
         * darker than they look.) A gray's OKLab lightness is the cube root
         * of its luminance.
         */
        if (grayscale && m_patchRect.isValid()) {
            const quint32 numPixels = m_patchRect.width() * m_patchRect.height();
            QVector<quint16> lab(int(numPixels) * 4);
            m_patchColorSpace->toLabA16(m_patchPixels.data(), reinterpret_cast<quint8 *>(lab.data()), numPixels);
            const quint16 neutral = KoLabColorSpaceMathsTraits<quint16>::halfValueAB;
            for (quint32 i = 0; i < numPixels; i++) {
                quint16 *px = lab.data() + 4 * i;
                px[0] = quint16(qBound(0.0, oklabGrayLightness(px[0], px[1], px[2]) * 65535.0 / 100.0 + 0.5, 65535.0));
                px[1] = neutral;
                px[2] = neutral;
            }
            m_patchColorSpace->fromLabA16(reinterpret_cast<const quint8 *>(lab.data()), m_patchPixels.data(), numPixels);
        }
    }

    void convertTo(const KoColorSpace* dstCS,
                   KoColorConversionTransformation::Intent renderingIntent,
                   KoColorConversionTransformation::ConversionFlags conversionFlags)
    {
        // we use two-stage check of the color space equivalence:
        // first check pointers, and if not, check the spaces themselves
        if ((dstCS == m_patchColorSpace || *dstCS == *m_patchColorSpace) &&
            conversionFlags == KoColorConversionTransformation::Empty) {

            return;
        }

        if (m_patchRect.isValid()) {
            const qint32 numPixels = m_patchRect.width() * m_patchRect.height();
            DataBuffer conversionCache(dstCS->pixelSize(), m_pool);

            m_patchColorSpace->convertPixelsTo(m_patchPixels.data(), conversionCache.data(), dstCS, numPixels, renderingIntent, conversionFlags);

            m_patchColorSpace = dstCS;
            conversionCache.swap(m_patchPixels);
        }
    }

    void proofTo(const KoColorSpace* dstCS,
                   KoColorConversionTransformation::ConversionFlags displayFlags,
                   KoColorConversionTransformation *proofingTransform)
    {
        if (dstCS == m_patchColorSpace && displayFlags == KoColorConversionTransformation::Empty) return;

        if (m_patchRect.isValid()) {
            const qint32 numPixels = m_patchRect.width() * m_patchRect.height();
            DataBuffer conversionCache(dstCS->pixelSize(), m_pool);

            m_patchColorSpace->proofPixelsTo(m_patchPixels.data(), conversionCache.data(), numPixels, proofingTransform);

            m_patchColorSpace = dstCS;
            conversionCache.swap(m_patchPixels);
        }
    }

    static KoColorConversionTransformation *generateProofingTransform(const KoColorSpace* srcCS,
                                                                      const KoColorSpace* dstCS, const KoColorSpace* proofingSpace,
                                                                      KoColorConversionTransformation::Intent renderingIntent,
                                                                      KoColorConversionTransformation::Intent proofingIntent,
                                                                      bool bpcFirstTransform,
                                                                      KoColor gamutWarning,
                                                                      KoColorConversionTransformation::ConversionFlags displayConversionFlags)
    {
        KoColor properGamutWarning = gamutWarning.convertedTo(dstCS);
        return srcCS->createProofingTransform(dstCS, proofingSpace, renderingIntent, proofingIntent, bpcFirstTransform, properGamutWarning.data(), displayConversionFlags);
    }

    inline quint8* data() const {
        return m_patchPixels.data();
    }

    inline int patchLevelOfDetail() const {
        return m_patchLevelOfDetail;
    }

    inline QPoint realPatchOffset() const {
        return QPoint(m_patchRect.x() - m_tileRect.x(),
                      m_patchRect.y() - m_tileRect.y());
    }

    inline QSize realPatchSize() const {
        return m_patchRect.size();
    }

    inline QRect realPatchRect() const {
        return m_patchRect;
    }

    inline QSize realTileSize() const {
        return m_tileRect.size();
    }

    inline bool isTopmost() const {
        return m_originalPatchRect.top() == m_currentImageRect.top();
    }

    inline bool isLeftmost() const {
        return m_originalPatchRect.left() == m_currentImageRect.left();
    }

    inline bool isRightmost() const {
        return m_originalPatchRect.right() == m_currentImageRect.right();
    }

    inline bool isBottommost() const {
        return m_originalPatchRect.bottom() == m_currentImageRect.bottom();
    }

    inline bool isEntireTileUpdated() const {
        return m_patchRect == m_tileRect;
    }

    inline qint32 tileCol() const {
        return m_tileCol;
    }

    inline qint32 tileRow() const {
        return m_tileRow;
    }

    inline int pixelSize() const {
        return m_patchColorSpace->pixelSize();
    }

    inline const KoColorSpace* patchColorSpace() const {
        return m_patchColorSpace;
    }

    inline quint32 patchPixelsLength() const {
        return m_patchPixels.size();
    }

    inline bool valid() const {
        return m_patchRect.isValid();
    }

    inline DataBuffer&& takePixelData() {
        return std::move(m_patchPixels);
    }

    inline void putPixelData(DataBuffer &&buffer, const KoColorSpace *colorSpace) {
        m_patchPixels = std::move(buffer);
        m_patchColorSpace = colorSpace;
    }

private:
    Q_DISABLE_COPY(KisTextureTileUpdateInfo)

private:
    qint32 m_tileCol {0};
    qint32 m_tileRow {0};
    QRect m_currentImageRect;
    QRect m_tileRect;
    QRect m_patchRect;
    const KoColorSpace* m_patchColorSpace {nullptr};

    QRect m_realPatchRect;
    QRect m_realPatchOffset;
    QRect m_realTileSize;
    int m_patchLevelOfDetail {0};

    QRect m_originalPatchRect;
    QRect m_originalTileRect;

    DataBuffer m_patchPixels;
    KisTextureTileInfoPoolSP m_pool;
};


#endif /* KIS_TEXTURE_TILE_UPDATE_INFO_H_ */

