#include "aster/cache/cache/filediskcache.h"
#include "aster/cache/cache/renderedmemorycache.h"
#include "aster/cache/renderer/imagerenderer.h"
#include "aster/cache/source/cachedsourceloader.h"
#include "aster/cache/source/qtnetworkservice.h"
#include "aster/gui/imagebox.h"

#include <QElapsedTimer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

using namespace aster::cache;
using namespace aster::gui;

class HttpIntegrationTests : public QObject {
    Q_OBJECT

    QString base_;

    QSharedPointer<ImagePipeline> pipeline() {
        return QSharedPointer<ImagePipeline>::create(
                QSharedPointer<RenderedMemoryCache>::create(1024 * 1024),
                QSharedPointer<CachedSourceLoader>::create(nullptr, nullptr, QSharedPointer<QtNetworkService>::create(5000)), ImageRenderer{});
    }

private Q_SLOTS:

    void initTestCase() {
        base_ = qEnvironmentVariable("ASTER_IMAGE_TEST_URL");
        QVERIFY2(!base_.isEmpty(), "Run using tests/http_server/run_tests.py");
    }

    void transportFaults() {
        QtNetworkService service(5000);
        std::atomic<bool> cancelled{false};
        NetworkFetchOptions options;
        const auto normal = service.fetch(QUrl(base_ + "/image/avatar.png"), options, cancelled);
        QVERIFY(normal);
        QCOMPARE(normal.value->status, 200);
        QCOMPARE(QImage::fromData(normal.value->body).pixelColor(0, 0), QColor(Qt::red));
        for (int status : {404, 500}) {
            const auto result = service.fetch(QUrl(base_ + "/status/" + QString::number(status)), options, cancelled);
            QVERIFY(result);
            QCOMPARE(result.value->status, status);
        }
        const auto redirect = service.fetch(QUrl(base_ + "/redirect/avatar.png"), options, cancelled);
        QVERIFY(redirect);
        QCOMPARE(redirect.value->status, 302);
        QCOMPARE(redirect.value->headers.value("location"), QByteArray("/image/avatar.png"));
        const auto wrong = service.fetch(QUrl(base_ + "/wrong-content-type/avatar.png"), options, cancelled);
        QVERIFY(wrong);
        QCOMPARE(wrong.value->headers.value("content-type"), QByteArray("text/plain"));
        QCOMPARE(wrong.value->body, normal.value->body);
        const auto stream = service.fetch(QUrl(base_ + "/stream/avatar.png"), options, cancelled);
        QVERIFY(stream);
        QCOMPARE(stream.value->body, normal.value->body);
        const auto truncated = service.fetch(QUrl(base_ + "/truncate/avatar.png"), options, cancelled);
        QVERIFY(!truncated);
        QCOMPARE(truncated.error, ImageError::IoError);
        options.maxBytes = 1024;
        const auto oversized = service.fetch(QUrl(base_ + "/oversize/avatar.png"), options, cancelled);
        QVERIFY(!oversized);
        QCOMPARE(oversized.error, ImageError::InvalidRequest);
        QtNetworkService impatient(100);
        const auto timeout = impatient.fetch(QUrl(base_ + "/delay/1/avatar.png"), options, cancelled);
        QVERIFY(!timeout);
        QCOMPARE(timeout.error, ImageError::IoError);
    }

