#include "aster/cache/cache/renderedmemorycache.h"
#include "aster/cache/pipeline/imagepipeline.h"
#include "aster/cache/renderer/imagerenderer.h"
#include "aster/cache/source/cachedsourceloader.h"
#include "reference/picasaresampler.h"

#include <QBuffer>
#include <QSharedPointer>

#include <future>
#include <iostream>
#include <stdexcept>

using namespace aster::cache;

namespace
{
void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

QImage pattern(QSize size)
{
    QImage image(size, QImage::Format_RGBA8888);
    for (int y = 0; y < size.height(); ++y)
        for (int x = 0; x < size.width(); ++x)
            image.setPixelColor(
                x, y,
                QColor((x * 71 + y * 13) % 256, (x * 17 + y * 39) % 256, (x * 47 + y * 23) % 256));
    return image;
}

void geometryTests()
{
    for (const auto dpr : {1.0, 1.25, 1.5, 2.0, 3.0})
    {
        const auto size = physicalTargetSize(QSizeF(200, 100), dpr);
        require(size && *size.value == QSize(int(200 * dpr), int(100 * dpr)), "DPR target");
    }
    require(*physicalTargetSize(QSizeF(101, 51), 1.25).value == QSize(127, 64), "Fractional ceil");
    require(*physicalTargetSize(QSizeF(301, 301), 1, 16).value == QSize(304, 304), "Bucket ceil");
    require(!physicalTargetSize(QSizeF(0, 10), 2), "Zero target");
    require(!physicalTargetSize(QSizeF(1e30, 10), 2), "Overflow target");
    require(!physicalTargetSize(QSizeF(10, 10), 0), "Zero DPR");
    require(renderGeometry(QSize(400, 200), QSize(100, 100), ImageFit::Contain).value->scaledSize ==
                QSize(100, 50),
            "Contain geometry");
    const auto cover = renderGeometry(QSize(400, 200), QSize(100, 100), ImageFit::Cover);
    require(cover.value->scaledSize == QSize(200, 100) &&
                cover.value->region == QRect(50, 0, 100, 100),
            "Cover geometry");
    require(renderGeometry(QSize(4, 2), QSize(100, 100), ImageFit::Fill).value->scaledSize ==
                QSize(100, 100),
            "Fill geometry");
    require(renderGeometry(QSize(4, 2), QSize(100, 100), ImageFit::ScaleDown).value->scaledSize ==
                QSize(4, 2),
            "ScaleDown geometry");
    require(renderGeometry(QSize(400, 200), QSize(1, 1), ImageFit::None).value->scaledSize ==
                QSize(400, 200),
            "None geometry");
    require(renderGeometry(QSize(1, 16384), QSize(100, 100), ImageFit::Contain).value->scaledSize ==
                QSize(1, 100),
            "Narrow geometry");
}

void filterTests()
{
    const std::atomic<bool> running{false};
    ImageResampler resampler({}, ResampleExecution::Automatic, KernelEvaluation::Exact);
    for (const auto algorithm : {ImageScaleAlgorithm::QtFast, ImageScaleAlgorithm::QtSmooth,
                                 ImageScaleAlgorithm::Bilinear, ImageScaleAlgorithm::Bicubic,
                                 ImageScaleAlgorithm::Lanczos3, ImageScaleAlgorithm::Lanczos4})
    {
        for (const auto size : {QSize(1, 1), QSize(1, 37), QSize(39, 1), QSize(31, 27)})
        {
            QImage image(size, QImage::Format_ARGB32);
            image.fill(QColor(80, 120, 160, 128));
            for (const auto target : {QSize(1, 1), QSize(3, 5), QSize(51, 49)})
            {
                const auto geometry = *renderGeometry(size, target, ImageFit::Fill).value;
                const auto result = resampler.resize(image, geometry, algorithm, running);
                require(result && result.value->size() == target, "Algorithm dimensions");
                const auto pixel = result.value->pixelColor(0, 0);
                require(std::abs(pixel.red() - 80) <= 2 && std::abs(pixel.alpha() - 128) <= 1,
                        "Flat alpha/color preservation");
            }
        }
        QImage image(2, 1, QImage::Format_ARGB32);
        image.setPixelColor(0, 0, QColor(255, 0, 0, 0));
        image.setPixelColor(1, 0, QColor(0, 0, 255, 255));
        const auto result =
            resampler.resize(image, {QSize(17, 1), QRect(0, 0, 17, 1)}, algorithm, running);
        require(bool(result), "Transparent edge resize");
        for (int x = 0; x < 17; ++x)
        {
            const auto* p = result.value->constScanLine(0) + x * 4;
            require(p[0] == 0 && p[0] <= p[3] && p[1] <= p[3] && p[2] <= p[3],
                    "Transparent red leaked or premultiplied invariant broken");
        }
    }

    const auto image = pattern(QSize(13, 9));
    // Independent floating-point kernel golden values for a 5x3 -> 3x2 reduction.
    // Allow one byte for the production fixed-point coefficient quantization.
    const int golden[3][18] = {
        {35, 22, 29, 147, 49, 103, 112, 76, 177, 51, 70, 57, 163, 97, 131, 128, 124, 205},
        {29, 18, 24, 156, 46, 101, 113, 74, 178, 47, 72, 56, 174, 100, 133, 131, 128, 210},
        {22, 16, 21, 164, 45, 100, 115, 74, 179, 40, 72, 55, 182, 101, 134, 133, 130, 213}};
    for (int filter = 0; filter < 3; ++filter)
    {
        const auto algorithm = ImageScaleAlgorithm(int(ImageScaleAlgorithm::Bilinear) + filter);
        const auto result = resampler.resize(pattern(QSize(5, 3)), {QSize(3, 2), QRect(0, 0, 3, 2)},
                                             algorithm, running);
        require(bool(result), "Golden filter resize");
        for (int y = 0; y < 2; ++y)
            for (int x = 0; x < 3; ++x)
                for (int channel = 0; channel < 3; ++channel)
                    require(std::abs(int(result.value->constScanLine(y)[x * 4 + channel]) -
                                     golden[filter][(y * 3 + x) * 3 + channel]) <= 1,
                            "Bilinear/Bicubic/Lanczos3 golden pixel mismatch");
    }
    for (const auto target : {QSize(5, 3), QSize(23, 17), QSize(1, 1)})
    {
        const auto reference = PicasaResampler::resizeLanczos4(image, target);
        const auto result = resampler.resize(image, {target, QRect(QPoint(), target)},
                                             ImageScaleAlgorithm::Lanczos4, running);
        require(result && result.value->convertToFormat(QImage::Format_RGBA8888) == reference,
                "Lanczos4 differs from verified Picasa reference on opaque pixels");
    }
    const QSize full(35, 29);
    const QRect tile(7, 3, 17, 19);
    const auto region =
        resampler.resize(image, {full, tile}, ImageScaleAlgorithm::Lanczos4, running);
    require(region && region.value->convertToFormat(QImage::Format_RGBA8888) ==
                          PicasaResampler::resizeLanczos4Region(image, full, tile),
            "Lanczos4 region mismatch");

    const auto qt = resampler.resize(image, {QSize(7, 5), QRect(0, 0, 7, 5)},
                                     ImageScaleAlgorithm::QtSmooth, running);
    require(qt && *qt.value == image.convertToFormat(QImage::Format_RGBA8888_Premultiplied)
                                   .scaled(7, 5, Qt::IgnoreAspectRatio, Qt::SmoothTransformation),
            "QtSmooth must use Qt smooth path");

    // Analytic bilinear midpoint and cubic/Lanczos symmetric impulse invariants.
    QImage ramp(2, 1, QImage::Format_RGB32);
    ramp.setPixel(0, 0, qRgb(0, 0, 0));
    ramp.setPixel(1, 0, qRgb(200, 200, 200));
    const auto midpoint = resampler.resize(ramp, {QSize(3, 1), QRect(0, 0, 3, 1)},
                                           ImageScaleAlgorithm::Bilinear, running);
    require(midpoint && midpoint.value->pixelColor(1, 0).red() == 100, "Bilinear midpoint");
    for (auto algorithm : {ImageScaleAlgorithm::Bicubic, ImageScaleAlgorithm::Lanczos3})
    {
        QImage impulse(9, 1, QImage::Format_RGB32);
        impulse.fill(Qt::black);
        impulse.setPixel(4, 0, qRgb(255, 255, 255));
        auto filtered =
            resampler.resize(impulse, {QSize(17, 1), QRect(0, 0, 17, 1)}, algorithm, running);
        require(filtered && filtered.value->pixelColor(8, 0).red() == 255, "Impulse centre");
        for (int x = 0; x < 8; ++x)
            require(filtered.value->pixel(x, 0) == filtered.value->pixel(16 - x, 0),
                    "Symmetric filter impulse");
    }
    std::atomic<bool> cancelled{true};
    require(resampler.resize(image, {full, tile}, ImageScaleAlgorithm::Lanczos4, cancelled).error ==
                ImageError::Cancelled,
            "Resampling cancel");
    ResampleLimits limits;
    limits.maxWorkingBytes = 64;
    require(ImageResampler(limits)
                    .resize(image, {full, tile}, ImageScaleAlgorithm::Lanczos4, running)
                    .error == ImageError::ResourceLimit,
            "Memory bound");

    limits = {};
    limits.maxPixels = 1;
    require(ImageResampler(limits)
                    .resize(image, {full, tile}, ImageScaleAlgorithm::QtSmooth, running)
                    .error == ImageError::ResourceLimit,
            "Explicit pixel bound");

    // Exercise both the former pixel limit and the former 256 MiB working-memory limit.
    QImage large(6000, 6000, QImage::Format_RGB32);
    large.fill(Qt::red);
    ResampleMetrics metrics;
    const auto thumbnail = resampler.resize(large, {QSize(1, 1), QRect(0, 0, 1, 1)},
                                            ImageScaleAlgorithm::QtFast, running, &metrics);
    require(thumbnail && thumbnail.value->pixelColor(0, 0) == QColor(Qt::red),
            "Default resampling accepts large images");
    require(metrics.workingBytes > 256LL * 1024 * 1024, "Former memory limit exceeded");
}

void identityTests()
{
    RenderOptions options;
    require(options.scaleAlgorithm == ImageScaleAlgorithm::QtSmooth, "Default algorithm");
    options.physicalTargetSize = QSize(7, 5);
    const auto sourceKey = *KeyBuilder().network(QUrl("https://test/image")).value;
    const auto smooth = *KeyBuilder().render(sourceKey, options).value;
    options.scaleAlgorithm = ImageScaleAlgorithm::Lanczos4;
    require(!(smooth == *KeyBuilder().render(sourceKey, options).value), "Algorithm key collision");
    options.resamplerVersion = 2;
    const auto version2 = *KeyBuilder().render(sourceKey, options).value;
    options.resamplerVersion = 1;
    require(!(version2 == *KeyBuilder().render(sourceKey, options).value), "Version key collision");

    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    pattern(QSize(13, 9)).save(&buffer, "PNG");
    std::atomic<int> renders{0};
    ImagePipeline pipeline(
        QSharedPointer<RenderedMemoryCache>::create(65536),
        QSharedPointer<CachedSourceLoader>::create(nullptr),
        [&](const QByteArray& data, const RenderOptions& render, const std::atomic<bool>& token)
        {
            ++renders;
            return ImageRenderer{}(data, render, token);
        });
    SourceRequest request;
    request.source.data = bytes;
    request.render = options;
    auto fetch = [&]
    {
        std::promise<ImageResult> promise;
        auto future = promise.get_future();
        auto sub = pipeline.request(request,
                                    [&](ImageResult result)
                                    {
                                        promise.set_value(result);
                                    });
        require(future.wait_for(std::chrono::seconds(10)) == std::future_status::ready,
                "Render timeout");
        return future.get();
    };
    require(bool(fetch()), "Lanczos render");
    require(fetch().source == CacheResultSource::RenderedMemory && renders == 1, "Rendered reuse");
    request.render.scaleAlgorithm = ImageScaleAlgorithm::QtSmooth;
    require(bool(fetch()) && renders == 2, "Algorithm must rerender");
    request.render.resamplerVersion = 2;
    require(fetch().error == ImageError::InvalidRequest, "Unsupported implementation version");
    require(pipeline.waitForIdle(), "Render drain");
}
}

