#include "aster/cache/cache/renderedmemorycache.h"
#include "aster/cache/renderer/imagerenderer.h"
#include "aster/cache/source/cachedsourceloader.h"
#include "aster/gui/imagebox.h"

#include <QBuffer>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <condition_variable>
#include <mutex>

using namespace aster::cache;
using namespace aster::gui;

namespace
{
ImageResult decode(const QByteArray& bytes, const RenderOptions&, const std::atomic<bool>&)
{
    const auto image = QImage::fromData(bytes);
    return image.isNull() ? ImageResult::failure(ImageError::CorruptedEntry)
                          : ImageResult::success(image);
}

QByteArray pixels(const QColor& color)
{
    QImage image(8, 8, QImage::Format_ARGB32);
    image.fill(color);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return bytes;
}

class FakeNetwork final : public INetworkService
{
public:
    std::atomic<int> calls{0};

    Result<NetworkResponse> fetch(const QUrl&, const NetworkFetchOptions&,
                                  const std::atomic<bool>&) override
    {
        ++calls;
        return Result<NetworkResponse>::success(
            {200, {{"cache-control", "max-age=600"}}, pixels(Qt::green)});
    }
};

class ControlledLoader final : public IImageSourceLoader
{
public:
    ~ControlledLoader() override = default;

    Result<SourceKey> key(const ImageSource& source) const override
    {
        return KeyBuilder().network(source.url);
    }

    Result<SourcePayload> load(const ImageSource& source, const SourceKey&,
                               const SourceLoadOptions&, const std::atomic<bool>&) override
    {
        std::unique_lock<std::mutex> lock(mutex_);
        const auto name = source.url.path();
        ++started_[name];
        condition_.wait_for(lock, std::chrono::seconds(10),
                            [&]
                            {
                                return released_.contains(name) || all_;
                            });
        SourcePayload payload;
        payload.bytes = pixels(name == "/a" ? Qt::red : Qt::blue);
        // Deliberately ignore cancellation to simulate an uncooperative late producer.
        return Result<SourcePayload>::success(payload);
    }

    int started(const QString& name)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return started_.value(name);
    }

    void release(const QString& name)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        released_.insert(name);
        condition_.notify_all();
    }

    void releaseAll()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        all_ = true;
        condition_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    QMap<QString, int> started_;
    QSet<QString> released_;
    bool all_ = false;
};

struct ControlledPipeline
{
    std::shared_ptr<ControlledLoader> loader = std::make_shared<ControlledLoader>();
    std::shared_ptr<ActiveResourceStore> active = std::make_shared<ActiveResourceStore>(65536);
    std::shared_ptr<ImagePipeline> pipeline =
        std::make_shared<ImagePipeline>(std::make_shared<RenderedMemoryCache>(65536), loader,
                                        decode, 4, EventSink{}, PipelineResources{nullptr, active});

    ~ControlledPipeline()
    {
        loader->releaseAll();
        pipeline->waitForIdle();
    }
};

std::shared_ptr<ImagePipeline> normalPipeline(std::shared_ptr<INetworkService> network = {})
{
    return std::make_shared<ImagePipeline>(
        std::make_shared<RenderedMemoryCache>(65536),
        std::make_shared<CachedSourceLoader>(std::make_shared<EncodedMemoryCache>(65536, 32768),
                                             nullptr, network),
        decode);
}

void flushDeletes()
{
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}
}

class ImageBoxTests : public QObject
{
    Q_OBJECT

private Q_SLOTS:

