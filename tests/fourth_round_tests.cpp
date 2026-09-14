#include "aster/cache/cache/filediskcache.h"
#include "aster/cache/cache/rendereddiskcache.h"
#include "aster/cache/cache/renderedmemorycache.h"
#include "aster/cache/decoder/boundedimagedecoder.h"
#include "aster/cache/pipeline/imagepipeline.h"
#include "aster/cache/source/cachedsourceloader.h"
#include "aster/cache/testing/fakeclock.h"

#include <QBuffer>
#include <QDirIterator>
#include <QFile>
#include <QSharedPointer>
#include <QTemporaryDir>
#include <QtEndian>

#include <future>
#include <iostream>
#include <random>
#include <stdexcept>
#include <thread>

using namespace aster::cache;

#define ENSURE(condition)                                                                                                                                      \
    do {                                                                                                                                                       \
        if (!(condition))                                                                                                                                      \
            throw std::runtime_error(std::string(__FILE__) + ":" + std::to_string(__LINE__) + " " #condition);                                                 \
    } while (false)

namespace {
QByteArray png(QSize size = QSize(16, 16)) {
    QImage image(size, QImage::Format_ARGB32);
    image.fill(QColor(16, 32, 64, 128));
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    ENSURE(image.save(&buffer, "PNG"));
    return bytes;
}

RenderKey key(const QByteArray& nameSpace, int variant = 0) {
    KeyContext context;
    context.nameSpace = nameSpace;
    auto source = KeyBuilder().network(QUrl("https://test/image?token=private-secret"), context);
    RenderOptions options;
    options.physicalTargetSize = QSize(16 + variant, 16);
    return *KeyBuilder().render(*source.value, options).value;
}

void exactStatsAndDump() {
    auto clock = QSharedPointer<testing::FakeClock>::create();
    QImage image(16, 16, QImage::Format_ARGB32);
    image.fill(Qt::red);
    const auto cost = RenderedMemoryCache::costOf(image);
    RenderedMemoryCache cache(cost * 2, clock);
    const auto a = key("one"), b = key("one", 1), c = key("two");

    ENSURE(!cache.get(a));
    ENSURE(cache.put(a, image));
    ENSURE(cache.put(b, image));
    ENSURE(cache.get(a));
    ENSURE(cache.put(c, image));
    ENSURE(!cache.get(b));
    ENSURE(cache.get(a));
    ENSURE(cache.get(c));

    const auto stats = cache.stats();
    ENSURE(stats.hits == 3 && stats.misses == 2 && stats.evictions == 1);
    ENSURE(stats.entryCount == 2 && stats.totalBytes == cost * 2);
    ENSURE(stats.lookup.samples == 5 && stats.lookup.totalNs >= stats.lookup.maxNs);

    clock->advance(123);
    const auto dump = cache.debugDump(1);
    ENSURE(dump.size() == 1 && dump[0].ageMs == 123 && dump[0].bytes == cost);
    ENSURE(dump[0].keyDigest.size() == 32 && dump[0].namespaceDigest.size() == 32);
    ENSURE(!dump[0].keyDigest.toHex().contains("private-secret"));
    ENSURE(cache.debugDump(0).isEmpty());
    ENSURE(cache.stats().lookup.samples == 5);
    ENSURE(!cache.invalidate({}));
    ENSURE(cache.invalidate({{}, KeyBuilder::namespaceDigest("one")}));
    ENSURE(!cache.get(a) && cache.get(c));
}

void boundedDecode() {
    const auto bytes = png();
    const std::atomic<bool> running{false}, cancelled{true};
    BoundedImageDecoder decoder;
    auto result = decoder.decode(bytes, running);
    ENSURE(result && result.value->size() == QSize(16, 16));
    ENSURE(result.value->pixelColor(0, 0) == QColor(16, 32, 64, 128));
    ENSURE(decoder.decode(bytes, cancelled).error == ImageError::Cancelled);
    ENSURE(decoder.decode(bytes, running, QSize(17, 16)).error == ImageError::CorruptedEntry);
    ENSURE(!decoder.decode({}, running));
    ENSURE(!decoder.decode(bytes.left(20), running));

    // Default decoding accepts images above the former 16 Mi-pixel / 256 MiB preflight limits.
    const auto large = decoder.decode(png(QSize(4097, 4097)), running);
    ENSURE(large && large.value->size() == QSize(4097, 4097));

    DecodeLimits limits;
    limits.maxEncodedBytes = bytes.size() - 1;
    ENSURE(BoundedImageDecoder(limits).decode(bytes, running).error == ImageError::ResourceLimit);
    limits = {};
    limits.maxSide = 15;
    ENSURE(BoundedImageDecoder(limits).decode(bytes, running).error == ImageError::ResourceLimit);
    limits = {};
    limits.maxPixels = 255;
    ENSURE(BoundedImageDecoder(limits).decode(bytes, running).error == ImageError::ResourceLimit);
    limits = {};
    limits.maxDecodedBytes = 256 * 16 - 1;
    ENSURE(BoundedImageDecoder(limits).decode(bytes, running).error == ImageError::ResourceLimit);
    limits.maxDecodedBytes = 256 * 16;
    ENSURE(BoundedImageDecoder(limits).decode(bytes, running));
    limits = {};
    limits.allowedFormats = {"jpeg"};
    ENSURE(BoundedImageDecoder(limits).decode(bytes, running).error == ImageError::UnsupportedFormat);

    // Keep the PNG IHDR checksum valid so preflight sees the malicious dimensions.
    QByteArray header = bytes;
    qToBigEndian<quint32>(100000, reinterpret_cast<uchar*>(header.data() + 16));
    qToBigEndian<quint32>(100000, reinterpret_cast<uchar*>(header.data() + 20));
    quint32 crc = 0xffffffff;
    for (int i = 12; i < 29; ++i) {
        crc ^= quint8(header[i]);
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0u);
    }
    qToBigEndian<quint32>(crc ^ 0xffffffff, reinterpret_cast<uchar*>(header.data() + 29));
    ENSURE(decoder.decode(header, running).error == ImageError::ResourceLimit);

    const auto compressed = png(QSize(1024, 1024));
    ENSURE(compressed.size() < 100000);
    limits = {};
    limits.maxPixels = 65536;
    ENSURE(BoundedImageDecoder(limits).decode(compressed, running).error == ImageError::ResourceLimit);

    std::mt19937 random(912);
    limits = {};
    limits.maxSide = 64;
    limits.maxPixels = 4096;
    limits.maxDecodedBytes = 65536;
    BoundedImageDecoder fuzz(limits);
    for (int i = 0; i < 500; ++i) {
        QByteArray invalid(1 + int(random() % 256), '\0');
        for (auto& byte : invalid)
            byte = char(random() & 255);
        const auto decoded = fuzz.decode(invalid, running);
        ENSURE(!decoded || (decoded.value->width() <= 64 && decoded.value->height() <= 64));
    }
}

class Network final : public INetworkService {
public:
    QByteArray bytes = png();
    std::atomic<int> calls{0};
    std::atomic<bool> validate{false};
    std::atomic<bool> noStore{false};

    Result<NetworkResponse> fetch(const QUrl&, const NetworkFetchOptions&, const std::atomic<bool>&) override {
        ++calls;
        return Result<NetworkResponse>::success(
                {validate ? 304 : 200, {{"cache-control", noStore ? "no-store" : "max-age=60"}, {"etag", "v1"}}, validate ? QByteArray{} : bytes});
    }
};

class ThrowingDisk final : public IDiskCache {
public:
    Result<DiskEntry> get(const QByteArray&) override {
        throw std::runtime_error("injected read failure");
    }

    bool put(const QByteArray&, const DiskEntry&) override {
        throw std::runtime_error("injected write failure");
    }

    bool remove(const QByteArray&) override {
        throw std::runtime_error("injected remove failure");
    }

    bool contains(const QByteArray&) override {
        return false;
    }

    bool clear(const std::atomic<bool>*) override {
        return false;
    }

    bool trim(qint64, const std::atomic<bool>*) override {
        return false;
    }

    DiskStats stats() const override {
        return {};
    }
};

void diskExceptionFallback() {
    auto network = QSharedPointer<Network>::create();
    CachedSourceLoader loader(nullptr, QSharedPointer<ThrowingDisk>::create(), network);
    ImageSource source;
    source.kind = ImageSource::Kind::Network;
    source.url = QUrl("https://test/failure");
    const auto identity = *loader.key(source).value;
    const std::atomic<bool> running{false};
    auto result = loader.load(source, identity, {}, running);
    ENSURE(result && result.source == CacheResultSource::Network);
    ENSURE(result.value->bytes == network->bytes);
    ENSURE(loader.cacheStats().rawDisk.ioErrors == 2);
    SourceLoadOptions options;
    options.cache.read = CacheReadPolicy::CacheOnly;
    ENSURE(loader.load(source, identity, options, running).error == ImageError::CacheMiss);
    ENSURE(network->calls == 1 && loader.cacheStats().rawDisk.ioErrors == 3);
    network->noStore = true;
    result = loader.load(source, identity, {}, running);
    ENSURE(result && result.value->noStore);
    ENSURE(network->calls == 2 && loader.cacheStats().rawDisk.ioErrors == 5);
}

ImageResult request(ImagePipeline& pipeline, const SourceRequest& input) {
    auto promise = QSharedPointer<std::promise<ImageResult>>::create();
    auto future = promise->get_future();
    auto subscription = pipeline.request(input, [promise](ImageResult result) { promise->set_value(std::move(result)); });
    ENSURE(future.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
    auto result = future.get();
    ENSURE(pipeline.waitForIdle());
    return result;
}

void layeredMaintenance() {
    QTemporaryDir dir;
    auto clock = QSharedPointer<testing::FakeClock>::create();
    auto raw = QSharedPointer<FileDiskCache>::create(dir.filePath("raw"), 1024 * 1024, 65536, clock);
    auto backing = QSharedPointer<FileDiskCache>::create(dir.filePath("rendered"), 1024 * 1024, 65536, clock);
    auto disk = QSharedPointer<RenderedDiskCache>::create(backing);
    auto encoded = QSharedPointer<EncodedMemoryCache>::create(65536, 65536, clock);
    auto memory = QSharedPointer<RenderedMemoryCache>::create(65536, clock);
    auto active = QSharedPointer<ActiveResourceStore>::create(65536, clock);
    auto network = QSharedPointer<Network>::create();
    auto loader = QSharedPointer<CachedSourceLoader>::create(encoded, raw, network, clock);
    std::atomic<int> renders{0};
    std::mutex eventMutex;
    std::vector<PipelineEvent> events;
    ImagePipeline pipeline(
            memory, loader,
            [&](const auto& bytes, const auto&, const auto& cancelled) {
                ++renders;
                return BoundedImageDecoder().decode(bytes, cancelled);
            },
            2,
            [&](const PipelineEvent& event) {
                std::lock_guard<std::mutex> lock(eventMutex);
                events.push_back(event);
            },
            {disk, active});

    SourceRequest input;
    input.source.kind = ImageSource::Kind::Network;
    input.source.url = QUrl("https://test/image?token=private-secret");
    input.source.context.nameSpace = "one";
    input.render.physicalTargetSize = QSize(16, 16);
    input.diskStrategy = DiskCacheStrategy::All;
    const auto source = *loader->key(input.source).value;

    auto first = request(pipeline, input);
    ENSURE(first && first.source == CacheResultSource::Network);
    ENSURE(request(pipeline, input).source == CacheResultSource::ActiveResource);
    ENSURE(network->calls == 1 && renders == 1);
    auto stats = pipeline.cacheStats();
    ENSURE(stats.network.requests == 1 && stats.network.receivedBytes == quint64(network->bytes.size()));
    ENSURE(stats.encodedMemory.hits == 1 && stats.encodedMemory.misses == 1);
    ENSURE(stats.active.hits == 1 && stats.rawDisk.misses == 1);

    ENSURE(pipeline.removeRenderVariants(source));
    ENSURE(first.handle && active->stats().entries == 0);
    ENSURE(request(pipeline, input));
    ENSURE(renders == 2 && network->calls == 1);

    ENSURE(pipeline.removeSource(source));
    ENSURE(request(pipeline, input).source == CacheResultSource::Network);
    ENSURE(network->calls == 2 && renders == 3);

    clock->advance(61000);
    network->validate = true;
    ENSURE(request(pipeline, input));
    stats = pipeline.cacheStats();
    ENSURE(stats.network.validations == 1 && stats.network.requests == 3);
    ENSURE(stats.network.latency.samples == 3);
    network->validate = false;

    input.source.context.nameSpace = "two";
    ENSURE(request(pipeline, input));
    const auto otherSource = *loader->key(input.source).value;
    ENSURE(pipeline.clearNamespace("one"));
    ENSURE(!encoded->get(source));
    ENSURE(encoded->get(otherSource));
    const auto before = renders.load();
    input.source.context.nameSpace = "one";
    ENSURE(request(pipeline, input));
    ENSURE(renders == before + 1);
    ENSURE(!pipeline.removeSource({}));

    for (const auto& entry : pipeline.debugDump())
        ENSURE(entry.keyDigest.size() == 32 && entry.namespaceDigest.size() == 32);
    {
        std::lock_guard<std::mutex> lock(eventMutex);
        bool sourceEvent = false, completed = false;
        for (const auto& event : events) {
            ENSURE(event.keyDigest.size() == 32);
            sourceEvent |= event.kind == PipelineEvent::Kind::SourceCompleted && event.source == CacheResultSource::Network;
            completed |= event.kind == PipelineEvent::Kind::Completed && event.source == CacheResultSource::ActiveResource;
        }
        ENSURE(sourceEvent && completed);
    }

    QDirIterator files(dir.path(), QDir::Files, QDirIterator::Subdirectories);
    while (files.hasNext()) {
        files.next();
        ENSURE(!files.fileName().contains("private-secret"));
        ENSURE(files.fileName().endsWith(".entry") && files.fileName().size() == 70);
    }

    FileDiskCache reopened(dir.filePath("rendered"), 1024 * 1024, 65536, clock);
    ENSURE(reopened.invalidate({{}, KeyBuilder::namespaceDigest("one")}));
    ENSURE(reopened.stats().entryCount == 1);
    ENSURE(reopened.stats().hits == 0 && reopened.stats().misses == 0);

    FileDiskCache upgraded(dir.filePath("rendered"), 1024 * 1024, 65536, clock, {}, "v2");
    ENSURE(upgraded.recover());
    ENSURE(upgraded.stats().entryCount == 0);
    bool invalidVersion = false;
    try {
        FileDiskCache invalid(dir.path(), 65536, 32768, clock, {}, "../escape");
    } catch (const std::invalid_argument&) {
        invalidVersion = true;
    }
    ENSURE(invalidVersion);
}

void maintenanceRaceAndCorruption() {
    RenderedMemoryCache memory(32768);
    QImage image(16, 16, QImage::Format_ARGB32);
    image.fill(Qt::green);
    const auto render = key("race");
    std::vector<std::thread> threads;
    for (int n = 0; n < 4; ++n)
        threads.emplace_back([&] {
            for (int i = 0; i < 500; ++i) {
                memory.put(render, image);
                memory.get(render);
                memory.invalidate({render.source.digest, {}});
                memory.debugDump(2);
            }
        });
    for (auto& thread : threads)
        thread.join();
    memory.invalidate({render.source.digest, {}});
    ENSURE(memory.stats().entryCount == 0 && memory.stats().totalBytes == 0);

    QTemporaryDir dir;
    auto backing = QSharedPointer<FileDiskCache>::create(dir.path(), 65536, 32768);
    RenderedDiskCache disk(backing);
    ENSURE(disk.put(render, image));
    const auto storage = disk.storageKey(render);
    auto entry = backing->get(storage);
    ENSURE(entry);
    entry.value->metadata.insert("encoderVersion", 99);
    ENSURE(backing->put(storage, *entry.value));
    ENSURE(!disk.get(render));
    ENSURE(disk.stats().hits == 0 && disk.stats().misses == 1 && disk.stats().corruptions == 1);
    ENSURE(!backing->contains(storage));

    ENSURE(disk.put(render, image));
    entry = backing->get(storage);
    entry.value->metadata.insert("schema", 999);
    ENSURE(backing->put(storage, *entry.value));
    ENSURE(!disk.get(render));
    ENSURE(disk.put(render, image));
    ENSURE(disk.get(render));

    DiskEntry raw{"raw",
                  {{"http", 1},
                   {"sourceDigest", QString::fromLatin1(render.source.digest.toHex())},
                   {"namespaceDigest", QString::fromLatin1(render.source.namespaceDigest.toHex())}}};
    ENSURE(backing->put(render.source.digest, raw));
    ENSURE(disk.invalidate({render.source.digest, {}}));
    ENSURE(!disk.get(render));
    ENSURE(backing->get(render.source.digest));

    raw.metadata = {{"http", 1}};
    ENSURE(backing->put(render.source.digest, raw));
    ENSURE(backing->invalidate({{}, KeyBuilder::namespaceDigest("legacy")}));
    ENSURE(!backing->contains(render.source.digest));
}

void drainBoundaries() {
    auto memory = QSharedPointer<RenderedMemoryCache>::create(65536);
    auto loader = QSharedPointer<CachedSourceLoader>::create(nullptr);
    std::promise<void> entered, release;
    auto entering = entered.get_future();
    auto gate = release.get_future().share();
    std::atomic<bool> workerRejected{false};
    ImagePipeline* pointer = nullptr;
    ImagePipeline pipeline(memory, loader, [&](const auto& bytes, const auto&, const auto& cancelled) {
        workerRejected = !pointer->waitForIdle(0);
        entered.set_value();
        gate.wait();
        return BoundedImageDecoder().decode(bytes, cancelled);
    });
    pointer = &pipeline;
    SourceRequest input;
    input.source.data = png();
    input.render.physicalTargetSize = QSize(16, 16);
    auto promise = QSharedPointer<std::promise<ImageResult>>::create();
    auto future = promise->get_future();
    auto subscription = pipeline.request(input, [promise](ImageResult result) { promise->set_value(std::move(result)); });
    const bool started = entering.wait_for(std::chrono::seconds(10)) == std::future_status::ready;
    const bool busy = !pipeline.waitForIdle(0);
    release.set_value();
    ENSURE(started && busy);
    ENSURE(future.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
    ENSURE(future.get());
    ENSURE(workerRejected && pipeline.waitForIdle());
    ENSURE(!pipeline.waitForIdle(-1));
}
} // namespace

void fourthRoundTests() {
    exactStatsAndDump();
    std::cout << "PASS exact cache stats and redacted dump\n";
    boundedDecode();
    std::cout << "PASS bounded decode, malicious dimensions and 500 invalid inputs\n";
    layeredMaintenance();
    std::cout << "PASS layered metrics, source/namespace invalidation and restart\n";
    maintenanceRaceAndCorruption();
    std::cout << "PASS concurrent maintenance and schema corruption recovery\n";
    drainBoundaries();
    std::cout << "PASS drain timeout and worker deadlock prevention\n";
    diskExceptionFallback();
    std::cout << "PASS raw disk exception fallback and cache-only isolation\n";
}
