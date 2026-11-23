/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "KisLanczos.h"

#include <QThread>

#include <algorithm>
#include <cmath>
#include <thread>
#include <vector>

namespace
{

/// Runs \p rows(first, last) over [0, count) on a few threads when there is enough work
template<typename Rows>
void forRows(int count, qint64 work, Rows rows)
{
    const int threads = work < 2000000 ? 1 : qBound(1, QThread::idealThreadCount(), std::min(8, count));
    if (threads == 1) {
        rows(0, count);
        return;
    }
    std::vector<std::thread> running;
    for (int t = 0; t < threads; t++) {
        const int first = count * t / threads;
        const int last = count * (t + 1) / threads;
        running.emplace_back([&rows, first, last]() { rows(first, last); });
    }
    for (std::thread &each : running) each.join();
}

double lanczos3(double x)
{
    x = std::abs(x);
    if (x < 1e-8) return 1.0;
    if (x >= 3.0) return 0.0;
    const double px = M_PI * x;
    return 3.0 * std::sin(px) * std::sin(px / 3.0) / (px * px);
}

/// For each new pixel along an axis: the first source pixel it takes and the weights from there
struct Taps {
    std::vector<int> first;
    std::vector<int> count;
    std::vector<float> weights;
    int most {0};
};

/// \p in pixels standing for \p span of the original, made \p out pixels
Taps tapsFor(int in, double span, int out)
{
    Taps taps;
    const double ratio = span / out;
    const double pixel = span / in; // original pixels in one of these
    const double scale = std::max(1.0, ratio / pixel);
    const double support = 3.0 * scale;
    taps.most = int(std::ceil(support)) * 2 + 2;
    taps.first.resize(out);
    taps.count.resize(out);
    taps.weights.assign(size_t(out) * taps.most, 0.0f);
    for (int x = 0; x < out; x++) {
        const double center = ((x + 0.5) * ratio) / pixel - 0.5;
        const int left = int(std::floor(center - support)) + 1;
        const int right = int(std::floor(center + support));
        std::vector<double> w;
        double sum = 0;
        for (int i = left; i <= right; i++) {
            const double v = lanczos3((i - center) / scale);
            w.push_back(v);
            sum += v;
        }
        // edges repeat the outermost pixel: fold the weights that fall outside onto it
        const int first = std::max(0, left);
        const int last = std::min(in - 1, right);
        taps.first[x] = first;
        taps.count[x] = last - first + 1;
        float *row = &taps.weights[size_t(x) * taps.most];
        for (int i = left; i <= right; i++) {
            const int at = std::clamp(i, first, last) - first;
            row[at] += float(w[i - left] / sum);
        }
    }
    return taps;
}

/// Averages \p factor × \p factor blocks: shrinks a large picture cheaply to a few times the size asked
QImage boxShrink(const QImage &image, int fx, int fy)
{
    const int w = (image.width() + fx - 1) / fx;
    const int h = (image.height() + fy - 1) / fy;
    QImage out(w, h, QImage::Format_ARGB32_Premultiplied);
    if (out.isNull()) return out;
    // taken once: scanLine() may detach, which threads must not do at once
    uchar *outBits = out.bits();
    const qsizetype outStride = out.bytesPerLine();
    forRows(h, qint64(image.width()) * image.height(), [&](int firstRow, int lastRow) {
        std::vector<quint32> sums(size_t(w) * 4);
        for (int y = firstRow; y < lastRow; y++) {
            std::fill(sums.begin(), sums.end(), 0);
            const int top = y * fy;
            const int bottom = std::min(image.height(), top + fy);
            for (int sy = top; sy < bottom; sy++) {
                const QRgb *src = reinterpret_cast<const QRgb *>(image.constScanLine(sy));
                for (int sx = 0; sx < image.width(); sx++) {
                    quint32 *s = &sums[size_t(sx / fx) * 4];
                    const QRgb p = src[sx];
                    s[0] += qBlue(p);
                    s[1] += qGreen(p);
                    s[2] += qRed(p);
                    s[3] += qAlpha(p);
                }
            }
            QRgb *dst = reinterpret_cast<QRgb *>(outBits + y * outStride);
            for (int x = 0; x < w; x++) {
                const quint32 n = quint32((std::min(image.width(), (x + 1) * fx) - x * fx) * (bottom - top));
                const quint32 *s = &sums[size_t(x) * 4];
                dst[x] = qRgba((s[2] + n / 2) / n, (s[1] + n / 2) / n, (s[0] + n / 2) / n, (s[3] + n / 2) / n);
            }
        }
    });
    return out;
}

} // namespace