void resamplerTests()
{
    ImageResampler exact({}, ResampleExecution::Automatic, KernelEvaluation::Exact);
    ImageResampler lut({}, ResampleExecution::Automatic, KernelEvaluation::Lut);
    const std::atomic<bool> lutRunning{false};
    int maximumError = 0;
    qint64 difference = 0;
    qint64 channels = 0;
    for (auto algorithm : {ImageScaleAlgorithm::Lanczos3, ImageScaleAlgorithm::Lanczos4})
        for (auto size : {QSize(1, 1), QSize(1, 97), QSize(513, 7), QSize(127, 93)})
            for (auto target : {QSize(1, 1), QSize(31, 29), QSize(173, 111)})
                for (bool alpha : {false, true})
                {
                    auto input = pattern(size);
                    if (alpha)
                        for (int y = 0; y < size.height(); ++y)
                            for (int x = 0; x < size.width(); ++x)
                            {
                                auto color = input.pixelColor(x, y);
                                color.setAlpha((x * 37 + y * 53) % 256);
                                input.setPixelColor(x, y, color);
                            }
                    const RenderGeometry geometry{target, QRect(QPoint(), target)};
                    const auto a = exact.resize(input, geometry, algorithm, lutRunning);
                    const auto b = lut.resize(input, geometry, algorithm, lutRunning);
                    require(a && b, "LUT comparison failed");
                    for (int y = 0; y < target.height(); ++y)
                        for (int x = 0; x < target.width() * 4; ++x)
                        {
                            const int delta = std::abs(int(a.value->constScanLine(y)[x]) -
                                                       int(b.value->constScanLine(y)[x]));
                            maximumError = std::max(maximumError, delta);
                            difference += delta;
                            ++channels;
                        }
                }
    std::cout << "LUT max_channel_error=" << maximumError
              << " mean_error=" << double(difference) / channels << '\n';
    require(maximumError <= 2, "LUT pixel error exceeds tolerance");
    const std::atomic<bool> running{false};
    ImageResampler scalar({}, ResampleExecution::Scalar);
    ImageResampler automatic;
    for (const auto algorithm : {ImageScaleAlgorithm::Bilinear, ImageScaleAlgorithm::Bicubic,
                                 ImageScaleAlgorithm::Lanczos3, ImageScaleAlgorithm::Lanczos4})
        for (const auto sourceSize :
             {QSize(1, 1), QSize(1, 73), QSize(79, 1), QSize(31, 27), QSize(513, 7)})
            for (const auto target : {QSize(1, 1), QSize(3, 5), QSize(47, 39)})
            {
                auto source = pattern(sourceSize).convertToFormat(QImage::Format_ARGB32);
                for (int y = 0; y < source.height(); ++y)
                    for (int x = 0; x < source.width(); ++x)
                    {
                        auto color = source.pixelColor(x, y);
                        color.setAlpha((x * 37 + y * 53) % 256);
                        source.setPixelColor(x, y, color);
                    }
                const QRect region = target.width() > 3
                                         ? QRect(1, 1, target.width() - 2, target.height() - 2)
                                         : QRect(QPoint(), target);
                const auto expected = scalar.resize(source, {target, region}, algorithm, running);
                const auto actual = automatic.resize(source, {target, region}, algorithm, running);
                require(expected && actual && *expected.value == *actual.value,
                        "SIMD/scalar pixel mismatch");
            }
    geometryTests();
    filterTests();
    identityTests();
    std::cout << "PASS geometry, six resamplers, Picasa parity, alpha and cache identity\n";
}
