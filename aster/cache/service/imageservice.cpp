#include "imageservice.h"

#include "aster/cache/cache/renderedmemorycache.h"

#include <mutex>
#include <stdexcept>

namespace aster::cache
{
namespace
{
enum class Phase
{
    Empty,
    Configuring,
    Ready,
    Stopped
};

struct ServiceState
{
    std::mutex mutex;
    Phase phase = Phase::Empty;
    std::shared_ptr<ImagePipeline> pipeline;
};

ServiceState& state()
{
    static ServiceState instance;
    return instance;
}

std::shared_ptr<ImagePipeline> createPipeline(const ImageServiceConfig& config)
{
    if (!config.renderer || !config.clock || config.workerCount <= 0 ||
        config.renderedMemoryBytes < 0 || config.encodedMemoryBytes < 0 ||
        config.activeMemoryBytes < 0 || config.maxEncodedEntryBytes <= 0)
        throw std::invalid_argument("Invalid image service configuration");

    if (config.sourceLoader &&
        (config.network || config.sourceDisk || config.encodedMemoryBytes != 0))
        throw std::invalid_argument("Custom source loader owns its source cache configuration");

    auto memory = std::make_shared<RenderedMemoryCache>(config.renderedMemoryBytes, config.clock);
    auto loader = config.sourceLoader;
    if (!loader)
    {
        std::shared_ptr<EncodedMemoryCache> encoded;
        if (config.encodedMemoryBytes > 0)
            encoded = std::make_shared<EncodedMemoryCache>(
                config.encodedMemoryBytes, config.maxEncodedEntryBytes, config.clock);

        loader = std::make_shared<CachedSourceLoader>(encoded, config.sourceDisk, config.network,
                                                      config.clock, config.sourceCache);
    }

    PipelineResources resources;
    resources.renderedDisk = config.renderedDisk;
    if (config.activeMemoryBytes > 0)
        resources.active =
            std::make_shared<ActiveResourceStore>(config.activeMemoryBytes, config.clock);

    return std::make_shared<ImagePipeline>(memory, loader, config.renderer, config.workerCount,
                                           config.events, std::move(resources));
}
}

void ImageService::configure(const ImageServiceConfig& config)
{
    auto& service = state();
    {
        std::lock_guard<std::mutex> lock(service.mutex);
        if (service.phase != Phase::Empty)
            throw std::logic_error("Image service can only be configured once before shutdown");

        service.phase = Phase::Configuring;
    }

    // Construct and destroy injected dependencies outside the global state lock.
    try
    {
        auto pipeline = createPipeline(config);
        std::lock_guard<std::mutex> lock(service.mutex);
        if (service.phase == Phase::Stopped)
            throw std::logic_error("Image service was shut down during configuration");

        service.pipeline = std::move(pipeline);
        service.phase = Phase::Ready;
    }
    catch (...)
    {
        std::lock_guard<std::mutex> lock(service.mutex);
        if (service.phase == Phase::Configuring)
            service.phase = Phase::Empty;
        throw;
    }
}

std::shared_ptr<ImagePipeline> ImageService::pipeline()
{
    auto& service = state();
    std::lock_guard<std::mutex> lock(service.mutex);
    if (service.phase != Phase::Ready)
        throw std::logic_error("Image service is not configured or has been shut down");

    return service.pipeline;
}

bool ImageService::isConfigured()
{
    auto& service = state();
    std::lock_guard<std::mutex> lock(service.mutex);
    return service.phase == Phase::Ready;
}

void ImageService::shutdown()
{
    std::shared_ptr<ImagePipeline> released;
    auto& service = state();
    {
        std::lock_guard<std::mutex> lock(service.mutex);
        service.phase = Phase::Stopped;
        released = std::move(service.pipeline);
    }
}
}
