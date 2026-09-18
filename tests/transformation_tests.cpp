#include "aster/cache/cache/renderedmemorycache.h"
#include "aster/cache/core/imagetransformation.h"
#include "aster/cache/pipeline/imagepipeline.h"
#include "aster/cache/renderer/imagerenderer.h"
#include "aster/cache/source/cachedsourceloader.h"

#include <QBuffer>
#include <QSharedPointer>
#include <QStringList>

#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>

using namespace aster::cache;

namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

QByteArray encoded(const QImage& image) {
    QByteArray bytes;
    QBuffer buffer(&bytes);
    require(buffer.open(QIODevice::WriteOnly) && image.save(&buffer, "PNG"), "Could not encode transformation test image");
    return bytes;
}

RenderOptions options(QSize size = QSize(3, 3)) {
    RenderOptions result;
    result.physicalTargetSize = size;
    result.fitMode = "fill";
    return result;
}

ImageResult render(const QImage& image, RenderOptions renderOptions, const std::atomic<bool>& cancelled) {
    return ImageRenderer{}(encoded(image), renderOptions, cancelled);
}

void factoryAndIdentityTests() {
    bool threw = false;
    try {
        ImageTransformation::create({}, [](QImage image, const std::atomic<bool>&) { return ImageResult::success(std::move(image)); });
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    require(threw, "Empty transformation identifier was accepted");

    threw = false;
    try {
        ImageTransformation::create({"custom", 0, {}}, [](QImage image, const std::atomic<bool>&) { return ImageResult::success(std::move(image)); });
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    require(threw, "Zero transformation version was accepted");

    threw = false;
    try {
        ImageTransformation::create({"custom", 1, {}}, {});
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    require(threw, "Empty transformation handler was accepted");

    for (const auto invalidOpacity : {-0.1, 1.1}) {
        threw = false;
        try {
            ImageTransformation::opacity(invalidOpacity);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        require(threw, "Invalid opacity was accepted");
    }
    for (const auto invalidRadius : {0, 257}) {
        threw = false;
        try {
            ImageTransformation::blur(invalidRadius);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        require(threw, "Invalid blur radius was accepted");
    }

    const auto source = *KeyBuilder().network(QUrl("https://example.test/transformation")).value;
    auto first = options();
    first.transformations = {ImageTransformation::grayscale(), ImageTransformation::opacity(0.5)};
    auto second = options();
    second.transformations = {ImageTransformation::opacity(0.5), ImageTransformation::grayscale()};
    require(!(*KeyBuilder().render(source, first).value == *KeyBuilder().render(source, second).value), "Transformation order missing from cache key");

    second = first;
    second.transformations[1] = ImageTransformation::opacity(0.75);
    require(!(*KeyBuilder().render(source, first).value == *KeyBuilder().render(source, second).value), "Transformation parameters missing from cache key");
    second.transformations[1] = ImageTransformation::opacity(0.5);
    require(*KeyBuilder().render(source, first).value == *KeyBuilder().render(source, second).value, "Equivalent transformations changed the cache key");

    second.transformations = {QSharedPointer<ImageTransformation>{}};
    require(!KeyBuilder().render(source, second), "Null transformation was accepted in a cache key");
}

void builtInTests() {
    const std::atomic<bool> running{false};
    QImage red(3, 3, QImage::Format_ARGB32);
    red.fill(QColor(255, 0, 0));

    auto renderOptions = options();
    renderOptions.transformations = {ImageTransformation::grayscale()};
    auto result = render(red, renderOptions, running);
    require(result && result.value->pixelColor(1, 1) == QColor(54, 54, 54), "Grayscale transformation produced the wrong pixel");

    renderOptions.transformations = {ImageTransformation::invert()};
    result = render(red, renderOptions, running);
    require(result && result.value->pixelColor(1, 1) == QColor(0, 255, 255), "Invert transformation produced the wrong pixel");

    renderOptions.transformations = {ImageTransformation::opacity(0.5)};
    result = render(red, renderOptions, running);
    require(result && result.value->pixelColor(1, 1).alpha() == 128 && result.value->pixelColor(1, 1).red() == 255,
            "Opacity transformation produced the wrong pixel");

    renderOptions.transformations = {ImageTransformation::tint(QColor(0, 0, 255), 1)};
    result = render(red, renderOptions, running);
    require(result && result.value->pixelColor(1, 1) == QColor(0, 0, 255), "Tint transformation produced the wrong pixel");

    QImage impulse(3, 3, QImage::Format_ARGB32);
    impulse.fill(Qt::black);
    impulse.setPixelColor(1, 1, Qt::white);
    renderOptions.transformations = {ImageTransformation::blur(1)};
    result = render(impulse, renderOptions, running);
    const auto blurred = result ? result.value->pixelColor(1, 1) : QColor{};
    require(result && blurred.red() == 28 && blurred.green() == 28 && blurred.blue() == 28, "Blur transformation produced the wrong centre pixel");

    auto events = QSharedPointer<QStringList>::create();
    auto first = ImageTransformation::create({"test/first", 1, {}}, [events](QImage image, const std::atomic<bool>&) {
        events->push_back("first");
        return ImageResult::success(std::move(image));
    });
    auto second = ImageTransformation::create({"test/second", 1, {}}, [events](QImage image, const std::atomic<bool>&) {
        events->push_back("second");
        return ImageResult::success(std::move(image));
    });
    renderOptions.transformations = {first, second};
    require(render(red, renderOptions, running) && events->join('|') == "first|second", "Transformations ran out of order");
}

void failureAndCancellationTests() {
    QImage image(3, 3, QImage::Format_ARGB32);
    image.fill(Qt::red);
    const std::atomic<bool> running{false};
    auto renderOptions = options();

    renderOptions.transformations = {
            ImageTransformation::create({"test/throw", 1, {}}, [](QImage, const std::atomic<bool>&) -> ImageResult { throw std::runtime_error("failure"); })};
    require(render(image, renderOptions, running).error == ImageError::ProcessingError, "Transformation exception was not translated");

    renderOptions.transformations = {ImageTransformation::create(
            {"test/resize", 1, {}}, [](QImage input, const std::atomic<bool>&) { return ImageResult::success(input.scaled(1, 1)); })};
    require(render(image, renderOptions, running).error == ImageError::ProcessingError, "Transformation dimension change was accepted");

    renderOptions.transformations = {ImageTransformation::create({"test/empty", 1, {}}, [](QImage, const std::atomic<bool>&) { return ImageResult{}; })};
    require(render(image, renderOptions, running).error == ImageError::ProcessingError, "Empty transformation result was accepted");

    renderOptions.transformations = {ImageTransformation::create(
            {"test/error", 1, {}}, [](QImage, const std::atomic<bool>&) { return ImageResult::failure(ImageError::IoError, "custom failure"); })};
    const auto failure = render(image, renderOptions, running);
    require(failure.error == ImageError::IoError && failure.message == "custom failure", "Transformation error was not preserved");

    const std::atomic<bool> cancelled{true};
    renderOptions.transformations = {ImageTransformation::blur(1)};
    require(render(image, renderOptions, cancelled).error == ImageError::Cancelled, "Transformation cancellation was ignored");
}

void renderedCacheTests() {
    QImage image(3, 3, QImage::Format_ARGB32);
    image.fill(Qt::red);
    auto calls = QSharedPointer<std::atomic<int>>::create(0);
    auto transformation = ImageTransformation::create({"test/count", 1, {}}, [calls](QImage input, const std::atomic<bool>&) {
        ++*calls;
        return ImageResult::success(std::move(input));
    });
    ImagePipeline pipeline(QSharedPointer<RenderedMemoryCache>::create(64 * 1024), QSharedPointer<CachedSourceLoader>::create(nullptr), ImageRenderer{});
    SourceRequest request;
    request.source.data = encoded(image);
    request.render = options();
    request.render.transformations = {transformation};

    auto fetch = [&] {
        std::promise<ImageResult> promise;
        auto future = promise.get_future();
        auto subscription = pipeline.request(request, [&promise](ImageResult result) { promise.set_value(std::move(result)); });
        require(future.wait_for(std::chrono::seconds(10)) == std::future_status::ready, "Transformation pipeline timed out");
        return future.get();
    };
    require(fetch() && calls->load() == 1, "Transformation pipeline did not render");
    const auto cached = fetch();
    require(cached && cached.source == CacheResultSource::RenderedMemory && calls->load() == 1, "Transformed image did not use rendered memory cache");
    require(pipeline.waitForIdle(), "Transformation pipeline did not drain");
}
} // namespace

void transformationTests() {
    factoryAndIdentityTests();
    builtInTests();
    failureAndCancellationTests();
    renderedCacheTests();
    std::cout << "PASS image transformations, failures, cancellation and cache identity\n";
}
