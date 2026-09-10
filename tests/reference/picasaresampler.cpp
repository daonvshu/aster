#include "picasaresampler.h"

#include <QThread>
#include <QtMath>

#include <algorithm>
#include <thread>
#include <vector>

#if defined(_M_X64) || defined(__SSE2__)
#include <emmintrin.h>
#define PICASA_RESAMPLER_HAS_SSE2 1
#else
#define PICASA_RESAMPLER_HAS_SSE2 0
#endif

namespace
{
constexpr double kPi = 3.1415926535897932384626433832795;
constexpr double kRadius = 4.0;
constexpr int kFixedWeightScale = 16383;

double sinc(double x)
{
    if (qAbs(x) < 1.0e-12)
        return 1.0;
    const double angle = kPi * x;
    return qSin(angle) / angle;
}

#if PICASA_RESAMPLER_HAS_SSE2
// Each Lanczos coefficient applies to all four RGBA channels.  Picasa's
// original hot loop also uses XMM integer multiply/add instructions (IDA
// shows pmaddwd/psrad/packuswb around 0x5246BC).  This variant preserves our
// exact 16383 divisor and rounding, unlike a float SIMD approximation.
inline __m128i multiplyRgbaByWeight(const uchar* pixel, int weight)
{
    const __m128i bytes = _mm_cvtsi32_si128(*reinterpret_cast<const int*>(pixel));
    const __m128i values = _mm_unpacklo_epi8(bytes, _mm_setzero_si128());
    const __m128i weights = _mm_set1_epi16(static_cast<short>(weight));
    const __m128i low = _mm_mullo_epi16(values, weights);
    const __m128i high = _mm_mulhi_epi16(values, weights);
    return _mm_unpacklo_epi16(low, high);
}

template <typename PixelAt>
inline void convolveRgbaSse2(uchar* destination, const int* weights, int tapCount,
                             PixelAt&& pixelAt)
{
    __m128i sum = _mm_setzero_si128();
    for (int tap = 0; tap < tapCount; ++tap)
        sum = _mm_add_epi32(sum, multiplyRgbaByWeight(pixelAt(tap), weights[tap]));

    alignas(16) qint32 channels[4];
    _mm_store_si128(reinterpret_cast<__m128i*>(channels), sum);
    for (int channel = 0; channel < 4; ++channel)
    {
        const qint64 value =
            (qint64(channels[channel]) + kFixedWeightScale / 2) / kFixedWeightScale;
        destination[channel] = static_cast<uchar>(std::clamp<qint64>(value, 0, 255));
    }
}
#endif

template <typename Function> void parallelRows(int rowCount, Function&& function)
{
    const int workerCount = std::min(rowCount, std::max(1, QThread::idealThreadCount()));
    if (workerCount == 1)
    {
        function(0, rowCount);
        return;
    }

    std::vector<std::thread> workers;
    workers.reserve(workerCount);
    for (int worker = 0; worker < workerCount; ++worker)
    {
        const int firstRow = rowCount * worker / workerCount;
        const int lastRow = rowCount * (worker + 1) / workerCount;
        workers.emplace_back(
            [firstRow, lastRow, &function]
            {
                function(firstRow, lastRow);
            });
    }
    for (std::thread& worker : workers)
        worker.join();
}
}

QImage PicasaResampler::resizeLanczos4(const QImage& source, QSize destinationSize)
{
    return resizeLanczos4Region(source, destinationSize, QRect(QPoint(0, 0), destinationSize));
}

