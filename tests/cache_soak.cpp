#include "aster/cache/cache/filediskcache.h"
#include "aster/cache/cache/renderedmemorycache.h"
#include "aster/cache/decoder/boundedimagedecoder.h"
#include "aster/cache/pipeline/imagepipeline.h"
#include "aster/cache/source/cachedsourceloader.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QDirIterator>
#include <QFile>
#include <QTemporaryDir>

#include <future>
#include <iostream>
#include <random>
#include <stdexcept>
#include <vector>

#ifdef Q_OS_WIN
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <QtCore/qt_windows.h>

#include <psapi.h>
#elif defined(Q_OS_LINUX)
#include <unistd.h>
#endif

using namespace aster::cache;

namespace
{
void require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

qint64 residentBytes()
{
#ifdef Q_OS_WIN
    PROCESS_MEMORY_COUNTERS info{};
    return GetProcessMemoryInfo(GetCurrentProcess(), &info, sizeof(info))
               ? qint64(info.WorkingSetSize)
               : -1;
#elif defined(Q_OS_LINUX)
    QFile file("/proc/self/statm");
    if (!file.open(QIODevice::ReadOnly))
        return -1;
    const auto values = file.readAll().simplified().split(' ');
    return values.size() > 1 ? values[1].toLongLong() * sysconf(_SC_PAGESIZE) : -1;
#else
    return -1;
#endif
}

class Network final : public INetworkService
{
public:
    QByteArray bytes;

    Network()
    {
        QImage image(16, 16, QImage::Format_ARGB32);
        image.fill(Qt::green);
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::WriteOnly);
        require(image.save(&buffer, "PNG"), "PNG encoding failed");
    }

    Result<NetworkResponse> fetch(const QUrl&, const NetworkFetchOptions&,
                                  const std::atomic<bool>& cancelled) override
    {
        if (cancelled.load())
            return Result<NetworkResponse>::failure(ImageError::Cancelled);
        return Result<NetworkResponse>::success({200, {{"cache-control", "max-age=60"}}, bytes});
    }
};

void checkBudget(const CacheStats& stats)
{
    require(stats.totalBytes >= 0 && stats.totalBytes <= stats.maxBytes, "Memory budget exceeded");
}

void checkBudget(const DiskStats& stats)
{
    require(stats.totalBytes >= 0 && stats.totalBytes <= stats.maxBytes, "Disk budget exceeded");
}

