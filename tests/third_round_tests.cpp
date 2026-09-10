#include "aster/cache/cache/filediskcache.h"
#include "aster/cache/cache/rendereddiskcache.h"
#include "aster/cache/cache/renderedmemorycache.h"
#include "aster/cache/pipeline/imagepipeline.h"
#include "aster/cache/source/cachedsourceloader.h"
#include "aster/cache/testing/fakeclock.h"

#include <QFile>
#include <QSharedPointer>
#include <QTemporaryDir>

#include <future>
#include <iostream>
#include <thread>

using namespace aster::cache;

#define REQUIRE(x)                                                                                 \
    do                                                                                             \
    {                                                                                              \
        if (!(x))                                                                                  \
            throw std::runtime_error(std::string(__FILE__) + ":" + std::to_string(__LINE__) +      \
                                     " " #x);                                                      \
    } while (false)

namespace
{
RenderKey key(int n = 0)
{
    RenderOptions options;
    options.physicalTargetSize = QSize(32, 32);
    return *KeyBuilder()
                .render(*KeyBuilder().network(QUrl(QString("https://test/%1").arg(n))).value,
                        options)
                .value;
}

QImage image()
{
    QImage value(32, 32, QImage::Format_ARGB32);
    value.fill(QColor(12, 34, 56, 78));
    value.setDevicePixelRatio(2);
    return value;
}

ImageResult request(ImagePipeline& pipeline, const SourceRequest& input)
{
    auto promise = QSharedPointer<std::promise<ImageResult>>::create();
    auto future = promise->get_future();
    auto sub = pipeline.request(input,
                                [promise](auto result)
                                {
                                    promise->set_value(std::move(result));
                                });
    REQUIRE(future.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
    return future.get();
}

void diskRoundTrip()
{
    QTemporaryDir dir;
    auto disk = QSharedPointer<FileDiskCache>::create(dir.path(), 1024 * 1024, 65536);
    RenderedDiskCache cache(disk);
    const auto original = image();
    REQUIRE(cache.put(key(), original));
    auto result = cache.get(key());
    REQUIRE(result);
    REQUIRE(result.source == CacheResultSource::RenderedDisk);
    REQUIRE(result.value->pixelColor(1, 1) == original.pixelColor(1, 1));
    REQUIRE(result.value->devicePixelRatio() == 2);
    RenderedDiskConfig changed;
    changed.encoderVersion = 2;
    RenderedDiskCache v2(disk, changed);
    REQUIRE(!v2.get(key()));
    changed = {};
    changed.maxPixels = 10;
    RenderedDiskCache limited(disk, changed);
    REQUIRE(!limited.put(key(), original));
    REQUIRE(!limited.get(key()));
    REQUIRE(!cache.get(key()));
    REQUIRE(disk->put(cache.storageKey(key()), {"invalid png", {{"schema", 1}}}));
    REQUIRE(!cache.get(key()));
    REQUIRE(!disk->contains(cache.storageKey(key())));
}

void activeLifecycle()
{
    auto clock = QSharedPointer<aster::cache::testing::FakeClock>::create();
    ActiveResourceStore active(1024 * 1024, clock);
    auto original = image();
    std::vector<ImageHandle> handles;
    for (int i = 0; i < 1000; ++i)
        handles.push_back(active.acquire(key(), original));
    REQUIRE(active.stats().entries == 1);
    clock->advance(60000);
    REQUIRE(active.stats().oldestAgeMs == 60000);
    REQUIRE(handles.front().image().constBits() == original.constBits());
    handles.clear();
    REQUIRE(active.stats().entries == 0);
    REQUIRE(active.stats().bytes == 0);

    auto retained = active.acquire(key(), original);
    active.setMaxCost(0);
    REQUIRE(active.stats().bytes == 0);
    REQUIRE(!active.find(key()));
    REQUIRE(!retained.image().isNull());
    auto rejected = active.acquire(key(1), original);
    REQUIRE(rejected);
    REQUIRE(active.stats().rejected == 1);
    active.setMaxCost(1024 * 1024);
    auto newer = active.acquire(key(), original);
    retained.reset();
    REQUIRE(active.stats().entries == 1);
    newer.reset();
    REQUIRE(active.stats().entries == 0);

    std::vector<std::thread> threads;
    for (int n = 0; n < 8; ++n)
        threads.emplace_back(
            [&, n]
            {
                for (int i = 0; i < 1000; ++i)
                {
                    auto handle = active.acquire(key(n), original);
                    auto shared = active.find(key(n));
                }
            });
    for (auto& t : threads)
        t.join();
    REQUIRE(active.stats().entries == 0);
}

void pipelineRoundTrip()
{
    QTemporaryDir dir;
    auto disk = QSharedPointer<FileDiskCache>::create(dir.path(), 1024 * 1024, 65536);
    auto renderedDisk = QSharedPointer<RenderedDiskCache>::create(disk);
    auto active = QSharedPointer<ActiveResourceStore>::create(1024 * 1024);
    auto memory = QSharedPointer<RenderedMemoryCache>::create(1024 * 1024);
    auto loader = QSharedPointer<CachedSourceLoader>::create(nullptr);
    std::atomic<int> renders{0};
    auto renderer = [&](const QByteArray&, const RenderOptions& options, const auto&)
    {
        ++renders;
        QImage value(options.physicalTargetSize, QImage::Format_ARGB32);
        value.fill(Qt::red);
        return ImageResult::success(value);
    };
    ImagePipeline pipeline(memory, loader, renderer, 2, {}, {renderedDisk, active});
    SourceRequest input;
    input.source.data = "bytes";
    input.render.physicalTargetSize = QSize(32, 32);
    input.diskStrategy = DiskCacheStrategy::RenderedOnly;
    auto first = request(pipeline, input);
    REQUIRE(first && first.handle);
    pipeline.trimMemory(MemoryPressure::Low);
    auto live = request(pipeline, input);
    REQUIRE(live.source == CacheResultSource::ActiveResource);
    REQUIRE(renders == 1);
    first.handle.reset();
    live.handle.reset();
    REQUIRE(active->stats().entries == 0);
    auto fromDisk = request(pipeline, input);
    REQUIRE(fromDisk.source == CacheResultSource::RenderedDisk);
    REQUIRE(renders == 1);
    pipeline.trimMemory(MemoryPressure::Critical);
    REQUIRE(active->stats().bytes == 0);
    REQUIRE(!fromDisk.handle.image().isNull());

    input.render.processors = {{"resize", 2, ""}};
    REQUIRE(request(pipeline, input));
    REQUIRE(renders == 2);
    input.render.dpr = 2;
    REQUIRE(request(pipeline, input));
    REQUIRE(renders == 3);
    input.render.fitMode = "cover";
    REQUIRE(request(pipeline, input));
    REQUIRE(renders == 4);
    input.source.data = "changed";
    REQUIRE(request(pipeline, input));
    REQUIRE(renders == 5);

    memory->clear();
    input.load.cache.read = CacheReadPolicy::BypassDisk;
    REQUIRE(request(pipeline, input));
    REQUIRE(renders == 6);
    memory->clear();
    input.load.cache.read = CacheReadPolicy::Default;
    input.diskStrategy = DiskCacheStrategy::None;
    REQUIRE(request(pipeline, input));
    REQUIRE(renders == 7);

    memory->clear();
    input.diskStrategy = DiskCacheStrategy::SourceOnly;
    REQUIRE(request(pipeline, input));
    REQUIRE(renders == 8);

    auto before = disk->stats().entryCount;
    input.source.data = "no-store";
    input.diskStrategy = DiskCacheStrategy::All;
    input.load.cache.write = CacheWritePolicy::NoStore;
    REQUIRE(request(pipeline, input));
    REQUIRE(disk->stats().entryCount == before);

    input.load.cache = {};
    input.diskStrategy = DiskCacheStrategy::Automatic;
    input.source.kind = ImageSource::Kind::Local;
    input.source.url = QUrl::fromLocalFile(dir.filePath("image.bin"));
    {
        QFile file(input.source.url.toLocalFile());
        REQUIRE(file.open(QIODevice::WriteOnly));
        file.write("local");
    }
    input.expensiveProcessing = false;
    REQUIRE(request(pipeline, input));
    REQUIRE(disk->stats().entryCount == before);
    memory->clear();
    input.expensiveProcessing = true;
    REQUIRE(request(pipeline, input));
    REQUIRE(disk->stats().entryCount == before + 1);
    memory->clear();
    const auto count = renders.load();
    REQUIRE(request(pipeline, input).source == CacheResultSource::RenderedDisk);
    REQUIRE(renders == count);
    {
        QFile file(input.source.url.toLocalFile());
        REQUIRE(file.open(QIODevice::WriteOnly));
        file.write("local changed");
    }
    REQUIRE(request(pipeline, input));
    REQUIRE(renders == count + 1);
}
}

void thirdRoundTests()
{
    diskRoundTrip();
    std::cout << "PASS rendered disk round trip and corruption\n";
    activeLifecycle();
    std::cout << "PASS active handles and concurrent release\n";
    pipelineRoundTrip();
    std::cout << "PASS rendered disk policies and memory pressure\n";
}
