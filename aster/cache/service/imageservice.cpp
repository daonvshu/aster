#include "imageservice.h"

#include "aster/cache/cache/renderedmemorycache.h"

#include <QSharedPointer>

#include <mutex>
#include <stdexcept>
#include <utility>

namespace aster::cache {
namespace {
enum class Phase { Empty, Configuring, Ready, Stopped };

struct ServiceState {
    std::mutex mutex;
    Phase phase = Phase::Empty;
    QSharedPointer<ImagePipeline> pipeline;
};

ServiceState& state() {
    static ServiceState instance;
    return instance;
}

QSharedPointer<ImagePipeline> buildPipeline(const ImageServiceConfig& config) {
    if (!config.renderer || !config.clock || config.workerCount <= 0 || config.renderedMemoryBytes < 0 || config.encodedMemoryBytes < 0 ||
        config.activeMemoryBytes < 0 || config.maxEncodedEntryBytes <= 0)
        throw std::invalid_argument("Invalid image service configuration");

    if (config.sourceLoader && (config.network || !config.httpInterceptors.isEmpty() || config.sourceDisk || config.encodedMemoryBytes != 0))
        throw std::invalid_argument("Custom source loader owns its source cache configuration");
    if (!config.httpInterceptors.isEmpty() && !config.network)
        throw std::invalid_argument("HTTP interceptors require a network service");

    auto memory = QSharedPointer<RenderedMemoryCache>::create(config.renderedMemoryBytes, config.clock);
    auto loader = config.sourceLoader;
    if (!loader) {
        QSharedPointer<EncodedMemoryCache> encoded;
        if (config.encodedMemoryBytes > 0)
            encoded = QSharedPointer<EncodedMemoryCache>::create(config.encodedMemoryBytes, config.maxEncodedEntryBytes, config.clock);

        auto network = config.network;
        if (!config.httpInterceptors.isEmpty())
            network = QSharedPointer<HttpInterceptorNetworkService>::create(network, config.httpInterceptors);
        loader = QSharedPointer<CachedSourceLoader>::create(encoded, config.sourceDisk, network, config.clock, config.sourceCache);
    }
    if (!config.sourceInterceptors.isEmpty())
        loader = QSharedPointer<SourceInterceptorLoader>::create(loader, config.sourceInterceptors);

    PipelineResources resources;
    resources.renderedDisk = config.renderedDisk;
    if (config.activeMemoryBytes > 0)
        resources.active = QSharedPointer<ActiveResourceStore>::create(config.activeMemoryBytes, config.clock);

    return QSharedPointer<ImagePipeline>::create(memory, loader, config.renderer, config.workerCount, config.events, std::move(resources));
}
} // namespace

ImageServiceConfig& ImageServiceConfig::addInterceptor(QSharedPointer<IHttpInterceptor> interceptor) {
    if (!interceptor)
        throw std::invalid_argument("HTTP interceptor must not be null");
    httpInterceptors.push_back(std::move(interceptor));
    return *this;
}

ImageServiceConfig& ImageServiceConfig::addInterceptor(QSharedPointer<ISourceInterceptor> interceptor) {
    if (!interceptor)
        throw std::invalid_argument("Source interceptor must not be null");
    sourceInterceptors.push_back(std::move(interceptor));
    return *this;
}

QSharedPointer<ImagePipeline> ImageService::createPipeline(const ImageServiceConfig& config) {
    return buildPipeline(config);
}

void ImageService::configure(const ImageServiceConfig& config) {
    auto& service = state();
    {
        std::lock_guard<std::mutex> lock(service.mutex);
        if (service.phase != Phase::Empty)
            throw std::logic_error("Image service can only be configured once before shutdown");

        service.phase = Phase::Configuring;
    }

    // Construct and destroy injected dependencies outside the global state lock.
    try {
        auto pipeline = createPipeline(config);
        std::lock_guard<std::mutex> lock(service.mutex);
        if (service.phase == Phase::Stopped)
            throw std::logic_error("Image service was shut down during configuration");

        service.pipeline = std::move(pipeline);
        service.phase = Phase::Ready;
    } catch (...) {
        std::lock_guard<std::mutex> lock(service.mutex);
        if (service.phase == Phase::Configuring)
            service.phase = Phase::Empty;
        throw;
    }
}

QSharedPointer<ImagePipeline> ImageService::pipeline() {
    auto& service = state();
    std::lock_guard<std::mutex> lock(service.mutex);
    if (service.phase != Phase::Ready)
        throw std::logic_error("Image service is not configured or has been shut down");

    return service.pipeline;
}

bool ImageService::isConfigured() {
    auto& service = state();
    std::lock_guard<std::mutex> lock(service.mutex);
    return service.phase == Phase::Ready;
}

void ImageService::shutdown() {
    QSharedPointer<ImagePipeline> released;
    auto& service = state();
    {
        std::lock_guard<std::mutex> lock(service.mutex);
        service.phase = Phase::Stopped;
        released = std::move(service.pipeline);
    }
}
} // namespace aster::cache