int run(int iterations, unsigned seed)
{
    QTemporaryDir directory;
    require(directory.isValid(), "Temporary directory unavailable");
    auto memory = std::make_shared<RenderedMemoryCache>(32768);
    auto encoded = std::make_shared<EncodedMemoryCache>(16384, 8192);
    auto raw = std::make_shared<FileDiskCache>(directory.filePath("raw"), 65536, 8192);
    auto backing = std::make_shared<FileDiskCache>(directory.filePath("rendered"), 65536, 8192);
    auto disk = std::make_shared<RenderedDiskCache>(backing);
    auto active = std::make_shared<ActiveResourceStore>(16384);
    auto loader = std::make_shared<CachedSourceLoader>(encoded, raw, std::make_shared<Network>());
    auto callbacks = std::make_shared<std::atomic<int>>(0);
    ImagePipeline pipeline(memory, loader,
                           [](const auto& bytes, const auto& options, const auto& cancelled)
                           {
                               auto result = BoundedImageDecoder().decode(bytes, cancelled);
                               if (result)
                                   result.value = result.value->scaled(options.physicalTargetSize);
                               return result;
                           },
                           4, {}, {disk, active});

    std::mt19937 random(seed);
    std::vector<ImageHandle> retained;
    qint64 peakRss = 0;
    int success = 0, cancelled = 0, misses = 0;
    std::cout << "completed,success,cancelled,cache_miss,rss_bytes,peak_sampled_rss_bytes,files\n";
    for (int base = 0; base < iterations; base += 32)
    {
        std::vector<std::future<ImageResult>> futures;
        std::vector<Subscription> subscriptions;
        for (int n = base; n < std::min(base + 32, iterations); ++n)
        {
            SourceRequest request;
            request.source.kind = ImageSource::Kind::Network;
            request.source.context.nameSpace = "soak/" + QByteArray::number(random() % 4);
            request.source.url =
                QUrl(QString("https://soak.test/%1?token=hidden").arg(random() % 16));
            request.render.physicalTargetSize = QSize(16 + int(random() % 2) * 16, 16);
            request.diskStrategy = DiskCacheStrategy::All;
            const auto policy = random() % 10;
            if (policy == 0)
                request.load.cache.read = CacheReadPolicy::CacheOnly;
            else if (policy == 1)
                request.load.cache.read = CacheReadPolicy::NoCache;
            if (random() % 10 == 0)
                request.load.cache.write = CacheWritePolicy::NoStore;

            auto promise = std::make_shared<std::promise<ImageResult>>();
            futures.push_back(promise->get_future());
            subscriptions.push_back(pipeline.request(request,
                                                     [promise, callbacks](ImageResult result)
                                                     {
                                                         ++*callbacks;
                                                         promise->set_value(std::move(result));
                                                     }));
            if (random() % 4 == 0)
                subscriptions.back().cancel();
        }

        for (auto& future : futures)
        {
            require(future.wait_for(std::chrono::seconds(30)) == std::future_status::ready,
                    "Request completion timeout");
            auto result = future.get();
            if (result)
            {
                ++success;
                if (random() % 8 == 0)
                    retained.push_back(result.handle);
            }
            else if (result.error == ImageError::Cancelled)
                ++cancelled;
            else if (result.error == ImageError::CacheMiss)
                ++misses;
            else
                require(false, "Unexpected request failure");
        }

        require(pipeline.waitForIdle(), "Pipeline did not drain");
        if (retained.size() > 8)
            retained.clear();

        if (random() % 4 == 0)
            require(pipeline.clearNamespace("soak/" + QByteArray::number(random() % 4)),
                    "Namespace invalidation failed");
        if (random() % 8 == 0)
            pipeline.trimMemory(MemoryPressure::Low);
        if (random() % 16 == 0)
        {
            pipeline.trimMemory(MemoryPressure::Critical);
            active->setMaxCost(16384);
        }

        const auto stats = pipeline.cacheStats();
        checkBudget(stats.renderedMemory);
        checkBudget(stats.encodedMemory);
        checkBudget(stats.rawDisk);
        checkBudget(stats.renderedDisk);
        require(stats.active.bytes >= 0 && stats.active.bytes <= stats.active.maxBytes,
                "Active tracking budget exceeded");
        const auto rss = residentBytes();
        peakRss = std::max(peakRss, rss);
        if (base % 1024 == 0 || base + 32 >= iterations)
        {
            int files = 0;
            QDirIterator entries(directory.path(), QDir::Files, QDirIterator::Subdirectories);
            while (entries.hasNext())
            {
                entries.next();
                ++files;
            }
            std::cout << std::min(base + 32, iterations) << ',' << success << ',' << cancelled
                      << ',' << misses << ',' << rss << ',' << peakRss << ',' << files << '\n';
        }
    }

    retained.clear();
    require(pipeline.waitForIdle(), "Final drain failed");
    require(callbacks->load() == iterations, "Callback count mismatch");
    require(active->stats().entries == 0, "Active handle leak");
    require(raw->clear() && backing->clear(), "Final disk clear failed");
    return 0;
}
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    int iterations = 3000;
    unsigned seed = 20260909;
    const auto args = app.arguments();
    for (int i = 1; i < args.size(); ++i)
    {
        const auto option = args[i];
        if ((option != "--iterations" && option != "--seed") || i + 1 >= args.size())
            return 2;
        bool ok = false;
        const auto value = args[++i].toUInt(&ok);
        if (!ok || (option == "--iterations" && (value == 0 || value > 10000000)))
            return 2;
        if (option == "--iterations")
            iterations = int(value);
        else
            seed = value;
    }
    try
    {
        return run(iterations, seed);
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
