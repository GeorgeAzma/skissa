/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "SkissaColorFilter.h"

#include <QVector>

#include <algorithm>
#include <memory>

#include <KoChannelInfo.h>
#include <KoColorConversionTransformation.h>
#include <KoColorSpace.h>
#include <KoColorSpaceRegistry.h>
#include <KoColorTransformation.h>
#include <filter/kis_filter_category_ids.h>
#include <filter/kis_filter_configuration.h>
#include <filter/kis_filter_registry.h>
#include <klocalizedstring.h>
#include <kpluginfactory.h>

#include "KoCompositeOpInLinear.h"
#include "SkissaColor.h"
#include "SkissaColorWidget.h"

K_PLUGIN_FACTORY_WITH_JSON(SkissaColorFactory, "kritaskissacolor.json", registerPlugin<SkissaColorPlugin>();)

namespace
{

/// The Color effect on pixels of one color space, through linear sRGB
class Transformation : public KoColorTransformation
{
public:
    Transformation(const KoColorSpace *cs, const SkissaColor::Settings &settings)
        : m_recipe(settings)
        , m_pixelSize(cs->pixelSize())
    {
        const KoColorSpace *linear = KoMixing::linearSpace();
        m_srgb8 = *cs == *KoColorSpaceRegistry::instance()->rgb8();
        m_linear = *cs == *linear;
        const KoChannelInfo::enumChannelValueType type = cs->channels().first()->channelValueType();
        // floats hold colors past sRGB, which are kept
        m_fit = type != KoChannelInfo::FLOAT16 && type != KoChannelInfo::FLOAT32 && type != KoChannelInfo::FLOAT64;
        if (!m_srgb8 && !m_linear) {
            KoColorSpaceRegistry *registry = KoColorSpaceRegistry::instance();
            m_toLinear.reset(registry->createColorConverter(cs, linear, KoColorConversionTransformation::internalRenderingIntent(),
                                                            KoColorConversionTransformation::internalConversionFlags()));
            m_toOwn.reset(registry->createColorConverter(linear, cs, KoColorConversionTransformation::internalRenderingIntent(),
                                                         KoColorConversionTransformation::internalConversionFlags()));
        }
    }

    void transform(const quint8 *src, quint8 *dst, qint32 nPixels) const override
    {
        if (m_recipe.changesNothing()) {
            if (src != dst) std::copy(src, src + size_t(nPixels) * m_pixelSize, dst);
            return;
        }
        if (m_srgb8) {
            // blue, green, red, alpha, as Krita keeps 8-bit RGB
            const float *decode = KoMixing::srgbToLinearTable();
            const quint8 *encode = KoMixing::linearToSrgbTable();
            for (qint32 i = 0; i < nPixels; i++, src += 4, dst += 4) {
                float rgb[3] = {decode[src[2]], decode[src[1]], decode[src[0]]};
                m_recipe.apply(rgb, true);
                auto out = [encode](float c) { return encode[int(std::clamp(c, 0.0f, 1.0f) * 65535 + 0.5f)]; };
                dst[0] = out(rgb[2]);
                dst[1] = out(rgb[1]);
                dst[2] = out(rgb[0]);
                dst[3] = src[3];
            }
            return;
        }
        m_buffer.resize(nPixels * 4);
        float *pixels = m_buffer.data();
        if (m_linear) {
            std::copy(src, src + size_t(nPixels) * m_pixelSize, reinterpret_cast<quint8 *>(pixels));
        } else {
            m_toLinear->transform(src, reinterpret_cast<quint8 *>(pixels), nPixels);
        }
        for (qint32 i = 0; i < nPixels; i++) m_recipe.apply(pixels + 4 * i, m_fit);
        if (m_linear) {
            std::copy(reinterpret_cast<const quint8 *>(pixels), reinterpret_cast<const quint8 *>(pixels) + size_t(nPixels) * m_pixelSize, dst);
        } else {
            m_toOwn->transform(reinterpret_cast<const quint8 *>(pixels), dst, nPixels);
        }
    }

private:
    SkissaColor::Recipe m_recipe;
    int m_pixelSize;
    bool m_srgb8 {false};
    bool m_linear {false};
    bool m_fit {true};
    std::unique_ptr<KoColorConversionTransformation> m_toLinear;
    std::unique_ptr<KoColorConversionTransformation> m_toOwn;
    /// Each thread has its own transformation (KisColorTransformationConfiguration), so this needs no lock
    mutable QVector<float> m_buffer;
};

} // namespace

SkissaColorPlugin::SkissaColorPlugin(QObject *parent, const QVariantList &)
    : QObject(parent)
{
    KisFilterRegistry::instance()->add(KisFilterSP(new SkissaColorFilter()));
}

SkissaColorFilter::SkissaColorFilter()
    : KisColorTransformationFilter(id(), FiltersCategoryAdjustId, i18nc("@action effect", "&Color..."))
{
    setColorSpaceIndependence(FULLY_INDEPENDENT);
    setSupportsPainting(true);
    setShowConfigurationWidget(true);
}

KoID SkissaColorFilter::id()
{
    return KoID(QStringLiteral("skissa_color"), i18nc("effect", "Color"));
}

KoColorTransformation *SkissaColorFilter::createTransformation(const KoColorSpace *cs, const KisFilterConfigurationSP config) const
{
    return new Transformation(cs, SkissaColor::Settings::from(*config));
}

KisConfigWidget *SkissaColorFilter::createConfigurationWidget(QWidget *parent, const KisPaintDeviceSP dev, bool) const
{
    return new SkissaColorWidget(parent, dev);
}

KisFilterConfigurationSP SkissaColorFilter::defaultConfiguration(KisResourcesInterfaceSP resourcesInterface) const
{
    return factoryConfiguration(resourcesInterface);
}

#include "SkissaColorFilter.moc"