    void diskValidationAndCacheControl() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        auto disk = QSharedPointer<FileDiskCache>::create(directory.path(), 1024 * 1024, 65536);
        auto network = QSharedPointer<QtNetworkService>::create(5000);
        CachedSourceLoader loader(nullptr, disk, network);
        std::atomic<bool> cancelled{false};
        auto load = [&](const QString& endpoint) {
            const auto source = ImageSource::fromString(base_ + endpoint);
            const auto key = loader.key(*source.value);
            return loader.load(*source.value, *key.value, {}, cancelled);
        };
        const auto first = load("/etag/avatar.png");
        QVERIFY(first);
        const auto second = load("/etag/avatar.png");
        QVERIFY(second);
        QCOMPARE(second.source, CacheResultSource::Validated);
        QCOMPARE(second.value->bytes, first.value->bytes);
        QVERIFY(!QImage::fromData(second.value->bytes).isNull());
        QCOMPARE(loader.cacheStats().network.validations, quint64(1));
        QCOMPARE(loader.cacheStats().network.requests, quint64(2));
        QVERIFY(load("/cache/avatar.png"));
        const auto requests = loader.cacheStats().network.requests;
        QVERIFY(load("/cache/avatar.png"));
        QCOMPARE(loader.cacheStats().network.requests, requests);
        const auto uncached = load("/no-store/avatar.png");
        QVERIFY(uncached);
        QVERIFY(uncached.value->noStore);
        QVERIFY(load("/no-store/avatar.png"));
        QCOMPARE(loader.cacheStats().network.requests, requests + 2);
        const auto source = ImageSource::fromString(base_ + "/no-store/avatar.png");
        QVERIFY(!disk->contains(loader.key(*source.value).value->digest));
    }

    void widgetErrors_data() {
        QTest::addColumn<QString>("endpoint");
        for (const auto& endpoint :
             {"/status/404", "/status/500", "/redirect/avatar.png", "/corrupt/avatar.png", "/truncate/avatar.png", "/oversize/avatar.png"})
            QTest::newRow(endpoint) << QString::fromLatin1(endpoint);
    }

    void widgetErrors() {
        QFETCH(QString, endpoint);
        ImageBox box;
        box.resize(32, 32);
        box.setPipeline(pipeline());
        box.show();
        QSignalSpy failed(&box, &ImageBox::loadFailed);
        box.setSource(base_ + endpoint);
        QCOMPARE(box.state(), ImageBoxState::Loading);
        QTRY_COMPARE(box.state(), ImageBoxState::Error);
        QCOMPARE(failed.count(), 1);
        QVERIFY(box.image().isNull());
    }

    void wrongContentTypeUsesDecoder() {
        ImageBox box;
        box.resize(32, 32);
        box.setPipeline(pipeline());
        box.setSource(base_ + "/wrong-content-type/avatar.png");
        QTRY_COMPARE(box.state(), ImageBoxState::Ready);
        QCOMPARE(box.image().pixelColor(0, 0), QColor(Qt::red));
    }

    void delayedRaceCancelAndDestroy() {
        auto shared = pipeline();
        ImageBox box;
        box.resize(32, 32);
        box.setPipeline(shared);
        box.show();
        QSignalSpy loaded(&box, &ImageBox::loaded);
        box.setSource(base_ + "/delay/3/avatar.png");
        QTest::qWait(100);
        QElapsedTimer timer;
        timer.start();
        box.setSource(base_ + "/image/landscape.png");
        QTRY_COMPARE_WITH_TIMEOUT(box.state(), ImageBoxState::Ready, 2000);
        QVERIFY(timer.elapsed() < 2500);
        QCOMPARE(box.image().pixelColor(0, 0), QColor(Qt::blue));
        QTest::qWait(3200);
        QCOMPARE(box.image().pixelColor(0, 0), QColor(Qt::blue));
        QCOMPARE(loaded.count(), 1);
        box.setSource(base_ + "/stream/avatar.png");
        QTest::qWait(100);
        box.cancelCurrentRequest();
        QVERIFY(shared->waitForIdle());
        QCoreApplication::processEvents();
        QCOMPARE(loaded.count(), 1);
        QCOMPARE(box.image().pixelColor(0, 0), QColor(Qt::blue));
        auto* victim = new ImageBox;
        victim->setPipeline(shared);
        victim->setSource(base_ + "/delay/2/avatar.png");
        QTest::qWait(100);
        delete victim;
        QVERIFY(shared->waitForIdle());
    }
};

QTEST_MAIN(HttpIntegrationTests)
#include "http_integration_tests.moc"
