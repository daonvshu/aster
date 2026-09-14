#include "aster/cache/cache/renderedmemorycache.h"
#include "aster/cache/pipeline/imagepipeline.h"
#include "aster/cache/testing/eventrecorder.h"
#include "aster/cache/testing/fakeclock.h"
#include "aster/cache/testing/fakefilesystem.h"
#include "aster/cache/testing/fakesourceloader.h"

#include <QCoreApplication>
#include <QFile>
#include <QSharedPointer>
#include <QTemporaryDir>

#include <atomic>
#include <condition_variable>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <thread>

using namespace aster::cache;
using namespace aster::cache::testing;

#define CHECK(condition)                                                                                                                                       \
    do {                                                                                                                                                       \
        if (!(condition))                                                                                                                                      \
            throw std::runtime_error(std::string(__FILE__) + ":" + std::to_string(__LINE__) + " " #condition);                                                 \
    } while (false)

SourceKey source(int id = 0) {
    auto result = KeyBuilder().network(QUrl(QString("https://example.test/%1?token=private").arg(id)));
    CHECK(result);
    return *result.value;
}

RenderOptions options(int size = 8) {
    RenderOptions o;
    o.physicalTargetSize = QSize(size, size);
    o.processors = {{"test-resize", 1, "nearest"}};
    return o;
}

RenderKey key(int id = 0) {
    return *KeyBuilder().render(source(id), options()).value;
}

QImage pixels(int size = 8) {
    QImage image(size, size, QImage::Format_ARGB32);
    image.fill(Qt::red);
    return image;
}

void keys() {
    KeyBuilder builder;
    CHECK(source() == source());
    const auto s = source();
    const auto base = *builder.render(s, options()).value;
    auto o = options();
    o.dpr = 2;
    CHECK(!(base == *builder.render(s, o).value));

    o = options();
    o.physicalTargetSize = QSize(9, 8);
    CHECK(!(base == *builder.render(s, o).value));

    o = options();
    o.fitMode = "cover";
    CHECK(!(base == *builder.render(s, o).value));

    o = options();
    o.processors[0].version = 2;
    CHECK(!(base == *builder.render(s, o).value));

    o = options();
    o.processors[0].parameters = "linear";
    CHECK(!(base == *builder.render(s, o).value));

    o = options();
    ++o.schemaVersion;
    CHECK(!(base == *builder.render(s, o).value));

    o = options();
    o.dpr = std::numeric_limits<double>::quiet_NaN();
    CHECK(!builder.render(s, o));

    o = options();
    o.physicalTargetSize = QSize(0, 8);
    CHECK(!builder.render(s, o));
    CHECK(!builder.render({}, options()));

    const QUrl url("https://example.test/image?token=a&size=1");
    CHECK(!(*builder.network(url).value == *builder.network(QUrl("https://example.test/image?token=b&size=1")).value));

    KeyContext a, b;
    b.tenant = "other";
    CHECK(!(*builder.network(url, a).value == *builder.network(url, b).value));

    b = {};
    b.authScope = "user2";
    CHECK(!(*builder.network(url, a).value == *builder.network(url, b).value));

    b = {};
    b.contentVariant = "language:en";
    CHECK(!(*builder.network(url, a).value == *builder.network(url, b).value));

    KeyBuilder normalized([](QUrl u) {
        u.setFragment(QString());
        return u;
    });
    CHECK(*normalized.network(QUrl("https://example.test/image#a")).value == *normalized.network(QUrl("https://example.test/image#b")).value);
    CHECK(!builder.network(QUrl("not-a-url")));

    o = options();
    o.processors = {{"a", 1, "bc"}, {"d", 1, ""}};
    auto order = builder.render(s, o);
    std::swap(o.processors[0], o.processors[1]);
    CHECK(!(*order.value == *builder.render(s, o).value));

    FakeFileSystem fs;
    fs.write("/images/a", "one", 1);
    auto before = builder.local(*fs.fingerprint("/images/a").value);
    fs.write("/images/a", "two", 2);
    CHECK(!(*before.value == *builder.local(*fs.fingerprint("/images/a").value).value));

    auto strict = builder.local(*fs.fingerprint("/images/a", true).value);
    fs.write("/images/a", "six", 2);
    CHECK(!(*strict.value == *builder.local(*fs.fingerprint("/images/a", true).value).value));
    CHECK(!fs.fingerprint("/missing"));

    QTemporaryDir dir;
    CHECK(dir.isValid());
    const auto path = dir.filePath("image.bin");
    {
        QFile file(path);
        CHECK(file.open(QIODevice::WriteOnly));
        CHECK(file.write("abc") == 3);
    }
    auto realBefore = builder.localFile(path, {}, true);
    CHECK(realBefore);
    {
        QFile file(path);
        CHECK(file.open(QIODevice::WriteOnly));
        CHECK(file.write("defg") == 4);
    }
    CHECK(!(*realBefore.value == *builder.localFile(path, {}, true).value));
    CHECK(!builder.localFile(dir.filePath("absent")));
}

void memory() {
    auto clock = QSharedPointer<FakeClock>::create(100);
    const QImage image = pixels();
    const auto cost = RenderedMemoryCache::costOf(image);
    RenderedMemoryCache cache(cost * 3, clock);
    CHECK(cache.put(key(0), image));
    CHECK(cache.put(key(1), image));
    CHECK(cache.put(key(2), image));
    CHECK(cache.get(key(0)));
    CHECK(cache.put(key(3), image));
    CHECK(!cache.get(key(1)));
    CHECK(cache.get(key(2)));
    CHECK(cache.get(key(0)));
    CHECK(cache.stats().evictions == 1);
    CHECK(cache.stats().totalBytes == 3 * cost);

    auto retained = cache.get(key(0));
    cache.clear();
    CHECK(retained.value->pixelColor(0, 0) == QColor(Qt::red));
    CHECK(cache.stats().totalBytes == 0);

    cache.setMaxCost(cost);
    CHECK(cache.put(key(), image));
    for (int i = 0; i < 100; ++i)
        CHECK(cache.put(key(), image));
    CHECK(cache.stats().entryCount == 1);
    CHECK(cache.stats().totalBytes == cost);

    cache.setMaxCost(cost - 1);
    CHECK(cache.stats().entryCount == 0);
    CHECK(!cache.put(key(), image));

    cache.setMaxCost(0);
    CHECK(!cache.put(key(), image));

    cache.setMaxCost(cost);
    CHECK(!cache.put(key(), pixels(100)));
    CHECK(!cache.put(key(), QImage()));
    CHECK(cache.put(key(), image, clock->now().addMSecs(10)));
    clock->advance(9);
    CHECK(cache.get(key()));
    clock->advance(1);
    CHECK(cache.get(key(), true));
    CHECK(!cache.get(key()));
    CHECK(cache.stats().expirations == 1);
    CHECK(cache.put(key(), image));
    auto copy = cache.get(key());
    copy.value->fill(Qt::blue);
    CHECK(cache.get(key()).value->pixelColor(0, 0) == QColor(Qt::red));

    cache.trim(0);
    CHECK(!cache.remove(key()));
    CHECK(cache.put(key(), image));
    CHECK(cache.remove(key()));
    CHECK(!cache.remove(key()));

    std::vector<uchar> external(8 * 8 * 4, 0);
    QImage borrowed(external.data(), 8, 8, QImage::Format_ARGB32);
    borrowed.fill(Qt::red);
    CHECK(cache.put(key(), borrowed));
    borrowed.fill(Qt::blue);
    CHECK(cache.get(key()).value->pixelColor(0, 0) == QColor(Qt::red));
}

void memoryConcurrency() {
    const auto image = pixels();
    const auto cost = RenderedMemoryCache::costOf(image);
    RenderedMemoryCache cache(cost * 100);
    std::vector<RenderKey> keys;
    for (int i = 0; i < 100; ++i)
        keys.push_back(key(i));
    std::vector<std::thread> threads;
    std::atomic<bool> good{true};
    for (int n = 0; n < 8; ++n)
        threads.emplace_back([&, n] {
            std::mt19937 rng(100 + n);
            for (int i = 0; i < 2000; ++i) {
                const auto& k = keys[rng() % keys.size()];
                switch (rng() % 6) {
                    case 0:
                        cache.put(k, image);
                        break;
                    case 1:
                        cache.get(k);
                        break;
                    case 2:
                        cache.remove(k);
                        break;
                    case 3:
                        cache.setMaxCost(cost * (rng() % 100));
                        break;
                    case 4:
                        cache.trim(cost * (rng() % 100));
                        break;
                    case 5:
                        cache.clear();
                        break;
                }
                const auto s = cache.stats();
                if (s.totalBytes != s.entryCount * cost || s.totalBytes > s.maxBytes)
                    good = false;
            }
        });
    for (auto& t : threads)
        t.join();
    CHECK(good);
    cache.clear();
    CHECK(cache.stats().totalBytes == 0);
}

void registry() {
    InFlightRegistry<int, int> registry;
    std::atomic<int> loads{0}, cancels{0}, successes{0}, cancelled{0};
    InFlightRegistry<int, int>::Completion done;
    auto start = [&](auto completion) -> CancelAction {
        ++loads;
        done = completion;
        return [&] { ++cancels; };
    };
    auto callback = [&](Result<int> r) {
        if (r)
            ++successes;
        else if (r.error == ImageError::Cancelled)
            ++cancelled;
    };
    std::vector<Subscription> subscribers(100);
    std::vector<std::thread> threads;
    for (int i = 0; i < 100; ++i)
        threads.emplace_back([&, i] { subscribers[i] = registry.subscribe(1, start, callback); });
    for (auto& t : threads)
        t.join();
    CHECK(loads == 1);
    CHECK(registry.count() == 1);
    for (int i = 0; i < 99; ++i)
        subscribers[i].cancel();
    CHECK(cancels == 0);
    done(Result<int>::success(42));
    done(Result<int>::success(43));
    CHECK(successes == 1);
    CHECK(cancelled == 99);
    CHECK(registry.count() == 0);
    subscribers.clear();
    CHECK(cancelled == 99);

    auto old = registry.subscribe(1, start, callback);
    auto oldDone = done;
    old.cancel();
    CHECK(cancels == 1);
    CHECK(registry.count() == 0);

    auto replacement = registry.subscribe(1, start, callback);
    oldDone(Result<int>::success(0));
    CHECK(registry.count() == 1);
    done(Result<int>::failure(ImageError::IoError));
    CHECK(registry.count() == 0);

    auto retry = registry.subscribe(1, start, callback);
    done(Result<int>::success(1));
    CHECK(registry.count() == 0);

    int errors = 0;
    auto exception = registry.subscribe(
            2, [](auto) -> CancelAction { throw std::runtime_error("fail"); },
            [&](Result<int> r) {
                if (r.error == ImageError::ProcessingError)
                    ++errors;
            });
    CHECK(errors == 1);
    CHECK(registry.count() == 0);

    auto badCallback = registry.subscribe(3, start, [](auto) { throw std::runtime_error("callback"); });
    auto goodCallback = registry.subscribe(3, start, callback);
    done(Result<int>::success(1));
    CHECK(registry.count() == 0);

    int synchronous = 0;
    auto sync = registry.subscribe(
            4,
            [](auto finish) -> CancelAction {
                finish(Result<int>::success(1));
                return {};
            },
            [&](auto r) {
                if (r)
                    ++synchronous;
            });
    CHECK(synchronous == 1);

    // Shutdown while a starter has not yet returned its cancellation action.
    InFlightRegistry<int, int> slow;
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false, released = false;
    std::atomic<int> lateCancel{0};
    Subscription delayed;
    std::thread worker([&] {
        delayed = slow.subscribe(
                1,
                [&](auto) -> CancelAction {
                    std::unique_lock<std::mutex> lock(mutex);
                    entered = true;
                    cv.notify_all();
                    cv.wait(lock, [&] { return released; });
                    return [&] { ++lateCancel; };
                },
                [](auto) {});
    });
    {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [&] { return entered; });
    }
    slow.shutdown();
    {
        std::lock_guard<std::mutex> lock(mutex);
        released = true;
    }
    cv.notify_all();
    worker.join();
    CHECK(lateCancel == 1);
    CHECK(slow.count() == 0);
}

