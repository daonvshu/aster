#include "aster/cache/cache/renderedmemorycache.h"
#include "aster/cache/pipeline/imagepipeline.h"
#include "aster/cache/source/cachedsourceloader.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QPointer>
#include <QSharedPointer>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <future>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace aster::cache;

namespace {
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

ImageResult awaitResult(ImageSubscription* subscription) {
    require(subscription != nullptr, "Missing subscription");
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    bool received = false;
    ImageResult result;
    QObject::connect(subscription, &ImageSubscription::finished, &loop, [&](ImageResult value) {
        received = true;
        result = std::move(value);
        loop.quit();
    });
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timer.start(10000);
    loop.exec();
    require(received, "Image subscription signal timeout");
    return result;
}

RenderOptions options() {
    RenderOptions render;
    render.physicalTargetSize = QSize(2, 1);
    return render;
}

void sourceStrings() {
    const auto remote = ImageSource::fromString("HTTPS://example.com/a.png?token=secret");
    require(remote && remote.value->kind == ImageSource::Kind::Network, "HTTP source detection");
    require(remote.value->url.query().contains("token=secret"), "HTTP identity lost query");
    for (const auto& path : {QString("D:/images/a.png"), QString("D:\\images\\a.png"), QString("\\\\server\\share\\a.png"), QString("//server/share/a.png"),
                             QString("images/a.png"), QString("/tmp/a.png")}) {
        const auto result = ImageSource::fromString(path);
        require(result && result.value->kind == ImageSource::Kind::Local && result.value->url.isLocalFile(), "Local path detection");
    }
    const auto relative = ImageSource::fromString("images/space #50%.png");
    require(relative && relative.value->url.toLocalFile() == QDir::current().absoluteFilePath("images/space #50%.png"),
            "Relative filenames must preserve literal URL punctuation");

    const auto colon = ImageSource::fromString(":/aster-test/sample.ppm");
    const auto qrc = ImageSource::fromString("qrc:/aster-test/folder/../sample.ppm");
    require(colon && qrc && colon.value->kind == ImageSource::Kind::Resource && colon.value->url == qrc.value->url, "Resource alias normalization");

    for (const auto& invalid : {QString(), QString("  "), QString("ftp://host/a.png"), QString("http:///a.png"), QString("qrc://host/a.png"),
                                QString("qrc:/a.png#fragment"), QString("file:///tmp/a.png?query")})
        require(!ImageSource::fromString(invalid), "Invalid source was accepted");

    QTemporaryDir directory;
    const auto path = directory.filePath(QString::fromUtf8("space #50% 图片.ppm"));
    QFile source(":/aster-test/sample.ppm");
    require(source.open(QIODevice::ReadOnly), "Test resource unavailable");
    QFile target(path);
    require(target.open(QIODevice::WriteOnly), "Test file unavailable");
    const auto bytes = source.readAll();
    require(target.write(bytes) == bytes.size(), "Test file write failed");
    target.close();

    CachedSourceLoader loader(nullptr);
    const auto file = ImageSource::fromString(path);
    const auto fileUrl = ImageSource::fromString(QUrl::fromLocalFile(path).toString(QUrl::FullyEncoded));
    require(file && fileUrl, "File URL parse failed");
    require(*loader.key(*file.value).value == *loader.key(*fileUrl.value).value, "File URL key mismatch");
    const std::atomic<bool> running{false};
    auto payload = loader.load(*file.value, *loader.key(*file.value).value, {}, running);
    require(payload && payload.value->bytes == bytes && payload.source == CacheResultSource::LocalFile, "Local string source read failed");

    auto encoded = QSharedPointer<EncodedMemoryCache>::create(32768, 8192);
    CachedSourceLoader resources(encoded);
    const auto resourceKey = resources.key(*colon.value);
    require(resourceKey && *resourceKey.value == *resources.key(*qrc.value).value, "Resource key mismatch");
    require(!(*resourceKey.value == *loader.key(*file.value).value), "File/resource identity collision");
    payload = resources.load(*colon.value, *resourceKey.value, {}, running);
    require(payload && payload.value->bytes == bytes && payload.source == CacheResultSource::Resource, "Resource source read failed");
    SourceLoadOptions cachedOnly;
    cachedOnly.cache.read = CacheReadPolicy::CacheOnly;
    require(resources.load(*colon.value, *resourceKey.value, cachedOnly, running).source == CacheResultSource::EncodedMemory, "Resource encoded cache miss");
    require(resources.invalidate({resourceKey.value->digest, {}}), "Resource invalidation failed");
    cachedOnly.maxBytes = 1;
    cachedOnly.cache = {};
    require(!resources.load(*colon.value, *resourceKey.value, cachedOnly, running), "Resource size limit bypassed");
    require(!resources.key(*ImageSource::fromString(":/aster-test/missing.ppm").value), "Missing resource accepted");
}

void deliveryAndCancellation() {
    auto memory = QSharedPointer<RenderedMemoryCache>::create(32768);
    auto loader = QSharedPointer<CachedSourceLoader>::create(nullptr);
    std::atomic<int> renders{0};
    ImagePipeline pipeline(memory, loader, [&](const auto& bytes, const auto&, const auto&) {
        ++renders;
        return ImageResult::success(QImage::fromData(bytes));
    });
    QObject owner;

    QPointer<ImageSubscription> invalid = pipeline.request(QString(), options(), &owner);
    require(invalid && !invalid->isFinished(), "Immediate error emitted before connect");
    require(awaitResult(invalid).error == ImageError::InvalidRequest, "Missing parse error signal");
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    require(invalid.isNull(), "Finished subscription was not released");

    QPointer<ImageSubscription> first = pipeline.request(":/aster-test/sample.ppm", options(), &owner);
    require(pipeline.waitForIdle(), "Resource request did not drain");
    require(first && !first->isFinished(), "Signal emitted without receiver event loop");
    QThread* deliveredThread = nullptr;
    QObject::connect(first, &ImageSubscription::finished, &owner, [&](const ImageResult&) { deliveredThread = QThread::currentThread(); });
    auto result = awaitResult(first);
    require(result && result.source == CacheResultSource::Resource && result.value->pixelColor(0, 0) == QColor(Qt::red), "Resource signal result is invalid");
    require(deliveredThread == owner.thread(), "Signal emitted on worker thread");

    SourceRequest explicitRequest;
    explicitRequest.source = *ImageSource::fromString("qrc:/aster-test/sample.ppm").value;
    explicitRequest.render = options();
    result = awaitResult(pipeline.request(explicitRequest, &owner));
    require(result.source == CacheResultSource::RenderedMemory && renders == 1, "Explicit source/cache-hit signal lost");

    QPointer<ImageSubscription> cancelled = pipeline.request(explicitRequest, &owner);
    require(pipeline.waitForIdle(), "Cancellation setup did not drain");
    int completions = 0;
    QObject::connect(cancelled, &ImageSubscription::finished, &owner, [&](const ImageResult&) { ++completions; });
    cancelled->cancel();
    cancelled->cancel();
    require(awaitResult(cancelled).error == ImageError::Cancelled, "Queued success won over cancellation");
    QCoreApplication::processEvents();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    require(completions == 1 && cancelled.isNull(), "Cancellation delivered more than once");

    QThread receiverThread;
    auto* receiver = new QObject;
    receiver->moveToThread(&receiverThread);
    QObject::connect(&receiverThread, &QThread::finished, receiver, &QObject::deleteLater);
    receiverThread.start();
    auto promise = QSharedPointer<std::promise<bool>>::create();
    auto future = promise->get_future();
    auto* crossThread = pipeline.request(explicitRequest, &owner);
    QObject::connect(
            crossThread, &ImageSubscription::finished, receiver,
            [promise, receiver](ImageResult value) { promise->set_value(bool(value) && QThread::currentThread() == receiver->thread()); },
            Qt::QueuedConnection);
    const auto directResult = awaitResult(crossThread);
    const bool ready = future.wait_for(std::chrono::seconds(10)) == std::future_status::ready;
    receiverThread.quit();
    receiverThread.wait();
    require(directResult && ready && future.get(), "Queued ImageResult metatype delivery failed");

    auto callback = QSharedPointer<std::promise<ImageResult>>::create();
    auto callbackFuture = callback->get_future();
    auto legacy = pipeline.request(QString(":/aster-test/sample.ppm"), options(), [callback](ImageResult value) { callback->set_value(std::move(value)); });
    require(callbackFuture.wait_for(std::chrono::seconds(10)) == std::future_status::ready && bool(callbackFuture.get()), "Callback API compatibility failed");
    require(pipeline.waitForIdle(), "Final pipeline drain failed");
}

void destroyedConsumers() {
    auto memory = QSharedPointer<RenderedMemoryCache>::create(32768);
    auto loader = QSharedPointer<CachedSourceLoader>::create(nullptr);
    std::promise<void> release;
    const auto gate = release.get_future().share();
    std::atomic<int> renders{0};
    ImagePipeline pipeline(memory, loader, [&](const auto& bytes, const auto&, const auto&) {
        ++renders;
        gate.wait();
        return ImageResult::success(QImage::fromData(bytes));
    });
    QObject observer;
    auto* owner = new QObject;
    QPointer<ImageSubscription> abandoned = pipeline.request(":/aster-test/sample.ppm", options(), owner);
    int abandonedCalls = 0;
    QObject::connect(abandoned, &ImageSubscription::finished, &observer, [&](const ImageResult&) { ++abandonedCalls; });
    auto* survivor = pipeline.request(":/aster-test/sample.ppm", options(), &observer);
    delete owner;
    release.set_value();
    require(awaitResult(survivor) && pipeline.waitForIdle(), "Other subscriber was cancelled");
    require(abandoned.isNull() && abandonedCalls == 0 && renders == 1, "Destroyed subscriber received a result or broke coalescing");

    // Race worker completion against deletion without running the delivery event loop.
    for (int i = 0; i < 200; ++i) {
        auto* transient = pipeline.request(":/aster-test/sample.ppm", options(), &observer);
        delete transient;
    }
    require(pipeline.waitForIdle(), "Destroyed request tasks did not drain");

    auto disposable = std::make_unique<ImagePipeline>(
            memory, loader, [](const auto& bytes, const auto&, const auto&) { return ImageResult::success(QImage::fromData(bytes)); });
    auto* pending = disposable->request(":/aster-test/sample.ppm", options(), &observer);
    disposable.reset();
    const auto result = awaitResult(pending);
    require(result || result.error == ImageError::Cancelled, "Pipeline destruction lost completion");
}
} // namespace

void friendlyRequestTests() {
    sourceStrings();
    std::cout << "PASS string source detection and resource loading\n";
    deliveryAndCancellation();
    std::cout << "PASS signal delivery, queued metatype and cancellation\n";
    destroyedConsumers();
    std::cout << "PASS subscriber destruction and coalescing\n";
}