QImage PicasaResampler::resizeLanczos4Region(const QImage& source, QSize destinationSize,
                                             QRect destinationRegion)
{
    if (source.isNull() || !destinationSize.isValid())
        return {};

    destinationSize.setWidth(std::max(1, destinationSize.width()));
    destinationSize.setHeight(std::max(1, destinationSize.height()));

    // Format_RGBA8888 has a documented R,G,B,A byte layout on every host.
    // Avoiding QRgb's native-endian layout fixes channel interpretation issues
    // when the source originated in an indexed or unusual image format.
    destinationRegion = destinationRegion.intersected(QRect(QPoint(0, 0), destinationSize));
    if (destinationRegion.isEmpty())
        return {};

    const QImage input = source.format() == QImage::Format_RGBA8888
                             ? source
                             : source.convertToFormat(QImage::Format_RGBA8888);
    if (input.size() == destinationSize)
        return input.copy(destinationRegion);

    const auto horizontal = buildContributions(input.width(), destinationSize.width(),
                                               destinationRegion.x(), destinationRegion.width());
    const auto vertical = buildContributions(input.height(), destinationSize.height(),
                                             destinationRegion.y(), destinationRegion.height());

    int firstSourceRow = input.height() - 1;
    int lastSourceRow = 0;
    for (const Contribution& contribution : vertical)
    {
        for (int sourceIndex : contribution.sourceIndexes)
        {
            firstSourceRow = std::min(firstSourceRow, sourceIndex);
            lastSourceRow = std::max(lastSourceRow, sourceIndex);
        }
    }
    const int intermediateHeight = lastSourceRow - firstSourceRow + 1;

    // Keep the intermediate in ARGB32.  Picasa has separate three/four-channel
    // paths; this is the four-channel path and intentionally filters alpha too.
    QImage intermediate(destinationRegion.width(), intermediateHeight, QImage::Format_RGBA8888);
    parallelRows(intermediateHeight,
                 [&](int firstRow, int lastRow)
                 {
                     for (int y = firstRow; y < lastRow; ++y)
                     {
                         const uchar* src = input.constScanLine(firstSourceRow + y);
                         uchar* dst = intermediate.scanLine(y);
                         for (int x = 0; x < destinationRegion.width(); ++x)
                         {
                             const Contribution& c = horizontal[x];
                             const int tapCount = c.sourceIndexes.size();
                             const int* indexes = c.sourceIndexes.constData();
                             const int* weights = c.weights.constData();
#if PICASA_RESAMPLER_HAS_SSE2
                             convolveRgbaSse2(dst + 4 * x, weights, tapCount,
                                              [src, indexes](int tap)
                                              {
                                                  return src + 4 * indexes[tap];
                                              });
#else
                qint64 a = 0, r = 0, g = 0, b = 0;
                for (int tap = 0; tap < tapCount; ++tap) {
                    const uchar *pixel = src + 4 * indexes[tap];
                    const int weight = weights[tap];
                    r += qint64(pixel[0]) * weight;
                    g += qint64(pixel[1]) * weight;
                    b += qint64(pixel[2]) * weight;
                    a += qint64(pixel[3]) * weight;
                }
                uchar *pixel = dst + 4 * x;
                pixel[0] = uchar(clampByte((r + kWeightScale / 2) / kWeightScale));
                pixel[1] = uchar(clampByte((g + kWeightScale / 2) / kWeightScale));
                pixel[2] = uchar(clampByte((b + kWeightScale / 2) / kWeightScale));
                pixel[3] = uchar(clampByte((a + kWeightScale / 2) / kWeightScale));
#endif
                         }
                     }
                 });

    QImage output(destinationRegion.size(), QImage::Format_RGBA8888);
    parallelRows(destinationRegion.height(),
                 [&](int firstRow, int lastRow)
                 {
                     for (int y = firstRow; y < lastRow; ++y)
                     {
                         uchar* dst = output.scanLine(y);
                         const Contribution& c = vertical[y];
                         const int tapCount = c.sourceIndexes.size();
                         const int* indexes = c.sourceIndexes.constData();
                         const int* weights = c.weights.constData();
                         for (int x = 0; x < destinationRegion.width(); ++x)
                         {
#if PICASA_RESAMPLER_HAS_SSE2
                             convolveRgbaSse2(dst + 4 * x, weights, tapCount,
                                              [&intermediate, indexes, firstSourceRow, x](int tap)
                                              {
                                                  return intermediate.constScanLine(
                                                             indexes[tap] - firstSourceRow) +
                                                         4 * x;
                                              });
#else
                qint64 a = 0, r = 0, g = 0, b = 0;
                for (int tap = 0; tap < tapCount; ++tap) {
                    const uchar *pixel = intermediate.constScanLine(indexes[tap] - firstSourceRow) + 4 * x;
                    const int weight = weights[tap];
                    r += qint64(pixel[0]) * weight;
                    g += qint64(pixel[1]) * weight;
                    b += qint64(pixel[2]) * weight;
                    a += qint64(pixel[3]) * weight;
                }
                uchar *pixel = dst + 4 * x;
                pixel[0] = uchar(clampByte((r + kWeightScale / 2) / kWeightScale));
                pixel[1] = uchar(clampByte((g + kWeightScale / 2) / kWeightScale));
                pixel[2] = uchar(clampByte((b + kWeightScale / 2) / kWeightScale));
                pixel[3] = uchar(clampByte((a + kWeightScale / 2) / kWeightScale));
#endif
                         }
                     }
                 });
    return output;
}

QVector<PicasaResampler::Contribution> PicasaResampler::buildContributions(int sourceLength,
                                                                           int destinationLength,
                                                                           int firstDestination,
                                                                           int count)
{
    QVector<Contribution> result(count);
    const double scale = double(destinationLength) / double(sourceLength);
    // Picasa expands the support when shrinking: a proper low-pass filter.
    const double filterScale = std::min(1.0, scale);
    const double support = kRadius / filterScale;

    for (int output = 0; output < count; ++output)
    {
        const int destination = firstDestination + output;
        const double centre = (double(destination) + 0.5) / scale - 0.5;
        const int first = std::max(0, int(qCeil(centre - support)));
        const int last = std::min(sourceLength - 1, int(qFloor(centre + support)));
        Contribution& contribution = result[output];

        QVector<double> floatingWeights;
        double total = 0.0;
        for (int source = first; source <= last; ++source)
        {
            const double weight = lanczos4((double(source) - centre) * filterScale) * filterScale;
            contribution.sourceIndexes.append(source);
            floatingWeights.append(weight);
            total += weight;
        }

        // The original writes signed 16-bit coefficients whose sum is exactly
        // 16383.  Make the largest tap absorb quantisation residual so flat
        // regions preserve brightness, including at truncated image edges.
        int sum = 0;
        int strongestTap = 0;
        double strongestMagnitude = -1.0;
        if (qAbs(total) < 1.0e-12)
        {
            // A defensive fallback for degenerate floating-point input.
            contribution.sourceIndexes = {std::clamp(qRound(centre), 0, sourceLength - 1)};
            contribution.weights = {kWeightScale};
            continue;
        }

        for (int tap = 0; tap < floatingWeights.size(); ++tap)
        {
            if (qAbs(floatingWeights.at(tap)) > strongestMagnitude)
            {
                strongestMagnitude = qAbs(floatingWeights.at(tap));
                strongestTap = tap;
            }
            const int quantized = qRound(floatingWeights.at(tap) / total * kWeightScale);
            contribution.weights.append(quantized);
            sum += quantized;
        }
        contribution.weights[strongestTap] += kWeightScale - sum;
    }
    return result;
}

double PicasaResampler::lanczos4(double distance)
{
    const double absoluteDistance = qAbs(distance);
    if (absoluteDistance >= kRadius)
        return 0.0;
    return sinc(distance) * sinc(distance / kRadius);
}

int PicasaResampler::clampByte(qint64 value)
{
    return int(std::clamp<qint64>(value, 0, 255));
}