void pipeline() {
    FakeNetwork network;
    EventRecorder recorder;
    auto cache = QSharedPointer<RenderedMemoryCache>::create(1024 * 1024);
    std::atomic<int> renders{0}, success{0}, errors{0};
    ImagePipeline pipeline(
            cache, network.task(),
            [&](const QByteArray&, const RenderOptions& o, const auto&) {
                ++renders;
                return ImageResult::success(pixels(o.physicalTargetSize.width()));
            },
            recorder.sink());
    ImageRequest a{source(), options(), {}};
    auto b = a;
    b.render = options(16);
    std::vector<Subscription> subs(100);
    std::vector<std::thread> threads;
    for (int i = 0; i < 100; ++i)
        threads.emplace_back([&, i] {
            subs[i] = pipeline.request(i % 2 ? a : b, [&](auto r) {
                if (r)
                    ++success;
                else
                    ++errors;
            });
        });
    for (auto& t : threads)
        t.join();
    CHECK(network.loadCount() == 1);
    CHECK(pipeline.stats().renderInFlight == 2);
    network.completeAll();
    CHECK(renders == 2);
    CHECK(success == 100);
    CHECK(errors == 0);
    CHECK(pipeline.stats().sourceInFlight == 0);
    CHECK(pipeline.stats().renderInFlight == 0);
    auto hit = pipeline.request(a, [&](auto r) {
        CHECK(r.source == CacheResultSource::RenderedMemory);
        ++success;
    });
    CHECK(success == 101);
    CHECK(network.loadCount() == 1);
    a.cache.read = CacheReadPolicy::CacheOnly;
    a.source = source(999);
    auto miss = pipeline.request(a, [&](auto r) {
        if (r.error == ImageError::CacheMiss)
            ++errors;
    });
    CHECK(errors == 1);
    CHECK(network.loadCount() == 1);

    a = {source(2), options(), {}};
    a.cache.read = CacheReadPolicy::NoCache;
    a.cache.write = CacheWritePolicy::NoStore;
    auto noStore = pipeline.request(a, [](auto) {});
    network.completeAll();
    CHECK(!cache->get(*KeyBuilder().render(a.source, a.render).value));

    a = {source(3), options(), {}};
    auto first = pipeline.request(a, [&](auto r) {
        if (!r)
            ++errors;
    });
    network.completeAll(Result<QByteArray>::failure(ImageError::IoError));
    CHECK(pipeline.stats().renderInFlight == 0);
    auto second = pipeline.request(a, [&](auto r) {
        if (r)
            ++success;
    });
    network.completeAll();
    CHECK(success == 102);

    a.source = source(4);
    std::vector<Subscription> cancelled;
    for (int i = 0; i < 100; ++i)
        cancelled.push_back(pipeline.request(a, [](auto) {}));
    cancelled.clear();
    CHECK(network.cancelCount() == 1);
    CHECK(pipeline.stats().sourceInFlight == 0);
    CHECK(pipeline.stats().renderInFlight == 0);
    network.completeAll();
    CHECK(!cache->get(*KeyBuilder().render(a.source, a.render).value));
    for (const auto& event : recorder.events())
        CHECK(event.keyDigest.size() == 32);

    FakeSourceLoader throwingSource;
    ImagePipeline throwing(cache, throwingSource.task(), [](const auto&, const auto&, const auto&) -> ImageResult { throw std::runtime_error("processor"); });
    int failures = 0;
    auto fail = throwing.request(a, [&](auto r) {
        if (r.error == ImageError::ProcessingError)
            ++failures;
    });
    throwingSource.completeAll();
    CHECK(failures == 1);
    CHECK(throwing.stats().renderInFlight == 0);
    auto failAgain = throwing.request(a, [](auto) {});
    throwingSource.completeAll();
    CHECK(throwingSource.loadCount() == 2);
}