QImage KisLanczos::scaled(const QImage &image, const QSize &size, Qt::AspectRatioMode mode)
{
    if (image.isNull()) return image;
    const QSize target = image.size().scaled(size, mode);
    if (target.isEmpty()) return QImage();
    if (target == image.size()) return image;
    if (target.width() >= image.width() && target.height() >= image.height()) {
        return image.scaled(target, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }

    QImage source = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    // far smaller: blocks averaged first, leaving Lanczos twice the size asked or more
    const int fx = std::max(1, source.width() / (2 * target.width()));
    const int fy = std::max(1, source.height() / (2 * target.height()));
    if (fx > 1 || fy > 1) source = boxShrink(source, fx, fy);
    if (source.isNull()) return QImage();

    const int inW = source.width();
    const int inH = source.height();
    const int outW = target.width();
    const int outH = target.height();
    const Taps across = tapsFor(inW, image.width(), outW);
    const Taps down = tapsFor(inH, image.height(), outH);

    // across first, into floats, then down
    std::vector<float> middle(size_t(outW) * inH * 4);
    forRows(inH, qint64(inH) * outW * across.most, [&](int first, int last) {
        for (int y = first; y < last; y++) {
            const QRgb *src = reinterpret_cast<const QRgb *>(source.constScanLine(y));
            float *dst = &middle[size_t(y) * outW * 4];
            for (int x = 0; x < outW; x++) {
                const float *w = &across.weights[size_t(x) * across.most];
                const QRgb *p = src + across.first[x];
                float b = 0, g = 0, r = 0, a = 0;
                for (int i = 0; i < across.count[x]; i++) {
                    b += w[i] * qBlue(p[i]);
                    g += w[i] * qGreen(p[i]);
                    r += w[i] * qRed(p[i]);
                    a += w[i] * qAlpha(p[i]);
                }
                dst[x * 4] = b;
                dst[x * 4 + 1] = g;
                dst[x * 4 + 2] = r;
                dst[x * 4 + 3] = a;
            }
        }
    });

    QImage out(target, QImage::Format_ARGB32_Premultiplied);
    if (out.isNull()) return out;
    uchar *outBits = out.bits();
    const qsizetype outStride = out.bytesPerLine();
    forRows(outH, qint64(outH) * outW * down.most, [&](int first, int last) {
        std::vector<float> sum(size_t(outW) * 4);
        for (int y = first; y < last; y++) {
            std::fill(sum.begin(), sum.end(), 0.0f);
            const float *w = &down.weights[size_t(y) * down.most];
            for (int i = 0; i < down.count[y]; i++) {
                const float *row = &middle[size_t(down.first[y] + i) * outW * 4];
                const float k = w[i];
                for (int x = 0; x < outW * 4; x++) sum[x] += k * row[x];
            }
            QRgb *dst = reinterpret_cast<QRgb *>(outBits + y * outStride);
            for (int x = 0; x < outW; x++) {
                // the overshoot Lanczos rings with stays within what premultiplied pixels can hold
                const int a = std::clamp(int(std::lround(sum[x * 4 + 3])), 0, 255);
                const int b = std::clamp(int(std::lround(sum[x * 4])), 0, a);
                const int g = std::clamp(int(std::lround(sum[x * 4 + 1])), 0, a);
                const int r = std::clamp(int(std::lround(sum[x * 4 + 2])), 0, a);
                dst[x] = qRgba(r, g, b, a);
            }
        }
    });
    out.setDevicePixelRatio(image.devicePixelRatio());
    return image.format() == QImage::Format_ARGB32_Premultiplied ? out : out.convertToFormat(image.format());
}

QImage KisLanczos::scaled(const QImage &image, int width, int height, Qt::AspectRatioMode mode)
{
    return scaled(image, QSize(width, height), mode);
}

QImage KisLanczos::scaledToWidth(const QImage &image, int width)
{
    if (image.isNull() || image.width() <= 0) return image;
    return scaled(image, QSize(width, std::max(1, int(std::lround(double(image.height()) * width / image.width())))));
}
