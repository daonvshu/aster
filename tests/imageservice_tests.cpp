#include "aster/cache/service/imageservice.h"

#include <QSharedPointer>

#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace aster::cache;

namespace
{
void require(bool condition)
{
    if (!condition)
        throw std::runtime_error("ImageService test failed");
}

ImageResult load(const QSharedPointer<ImagePipeline>& pipeline)
{
    SourceRequest request;
    request.source.data = "image service test";
    request.render.physicalTargetSize = QSize(16, 16);
    auto promise = QSharedPointer<std::promise<ImageResult>>::create();
    auto future = promise->get_future();
    auto subscription = pipeline->request(request,
                                          [promise](ImageResult result)
                                          {
                                              promise->set_value(std::move(result));
                                          });
    require(future.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
    auto result = future.get();
    require(pipeline->waitForIdle());
    return result;
}
}

void imageServiceTests()
{
    require(!ImageService::isConfigured());
    bool rejected = false;
    try
    {
        ImageService::pipeline();
    }
    catch (const std::logic_error&)
    {
        rejected = true;
    }
    require(rejected);

    try
    {
        ImageService::configure({});
        require(false);
    }
    catch (const std::invalid_argument&)
    {
    }
    require(!ImageService::isConfigured());

    auto renders = QSharedPointer<std::atomic<int>>::create(0);
    ImageServiceConfig config;
    config.renderedMemoryBytes = 65536;
    config.encodedMemoryBytes = 8192;
    config.activeMemoryBytes = 32768;
    config.sourceCache.encodedData = true;
    config.renderer = [renders](const auto&, const auto& options, const auto&)
    {
        ++*renders;
        QImage image(options.physicalTargetSize, QImage::Format_ARGB32);
        image.fill(Qt::green);
        return ImageResult::success(image);
    };

    auto conflict = config;
    conflict.sourceLoader = QSharedPointer<CachedSourceLoader>::create(nullptr);
    try
    {
        ImageService::configure(conflict);
        require(false);
    }
    catch (const std::invalid_argument&)
    {
    }

    std::atomic<int> configured{0}, rejectedConfigurations{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i)
        threads.emplace_back(
            [&]
            {
                try
                {
                    ImageService::configure(config);
                    ++configured;
                }
                catch (const std::logic_error&)
                {
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
        threads.emplace_back(
            [&]
            {
                if (ImageService::pipeline() == first)
                    ++identical;
            });
    for (auto& thread : threads)
        thread.join();
    require(identical == 8);

    auto result = load(first);
    require(bool(result) && bool(result.handle));
    require(load(ImageService::pipeline()).source == CacheResultSource::ActiveResource);
    require(renders->load() == 1);
    const auto stats = first->cacheStats();
    require(stats.renderedMemory.maxBytes == 65536);
    require(stats.encodedMemory.maxBytes == 8192 && stats.encodedMemory.hits == 1);
    require(stats.active.maxBytes == 32768);

    ImageService::shutdown();
    require(!ImageService::isConfigured());
    require(bool(load(first)));
    rejected = false;
    try
    {
        ImageService::pipeline();
    }
    catch (const std::logic_error&)
    {
        rejected = true;
    }
    require(rejected);
    rejected = false;
    try
    {
        ImageService::configure(config);
    }
    catch (const std::logic_error&)
    {
        rejected = true;
    }
    require(rejected);
    ImageService::shutdown();
    std::cout << "PASS global image service configuration, concurrency and lifetime\n";
}