void randomized() {
    InFlightRegistry<int, int> registry;
    std::mutex pendingMutex;
    std::vector<InFlightRegistry<int, int>::Completion> pending;
    std::atomic<int> callbacks{0};
    std::atomic<bool> producersDone{false};
    auto starter = [&](auto done) -> CancelAction {
        std::lock_guard<std::mutex> lock(pendingMutex);
        pending.push_back(std::move(done));
        return {};
    };
    std::thread completer([&] {
        while (true) {
            std::vector<InFlightRegistry<int, int>::Completion> batch;
            {
                std::lock_guard<std::mutex> lock(pendingMutex);
                batch.swap(pending);
            }
            if (batch.empty() && producersDone)
                break;
            for (auto& done : batch) {
                done(Result<int>::success(1));
                done(Result<int>::failure(ImageError::IoError));
            }
            std::this_thread::yield();
        }
    });
    std::vector<std::thread> producers;
    for (int n = 0; n < 8; ++n)
        producers.emplace_back([&, n] {
            std::mt19937 rng(42 + n);
            std::vector<Subscription> retained;
            for (int i = 0; i < 1250; ++i) {
                auto sub = registry.subscribe(int(rng() % 64), starter, [&](auto) { ++callbacks; });
                if (rng() % 2)
                    sub.cancel();
                else
                    retained.push_back(std::move(sub));
                if (retained.size() > 20)
                    retained.clear();
            }
        });
    for (auto& t : producers)
        t.join();
    producersDone = true;
    completer.join();
    CHECK(callbacks == 10000);
    CHECK(registry.count() == 0);
}

void secondRoundTests();
void thirdRoundTests();
void fourthRoundTests();
void imageServiceTests();
void friendlyRequestTests();
void resamplerTests();
int diskCrashProbe(const QStringList&);

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const auto probe = diskCrashProbe(app.arguments());
    if (probe >= 0)
        return probe;
    if (app.arguments().contains("--key-digest")) {
        std::cout << key().digest.toHex().constData() << '\n';
        return 0;
    }
    try {
        keys();
        std::cout << "PASS keys\n";
        memory();
        std::cout << "PASS memory\n";
        memoryConcurrency();
        std::cout << "PASS memory concurrency\n";
        registry();
        std::cout << "PASS registry and cancellation\n";
        pipeline();
        std::cout << "PASS pipeline\n";
        randomized();
        std::cout << "PASS 10000 randomized requests\n";
        secondRoundTests();
        thirdRoundTests();
        fourthRoundTests();
        friendlyRequestTests();
        resamplerTests();
        imageServiceTests();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}
