/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef SKISSADETAIL_H
#define SKISSADETAIL_H

#include <QImage>
#include <QRect>
#include <QString>
#include <QVector>

#include <cmath>
#include <vector>

#include <widgets/KisStudioCurve.h>

class KisPropertiesConfiguration;

/**
 * Detail: how strongly each size of feature is kept, from fine texture to the largest shapes. 0 removes it (a blur
 * of the small ones), 1 keeps it, more strengthens it (sharpening the small ones). Made of Gaussians: the result is
 * the image times the gain of the finest features, plus, for each size where the gain changes, the image blurred to
 * that size times the change. So a blur, a high pass or an unsharp mask is exactly one step, and Krita's radius
 * means the same here. With Edges, each blur is an edge-aware one (a guided filter), so strong edges stay.
 */
namespace SkissaDetail
{

/// Blur, Sharpen, High pass and Smooth are Detail with a slider or two: each its own effect
enum class Kind { Blur, Sharpen, HighPass, Smooth, Detail };

/// The curve's x axis: radii from 1 to 1024 px, evenly by ratio
constexpr qreal octaves = 10;
inline qreal radiusAt(qreal x);
qreal xOfRadius(qreal radius);
/// The curve's height as a gain, 1 in the middle, up to 3
float gainOf(qreal y);
qreal yOfGain(float gain);
/// Krita's Gaussian for a radius (KisGaussianKernel::sigmaFromRadius)
inline qreal sigmaOf(qreal radius) { return 0.3 * radius + 0.3; }

KisStudioCurve flatCurve();

struct Step {
    qreal radius;
    float gain; ///< added to the gain of every feature larger than the radius
    /// Up and down, when it differs from across (Krita's Gaussian blur can blur the two ways apart); negative: the same
    qreal radiusDown {-1};
};

struct Plan {
    float fine {1}; ///< the gain of the finest features
    QVector<Step> steps;
    /// 0: plain blurs; toward 1, ever weaker edges are held
    qreal edges {0};

    float coarse() const;
    bool changesNothing() const;
    /// How far around a pixel it reads, in pixels
    int reach() const;
    Plan scaled(qreal factor) const;
};

/// The settings of an effect of \p kind
Plan planOf(Kind kind, const KisPropertiesConfiguration &config);
/// A Detail curve as steps, every half octave where it changes
Plan planOfCurve(const KisStudioCurve &curve, qreal edges);

/// An image as premultiplied red, green, blue and alpha planes
struct Planes {
    int width {0};
    int height {0};
    std::vector<float> plane[4];

    void resize(int w, int h);
};

/**
 * \p plan applied to \p source (covering \p sourceRect), written to \p result for \p resultRect (inside it).
 * Pixels beyond the source repeat its edge.
 */
void apply(const Plan &plan, const Planes &source, const QRect &sourceRect, const QRect &resultRect, Planes &result);

/// Blurs \p data, \p w by \p h, in place by a Gaussian of \p sigma (Krita's kernel, wide ones as box blurs);
/// \p scratch is as large. Beyond the edges the edge pixels repeat.
void gaussian(float *data, float *scratch, int w, int h, qreal sigma);
/// The same, by \p sigmaAcross along rows and \p sigmaDown along columns
void gaussian(float *data, float *scratch, int w, int h, qreal sigmaAcross, qreal sigmaDown);
/// How far the blur of \p sigma reads around a pixel
int reachOf(qreal sigma);
/// The blur of \p sigma along one axis, as weights centered in the middle
std::vector<float> kernel1D(qreal sigma);

/// \p image (8-bit sRGB) with \p plan applied
QImage applied(const QImage &image, const Plan &plan);

/// How strong the features of each half-octave are around \p at in \p image: the size that stands out there
qreal busiestRadius(const QImage &image, const QPoint &at);

} // namespace SkissaDetail

inline qreal SkissaDetail::radiusAt(qreal x)
{
    return std::pow(2.0, x * octaves);
}

#endif
