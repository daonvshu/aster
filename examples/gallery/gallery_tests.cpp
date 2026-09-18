#include "aster/cache/cache/renderedmemorycache.h"
#include "aster/cache/renderer/imagerenderer.h"
#include "aster/cache/source/cachedsourceloader.h"
#include "gallerywindow.h"
#include "httptestpanel.h"
#ifdef ASTER_GALLERY_NETWORK
#include "aster/cache/cache/filediskcache.h"
#include "aster/cache/source/qtnetworkservice.h"
#endif

#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

using namespace aster;

class GalleryTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
#ifdef ASTER_GALLERY_NETWORK
    void httpPanelIntegration() {
        if (qEnvironmentVariableIsEmpty("ASTER_IMAGE_TEST_URL"))
            QSKIP("Run with the Python HTTP test launcher");
        QTemporaryDir directory;
        auto pipeline = QSharedPointer<cache::ImagePipeline>::create(
                QSharedPointer<cache::RenderedMemoryCache>::create(16 * 1024 * 1024),
                QSharedPointer<cache::CachedSourceLoader>::create(nullptr, QSharedPointer<cache::FileDiskCache>::create(directory.path(), 1024 * 1024, 65536),
                                                                  QSharedPointer<cache::QtNetworkService>::create(5000)),
                cache::ImageRenderer{});
        gallery::GalleryWindow window(pipeline);
        window.show();
        auto* scenario = window.findChild<QComboBox*>("httpScenario");
        auto* load = window.findChild<QPushButton*>("httpLoad");
        QVERIFY(scenario && load);
        scenario->setCurrentIndex(scenario->findData("etag"));
        load->click();
        auto* grid = window.findChild<gallery::ImageGrid*>();
        QCOMPARE(grid->imageCount(), 1);
        auto* box = grid->findChild<gui::ImageBox*>();
        QVERIFY(box);
        QTRY_COMPARE(box->state(), gui::ImageBoxState::Ready);
        grid->refresh();
        QTRY_COMPARE(box->state(), gui::ImageBoxState::Ready);
        QCOMPARE(pipeline->cacheStats().network.validations, quint64(1));
        window.findChild<QPushButton*>("httpRace")->click();
        box = grid->findChild<gui::ImageBox*>();
        QTRY_VERIFY(box->source().endsWith("/image/landscape.png"));
        QTRY_COMPARE(box->state(), gui::ImageBoxState::Ready);
        QCOMPARE(box->image().pixelColor(0, 0), QColor(Qt::blue));
        QTest::qWait(3100);
        QCOMPARE(box->image().pixelColor(0, 0), QColor(Qt::blue));
        window.findChild<QPushButton*>("httpAll")->click();
        QCOMPARE(grid->imageCount(), 13);
    }