    void dimensionsAlgorithmsAndResize()
    {
        class DprBox : public ImageBox
        {
        public:
            qreal ratio = 1;

        protected:
            qreal requestDevicePixelRatio() const override
            {
                return ratio;
            }
        };

        auto loader = std::make_shared<ControlledLoader>();
        loader->releaseAll();
        std::mutex mutex;
        QVector<RenderOptions> recorded;
        QThread* renderThread = nullptr;
        auto pipeline = std::make_shared<ImagePipeline>(
            std::make_shared<RenderedMemoryCache>(16 * 1024 * 1024), loader,
            [&](const QByteArray& data, const RenderOptions& options,
                const std::atomic<bool>& token)
            {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    recorded.push_back(options);
                    renderThread = QThread::currentThread();
                }
                return ImageRenderer{}(data, options, token);
            });
        DprBox box;
        box.setPipeline(pipeline);
        box.setFit(ImageFit::Fill);
        box.resize(101, 51);
        QSignalSpy loaded(&box, &ImageBox::loaded);
        QSignalSpy started(&box, &ImageBox::loadingStarted);
        QCOMPARE(box.scaleAlgorithm(), ImageScaleAlgorithm::QtSmooth);
        box.setSource("https://example.test/a");
        QTRY_COMPARE(loaded.count(), 1);
        QCOMPARE(box.image().size(), QSize(101, 51));
        {
            std::lock_guard<std::mutex> lock(mutex);
            QCOMPARE(recorded.back().scaleAlgorithm, ImageScaleAlgorithm::QtSmooth);
            QVERIFY(renderThread != QThread::currentThread());
        }
        for (const qreal ratio : {1.25, 1.5, 2.0, 3.0})
        {
            const int previous = loaded.count();
            box.ratio = ratio;
            QEvent change(QEvent::ScreenChangeInternal);
            QCoreApplication::sendEvent(&box, &change);
            QTRY_COMPARE(loaded.count(), previous + 1);
            QCOMPARE(box.image().size(), *physicalTargetSize(QSizeF(101, 51), ratio).value);
            QCOMPARE(box.image().devicePixelRatio(), ratio);
        }
        box.setScaleAlgorithm(ImageScaleAlgorithm::Lanczos4);
        QTRY_COMPARE(box.state(), ImageBoxState::Ready);
        {
            std::lock_guard<std::mutex> lock(mutex);
            QCOMPARE(recorded.back().scaleAlgorithm, ImageScaleAlgorithm::Lanczos4);
        }
        box.setScaleAlgorithm(ImageScaleAlgorithm::Bicubic);
        QTRY_COMPARE(box.state(), ImageBoxState::Ready);
        {
            std::lock_guard<std::mutex> lock(mutex);
            QCOMPARE(recorded.back().scaleAlgorithm, ImageScaleAlgorithm::Bicubic);
        }
        box.ratio = 1;
        box.setScaleAlgorithm(ImageScaleAlgorithm::QtSmooth);
        QTRY_COMPARE(box.state(), ImageBoxState::Ready);
        box.show();
        QCoreApplication::processEvents();
        box.setResizeDebounceInterval(150);
        const auto count = started.count();
        for (int i = 0; i < 1000; ++i)
            box.resize(100 + i % 99, 60 + i % 47);
        box.resize(301, 201);
        QCOMPARE(started.count(), count);
        QTRY_COMPARE(box.state(), ImageBoxState::Ready);
        QCOMPARE(started.count(), count + 1);
        QCOMPARE(box.image().size(), QSize(301, 201));
        box.setTargetSizeBucket(16);
        QTRY_COMPARE(box.state(), ImageBoxState::Ready);
        QCOMPARE(box.image().size(), QSize(304, 208));
        const auto bucketCount = started.count();
        box.resize(302, 202);
        box.resize(303, 203);
        QTest::qWait(200);
        QCOMPARE(started.count(), bucketCount);
        box.resize(0, 0);
        QTest::qWait(200);
        QCOMPARE(started.count(), bucketCount);
        box.resize(200, 100);
        box.cancelCurrentRequest();
        QTest::qWait(200);
        QCOMPARE(started.count(), bucketCount);
        box.reload();
        QTRY_COMPARE(box.state(), ImageBoxState::Ready);
        QVERIFY(pipeline->waitForIdle());
        auto* transient = new ImageBox;
        transient->setPipeline(pipeline);
        transient->resize(0, 0);
        transient->setSource("https://example.test/a");
        QCOMPARE(transient->state(), ImageBoxState::Empty);
        transient->show();
        transient->resize(33, 21);
        QPointer<ImageBox> destroyed = transient;
        delete transient;
        QVERIFY(destroyed.isNull());
        QTest::qWait(200);
        QVERIFY(pipeline->waitForIdle());
    }

    void sourcesAndPainting()
    {
        auto network = std::make_shared<FakeNetwork>();
        auto pipeline = normalPipeline(network);
        ImageBox box;
        box.resize(2, 1);
        box.setPipeline(pipeline);
        QSignalSpy loaded(&box, &ImageBox::loaded);
        QSignalSpy states(&box, &ImageBox::stateChanged);
        QSignalSpy started(&box, &ImageBox::loadingStarted);

        box.setSource(":/aster-test/sample.ppm");
        QCOMPARE(box.state(), ImageBoxState::Loading);
        QTRY_COMPARE(loaded.count(), 1);
        QCOMPARE(box.image().pixelColor(0, 0), QColor(Qt::red));
        QCOMPARE(states.count(), 2);
        QCOMPARE(qvariant_cast<ImageBoxState>(states[0][0]), ImageBoxState::Loading);
        QCOMPARE(qvariant_cast<ImageBoxState>(states[1][0]), ImageBoxState::Ready);

        QImage canvas(box.size(), QImage::Format_ARGB32);
        canvas.fill(Qt::black);
        box.render(&canvas);
        QCOMPARE(canvas.pixelColor(0, 0), QColor(Qt::red));
        QCOMPARE(canvas.pixelColor(1, 0), QColor(Qt::blue));

        box.setSource(box.source());
        QCOMPARE(started.count(), 1);
        box.reload();
        QTRY_COMPARE(loaded.count(), 2);
        QCOMPARE(started.count(), 2);
        QCOMPARE(box.image().pixelColor(0, 0), QColor(Qt::red));

        QTemporaryDir directory;
        const auto file = directory.filePath("local.png");
        QFile output(file);
        QVERIFY(output.open(QIODevice::WriteOnly));
        const auto data = pixels(Qt::yellow);
        QCOMPARE(output.write(data), qint64(data.size()));
        output.close();
        box.setSource(file);
        QTRY_COMPARE(loaded.count(), 3);
        QCOMPARE(box.image().pixelColor(0, 0), QColor(Qt::yellow));

        box.setSource("https://example.test/image.png");
        QTRY_COMPARE(loaded.count(), 4);
        QCOMPARE(network->calls.load(), 1);
        QCOMPARE(box.image().pixelColor(0, 0), QColor(Qt::green));
        box.reload();
        QTRY_COMPARE(loaded.count(), 5);
        QCOMPARE(network->calls.load(), 1);
        QVERIFY(pipeline->waitForIdle());
    }

    void failuresAndEmpty()
    {
        ImageBox box;
        box.setPipeline(normalPipeline());
        QSignalSpy failed(&box, &ImageBox::loadFailed);
        QSignalSpy loaded(&box, &ImageBox::loaded);
        box.setSource(":/aster-test/sample.ppm");
        QTRY_COMPARE(loaded.count(), 1);
        const auto image = box.image();

        box.setSource(":/aster-test/missing.png");
        QCOMPARE(box.image(), image);
        QTRY_COMPARE(failed.count(), 1);
        QCOMPARE(box.state(), ImageBoxState::Error);
        QCOMPARE(box.image(), image);
        QCOMPARE(box.error(), ImageError::IoError);

        QTemporaryDir directory;
        QFile output(directory.filePath("broken.png"));
        QVERIFY(output.open(QIODevice::WriteOnly));
        output.write("not an image");
        output.close();
        box.setSource(output.fileName());
        QTRY_COMPARE(failed.count(), 2);
        QCOMPARE(box.error(), ImageError::CorruptedEntry);
        QCOMPARE(box.image(), image);

        box.setSource("");
        QCOMPARE(box.state(), ImageBoxState::Empty);
        QVERIFY(box.image().isNull());
        QCOMPARE(box.error(), ImageError::None);
        box.reload();
        QCOMPARE(box.state(), ImageBoxState::Empty);

        ImageBox unconfigured;
        QSignalSpy missing(&unconfigured, &ImageBox::loadFailed);
        unconfigured.setSource(":/aster-test/sample.ppm");
        QTRY_COMPARE(missing.count(), 1);
        QCOMPARE(unconfigured.error(), ImageError::InvalidRequest);
        QVERIFY(!unconfigured.errorString().isEmpty());
        unconfigured.setPipeline(box.pipeline());
        QTRY_COMPARE(unconfigured.state(), ImageBoxState::Ready);
    }

    void lateResultsAndHandles()
    {
        ControlledPipeline fixture;
        ImageBox box;
        box.setPipeline(fixture.pipeline);
        QSignalSpy loaded(&box, &ImageBox::loaded);
        QSignalSpy failed(&box, &ImageBox::loadFailed);
        box.setSource("https://example.test/a");
        QTRY_COMPARE(fixture.loader->started("/a"), 1);
        box.setSource("https://example.test/b");
        QTRY_COMPARE(fixture.loader->started("/b"), 1);
        fixture.loader->release("/b");
        QTRY_COMPARE(loaded.count(), 1);
        QCOMPARE(box.image().pixelColor(0, 0), QColor(Qt::blue));
        fixture.loader->release("/a");
        QVERIFY(fixture.pipeline->waitForIdle());
        QCoreApplication::processEvents();
        flushDeletes();
        QCOMPARE(loaded.count(), 1);
        QCOMPARE(failed.count(), 0);
        QCOMPARE(box.source(), QString("https://example.test/b"));
        QCOMPARE(box.image().pixelColor(0, 0), QColor(Qt::blue));
        QCOMPARE(fixture.active->stats().entries, qint64(1));

        box.setSource("https://example.test/a");
        QTRY_COMPARE(loaded.count(), 2);
        flushDeletes();
        QCOMPARE(box.image().pixelColor(0, 0), QColor(Qt::red));
        QCOMPARE(fixture.active->stats().entries, qint64(1));
        box.setSource("");
        QCOMPARE(fixture.active->stats().entries, qint64(0));
        QVERIFY(box.findChildren<ImageSubscription*>().isEmpty());
    }

    void cancellationAndDestruction()
    {
        ControlledPipeline fixture;
        auto* box = new ImageBox;
        box->setPipeline(fixture.pipeline);
        box->setSource("https://example.test/a");
        QTRY_COMPARE(fixture.loader->started("/a"), 1);
        QPointer<ImageSubscription> pending = box->findChild<ImageSubscription*>();
        QVERIFY(pending);
        box->cancelCurrentRequest();
        box->cancelCurrentRequest();
        QVERIFY(!pending);
        QCOMPARE(box->state(), ImageBoxState::Empty);
        fixture.loader->release("/a");
        QVERIFY(fixture.pipeline->waitForIdle());
        QCoreApplication::processEvents();
        QVERIFY(box->image().isNull());

        box->reload();
        QTRY_COMPARE(box->state(), ImageBoxState::Ready);
        box->setSource("https://example.test/b");
        QTRY_COMPARE(fixture.loader->started("/b"), 1);
        box->cancelCurrentRequest();
        QCOMPARE(box->state(), ImageBoxState::Ready);
        QCOMPARE(box->image().pixelColor(0, 0), QColor(Qt::red));
        box->reload();
        pending = box->findChild<ImageSubscription*>();
        delete box;
        QVERIFY(!pending);
        fixture.loader->releaseAll();
        QVERIFY(fixture.pipeline->waitForIdle());
        QCoreApplication::processEvents();
        flushDeletes();
        QCOMPARE(fixture.active->stats().entries, qint64(0));
    }

    void rapidSwitchAndPipelineReplacement()
    {
        ControlledPipeline fixture;
        ImageBox box;
        box.setPipeline(fixture.pipeline);
        fixture.loader->releaseAll();
        for (int i = 0; i < 1000; ++i)
        {
            box.setSource(QString("https://example.test/%1").arg(i));
            if (i % 7 == 0)
                QCoreApplication::processEvents();
        }
        box.setSource("https://example.test/a");
        QTRY_COMPARE(box.state(), ImageBoxState::Ready);
        QVERIFY(fixture.pipeline->waitForIdle());
        QCOMPARE(box.image().pixelColor(0, 0), QColor(Qt::red));
        QCOMPARE(box.source(), QString("https://example.test/a"));

        box.setPipeline(normalPipeline());
        QTRY_COMPARE(box.state(), ImageBoxState::Error);
        QCOMPARE(box.image().pixelColor(0, 0), QColor(Qt::red));
        box.setSource(":/aster-test/sample.ppm");
        QTRY_COMPARE(box.state(), ImageBoxState::Ready);
        box.setPipeline(nullptr);
        QTRY_COMPARE(box.state(), ImageBoxState::Error);
        box.setSource("");
        QCOMPARE(box.state(), ImageBoxState::Empty);
    }

    void signalOrderAndReentrancy()
    {
        ImageBox box;
        box.setPipeline(normalPipeline());
        QStringList order;
        connect(&box, &ImageBox::stateChanged, &box,
                [&](ImageBoxState state)
                {
                    order << (state == ImageBoxState::Loading ? "loading" : "ready");
                });
        connect(&box, &ImageBox::loadingStarted, &box,
                [&]
                {
                    order << "started";
                });
        connect(&box, &ImageBox::loaded, &box,
                [&]
                {
                    order << "loaded";
                });
        box.setSource(":/aster-test/sample.ppm");
        QTRY_COMPARE(order.size(), 4);
        QCOMPARE(order, QStringList({"loading", "started", "ready", "loaded"}));

        auto connection = connect(&box, &ImageBox::stateChanged, &box,
                                  [&](ImageBoxState state)
                                  {
                                      if (state == ImageBoxState::Ready)
                                          box.setSource("");
                                  });
        QSignalSpy loaded(&box, &ImageBox::loaded);
        box.reload();
        QTRY_COMPARE(box.state(), ImageBoxState::Empty);
        QCOMPARE(loaded.count(), 0);
        disconnect(connection);

        connection = connect(&box, &ImageBox::loadingStarted, &box,
                             [&]
                             {
                                 box.cancelCurrentRequest();
                             });
        box.setSource(":/aster-test/sample.ppm");
        QCOMPARE(box.state(), ImageBoxState::Empty);
        QVERIFY(box.findChildren<ImageSubscription*>().isEmpty());
        disconnect(connection);

        for (const auto terminal : {false, true})
        {
            QPointer<ImageBox> victim = new ImageBox;
            victim->setPipeline(box.pipeline());
            connect(victim, &ImageBox::stateChanged, this,
                    [victim, terminal](ImageBoxState state)
                    {
                        if (state == (terminal ? ImageBoxState::Ready : ImageBoxState::Loading))
                            delete victim.data();
                    });
            victim->setSource(":/aster-test/sample.ppm");
            QTRY_VERIFY(victim.isNull());
        }
    }
};

QTEST_MAIN(ImageBoxTests)
#include "imagebox_tests.moc"
