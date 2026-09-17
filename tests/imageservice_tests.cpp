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
        fluent.addInterceptor({});
        require(false);
    } catch (const std::invalid_argument&) {
    }
    try {
        HttpInterceptor::create({});
        require(false);
    } catch (const std::invalid_argument&) {
    }
}

void independentPipelineTests() {
    ImageServiceConfig base;
    base.renderer = [](const auto&, const auto& options, const auto&) {
        QImage image(options.physicalTargetSize, QImage::Format_ARGB32);
        image.fill(Qt::green);
        return ImageResult::success(image);
    };

    auto pageANetwork = QSharedPointer<TestNetwork>::create();
    auto pageAConfig = base;
    pageAConfig.network = pageANetwork;
    pageAConfig.addInterceptor(HttpInterceptor::create([](HttpInterceptorChain& chain, HttpRequest request, const std::atomic<bool>& cancelled) {
        request.options.headers.insert("x-page", "a");
        return chain.proceed(std::move(request), cancelled);
    }));

    auto pageBNetwork = QSharedPointer<TestNetwork>::create();
    auto pageBConfig = base;
    pageBConfig.network = pageBNetwork;
    pageBConfig.addInterceptor(HttpInterceptor::create([](HttpInterceptorChain& chain, HttpRequest request, const std::atomic<bool>& cancelled) {
        request.options.headers.insert("x-page", "b");
        return chain.proceed(std::move(request), cancelled);
    }));

    auto pageAPipeline = ImageService::createPipeline(pageAConfig);
    auto pageBPipeline = ImageService::createPipeline(pageBConfig);
    require(pageAPipeline && pageBPipeline && pageAPipeline != pageBPipeline, "Independent pipelines were not created");
    require(!ImageService::isConfigured(), "Independent pipeline creation changed the global service state");

    require(bool(loadNetwork(pageAPipeline)), "Page A pipeline request failed");
    require(bool(loadNetwork(pageBPipeline)), "Page B pipeline request failed");
    require(pageANetwork->calls == 1 && pageANetwork->lastOptions.headers.value("x-page") == "a", "Page A request did not use its interceptor");
    require(pageBNetwork->calls == 1 && pageBNetwork->lastOptions.headers.value("x-page") == "b", "Page B request did not use its interceptor");
}
} // namespace

void imageServiceTests() {
    httpInterceptorTests();
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
    auto serviceInterceptor =
            HttpInterceptor::create([serviceIntercepted](HttpInterceptorChain& chain, HttpRequest request, const std::atomic<bool>& cancelled) {
                ++*serviceIntercepted;
                request.options.headers.insert("authorization", "test-token");
                return chain.proceed(std::move(request), cancelled);
            });
    config.network = serviceNetwork;
    config.addInterceptor(serviceInterceptor);
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
    require(serviceNetwork->lastOptions.headers.value("authorization") == "test-token", "Interceptor header did not reach the network service");
    auto cachedNetworkResult = loadNetwork(first);
    require(cachedNetworkResult && cachedNetworkResult.source == CacheResultSource::ActiveResource, "Cached network result did not use active memory");
    require(serviceIntercepted->load() == 1 && serviceNetwork->calls == 1, "Cached network result unexpectedly traversed the interceptor");
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
