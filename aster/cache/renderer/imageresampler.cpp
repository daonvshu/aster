#include "imageresampler.h"

#include "aster/cache/core/resampleconfig.h"
#include "lanczoslut.h"

#include <QElapsedTimer>

#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
#include <QColorSpace>
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

#if defined(_M_X64) || defined(__SSE2__) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <emmintrin.h>
#define ASTER_RESAMPLER_SSE2 1
#else
#define ASTER_RESAMPLER_SSE2 0
#endif

namespace aster::cache
{
namespace
{
// Adapted from the user-verified PicasaLanczosViewer/picasaresampler.cpp:
// centre alignment, shrink support expansion, edge normalization and 16383 weights.
constexpr int weightScale = 16383;
constexpr double pi = 3.1415926535897932384626433832795;

struct Contribution
{
    int first = 0;
    std::vector<int> weights;
    bool simdSafe = false;
};

double sinc(double x)
{
    return std::abs(x) < 1e-12 ? 1.0 : std::sin(pi * x) / (pi * x);
}

double radius(ImageScaleAlgorithm algorithm)
{
    switch (algorithm)
    {
    case ImageScaleAlgorithm::Bilinear:
        return 1;
    case ImageScaleAlgorithm::Bicubic:
        return 2;
    case ImageScaleAlgorithm::Lanczos3:
        return 3;
    default:
        return 4;
    }
}

double kernel(double x, ImageScaleAlgorithm algorithm, bool lut)
{
    x = std::abs(x);
    if (algorithm == ImageScaleAlgorithm::Bilinear)
        return std::max(0.0, 1.0 - x);
    if (algorithm == ImageScaleAlgorithm::Bicubic)
    {
        // Keys cubic convolution with a = -0.5 (Catmull-Rom).
        if (x < 1)
            return ((1.5 * x - 2.5) * x) * x + 1;
        if (x < 2)
            return ((-0.5 * x + 2.5) * x - 4) * x + 2;
        return 0;
    }
    const double support = radius(algorithm);
    if (lut && x < support)
    {
        const auto& table = algorithm == ImageScaleAlgorithm::Lanczos3 ? lanczos3Lut : lanczos4Lut;
        const double position = x * 4096 / support;
        const int index = std::min(4095, int(position));
        const qint64 fraction = qint64((position - index) * 65536);
        const qint64 value = qint64(table[size_t(index)]) * 65536 +
                             (qint64(table[size_t(index + 1)]) - table[size_t(index)]) * fraction;
        return double(value) / (65536.0 * 1073741824.0);
    }
    return x >= support ? 0 : sinc(x) * sinc(x / support);
}

qint64 tapBound(int source, int destination, ImageScaleAlgorithm algorithm)
{
    const double scale = std::min(1.0, double(destination) / source);
    return qint64(std::min(double(source), std::ceil(2 * radius(algorithm) / scale) + 2));
}

std::vector<Contribution> contributions(int source, int destination, int offset, int count,
                                        ImageScaleAlgorithm algorithm,
                                        const std::atomic<bool>& cancelled, bool lut)
{
    std::vector<Contribution> result(size_t(count), Contribution{});
    const double scale = double(destination) / source;
    const double filterScale = std::min(1.0, scale);
    const double support = radius(algorithm) / filterScale;
    for (int output = 0; output < count; ++output)
    {
        if (cancelled.load())
            return {};
        const double centre = (double(offset + output) + 0.5) / scale - 0.5;
        const int first = int(std::max(0.0, std::ceil(centre - support)));
        const int last = int(std::min(double(source - 1), std::floor(centre + support)));
        auto& c = result[size_t(output)];
        c.first = first;
        std::vector<double> weights;
        weights.reserve(size_t(last - first + 1));
        double total = 0;
        for (int index = first; index <= last; ++index)
        {
            const double weight =
                kernel((index - centre) * filterScale, algorithm, lut) * filterScale;
            weights.push_back(weight);
            total += weight;
        }
        if (std::abs(total) < 1e-12)
        {
            c.first = std::clamp(int(std::round(centre)), 0, source - 1);
            c.weights = {weightScale};
            continue;
        }
        int sum = 0;
        size_t strongest = 0;
        c.weights.reserve(weights.size());
        for (size_t tap = 0; tap < weights.size(); ++tap)
        {
            if (std::abs(weights[tap]) > std::abs(weights[strongest]))
                strongest = tap;
            const int quantized = int(std::round(weights[tap] / total * weightScale));
            c.weights.push_back(quantized);
            sum += quantized;
        }
        c.weights[strongest] += weightScale - sum;
        qint64 absoluteSum = 0;
        c.simdSafe = true;
        for (const int weight : c.weights)
        {
            c.simdSafe = c.simdSafe && weight >= -32768 && weight <= 32767;
            absoluteSum += std::abs(qint64(weight));
        }
        c.simdSafe = c.simdSafe && absoluteSum <= std::numeric_limits<qint32>::max() / 255;
    }
    return result;
}

template <class PixelAt>
void convolve(uchar* output, const Contribution& c, PixelAt pixelAt, bool useSimd)
{
    qint64 sum[4] = {};
#if ASTER_RESAMPLER_SSE2
    if (useSimd && c.simdSafe)
    {
        __m128i accumulator = _mm_setzero_si128();
        for (size_t tap = 0; tap < c.weights.size(); ++tap)
        {
            qint32 packed;
            // Exactly four bytes, safe for unaligned pixels and the final pixel of a row.
            std::memcpy(&packed, pixelAt(c.first + int(tap)), sizeof(packed));
            const auto values = _mm_unpacklo_epi8(_mm_cvtsi32_si128(packed), _mm_setzero_si128());
            const auto weight = _mm_set1_epi16(static_cast<short>(c.weights[tap]));
            const auto product = _mm_unpacklo_epi16(_mm_mullo_epi16(values, weight),
                                                    _mm_mulhi_epi16(values, weight));
            accumulator = _mm_add_epi32(accumulator, product);
        }
        alignas(16) qint32 channels[4];
        _mm_store_si128(reinterpret_cast<__m128i*>(channels), accumulator);
        for (int channel = 0; channel < 4; ++channel)
            sum[channel] = channels[channel];
    }
    else
#else
    (void)useSimd;
#endif
    {
        for (size_t tap = 0; tap < c.weights.size(); ++tap)
        {
            const auto* pixel = pixelAt(c.first + int(tap));
            for (int channel = 0; channel < 4; ++channel)
                sum[channel] += qint64(pixel[channel]) * c.weights[tap];
        }
    }
    for (int channel = 0; channel < 4; ++channel)
        output[channel] =
            uchar(std::clamp<qint64>((sum[channel] + weightScale / 2) / weightScale, 0, 255));
    // Negative lobes can overshoot. Keep the premultiplied-alpha invariant.
    for (int channel = 0; channel < 3; ++channel)
        output[channel] = std::min(output[channel], output[3]);
}
}

ImageResampler::ImageResampler(ResampleLimits limits, ResampleExecution execution,
                               KernelEvaluation kernel)
    : limits_(limits), execution_(execution), kernel_(kernel)
{
    if (limits.maxSide <= 0 || limits.maxSide > 32768 || limits.maxPixels <= 0 ||
        limits.maxPixels > 256 * 1024 * 1024 || limits.maxWorkingBytes <= 0)
        throw std::invalid_argument("Invalid resampling limits");
}

bool ImageResampler::simdAvailable()
{
    return ASTER_RESAMPLER_SSE2 != 0;
}

ImageResult ImageResampler::resize(const QImage& source, const RenderGeometry& geometry,
                                   ImageScaleAlgorithm algorithm,
                                   const std::atomic<bool>& cancelled,
                                   ResampleMetrics* metrics) const
{
    if (metrics)
        *metrics = {};
    if (cancelled.load())
        return ImageResult::failure(ImageError::Cancelled);
    const auto full = geometry.scaledSize;
    const auto region = geometry.region;
    if (source.isNull() || full.isEmpty() || region.isEmpty() ||
        !QRect(QPoint(), full).contains(region) || int(algorithm) < 0 ||
        int(algorithm) > int(ImageScaleAlgorithm::Lanczos4))
        return ImageResult::failure(ImageError::InvalidRequest);
    auto within = [this](QSize size)
    {
        return size.width() <= limits_.maxSide && size.height() <= limits_.maxSide &&
               qint64(size.width()) * size.height() <= limits_.maxPixels;
    };
    if (!within(source.size()) || !within(region.size()))
        return ImageResult::failure(ImageError::ResourceLimit);
    const bool qt =
        algorithm == ImageScaleAlgorithm::QtFast || algorithm == ImageScaleAlgorithm::QtSmooth;
    if (qt && !within(full))
        return ImageResult::failure(ImageError::ResourceLimit);
    const qint64 inputBytes = qint64(source.width()) * source.height() * 4;
    const qint64 outputBytes = qint64(region.width()) * region.height() * 4;
    const qint64 intermediateBytes = qt ? qint64(full.width()) * full.height() * 4
                                        : qint64(region.width()) * source.height() * 4;
    const qint64 horizontalTaps = tapBound(source.width(), full.width(), algorithm);
    const qint64 verticalTaps = tapBound(source.height(), full.height(), algorithm);
    const qint64 tableBytes =
        qt ? 0
           : (qint64(region.width()) + region.height()) * qint64(sizeof(Contribution)) +
                 (qint64(region.width()) * horizontalTaps +
                  qint64(region.height()) * verticalTaps) *
                     4 +
                 std::max(horizontalTaps, verticalTaps) * 8;
    const qint64 working =
        qint64(source.sizeInBytes()) + inputBytes + outputBytes + intermediateBytes + tableBytes;
    if (working > limits_.maxWorkingBytes)
        return ImageResult::failure(ImageError::ResourceLimit, "Resampling memory budget exceeded");
    if (metrics)
        metrics->workingBytes = working;

    try
    {
        const auto input = source.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
        if (input.isNull())
            return ImageResult::failure(ImageError::ResourceLimit);
        QImage output;
        if (input.size() == full)
            output = input.copy(region);
        else if (qt)
        {
            const auto scaled =
                input.scaled(full, Qt::IgnoreAspectRatio,
                             algorithm == ImageScaleAlgorithm::QtSmooth ? Qt::SmoothTransformation
                                                                        : Qt::FastTransformation);
            output = scaled.copy(region);
        }
        else
        {
            const bool lut = kernel_ == KernelEvaluation::Lut ||
                             (kernel_ == KernelEvaluation::Default && ASTER_LANCZOS_USE_LUT);
            QElapsedTimer weightTimer;
            if (metrics)
                weightTimer.start();
            const auto horizontal = contributions(input.width(), full.width(), region.x(),
                                                  region.width(), algorithm, cancelled, lut);
            const auto vertical = contributions(input.height(), full.height(), region.y(),
                                                region.height(), algorithm, cancelled, lut);
            if (metrics)
                metrics->weightGenerationNs = weightTimer.nsecsElapsed();
            if (cancelled.load())
                return ImageResult::failure(ImageError::Cancelled);
            int firstRow = input.height() - 1;
            int lastRow = 0;
            for (const auto& c : vertical)
            {
                firstRow = std::min(firstRow, c.first);
                lastRow = std::max(lastRow, c.first + int(c.weights.size()) - 1);
            }
            QImage intermediate(region.width(), lastRow - firstRow + 1,
                                QImage::Format_RGBA8888_Premultiplied);
            output = QImage(region.size(), QImage::Format_RGBA8888_Premultiplied);
            if (intermediate.isNull() || output.isNull())
                return ImageResult::failure(ImageError::ResourceLimit);
            for (int y = 0; y < intermediate.height(); ++y)
            {
                if (cancelled.load())
                    return ImageResult::failure(ImageError::Cancelled);
                const auto* row = input.constScanLine(y + firstRow);
                auto* out = intermediate.scanLine(y);
                for (int x = 0; x < region.width(); ++x)
                    convolve(
                        out + x * 4, horizontal[size_t(x)],
                        [row](int index)
                        {
                            return row + index * 4;
                        },
                        execution_ == ResampleExecution::Automatic);
            }
            for (int y = 0; y < output.height(); ++y)
            {
                if (cancelled.load())
                    return ImageResult::failure(ImageError::Cancelled);
                auto* out = output.scanLine(y);
                for (int x = 0; x < output.width(); ++x)
                    convolve(
                        out + x * 4, vertical[size_t(y)],
                        [&intermediate, firstRow, x](int index)
                        {
                            return intermediate.constScanLine(index - firstRow) + x * 4;
                        },
                        execution_ == ResampleExecution::Automatic);
            }
        }
        if (cancelled.load())
            return ImageResult::failure(ImageError::Cancelled);
        if (output.isNull())
            return ImageResult::failure(ImageError::ResourceLimit);
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
        output.setColorSpace(source.colorSpace());
#endif
        return ImageResult::success(std::move(output));
    }
    catch (const std::bad_alloc&)
    {
        return ImageResult::failure(ImageError::ResourceLimit);
    }
}
}
