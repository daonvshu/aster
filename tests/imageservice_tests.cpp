#include "aster/cache/service/imageservice.h"

#include <QSharedPointer>
#include <QStringList>

#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace aster::cache;

namespace {
class TestNetwork final : public INetworkService {
public:
    int calls = 0;
    int failuresRemaining = 0;
    QUrl lastUrl;
    NetworkFetchOptions lastOptions;

    Result<NetworkResponse> fetch(const QUrl& url, const NetworkFetchOptions& options, const std::atomic<bool>&) override {
        ++calls;
        lastUrl = url;
        lastOptions = options;
        if (failuresRemaining-- > 0)
            return Result<NetworkResponse>::failure(ImageError::IoError, "retry");
        return Result<NetworkResponse>::success({200, {{"cache-control", "max-age=600"}, {"content-type", "image/test"}}, "network image"},
                                                CacheResultSource::Network);
    }
};

class TestSourceLoader final : public IImageSourceLoader {
public:
    int loads = 0;
    bool throwOnLoad = false;
    QByteArray bytes = "cGxhaW4=";

    Result<SourceKey> key(const ImageSource&) const override {
        return Result<SourceKey>::success({QByteArray(32, 's'), QByteArray(32, 'n')});
    }

    Result<SourcePayload> load(const ImageSource&, const SourceKey&, const SourceLoadOptions&, const std::atomic<bool>&) override {
        ++loads;
        if (throwOnLoad)
            throw std::runtime_error("source loader failure");
        SourcePayload payload;
        payload.bytes = bytes;
        payload.contentType = "application/octet-stream";
        return Result<SourcePayload>::success(std::move(payload), CacheResultSource::Data);
    }
};

void require(bool condition, const char* message = "ImageService test failed") {
    if (!condition)
        throw std::runtime_error(message);
}

ImageResult load(const QSharedPointer<ImagePipeline>& pipeline) {
    SourceRequest request;
    request.source.data = "image service test";
    request.render.physicalTargetSize = QSize(16, 16);
    auto promise = QSharedPointer<std::promise<ImageResult>>::create();
    auto future = promise->get_future();
    auto subscription = pipeline->request(request, [promise](ImageResult result) { promise->set_value(std::move(result)); });
    require(future.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
    auto result = future.get();
    require(pipeline->waitForIdle());
    return result;
}

ImageResult loadNetwork(const QSharedPointer<ImagePipeline>& pipeline) {
    SourceRequest request;
    request.source = *ImageSource::fromString("https://example.test/original").value;
    request.render.physicalTargetSize = QSize(16, 16);
    auto promise = QSharedPointer<std::promise<ImageResult>>::create();
    auto future = promise->get_future();
    auto subscription = pipeline->request(request, [promise](ImageResult result) { promise->set_value(std::move(result)); });
    require(future.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
    auto result = future.get();
    require(pipeline->waitForIdle());
    return result;
}

void httpInterceptorTests() {
    auto events = QSharedPointer<QStringList>::create();
    auto terminal = QSharedPointer<TestNetwork>::create();
    auto first = HttpInterceptor::create([events](HttpInterceptorChain& chain, HttpRequest request, const std::atomic<bool>& cancelled) {
        events->push_back("first-before");
        request.options.headers.insert("x-first", "one");
        auto result = chain.proceed(std::move(request), cancelled);
        events->push_back("first-after");
        return result;
    });
    auto second = HttpInterceptor::create([events](HttpInterceptorChain& chain, HttpRequest request, const std::atomic<bool>& cancelled) {
        events->push_back("second-before");
        require(request.options.headers.value("x-first") == "one");
        request.url.setPath("/rewritten");
        auto result = chain.proceed(std::move(request), cancelled);
        events->push_back("second-after");
        return result;
    });
    HttpInterceptorNetworkService service(terminal, {first, second});
    std::atomic<bool> cancelled{false};
    auto result = service.fetch(QUrl("https://example.test/original"), {}, cancelled);
    require(result && result.value->status == 200);
    require(terminal->calls == 1 && terminal->lastUrl.path() == "/rewritten");
    require(events->join('|') == "first-before|second-before|second-after|first-after");

    auto shortCircuit = HttpInterceptor::create(
            [](HttpInterceptorChain&, HttpRequest, const std::atomic<bool>&) { return Result<NetworkResponse>::success({204, {}, {}}); });
    HttpInterceptorNetworkService shortCircuitService(terminal, {shortCircuit});
    result = shortCircuitService.fetch(QUrl("https://example.test/short"), {}, cancelled);
    require(result && result.value->status == 204 && terminal->calls == 1);

    terminal->failuresRemaining = 1;
    auto retry = HttpInterceptor::create([](HttpInterceptorChain& chain, HttpRequest request, const std::atomic<bool>& cancelled) {
        auto result = chain.proceed(request, cancelled);
        return result ? result : chain.proceed(std::move(request), cancelled);
    });
    HttpInterceptorNetworkService retryService(terminal, {retry});
    result = retryService.fetch(QUrl("https://example.test/retry"), {}, cancelled);
    require(result && terminal->calls == 3);

    auto throwing = HttpInterceptor::create(
            [](HttpInterceptorChain&, HttpRequest, const std::atomic<bool>&) -> Result<NetworkResponse> { throw std::runtime_error("interceptor failure"); });
    HttpInterceptorNetworkService throwingService(terminal, {throwing});
    result = throwingService.fetch(QUrl("https://example.test/throw"), {}, cancelled);
    require(!result && result.error == ImageError::IoError);

    cancelled.store(true);
    result = service.fetch(QUrl("https://example.test/cancel"), {}, cancelled);
    require(!result && result.error == ImageError::Cancelled && terminal->calls == 3);

    ImageServiceConfig fluent;
    require(&fluent.addInterceptor(first).addInterceptor(second) == &fluent);
    require(fluent.httpInterceptors.size() == 2);
    try {
        fluent.addInterceptor(QSharedPointer<IHttpInterceptor>{});
        require(false);
    } catch (const std::invalid_argument&) {
    }
    try {
        HttpInterceptor::create({});
        require(false);
    } catch (const std::invalid_argument&) {
    }
}

void sourceInterceptorTests() {
    auto events = QSharedPointer<QStringList>::create();
    auto terminal = QSharedPointer<TestSourceLoader>::create();
    auto first = SourceInterceptor::create([events](SourceInterceptorChain& chain, SourceLoadRequest request, const std::atomic<bool>& cancelled) {
        events->push_back("first-before");
        auto result = chain.proceed(std::move(request), cancelled);
        events->push_back("first-after");
        return result;
    });
    auto decrypt = SourceInterceptor::create([events](SourceInterceptorChain& chain, SourceLoadRequest request, const std::atomic<bool>& cancelled) {
        events->push_back("decrypt-before");
        auto result = chain.proceed(std::move(request), cancelled);
        if (result) {
            result.value->bytes = QByteArray::fromBase64(result.value->bytes);
            result.value->contentType = "image/test";
        }
        events->push_back("decrypt-after");
        return result;
    });
    SourceInterceptorLoader loader(terminal, {first, decrypt});
    ImageSource source;
    source.data = "ignored";
    auto key = loader.key(source);
    require(bool(key), "Source interceptor loader did not delegate key generation");
    SourceLoadOptions options;
    options.maxBytes = 64;
    std::atomic<bool> cancelled{false};
    auto result = loader.load(source, *key.value, options, cancelled);
    require(result && result.value->bytes == "plain" && result.value->contentType == "image/test", "Source interceptor did not transform the payload");
    require(result.source == CacheResultSource::Data, "Source interceptor did not preserve the payload origin");
    require(events->join('|') == "first-before|decrypt-before|decrypt-after|first-after", "Source interceptor order is incorrect");

    auto expand = SourceInterceptor::create([](SourceInterceptorChain& chain, SourceLoadRequest request, const std::atomic<bool>& cancelled) {
        auto result = chain.proceed(std::move(request), cancelled);
        if (result)
            result.value->bytes = "12345";
        return result;
    });
    terminal->bytes = "raw";
    SourceInterceptorLoader expandingLoader(terminal, {expand});
    options.maxBytes = 4;
    result = expandingLoader.load(source, *key.value, options, cancelled);
    require(!result && result.error == ImageError::InvalidRequest, "Expanded source payload bypassed the byte limit");
    terminal->bytes = "cGxhaW4=";

    auto throwing = SourceInterceptor::create([](SourceInterceptorChain&, SourceLoadRequest, const std::atomic<bool>&) -> Result<SourcePayload> {
        throw std::runtime_error("source interceptor failure");
    });
    SourceInterceptorLoader throwingLoader(terminal, {throwing});
    options.maxBytes = 64;
    result = throwingLoader.load(source, *key.value, options, cancelled);
    require(!result && result.error == ImageError::ProcessingError, "Source interceptor exception was not converted to a failure");

    terminal->throwOnLoad = true;
    SourceInterceptorLoader failingLoader(terminal, {first});
    result = failingLoader.load(source, *key.value, options, cancelled);
    require(!result && result.error == ImageError::IoError, "Source loader exception was not preserved as an I/O failure");
    terminal->throwOnLoad = false;

    const auto loads = terminal->loads;
    cancelled.store(true);
    result = loader.load(source, *key.value, options, cancelled);
    require(!result && result.error == ImageError::Cancelled && terminal->loads == loads, "Cancelled source request unexpectedly reached the loader");

    ImageServiceConfig fluent;
    require(&fluent.addInterceptor(first).addInterceptor(decrypt) == &fluent);
    require(fluent.sourceInterceptors.size() == 2);
    try {
        fluent.addInterceptor(QSharedPointer<ISourceInterceptor>{});
        require(false);
    } catch (const std::invalid_argument&) {
    }
    try {
        SourceInterceptor::create({});
        require(false);
    } catch (const std::invalid_argument&) {
    }
    try {
        SourceInterceptorLoader invalid({}, {});
        require(false);
    } catch (const std::invalid_argument&) {
    }
}

void pipelineInterceptorTests() {
    auto events = QSharedPointer<QStringList>::create();
    auto renders = QSharedPointer<std::atomic<int>>::create(0);
    auto first = PipelineInterceptor::create([events](PipelineInterceptorChain chain, SourceRequest request, PipelineCompletion completion) {
        events->push_back("first-before");
        return chain.proceed(std::move(request), [events, completion = std::move(completion)](ImageResult result) mutable {
            events->push_back("first-after");
            completion(std::move(result));
        });
    });
    auto second = PipelineInterceptor::create([events](PipelineInterceptorChain chain, SourceRequest request, PipelineCompletion completion) {
        events->push_back("second-before");
        request.source.data = "pipeline-intercepted";
        request.render.physicalTargetSize = QSize(7, 9);
        return chain.proceed(std::move(request), [events, completion = std::move(completion)](ImageResult result) mutable {
            events->push_back("second-after");
            completion(std::move(result));
        });
    });

    ImageServiceConfig config;
    config.renderedMemoryBytes = 65536;
    config.renderer = [renders](const QByteArray& bytes, const auto& options, const auto&) {
        ++*renders;
        QImage image(options.physicalTargetSize, QImage::Format_ARGB32);
        image.fill(bytes == "pipeline-intercepted" ? Qt::red : Qt::blue);
        return ImageResult::success(image);
    };
    config.addInterceptor(first).addInterceptor(second);
    auto pipeline = ImageService::createPipeline(config);

    auto result = load(pipeline);
    require(result && result.value->size() == QSize(7, 9) && result.value->pixelColor(0, 0) == QColor(Qt::red),
            "Pipeline interceptor did not modify the complete request");
    require(events->join('|') == "first-before|second-before|second-after|first-after", "Pipeline interceptor order is incorrect");

    result = load(pipeline);
    require(result && result.source == CacheResultSource::RenderedMemory, "Second pipeline request did not use rendered memory");
    require(renders->load() == 1, "Rendered memory hit unexpectedly invoked the renderer");
    require(events->size() == 8, "Rendered memory hit bypassed pipeline interceptors");

    auto shortCircuit = PipelineInterceptor::create([](PipelineInterceptorChain, SourceRequest, PipelineCompletion completion) {
        QImage image(3, 4, QImage::Format_ARGB32);
        image.fill(Qt::yellow);
        completion(ImageResult::success(std::move(image)));
        return Subscription{};
    });
    auto shortConfig = config;
    shortConfig.pipelineInterceptors.clear();
    shortConfig.addInterceptor(shortCircuit);
    auto shortPipeline = ImageService::createPipeline(shortConfig);
    result = load(shortPipeline);
    require(result && result.value->size() == QSize(3, 4) && result.value->pixelColor(0, 0) == QColor(Qt::yellow),
            "Pipeline interceptor did not short-circuit the request");
    require(renders->load() == 1, "Short-circuited pipeline request reached the renderer");

    auto throwing = PipelineInterceptor::create(
            [](PipelineInterceptorChain, SourceRequest, PipelineCompletion) -> Subscription { throw std::runtime_error("pipeline interceptor failure"); });
    auto throwingConfig = config;
    throwingConfig.pipelineInterceptors.clear();
    throwingConfig.addInterceptor(throwing);
    auto throwingPipeline = ImageService::createPipeline(throwingConfig);
    result = load(throwingPipeline);
    require(!result && result.error == ImageError::ProcessingError, "Pipeline interceptor exception was not converted to a failure");

    ImageServiceConfig fluent;
    require(&fluent.addInterceptor(first).addInterceptor(second) == &fluent);
    require(fluent.pipelineInterceptors.size() == 2);
    try {
        fluent.addInterceptor(QSharedPointer<IPipelineInterceptor>{});
        require(false);
    } catch (const std::invalid_argument&) {
    }
    try {
        PipelineInterceptor::create({});
        require(false);
    } catch (const std::invalid_argument&) {
    }
}

void independentPipelineTests() {
    ImageServiceConfig base;
    base.renderer = [](const QByteArray& bytes, const auto& options, const auto&) {
        QImage image(options.physicalTargetSize, QImage::Format_ARGB32);
        image.fill(bytes == "page-a" ? Qt::red : bytes == "page-b" ? Qt::blue : Qt::green);
        return ImageResult::success(image);
    };

    auto pageANetwork = QSharedPointer<TestNetwork>::create();
    auto pageAConfig = base;
    pageAConfig.network = pageANetwork;
    pageAConfig.addInterceptor(HttpInterceptor::create([](HttpInterceptorChain& chain, HttpRequest request, const std::atomic<bool>& cancelled) {
        request.options.headers.insert("x-page", "a");
        return chain.proceed(std::move(request), cancelled);
    }));
    pageAConfig.addInterceptor(SourceInterceptor::create([](SourceInterceptorChain& chain, SourceLoadRequest request, const std::atomic<bool>& cancelled) {
        auto result = chain.proceed(std::move(request), cancelled);
        if (result)
            result.value->bytes = "page-a";
        return result;
    }));

    auto pageBNetwork = QSharedPointer<TestNetwork>::create();
    auto pageBConfig = base;
    pageBConfig.network = pageBNetwork;
    pageBConfig.addInterceptor(HttpInterceptor::create([](HttpInterceptorChain& chain, HttpRequest request, const std::atomic<bool>& cancelled) {
        request.options.headers.insert("x-page", "b");
        return chain.proceed(std::move(request), cancelled);
    }));
    pageBConfig.addInterceptor(SourceInterceptor::create([](SourceInterceptorChain& chain, SourceLoadRequest request, const std::atomic<bool>& cancelled) {
        auto result = chain.proceed(std::move(request), cancelled);
        if (result)
            result.value->bytes = "page-b";
        return result;
    }));

    auto pageAPipeline = ImageService::createPipeline(pageAConfig);
    auto pageBPipeline = ImageService::createPipeline(pageBConfig);
    require(pageAPipeline && pageBPipeline && pageAPipeline != pageBPipeline, "Independent pipelines were not created");
    require(!ImageService::isConfigured(), "Independent pipeline creation changed the global service state");

    const auto pageAResult = loadNetwork(pageAPipeline);
    const auto pageBResult = loadNetwork(pageBPipeline);
    require(pageAResult && pageAResult.value->pixelColor(0, 0) == QColor(Qt::red), "Page A source interceptor was not applied");
    require(pageBResult && pageBResult.value->pixelColor(0, 0) == QColor(Qt::blue), "Page B source interceptor was not applied");
    require(pageANetwork->calls == 1 && pageANetwork->lastOptions.headers.value("x-page") == "a", "Page A request did not use its interceptor");
    require(pageBNetwork->calls == 1 && pageBNetwork->lastOptions.headers.value("x-page") == "b", "Page B request did not use its interceptor");
}
} // namespace

void imageServiceTests() {
    httpInterceptorTests();
    sourceInterceptorTests();
    pipelineInterceptorTests();
    independentPipelineTests();
    require(!ImageService::isConfigured());
    bool rejected = false;
    try {
        ImageService::pipeline();
    } catch (const std::logic_error&) {
        rejected = true;
    }
    require(rejected);

    try {
        ImageService::configure({});
        require(false);
    } catch (const std::invalid_argument&) {
    }
    require(!ImageService::isConfigured());

    auto renders = QSharedPointer<std::atomic<int>>::create(0);
    ImageServiceConfig config;
    config.renderedMemoryBytes = 65536;
    config.encodedMemoryBytes = 8192;
    config.activeMemoryBytes = 32768;
    config.sourceCache.encodedData = true;
    auto serviceNetwork = QSharedPointer<TestNetwork>::create();
    auto serviceIntercepted = QSharedPointer<std::atomic<int>>::create(0);
    auto sourceIntercepted = QSharedPointer<std::atomic<int>>::create(0);
    auto serviceInterceptor =
            HttpInterceptor::create([serviceIntercepted](HttpInterceptorChain& chain, HttpRequest request, const std::atomic<bool>& cancelled) {
                ++*serviceIntercepted;
                request.options.headers.insert("authorization", "test-token");
                return chain.proceed(std::move(request), cancelled);
            });
    config.network = serviceNetwork;
    config.addInterceptor(serviceInterceptor);
    config.addInterceptor(
            SourceInterceptor::create([sourceIntercepted](SourceInterceptorChain& chain, SourceLoadRequest request, const std::atomic<bool>& cancelled) {
                if (request.source.kind == ImageSource::Kind::Network)
                    ++*sourceIntercepted;
                return chain.proceed(std::move(request), cancelled);
            }));
    config.renderer = [renders](const auto&, const auto& options, const auto&) {
        ++*renders;
        QImage image(options.physicalTargetSize, QImage::Format_ARGB32);
        image.fill(Qt::green);
        return ImageResult::success(image);
    };

    auto missingNetwork = config;
    missingNetwork.network.clear();
    try {
        ImageService::configure(missingNetwork);
        require(false);
    } catch (const std::invalid_argument&) {
    }
    require(!ImageService::isConfigured());

    auto conflict = config;
    conflict.sourceLoader = QSharedPointer<CachedSourceLoader>::create(nullptr);
    try {
        ImageService::configure(conflict);
        require(false);
    } catch (const std::invalid_argument&) {
    }

    std::atomic<int> configured{0}, rejectedConfigurations{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i)
        threads.emplace_back([&] {
            try {
                ImageService::configure(config);
                ++configured;
            } catch (const std::logic_error&) {
                ++rejectedConfigurations;
            }
        });
    for (auto& thread : threads)
        thread.join();
    require(configured == 1 && rejectedConfigurations == 7);

    auto first = ImageService::pipeline();
    require(ImageService::isConfigured());
    threads.clear();
    std::atomic<int> identical{0};
    for (int i = 0; i < 8; ++i)
        threads.emplace_back([&] {
            if (ImageService::pipeline() == first)
                ++identical;
        });
    for (auto& thread : threads)
        thread.join();
    require(identical == 8);

    auto result = load(first);
    require(bool(result) && bool(result.handle));
    auto networkResult = loadNetwork(first);
    require(networkResult && networkResult.source == CacheResultSource::Network, "Intercepted network result is invalid");
    require(serviceIntercepted->load() == 1 && serviceNetwork->calls == 1, "Initial network request did not traverse the interceptor once");
    require(sourceIntercepted->load() == 1, "Initial network request did not traverse the source interceptor once");
    require(serviceNetwork->lastOptions.headers.value("authorization") == "test-token", "Interceptor header did not reach the network service");
    auto cachedNetworkResult = loadNetwork(first);
    require(cachedNetworkResult && cachedNetworkResult.source == CacheResultSource::ActiveResource, "Cached network result did not use active memory");
    require(serviceIntercepted->load() == 1 && serviceNetwork->calls == 1, "Cached network result unexpectedly traversed the interceptor");
    require(sourceIntercepted->load() == 2, "Encoded cache hit did not traverse the source interceptor");
    require(load(ImageService::pipeline()).source == CacheResultSource::ActiveResource);
    require(renders->load() == 2);
    const auto stats = first->cacheStats();
    require(stats.renderedMemory.maxBytes == 65536);
    require(stats.encodedMemory.maxBytes == 8192 && stats.encodedMemory.hits == 2, "Encoded memory cache statistics are unexpected");
    require(stats.active.maxBytes == 32768);

    ImageService::shutdown();
    require(!ImageService::isConfigured());
    require(bool(load(first)));
    rejected = false;
    try {
        ImageService::pipeline();
    } catch (const std::logic_error&) {
        rejected = true;
    }
    require(rejected);
    rejected = false;
    try {
        ImageService::configure(config);
    } catch (const std::logic_error&) {
        rejected = true;
    }
    require(rejected);
    ImageService::shutdown();
    std::cout << "PASS global image service configuration, concurrency and lifetime\n";
}
