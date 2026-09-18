#include "aster/cache/cache/encodedmemorycache.h"
#include "aster/cache/cache/filediskcache.h"
#include "aster/cache/cache/renderedmemorycache.h"
#include "aster/cache/pipeline/imagepipeline.h"
#include "aster/cache/source/cachedsourceloader.h"
#include "aster/cache/testing/fakeclock.h"

#ifdef ASTER_TEST_QT_NETWORK
#include "aster/cache/source/qtnetworkservice.h"

#include <QSharedPointer>
#include <QTcpServer>
#include <QTcpSocket>
#endif

#include <QBuffer>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDirIterator>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>

#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace aster::cache;
using namespace aster::cache::testing;

#define VERIFY(condition)                                                                                                                                      \
    do {                                                                                                                                                       \
        if (!(condition))                                                                                                                                      \
            throw std::runtime_error(std::string(__FILE__) + ":" + std::to_string(__LINE__) + " " #condition);                                                 \
    } while (false)

namespace {
QByteArray digest(int n = 0) {
    return QCryptographicHash::hash(QByteArray::number(n), QCryptographicHash::Sha256);
}

class ScriptedNetwork final : public INetworkService {
public:
    int calls = 0;
    NetworkResponse response{200, {{"cache-control", "max-age=10"}, {"etag", "v1"}, {"content-type", "image/test"}}, "red"};
    NetworkFetchOptions last;
    bool fail = false;

    Result<NetworkResponse> fetch(const QUrl&, const NetworkFetchOptions& options, const std::atomic<bool>&) override {
        ++calls;
        last = options;
        return fail ? Result<NetworkResponse>::failure(ImageError::IoError) : Result<NetworkResponse>::success(response);
    }
};

ImageSource remote() {
    ImageSource source;
    source.kind = ImageSource::Kind::Network;
    source.url = QUrl("https://example.test/image?token=secret");
    return source;
}

void encodedTests() {
    auto clock = QSharedPointer<FakeClock>::create();
    SourcePayload p;
    p.bytes = "abcd";
    const auto cost = EncodedMemoryCache::costOf(p);
    EncodedMemoryCache cache(2 * cost, 4, clock);
    VERIFY(cache.put({digest(1)}, p));
    VERIFY(cache.put({digest(2)}, p));
    VERIFY(cache.get({digest(1)}));
    VERIFY(cache.put({digest(3)}, p));
    VERIFY(!cache.get({digest(2)}));
    VERIFY(cache.stats().totalBytes == 2 * cost);
    auto retained = cache.get({digest(1)});
    cache.clear();
    VERIFY(retained.value->bytes == "abcd");
    cache.setMaxCost(cost - 1);
    VERIFY(!cache.put({digest()}, p));
    cache.setMaxCost(cost);
    VERIFY(cache.put({digest()}, p));
    p.bytes = "abcde";
    VERIFY(!cache.put({digest()}, p));
    p.bytes.clear();
    VERIFY(!cache.put({digest()}, p));
    p.bytes = "abcd";
    p.noStore = true;
    VERIFY(!cache.put({digest()}, p));
    p.noStore = false;
    p.expiresAt = clock->now().addMSecs(1);
    VERIFY(cache.put({digest()}, p));
    clock->advance(1);
    VERIFY(cache.get({digest()}, true));
    VERIFY(!cache.get({digest()}));
    cache.setMaxCost(0);
    VERIFY(!cache.put({digest()}, p));
}

void diskTests() {
    QTemporaryDir dir;
    VERIFY(dir.isValid());
    auto clock = QSharedPointer<FakeClock>::create(1000);
    DiskEntry entry{"old", {{"type", "test"}}};
    FileDiskCache disk(dir.path(), 1024 * 1024, 1024, clock);
    VERIFY(disk.stats().indexComplete);
    VERIFY(disk.put(digest(), entry));
    VERIFY(disk.get(digest()).value->bytes == "old");
    const auto cost = disk.stats().totalBytes;
    VERIFY(cost > entry.bytes.size());
    VERIFY(disk.stats().entryCount == 1);
    VERIFY(!disk.put("../outside", entry));
    VERIFY(!disk.put(digest(1), {QByteArray(1025, 'x'), {}}));

    for (auto stage : {FileDiskCache::WriteStage::TenPercent, FileDiskCache::WriteStage::Half, FileDiskCache::WriteStage::BeforeCommit}) {
        FileDiskCache faulty(dir.path(), 1024 * 1024, 1024, clock, [stage](auto reached) { return reached != stage; });
        VERIFY(!faulty.put(digest(), {"new", {}}));
        VERIFY(disk.get(digest()).value->bytes == "old");
    }

    // Terminate a separate process without stack unwinding at each write checkpoint.
    for (int stage = 0; stage < 3; ++stage) {
        QProcess child;
        child.start(QCoreApplication::applicationFilePath(), {"--disk-crash", dir.path(), QString::number(stage)});
        VERIFY(child.waitForFinished(15000));
        VERIFY(child.exitCode() == 73);
        FileDiskCache reopened(dir.path(), 1024 * 1024, 1024, clock);
        VERIFY(reopened.get(digest()).value->bytes == "old");
        VERIFY(reopened.recover());
    }

    FileDiskCache restarted(dir.path(), 1024 * 1024, 1024, clock);
    VERIFY(restarted.stats().indexComplete);
    VERIFY(restarted.stats().totalBytes == cost);
    VERIFY(restarted.get(digest()));
    VERIFY(restarted.recover());
    VERIFY(restarted.stats().totalBytes == cost);
    const auto hex = QString::fromLatin1(digest().toHex());
    const auto path = dir.filePath("aster-cache-v1/" + hex.left(2) + "/" + hex + ".entry");
    {
        QFile file(path);
        VERIFY(file.open(QIODevice::ReadWrite));
        VERIFY(file.seek(file.size() - 1));
        VERIFY(file.write("!") == 1);
    }
    VERIFY(!restarted.get(digest()));
    VERIFY(restarted.stats().corruptions == 1);
    VERIFY(!QFileInfo::exists(path));

    VERIFY(disk.clear());
    std::vector<std::thread> threads;
    std::atomic<bool> good{true};
    for (int i = 0; i < 8; ++i)
        threads.emplace_back([&, i] {
            if (!disk.put(digest(), {QByteArray(100, char('a' + i)), {}}))
                good = false;
        });
    for (auto& t : threads)
        t.join();
    VERIFY(good);
    auto result = disk.get(digest());
    VERIFY(result);
    VERIFY(result.value->bytes == QByteArray(100, result.value->bytes[0]));
    std::atomic<bool> stop{true};
    VERIFY(!disk.clear(&stop));
    stop = false;
    VERIFY(disk.clear(&stop));
    VERIFY(disk.stats().totalBytes == 0);

    for (int i = 0; i < 3; ++i) {
        clock->advance(1);
        VERIFY(disk.put(digest(i), entry));
    }
    clock->advance(1);
    VERIFY(disk.get(digest(0)));
    VERIFY(disk.flushAccessTimes());
    FileDiskCache accessRecovered(dir.path(), 1024 * 1024, 1024, clock);
    VERIFY(accessRecovered.trim(2 * cost));
    VERIFY(!accessRecovered.get(digest(1)));
    VERIFY(accessRecovered.get(digest(0)));
    VERIFY(disk.recover());
    VERIFY(disk.trim(2 * cost));
    VERIFY(!disk.get(digest(1)));
    VERIFY(disk.get(digest(0)));
    disk.setMaxCost(0);
    VERIFY(!disk.put(digest(), entry));

    const auto blocked = dir.filePath("not-a-directory");
    {
        QFile file(blocked);
        VERIFY(file.open(QIODevice::WriteOnly));
    }
    bool rejected = false;
    try {
        FileDiskCache unavailable(blocked, 1024, 1024);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    VERIFY(rejected);
}

void sourceTests() {
    QTemporaryDir dir;
    VERIFY(dir.isValid());
    auto clock = QSharedPointer<FakeClock>::create(100000);
    auto encoded = QSharedPointer<EncodedMemoryCache>::create(65536, 4096, clock);
    auto disk = QSharedPointer<FileDiskCache>::create(dir.path(), 65536, 4096, clock);
    auto net = QSharedPointer<ScriptedNetwork>::create();
    CachedSourceLoader loader(encoded, disk, net, clock);
    auto source = remote();
    const auto key = *loader.key(source).value;
    SourceLoadOptions options;
    std::atomic<bool> cancelled{false};
    auto load = [&] { return loader.load(source, key, options, cancelled); };

    VERIFY(load().source == CacheResultSource::Network);
    VERIFY(load().source == CacheResultSource::EncodedMemory);
    VERIFY(net->calls == 1);
    encoded->clear();
    VERIFY(load().source == CacheResultSource::RawDisk);
    VERIFY(net->calls == 1);
    clock->advance(10000);
    net->response.status = 304;
    net->response.body.clear();
    auto validated = load();
    VERIFY(validated);
    VERIFY(validated.source == CacheResultSource::Validated);
    VERIFY(validated.value->bytes == "red");
    VERIFY(net->last.headers.value("if-none-match") == "v1");
    clock->advance(10000);
    net->fail = true;
    VERIFY(!load());
    options.cache.allowStale = true;
    VERIFY(load());
    options.cache.allowStale = false;
    net->fail = false;

    options.cache.read = CacheReadPolicy::BypassMemory;
    net->response = {200, {{"cache-control", "no-store"}}, "secret"};
    VERIFY(load().value->noStore);
    VERIFY(net->last.reload);
    VERIFY(!disk->contains(key.digest));
    VERIFY(!encoded->get(key));
    const auto calls = net->calls;
    options.cache.read = CacheReadPolicy::CacheOnly;
    VERIFY(!load());
    VERIFY(net->calls == calls);

    auto other = source;
    other.headers.insert("Authorization", "Bearer another");
    VERIFY(!(key == *loader.key(other).value));
    other = source;
    other.context.tenant = "other";
    VERIFY(!(key == *loader.key(other).value));
    other = source;
    other.headers.insert("Accept-Language", "en");
    VERIFY(!(key == *loader.key(other).value));

    options.cache = {};
    net->response = {200, {{"cache-control", "private, max-age=1, must-revalidate"}}, "private"};
    VERIFY(load());
    clock->advance(1000);
    net->fail = true;
    options.cache.allowStale = true;
    VERIFY(!load());
    net->fail = false;
    options.cache = {};
    net->response = {200, {{"cache-control", "max-age=10"}, {"vary", "*"}}, "variant"};
    VERIFY(load().value->noStore);
    VERIFY(!disk->contains(key.digest));
    net->response = {200, {{"cache-control", "max-age=10"}}, QByteArray(10, 'a')};
    options.maxBytes = 9;
    VERIFY(!load());
    options.maxBytes = 32;
    options.cache.write = CacheWritePolicy::NoStore;
    VERIFY(load());
    VERIFY(!disk->contains(key.digest));
    options.cache = {};
    cancelled = true;
    VERIFY(load().error == ImageError::Cancelled);
    cancelled = false;

    ImageSource data;
    data.data = "data";
    auto dataKey = *loader.key(data).value;
    VERIFY(loader.load(data, dataKey, options, cancelled).source == CacheResultSource::Data);
    data.data.clear();
    dataKey = *loader.key(data).value;
    VERIFY(!loader.load(data, dataKey, options, cancelled));
    ImageSource local;
    local.kind = ImageSource::Kind::Local;
    local.url = QUrl::fromLocalFile(dir.filePath("local.bin"));
    {
        QFile file(local.url.toLocalFile());
        VERIFY(file.open(QIODevice::WriteOnly));
        file.write("local");
    }
    auto localKey = *loader.key(local).value;
    VERIFY(loader.load(local, localKey, options, cancelled).source == CacheResultSource::LocalFile);
    VERIFY(!encoded->get(localKey));
    VERIFY(!disk->contains(localKey.digest));
    {
        QFile file(local.url.toLocalFile());
        VERIFY(file.open(QIODevice::WriteOnly));
        file.write("changed");
    }
    VERIFY(loader.load(local, localKey, options, cancelled).error == ImageError::SourceChanged);

    SourceCacheConfig disabled;
    disabled.encodedNetwork = false;
    CachedSourceLoader noEncoded(encoded, {}, net, clock, disabled);
    net->response = {200, {{"cache-control", "max-age=100"}}, "bytes"};
    const auto before = net->calls;
    VERIFY(noEncoded.load(source, key, options, cancelled));
    VERIFY(noEncoded.load(source, key, options, cancelled));
    VERIFY(net->calls == before + 2);

    SourceCacheConfig mimeConfig;
    mimeConfig.excludedMimeTypes = {"image/test"};
    CachedSourceLoader noMime(encoded, {}, net, clock, mimeConfig);
    encoded->clear();
    net->response.headers["content-type"] = "image/test";
    VERIFY(noMime.load(source, key, options, cancelled));
    VERIFY(!encoded->get(key));

    options.cache.read = CacheReadPolicy::NoCache;
    net->response.status = 404;
    VERIFY(!load());
    net->response.status = 500;
    VERIFY(!load());
    net->response.status = 304;
    VERIFY(load().error == ImageError::CorruptedEntry);
    net->response = {200, {{"cache-control", "max-age=1"}, {"last-modified", "Wed, 09 Sep 2026 00:00:00 GMT"}}, "local-time"};
    VERIFY(load());
    clock->advance(1000);
    options.cache.read = CacheReadPolicy::Default;
    net->response.status = 304;
    net->response.body.clear();
    VERIFY(load().source == CacheResultSource::Validated);
    VERIFY(net->last.headers.contains("if-modified-since"));

    other = remote();
    other.headers["cache-control"] = "no-store";
    const auto privateKey = *loader.key(other).value;
    net->response = {200, {{"cache-control", "max-age=100"}}, "request-private"};
    auto privateResult = loader.load(other, privateKey, {}, cancelled);
    VERIFY(privateResult && privateResult.value->noStore);
    VERIFY(!disk->contains(privateKey.digest));
    VERIFY(!encoded->get(privateKey));
}

ImageResult waitRequest(ImagePipeline& pipeline, const SourceRequest& request) {
    auto promise = QSharedPointer<std::promise<ImageResult>>::create();
    auto future = promise->get_future();
    auto subscription = pipeline.request(request, [promise](auto result) { promise->set_value(std::move(result)); });
    VERIFY(future.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
    return future.get();
}

void secondPipelineTests() {
    auto clock = QSharedPointer<FakeClock>::create(100000);
    auto encoded = QSharedPointer<EncodedMemoryCache>::create(65536, 4096, clock);
    auto net = QSharedPointer<ScriptedNetwork>::create();
    auto loader = QSharedPointer<CachedSourceLoader>::create(encoded, nullptr, net, clock);
    auto rendered = QSharedPointer<RenderedMemoryCache>::create(65536, clock);
    std::atomic<int> renders{0};
    ImagePipeline pipeline(rendered, loader, [&](const QByteArray& data, const RenderOptions& o, const auto&) {
        ++renders;
        QImage image(o.physicalTargetSize, QImage::Format_ARGB32);
        image.fill(data == "red" ? Qt::red : Qt::blue);
        return ImageResult::success(image);
    });
    SourceRequest request;
    request.source = remote();
    request.render.physicalTargetSize = QSize(8, 8);
    VERIFY(waitRequest(pipeline, request));
    request.render.physicalTargetSize = QSize(16, 16);
    auto second = waitRequest(pipeline, request);
    VERIFY(second);
    VERIFY(second.source == CacheResultSource::EncodedMemory);
    VERIFY(net->calls == 1);
    VERIFY(renders == 2);
    VERIFY(waitRequest(pipeline, request).source == CacheResultSource::RenderedMemory);

    request.load.cache.read = CacheReadPolicy::BypassMemory;
    net->response.body = "blue";
    VERIFY(waitRequest(pipeline, request).value->pixelColor(0, 0) == QColor(Qt::blue));
    request.load.cache.read = CacheReadPolicy::Default;
    request.render.physicalTargetSize = QSize(8, 8);
    VERIFY(waitRequest(pipeline, request).value->pixelColor(0, 0) == QColor(Qt::blue));

    request.load.cache.read = CacheReadPolicy::NoCache;
    request.load.cache.write = CacheWritePolicy::NoStore;
    net->response.headers["cache-control"] = "no-store";
    VERIFY(waitRequest(pipeline, request));
    VERIFY(encoded->stats().entryCount == 0);
    rendered->clear();
    request.load.cache.read = CacheReadPolicy::CacheOnly;
    const auto before = net->calls;
    VERIFY(waitRequest(pipeline, request).error == ImageError::CacheMiss);
    VERIFY(net->calls == before);
    VERIFY(pipeline.stats().sourceInFlight == 0);
    VERIFY(pipeline.stats().renderInFlight == 0);
}

class BlockingLoader final : public IImageSourceLoader {
public:
    std::atomic<int> calls{0};
    std::atomic<int> cancellations{0};
    std::atomic<bool> release{false};

    Result<SourceKey> key(const ImageSource&) const override {
        return Result<SourceKey>::success({digest(555)});
    }

    Result<SourcePayload> load(const ImageSource&, const SourceKey&, const SourceLoadOptions&, const std::atomic<bool>& cancelled) override {
        ++calls;
        while (!release.load()) {
            if (cancelled.load()) {
                ++cancellations;
                return Result<SourcePayload>::failure(ImageError::Cancelled);
            }
            std::this_thread::yield();
        }
        SourcePayload value;
        value.bytes = "bytes";
        return Result<SourcePayload>::success(std::move(value));
    }
};

void decodeBudgetTests() {
    constexpr qint64 encodedBudget = 65536;
    constexpr qint64 renderedBudget = 256 * 1024;
    auto encoded = QSharedPointer<EncodedMemoryCache>::create(encodedBudget, 32768);
    auto rendered = QSharedPointer<RenderedMemoryCache>::create(renderedBudget);
    SourceCacheConfig config;
    config.encodedData = true;
    auto loader = QSharedPointer<CachedSourceLoader>::create(encoded, nullptr, nullptr, QSharedPointer<SystemClock>::create(), config);
    ImagePipeline pipeline(rendered, loader,
                           [](const QByteArray& bytes, const RenderOptions&, const auto&) { return ImageResult::success(QImage::fromData(bytes)); });
    SourceRequest request;
    request.source.contentType = "image/png";
    request.render.physicalTargetSize = QSize(128, 128);
    QImage pixels(128, 128, QImage::Format_ARGB32);
    pixels.fill(Qt::red);
    QBuffer buffer(&request.source.data);
    VERIFY(buffer.open(QIODevice::WriteOnly));
    VERIFY(pixels.save(&buffer, "PNG"));
    buffer.close();
    auto image = waitRequest(pipeline, request);
    VERIFY(image);
    VERIFY(encoded->stats().totalBytes <= encodedBudget);
    VERIFY(rendered->stats().totalBytes <= renderedBudget);
    encoded->clear();
    VERIFY(rendered->stats().entryCount == 1);
    VERIFY(image.value->pixelColor(0, 0) == QColor(Qt::red));
    VERIFY(waitRequest(pipeline, request));
    rendered->clear();
    VERIFY(encoded->stats().entryCount == 1);
    request.source.data = "invalid image";
    VERIFY(waitRequest(pipeline, request).error == ImageError::ProcessingError);
    request.source.data = QByteArray(32769, 'x');
    request.load.maxBytes = 32768;
    VERIFY(waitRequest(pipeline, request).error == ImageError::InvalidRequest);
}

void workerLifecycleTests() {
    auto loader = QSharedPointer<BlockingLoader>::create();
    auto cache = QSharedPointer<RenderedMemoryCache>::create(65536);
    auto renderer = [](const QByteArray&, const RenderOptions& o, const auto&) {
        QImage image(o.physicalTargetSize, QImage::Format_ARGB32);
        image.fill(Qt::red);
        return ImageResult::success(image);
    };
    auto pipeline = std::make_unique<ImagePipeline>(cache, loader, renderer);
    SourceRequest request;
    request.render.physicalTargetSize = QSize(8, 8);
    std::atomic<int> success{0}, cancelled{0};
    auto done = [&](auto result) {
        if (result)
            ++success;
        else if (result.error == ImageError::Cancelled)
            ++cancelled;
    };
    std::vector<Subscription> subs;
    for (int i = 0; i < 100; ++i)
        subs.push_back(pipeline->request(request, done));
    for (int i = 0; i < 99; ++i)
        subs[i].cancel();
    loader->release = true;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (success == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    VERIFY(success == 1);
    VERIFY(cancelled == 99);
    VERIFY(loader->calls == 1);
    subs.clear();

    loader->release = false;
    auto pending = pipeline->request(request, done);
    const auto started = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (loader->calls < 2 && std::chrono::steady_clock::now() < started)
        std::this_thread::yield();
    VERIFY(loader->calls == 2);
    pending.cancel();
    pipeline.reset();
    const auto stopped = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (loader->cancellations == 0 && std::chrono::steady_clock::now() < stopped)
        std::this_thread::yield();
    VERIFY(loader->cancellations == 1);

    loader->release = true;
    auto workerOwned = QSharedPointer<std::unique_ptr<ImagePipeline>>::create(std::make_unique<ImagePipeline>(cache, loader, renderer));
    auto completed = QSharedPointer<std::promise<void>>::create();
    auto completion = completed->get_future();
    auto handle = (*workerOwned)->request(request, [workerOwned, completed](auto) {
        workerOwned->reset();
        completed->set_value();
    });
    VERIFY(completion.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
}

#ifdef ASTER_TEST_QT_NETWORK
void httpServerTest() {
    std::promise<quint16> ready;
    std::atomic<bool> conditional{false};
    std::atomic<int> bodyBytes{0};
    std::thread server([&] {
        QTcpServer listener;
        if (!listener.listen(QHostAddress::LocalHost)) {
            ready.set_value(0);
            return;
        }
        ready.set_value(listener.serverPort());
        for (int i = 0; i < 3; ++i) {
            if (!listener.waitForNewConnection(10000))
                return;
            std::unique_ptr<QTcpSocket> socket(listener.nextPendingConnection());
            QByteArray request;
            while (!request.contains("\r\n\r\n")) {
                if (!socket->waitForReadyRead(10000))
                    return;
                request += socket->readAll();
            }
            QByteArray response;
            if (i == 0) {
                response = "HTTP/1.1 200 OK\r\nContent-Length: 3\r\nETag: v1\r\nCache-Control: "
                           "max-age=1\r\nConnection: close\r\n\r\nred";
                bodyBytes += 3;
            } else if (i == 1) {
                conditional = request.toLower().contains("if-none-match: v1");
                response = "HTTP/1.1 304 Not Modified\r\nContent-Length: 0\r\nCache-Control: "
                           "max-age=10\r\nConnection: close\r\n\r\n";
            } else {
                response = "HTTP/1.1 200 OK\r\nContent-Length: 100\r\nConnection: close\r\n\r\ncut";
            }
            socket->write(response);
            socket->waitForBytesWritten(10000);
            socket->disconnectFromHost();
        }
    });

    struct Join {
        std::thread& thread;

        ~Join() {
            thread.join();
        }
    } join{server};

    const auto port = ready.get_future().get();
    VERIFY(port != 0);
    QTemporaryDir dir;
    auto clock = QSharedPointer<FakeClock>::create(1000);
    auto disk = QSharedPointer<FileDiskCache>::create(dir.path(), 65536, 4096, clock);
    auto service = QSharedPointer<QtNetworkService>::create(5000);
    CachedSourceLoader loader({}, disk, service, clock);
    auto source = remote();
    source.url = QUrl(QString("http://127.0.0.1:%1/image").arg(port));
    const auto key = *loader.key(source).value;
    std::atomic<bool> cancelled{false};
    auto first = loader.load(source, key, {}, cancelled);
    VERIFY(first);
    VERIFY(first.value->bytes == "red");
    clock->advance(1000);
    auto second = loader.load(source, key, {}, cancelled);
    VERIFY(second);
    VERIFY(second.source == CacheResultSource::Validated);
    VERIFY(second.value->bytes == "red");
    VERIFY(conditional);
    VERIFY(bodyBytes == 3);
    VERIFY(!service->fetch(source.url, {}, cancelled));
}
#endif
} // namespace

int diskCrashProbe(const QStringList& arguments) {
    if (!arguments.contains("--disk-crash"))
        return -1;
    if (arguments.size() != 4)
        return 2;
    const int stage = arguments[3].toInt();
    FileDiskCache disk(arguments[2], 1024 * 1024, 1024, QSharedPointer<SystemClock>::create(), [stage](auto reached) {
        if (int(reached) == stage)
            std::_Exit(73);
        return true;
    });
    disk.put(digest(), {"interrupted", {}});
    return 3;
}

void secondRoundTests() {
    encodedTests();
    std::cout << "PASS encoded cache\n";
    diskTests();
    std::cout << "PASS disk cache and process interruption\n";
    sourceTests();
    std::cout << "PASS local/data/HTTP cache policies\n";
    secondPipelineTests();
    std::cout << "PASS encoded pipeline integration\n";
    workerLifecycleTests();
    std::cout << "PASS worker cancellation and destruction\n";
    decodeBudgetTests();
    std::cout << "PASS encoded/rendered budgets and invalid image\n";
#ifdef ASTER_TEST_QT_NETWORK
    httpServerTest();
    std::cout << "PASS local HTTP 200/304 integration\n";
#endif
}
