#include "aster/cache/core/imagekey.h"
#include "aster/cache/decoder/imagedecoder.h"
#include "aster/cache/renderer/imagerenderer.h"
#include "aster/cache/service/imageservice.h"

#include <QBuffer>
#include <QSharedPointer>
#include <QStringList>

#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <new>
#include <stdexcept>

using namespace aster::cache;

namespace {
class InvalidIdentityDecoder final : public IImageDecoder {
public:
    const ProcessorIdentity& identity() const noexcept override {
        return identity_;
    }

    bool supports(const QByteArray&, const QByteArray&) const override {
        return false;
    }

    ImageResult decode(const QByteArray&, const DecodeContext&, const std::atomic<bool>&) const override {
        return ImageResult::failure(ImageError::UnsupportedFormat);
    }

private:
    ProcessorIdentity identity_;
};

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

QByteArray png() {
    QImage image(2, 2, QImage::Format_ARGB32);
    image.fill(Qt::green);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    require(buffer.open(QIODevice::WriteOnly) && image.save(&buffer, "PNG"), "Could not encode decoder test image");
    return bytes;
}

RenderOptions options() {
    RenderOptions result;
    result.physicalTargetSize = QSize(4, 4);
    result.fitMode = "fill";
    return result;
}

QSharedPointer<IImageDecoder> decoder(ProcessorIdentity identity = {"test/custom", 1, {}}) {
    return ImageDecoder::create(
            std::move(identity), [](const QByteArray& bytes, const QByteArray&) { return bytes.startsWith("CSTM"); },
            [](const QByteArray&, const DecodeContext&, const std::atomic<bool>& cancelled) {
                if (cancelled.load())
                    return ImageResult::failure(ImageError::Cancelled);
                QImage image(2, 2, QImage::Format_ARGB32);
                image.fill(Qt::red);
                return ImageResult::success(std::move(image));
            });
}

ImageResult render(const QByteArray& bytes, RenderOptions renderOptions, const std::atomic<bool>& cancelled = std::atomic<bool>{false}) {
    return ImageRenderer{}(bytes, renderOptions, cancelled);
}

void factoryAndIdentityTests() {
    bool threw = false;
    try {
        ImageDecoder::create(
                {}, [](const QByteArray&, const QByteArray&) { return true; },
                [](const QByteArray&, const DecodeContext&, const std::atomic<bool>&) { return ImageResult{}; });
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    require(threw, "Empty decoder identifier was accepted");

    threw = false;
    try {
        ImageDecoder::create(
                {"test", 0, {}}, [](const QByteArray&, const QByteArray&) { return true; },
                [](const QByteArray&, const DecodeContext&, const std::atomic<bool>&) { return ImageResult{}; });
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    require(threw, "Zero decoder version was accepted");

    threw = false;
    try {
        ImageDecoder::create({"test", 1, {}}, {}, [](const QByteArray&, const DecodeContext&, const std::atomic<bool>&) { return ImageResult{}; });
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    require(threw, "Empty decoder matcher was accepted");

    threw = false;
    try {
        ImageDecoder::create({"test", 1, {}}, [](const QByteArray&, const QByteArray&) { return true; }, {});
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    require(threw, "Empty decoder handler was accepted");

    const auto source = *KeyBuilder().network(QUrl("https://example.test/custom")).value;
    auto first = options();
    first.decoders = {decoder({"one", 1, "a"}), decoder({"two", 1, {}})};
    auto second = options();
    second.decoders = {decoder({"two", 1, {}}), decoder({"one", 1, "a"})};
    require(!(*KeyBuilder().render(source, first).value == *KeyBuilder().render(source, second).value), "Decoder order missing from cache key");

    second = first;
    second.decoders[0] = decoder({"one", 2, "a"});
    require(!(*KeyBuilder().render(source, first).value == *KeyBuilder().render(source, second).value), "Decoder version missing from cache key");
    second.decoders[0] = decoder({"one", 1, "b"});
    require(!(*KeyBuilder().render(source, first).value == *KeyBuilder().render(source, second).value), "Decoder parameters missing from cache key");

    second = first;
    second.contentTypeHint = "image/x-custom";
    require(!(*KeyBuilder().render(source, first).value == *KeyBuilder().render(source, second).value), "Content-Type hint missing from cache key");
    second.decoders = {QSharedPointer<IImageDecoder>{}};
    require(!KeyBuilder().render(source, second), "Null decoder was accepted in a cache key");
}

void matchingAndFallbackTests() {
    auto events = QSharedPointer<QStringList>::create();
    auto first = ImageDecoder::create(
            {"test/first", 1, {}},
            [events](const QByteArray&, const QByteArray&) {
                events->push_back("first-match");
                return false;
            },
            [](const QByteArray&, const DecodeContext&, const std::atomic<bool>&) { return ImageResult::failure(ImageError::ProcessingError); });
    auto second = ImageDecoder::create(
            {"test/second", 1, {}},
            [events](const QByteArray& bytes, const QByteArray& contentType) {
                events->push_back("second-match:" + QString::fromLatin1(contentType));
                return bytes.startsWith("CSTM");
            },
            [events](const QByteArray&, const DecodeContext& context, const std::atomic<bool>&) {
                events->push_back("second-decode:" + QString::fromLatin1(context.contentType));
                QImage image(2, 2, QImage::Format_ARGB32);
                image.fill(Qt::red);
                return ImageResult::success(std::move(image));
            });
    auto renderOptions = options();
    renderOptions.contentTypeHint = "IMAGE/X-CUSTOM; charset=binary";
    renderOptions.decoders = {first, second};
    const auto custom = render("CSTM payload", renderOptions);
    require(custom && custom.value->size() == QSize(4, 4) && custom.value->pixelColor(0, 0) == QColor(Qt::red), "Custom decoder did not render");
    require(events->join('|') == "first-match|second-match:image/x-custom|second-decode:image/x-custom", "Decoder order or Content-Type is incorrect");

    events->clear();
    const auto fallback = render(png(), renderOptions);
    require(fallback && fallback.value->pixelColor(0, 0) == QColor(Qt::green), "Qt decoder fallback failed");
    require(events->join('|') == "first-match|second-match:image/x-custom", "Fallback decoder matching order is incorrect");

    auto terminal = ImageDecoder::create(
            {"test/terminal", 1, {}}, [](const QByteArray&, const QByteArray&) { return true; },
            [](const QByteArray&, const DecodeContext&, const std::atomic<bool>&) {
                return ImageResult::failure(ImageError::CorruptedEntry, "custom corrupt");
            });
    auto laterCalls = QSharedPointer<std::atomic<int>>::create(0);
    auto later = ImageDecoder::create(
            {"test/later", 1, {}}, [](const QByteArray&, const QByteArray&) { return true; },
            [laterCalls](const QByteArray&, const DecodeContext&, const std::atomic<bool>&) {
                ++*laterCalls;
                return ImageResult::success(QImage(2, 2, QImage::Format_ARGB32));
            });
    renderOptions.decoders = {terminal, later};
    const auto failure = render("CSTM payload", renderOptions);
    require(failure.error == ImageError::CorruptedEntry && failure.message == "custom corrupt" && laterCalls->load() == 0,
            "Matched decoder failure incorrectly fell through");
}

void failureCancellationAndLimitTests() {
    auto renderOptions = options();
    renderOptions.decoders = {ImageDecoder::create(
            {"test/matcher-throw", 1, {}}, [](const QByteArray&, const QByteArray&) -> bool { throw std::runtime_error("failure"); },
            [](const QByteArray&, const DecodeContext&, const std::atomic<bool>&) { return ImageResult{}; })};
    require(render("data", renderOptions).error == ImageError::ProcessingError, "Matcher exception was not translated");

    renderOptions.decoders = {ImageDecoder::create(
            {"test/decode-throw", 1, {}}, [](const QByteArray&, const QByteArray&) { return true; },
            [](const QByteArray&, const DecodeContext&, const std::atomic<bool>&) -> ImageResult { throw std::runtime_error("failure"); })};
    require(render("data", renderOptions).error == ImageError::ProcessingError, "Decoder exception was not translated");

    renderOptions.decoders = {ImageDecoder::create(
            {"test/allocation", 1, {}}, [](const QByteArray&, const QByteArray&) { return true; },
            [](const QByteArray&, const DecodeContext&, const std::atomic<bool>&) -> ImageResult { throw std::bad_alloc(); })};
    require(render("data", renderOptions).error == ImageError::ResourceLimit, "Decoder allocation failure was not translated");

    renderOptions.decoders = {ImageDecoder::create(
            {"test/empty", 1, {}}, [](const QByteArray&, const QByteArray&) { return true; },
            [](const QByteArray&, const DecodeContext&, const std::atomic<bool>&) { return ImageResult{}; })};
    require(render("data", renderOptions).error == ImageError::ProcessingError, "Empty decoder result was accepted");

    renderOptions.decoders = {ImageDecoder::create(
            {"test/null-image", 1, {}}, [](const QByteArray&, const QByteArray&) { return true; },
            [](const QByteArray&, const DecodeContext&, const std::atomic<bool>&) { return ImageResult::success(QImage{}); })};
    require(render("data", renderOptions).error == ImageError::ProcessingError, "Null decoded image was accepted");

    DecodeLimits limits;
    limits.maxSide = 2;
    ImageRenderer limited(limits);
    renderOptions.decoders = {ImageDecoder::create(
            {"test/oversized", 1, {}}, [](const QByteArray&, const QByteArray&) { return true; },
            [](const QByteArray&, const DecodeContext&, const std::atomic<bool>&) { return ImageResult::success(QImage(3, 2, QImage::Format_ARGB32)); })};
    const std::atomic<bool> running{false};
    require(limited("data", renderOptions, running).error == ImageError::ResourceLimit, "Oversized custom decode was accepted");

    auto matcherCalls = QSharedPointer<std::atomic<int>>::create(0);
    renderOptions.decoders = {ImageDecoder::create(
            {"test/cancel", 1, {}},
            [matcherCalls](const QByteArray&, const QByteArray&) {
                ++*matcherCalls;
                return true;
            },
            [](const QByteArray&, const DecodeContext&, const std::atomic<bool>&) { return ImageResult{}; })};
    const std::atomic<bool> cancelled{true};
    require(render("data", renderOptions, cancelled).error == ImageError::Cancelled && matcherCalls->load() == 0, "Decoder cancellation was ignored");
}

void servicePipelineTests() {
    auto decodeCalls = QSharedPointer<std::atomic<int>>::create(0);
    auto contentTypeSeen = QSharedPointer<QByteArray>::create();
    auto custom = ImageDecoder::create(
            {"test/service", 1, {}}, [](const QByteArray& bytes, const QByteArray&) { return bytes.startsWith("CSTM"); },
            [decodeCalls, contentTypeSeen](const QByteArray&, const DecodeContext& context, const std::atomic<bool>&) {
                ++*decodeCalls;
                *contentTypeSeen = context.contentType;
                QImage image(2, 2, QImage::Format_ARGB32);
                image.fill(Qt::blue);
                return ImageResult::success(std::move(image));
            });

    ImageServiceConfig config;
    config.renderer = ImageRenderer{};
    config.renderedMemoryBytes = 64 * 1024;
    require(&config.addDecoder(custom) == &config && config.decoders.size() == 1, "Decoder fluent configuration failed");
    bool threw = false;
    try {
        config.addDecoder({});
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    require(threw, "Null decoder configuration was accepted");

    auto invalidConfig = config;
    invalidConfig.decoders = {QSharedPointer<InvalidIdentityDecoder>::create()};
    threw = false;
    try {
        ImageService::createPipeline(invalidConfig);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    require(threw, "Invalid decoder identity was accepted by the pipeline");

    auto pipeline = ImageService::createPipeline(config);
    SourceRequest request;
    request.source.data = "CSTM pipeline";
    request.source.contentType = "image/x-custom";
    request.render = options();
    auto fetch = [&] {
        auto promise = QSharedPointer<std::promise<ImageResult>>::create();
        auto future = promise->get_future();
        auto subscription = pipeline->request(request, [promise](ImageResult result) { promise->set_value(std::move(result)); });
        require(future.wait_for(std::chrono::seconds(10)) == std::future_status::ready, "Decoder pipeline timed out");
        return future.get();
    };
    const auto first = fetch();
    require(first && first.value->pixelColor(0, 0) == QColor(Qt::blue) && decodeCalls->load() == 1 && *contentTypeSeen == "image/x-custom",
            "Configured decoder did not receive the source payload");
    const auto cached = fetch();
    require(cached && cached.source == CacheResultSource::RenderedMemory && decodeCalls->load() == 1, "Custom decoded image did not use rendered cache");
    require(pipeline->waitForIdle(), "Decoder pipeline did not drain");
}
} // namespace

void decoderTests() {
    factoryAndIdentityTests();
    matchingAndFallbackTests();
    failureCancellationAndLimitTests();
    servicePipelineTests();
    std::cout << "PASS custom decoders, fallback, limits, cancellation and cache identity\n";
}