#endif

    void windowFolderLoading() {
        QTemporaryDir directory;
        for (int i = 0; i < 24; ++i) {
            QImage image(400, 240, QImage::Format_RGB32);
            image.fill(QColor::fromHsv((i * 37) % 360, 120, 160));
            QPainter painter(&image);
            painter.setPen(Qt::white);
            QFont font;
            font.setPixelSize(64);
            painter.setFont(font);
            painter.drawText(image.rect(), Qt::AlignCenter, QString::number(i + 1));
            painter.end();
            QVERIFY(image.save(directory.filePath(QString("Sample %1.png").arg(i + 1, 2, 10, QChar('0')))));
        }
        auto pipeline = QSharedPointer<cache::ImagePipeline>::create(QSharedPointer<cache::RenderedMemoryCache>::create(16 * 1024 * 1024),
                                                                     QSharedPointer<cache::CachedSourceLoader>::create(nullptr), cache::ImageRenderer{});
        gallery::GalleryWindow window(pipeline);
        window.show();
        auto* diskCache = window.findChild<QComboBox*>("diskCache");
        QVERIFY(diskCache);
        QVERIFY(!diskCache->isEnabled());
        QVERIFY(window.findChild<QLabel*>("diskCacheSize"));
        QVERIFY(window.findChild<QPushButton*>("clearDiskCache"));
        auto replacement = QSharedPointer<cache::ImagePipeline>::create(QSharedPointer<cache::RenderedMemoryCache>::create(16 * 1024 * 1024),
                                                                        QSharedPointer<cache::CachedSourceLoader>::create(nullptr), cache::ImageRenderer{});
        window.setDiskCacheFactory([replacement](gallery::GalleryWindow::DiskCacheKind) { return replacement; });
        QVERIFY(diskCache->isEnabled());
#if ASTER_ENABLE_SQLITE_DISK_CACHE
        const auto sqliteIndex = diskCache->findData(int(gallery::GalleryWindow::DiskCacheKind::Sqlite));
        QVERIFY(sqliteIndex >= 0);
        diskCache->setCurrentIndex(sqliteIndex);
#endif
        window.openFolder(directory.path());
        auto* grid = window.findChild<gallery::ImageGrid*>();
        QVERIFY(grid);
        QTRY_COMPARE(grid->imageCount(), 24);
        QVERIFY(!grid->findChildren<gui::ImageBox*>().isEmpty());
        QCOMPARE(grid->findChildren<gui::ImageBox*>().first()->pipeline().data(), replacement.data());
        auto* offscreenPolicy = window.findChild<QComboBox*>("offscreenPolicy");
        QVERIFY(offscreenPolicy);
        QCOMPARE(offscreenPolicy->currentData().toInt(), int(gui::OffscreenPolicy::ReleaseImage));
        offscreenPolicy->setCurrentIndex(offscreenPolicy->findData(int(gui::OffscreenPolicy::Keep)));
        auto* transitionPolicy = window.findChild<QComboBox*>("transitionPolicy");
        QVERIFY(transitionPolicy);
        QCOMPARE(transitionPolicy->currentData().toInt(), int(gui::TransitionPolicy::FirstLoadOrNonMemoryCache));
        for (auto* box : grid->findChildren<gui::ImageBox*>()) {
            QCOMPARE(box->config().fit(), gui::ImageFit::Cover);
            QCOMPARE(box->config().transition(), gui::ImageTransition::CrossFade);
        }
        transitionPolicy->setCurrentIndex(transitionPolicy->findData(int(gui::TransitionPolicy::Always)));
        for (auto* box : grid->findChildren<gui::ImageBox*>())
            QCOMPARE(box->config().offscreenPolicy(), gui::OffscreenPolicy::Keep);
        for (auto* box : grid->findChildren<gui::ImageBox*>())
            QCOMPARE(box->config().transitionPolicy(), gui::TransitionPolicy::Always);
        auto ready = [&window] {
            for (auto* box : window.findChildren<gui::ImageBox*>())
                if (box->state() != gui::ImageBoxState::Ready)
                    return false;
            return true;
        };
        QTRY_VERIFY(ready());
        const auto screenshot = qEnvironmentVariable("ASTER_GALLERY_SCREENSHOT");
        if (!screenshot.isEmpty()) {
            QTest::qWait(550);
            QVERIFY(window.grab().save(screenshot));
        }
        window.openFolder(directory.filePath("missing"));
        QCOMPARE(grid->imageCount(), 24);
    }

    void folderScanAndWebsiteStub() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QVERIFY(QDir(directory.path()).mkdir("nested"));
        QImage image(8, 8, QImage::Format_RGB32);
        image.fill(Qt::red);
        QVERIFY(image.save(directory.filePath("图片 2.PNG")));
        QVERIFY(image.save(directory.filePath("图片 10.png")));
        QVERIFY(image.save(directory.filePath("nested/child.png")));
        QFile other(directory.filePath("notes.txt"));
        QVERIFY(other.open(QIODevice::WriteOnly));
        other.write("not an image");
        other.close();
        gallery::ImageCatalog catalog;
        QSignalSpy ready(&catalog, &gallery::ImageCatalog::ready);
        QSignalSpy failed(&catalog, &gallery::ImageCatalog::failed);
        catalog.loadFolder(directory.path(), false);
        QTRY_COMPARE(ready.count(), 1);
        QCOMPARE(ready.takeFirst().first().toStringList().size(), 2);
        catalog.loadFolder(directory.path(), true);
        QTRY_COMPARE(ready.count(), 1);
        QCOMPARE(ready.takeFirst().first().toStringList().size(), 3);
        catalog.loadFolder(directory.path(), true);
        catalog.loadFolder(directory.filePath("nested"), false);
        QTRY_COMPARE(ready.count(), 1);
        QCOMPARE(ready.takeFirst().first().toStringList().size(), 1);
        catalog.loadWebsite(QUrl("https://example.test"));
        QCOMPARE(failed.count(), 1);
        catalog.loadFolder(directory.filePath("missing"), false);
        QCOMPARE(failed.count(), 2);
    }

    void sixColumnGridAndRecycling() {
        QTemporaryDir directory;
        QImage image(100, 60, QImage::Format_RGB32);
        image.fill(Qt::red);
        const auto path = directory.filePath("image.png");
        QVERIFY(image.save(path));
        auto active = QSharedPointer<cache::ActiveResourceStore>::create(4 * 1024 * 1024);
        auto pipeline = QSharedPointer<cache::ImagePipeline>::create(QSharedPointer<cache::RenderedMemoryCache>::create(4 * 1024 * 1024),
                                                                     QSharedPointer<cache::CachedSourceLoader>::create(nullptr), cache::ImageRenderer{}, 4,
                                                                     cache::EventSink{}, cache::PipelineResources{nullptr, active});
        gallery::ImageGrid grid(pipeline);
        grid.resize(1000, 600);
        grid.show();
        QStringList files;
        for (int i = 0; i < 1200; ++i)
            files.push_back(path);
        grid.setFiles(files);
        QCOMPARE(grid.imageCount(), 1200);
        QVERIFY(grid.visibleItemCount() >= 12);
        QVERIFY(grid.visibleItemCount() < 80);
        auto boxes = grid.findChildren<gui::ImageBox*>();
        auto allReady = [&grid] {
            for (auto* box : grid.findChildren<gui::ImageBox*>())
                if (box->state() != gui::ImageBoxState::Ready)
                    return false;
            return true;
        };
        QTRY_VERIFY(allReady());
        QSet<int> columns;
        for (auto* box : boxes)
            columns.insert(box->parentWidget()->x());
        QCOMPARE(columns.size(), 6);
        QPointer<gui::ImageBox> old = boxes.first();
        const int pageStep = grid.verticalScrollBar()->pageStep();
        grid.verticalScrollBar()->setValue(pageStep);
        QVERIFY(!old.isNull());
        QTRY_VERIFY(allReady());
        QVERIFY(grid.visibleItemCount() < 80);
        grid.verticalScrollBar()->setValue(pageStep * 2);
        QVERIFY(old.isNull());
        QTRY_VERIFY(allReady());
        grid.verticalScrollBar()->setValue(grid.verticalScrollBar()->maximum());
        QTRY_VERIFY(allReady());
        QVERIFY(grid.visibleItemCount() < 80);
        grid.setAlgorithm(gui::ImageScaleAlgorithm::Lanczos4);
        grid.setFit(gui::ImageFit::Cover);
        QTRY_VERIFY(allReady());
        grid.setFiles({});
        QCOMPARE(grid.visibleItemCount(), 0);
        QVERIFY(pipeline->waitForIdle());
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCOMPARE(active->stats().entries, qint64(0));
    }
};

QTEST_MAIN(GalleryTests)
#include "gallery_tests.moc"
