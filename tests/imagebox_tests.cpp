#include "aster/cache/cache/renderedmemorycache.h"
#include "aster/cache/renderer/imagerenderer.h"
#include "aster/cache/source/cachedsourceloader.h"
#include "aster/gui/imagebox.h"
#include "aster/gui/private/imageboxpresentation.h"

#include <QBuffer>
#include <QFile>
#include <QLabel>
#include <QMetaEnum>
#include <QSharedPointer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <condition_variable>
#include <limits>
#include <mutex>
#include <stdexcept>

using namespace aster::cache;
using namespace aster::gui;

namespace {
ImageResult decode(const QByteArray& bytes, const RenderOptions&, const std::atomic<bool>&) {
    const auto image = QImage::fromData(bytes);
    return image.isNull() ? ImageResult::failure(ImageError::CorruptedEntry) : ImageResult::success(image);
}

QByteArray pixels(const QColor& color) {
    QImage image(8, 8, QImage::Format_ARGB32);
    image.fill(color);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return bytes;
}

class FakeNetwork final : public INetworkService {
public:
    std::atomic<int> calls{0};

    Result<NetworkResponse> fetch(const QUrl&, const NetworkFetchOptions&, const std::atomic<bool>&) override {
        ++calls;
        return Result<NetworkResponse>::success({200, {{"cache-control", "max-age=600"}}, pixels(Qt::green)});
    }
};

class ControlledLoader final : public IImageSourceLoader {
public:
    ~ControlledLoader() override = default;

    Result<SourceKey> key(const ImageSource& source) const override {
        return KeyBuilder().network(source.url);
    }

    Result<SourcePayload> load(const ImageSource& source, const SourceKey&, const SourceLoadOptions&, const std::atomic<bool>&) override {
        std::unique_lock<std::mutex> lock(mutex_);
        const auto name = source.url.path();
        ++started_[name];
        condition_.wait_for(lock, std::chrono::seconds(10), [&] { return released_.contains(name) || all_; });
        SourcePayload payload;
        payload.bytes = pixels(name == "/a" ? Qt::red : Qt::blue);
        // Deliberately ignore cancellation to simulate an uncooperative late producer.
        return Result<SourcePayload>::success(payload);
    }

    int started(const QString& name) {
        std::lock_guard<std::mutex> lock(mutex_);
        return started_.value(name);
    }

    void release(const QString& name) {
        std::lock_guard<std::mutex> lock(mutex_);
        released_.insert(name);
        condition_.notify_all();
    }

    void releaseAll() {
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

struct ControlledPipeline {
    QSharedPointer<ControlledLoader> loader = QSharedPointer<ControlledLoader>::create();
    QSharedPointer<ActiveResourceStore> active = QSharedPointer<ActiveResourceStore>::create(65536);
    QSharedPointer<ImagePipeline> pipeline = QSharedPointer<ImagePipeline>::create(QSharedPointer<RenderedMemoryCache>::create(65536), loader, decode, 4,
                                                                                   EventSink{}, PipelineResources{nullptr, active});

    ~ControlledPipeline() {
        loader->releaseAll();
        pipeline->waitForIdle();
    }
};

QSharedPointer<ImagePipeline> normalPipeline(QSharedPointer<INetworkService> network = {}) {
    return QSharedPointer<ImagePipeline>::create(
            QSharedPointer<RenderedMemoryCache>::create(65536),
            QSharedPointer<CachedSourceLoader>::create(QSharedPointer<EncodedMemoryCache>::create(65536, 32768), nullptr, network), decode);
}

void flushDeletes() {
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}
} // namespace

class ImageBoxTests : public QObject {
    Q_OBJECT

private Q_SLOTS:

    void configurationValidation() {
        QVERIFY(QMetaEnum::fromType<ImageTransition>().isValid());
        QVERIFY(QMetaEnum::fromType<OffscreenPolicy>().isValid());
        QVERIFY(QMetaEnum::fromType<ImageBoxState>().isValid());

        ImageBoxConfig config;
        config.fit(ImageFit::Cover)
                .scaleAlgorithm(ImageScaleAlgorithm::Lanczos3)
                .resizeDebounce(100)
                .sizeBucket(8)
                .transition(ImageTransition::Fade)
                .transitionDuration(300)
                .offscreenPolicy(OffscreenPolicy::ReleaseHandle);

        QVERIFY_EXCEPTION_THROWN(config.fit(static_cast<ImageFit>(-1)), std::invalid_argument);
        QVERIFY_EXCEPTION_THROWN(config.fit(static_cast<ImageFit>(int(ImageFit::ScaleDown) + 1)), std::invalid_argument);
        QCOMPARE(config.fit(), ImageFit::Cover);
        QVERIFY_EXCEPTION_THROWN(config.scaleAlgorithm(static_cast<ImageScaleAlgorithm>(-1)), std::invalid_argument);
        QVERIFY_EXCEPTION_THROWN(config.scaleAlgorithm(static_cast<ImageScaleAlgorithm>(int(ImageScaleAlgorithm::Lanczos4) + 1)), std::invalid_argument);
        QCOMPARE(config.scaleAlgorithm(), ImageScaleAlgorithm::Lanczos3);
        QVERIFY_EXCEPTION_THROWN(config.resizeDebounce(-1), std::invalid_argument);
        QVERIFY_EXCEPTION_THROWN(config.resizeDebounce(60001), std::invalid_argument);
        QCOMPARE(config.resizeDebounce(), 100);
        QVERIFY_EXCEPTION_THROWN(config.sizeBucket(0), std::invalid_argument);
        QVERIFY_EXCEPTION_THROWN(config.sizeBucket(4097), std::invalid_argument);
        QCOMPARE(config.sizeBucket(), 8);
        QVERIFY_EXCEPTION_THROWN(config.transition(static_cast<ImageTransition>(-1)), std::invalid_argument);
        QVERIFY_EXCEPTION_THROWN(config.transition(static_cast<ImageTransition>(int(ImageTransition::FadeZoom) + 1)), std::invalid_argument);
        QCOMPARE(config.transition(), ImageTransition::Fade);
        QVERIFY_EXCEPTION_THROWN(config.transitionDuration(-1), std::invalid_argument);
        QVERIFY_EXCEPTION_THROWN(config.transitionDuration(60001), std::invalid_argument);
        QCOMPARE(config.transitionDuration(), 300);
        QVERIFY_EXCEPTION_THROWN(config.offscreenPolicy(static_cast<OffscreenPolicy>(-1)), std::invalid_argument);
        QVERIFY_EXCEPTION_THROWN(config.offscreenPolicy(static_cast<OffscreenPolicy>(int(OffscreenPolicy::ReleaseImage) + 1)), std::invalid_argument);
        QCOMPARE(config.offscreenPolicy(), OffscreenPolicy::ReleaseHandle);

        config.resizeDebounce(0).resizeDebounce(60000).sizeBucket(1).sizeBucket(4096);
        config.transitionDuration(0).transitionDuration(60000);
    }

    void roundedCorners() {
        ControlledPipeline fixture;
        fixture.loader->releaseAll();
        fixture.pipeline = QSharedPointer<ImagePipeline>::create(QSharedPointer<RenderedMemoryCache>::create(65536), fixture.loader, ImageRenderer{});
        ImageBox box;
        box.resize(40, 40);
        box.setPipeline(fixture.pipeline);
        auto palette = box.palette();
        palette.setColor(QPalette::Window, Qt::black);
        box.setPalette(palette);
        box.setAutoFillBackground(true);
        box.show();
        QImage placeholder(box.size(), QImage::Format_RGB32);
        placeholder.fill(Qt::yellow);
        box.setConfig(ImageBoxConfig().fit(ImageFit::Fill).placeholder(placeholder).build());
        QVERIFY(!box.config().placeholder().isNull());
        QCOMPARE(box.config().placeholder().pixelColor(0, 0), QColor(Qt::yellow));
        const auto render = [&] {
            QImage canvas(box.size(), QImage::Format_RGB32);
            canvas.fill(Qt::black);
            box.render(&canvas);
            return canvas;
        };

        QCOMPARE(box.config().cornerRadius(), qreal(0));
        QCOMPARE(render().pixelColor(0, 0), QColor(Qt::yellow));
        box.setConfig(box.config().cornerRadius(12).build());
        QCOMPARE(box.config().cornerRadius(), qreal(12));
        QCOMPARE(render().pixelColor(0, 0), QColor(Qt::black));
        QCOMPARE(render().pixelColor(box.rect().center()), QColor(Qt::yellow));
        QVERIFY_EXCEPTION_THROWN(ImageBoxConfig().cornerRadius(-1), std::invalid_argument);
        QVERIFY_EXCEPTION_THROWN(ImageBoxConfig().cornerRadius(std::numeric_limits<qreal>::infinity()), std::invalid_argument);
        QVERIFY_EXCEPTION_THROWN(ImageBoxConfig().cornerRadius(std::numeric_limits<qreal>::quiet_NaN()), std::invalid_argument);
        QCOMPARE(box.config().cornerRadius(), qreal(12));

        box.setSource("https://example.test/a");
        QTRY_COMPARE(box.state(), ImageBoxState::Ready);
        QCOMPARE(render().pixelColor(0, 0), QColor(Qt::black));
        QCOMPARE(render().pixelColor(box.rect().center()), QColor(Qt::red));
        QSignalSpy loading(&box, &ImageBox::loadingStarted);
        box.setConfig(box.config().cornerRadius(0).build());
        QCOMPARE(render().pixelColor(0, 0), QColor(Qt::red));
        box.setConfig(box.config().cornerRadius(1000).build());
        QCOMPARE(render().pixelColor(0, 0), QColor(Qt::black));
        QCOMPARE(render().pixelColor(box.rect().center()), QColor(Qt::red));
        QCOMPARE(loading.count(), 0);

        box.setConfig(box.config().transition(ImageTransition::CrossFade).transitionDuration(1000).build());
        box.setSource("https://example.test/b");
        QTRY_COMPARE(box.state(), ImageBoxState::Ready);
        QTRY_VERIFY(box.transitionProgress() >= 0.3);
        QVERIFY(box.isTransitionRunning());
        const auto blended = render();
        QCOMPARE(blended.pixelColor(0, 0), QColor(Qt::black));
        QVERIFY(blended.pixelColor(box.rect().center()).red() > 0);
        QVERIFY(blended.pixelColor(box.rect().center()).blue() > 0);
        box.setConfig(box.config().transition(ImageTransition::None).build());
        box.setContentsMargins(4, 4, 4, 4);
        QCOMPARE(render().pixelColor(4, 4), QColor(Qt::black));
        QCOMPARE(render().pixelColor(box.rect().center()), QColor(Qt::blue));

        placeholder.fill(Qt::green);
        box.setConfig(box.config().errorImage(placeholder).errorReplacesImage().build());
        box.setPipeline(normalPipeline());
        box.setSource(":/missing-rounded.png");
        QTRY_COMPARE(box.state(), ImageBoxState::Error);
        QCOMPARE(render().pixelColor(4, 4), QColor(Qt::black));
        QCOMPARE(render().pixelColor(box.rect().center()), QColor(Qt::green));
    }

    void loadingErrorReplacements() {
        const auto widgetFactory = [](QWidget* parent) {
            auto* label = new QLabel("custom status", parent);
            label->setObjectName("customStatus");
            label->setStyleSheet("background: rgb(12, 34, 56); color: white;");
            return label;
        };
        ControlledPipeline fixture;
        ImageBox box;
        box.resize(32, 32);
        box.setPipeline(fixture.pipeline);
        box.show();
        box.setConfig(box.config().loadingErrorWidget(widgetFactory).build());
        QPointer<QLabel> replacement = box.findChild<QLabel*>("customStatus", Qt::FindDirectChildrenOnly);
        QVERIFY(replacement);
        auto* originalReplacement = replacement.data();
        const auto throwingFactory = [](QWidget*) -> QWidget* { throw std::runtime_error("factory failure"); };
        QVERIFY_EXCEPTION_THROWN(box.setConfig(box.config().loadingErrorWidget(throwingFactory).build()), std::runtime_error);
        QCOMPARE(box.findChild<QLabel*>("customStatus", Qt::FindDirectChildrenOnly), originalReplacement);
        QVERIFY(originalReplacement->parentWidget() == &box);
        box.setSource("https://example.test/a");
        QTRY_COMPARE(box.state(), ImageBoxState::Loading);
        QVERIFY(replacement->isVisible());
        fixture.loader->release("/a");
        QTRY_COMPARE(box.state(), ImageBoxState::Ready);
        QVERIFY(!replacement->isVisible());

        box.setPipeline(normalPipeline());
        box.setSource(":/missing-status.png");
        QTRY_COMPARE(box.state(), ImageBoxState::Error);
        QVERIFY(replacement->isVisible());
        box.setConfig(box.config().loadingErrorWidget(ImageBoxConfig::LoadingErrorWidgetFactory{}).build());
        QVERIFY(replacement.isNull());

        QImage status(box.size(), QImage::Format_RGB32);
        status.fill(Qt::magenta);
        box.setConfig(box.config().loadingErrorImage(status).build());
        QVERIFY(!box.config().loadingErrorImage().isNull());
        box.setPipeline(fixture.pipeline);
        box.setSource("https://example.test/a");
        QTRY_COMPARE(box.state(), ImageBoxState::Loading);
        QImage canvas(box.size(), QImage::Format_RGB32);
        canvas.fill(Qt::black);
        box.render(&canvas);
        QCOMPARE(canvas.pixelColor(box.rect().center()), QColor(Qt::magenta));
        fixture.loader->release("/a");
        QTRY_COMPARE(box.state(), ImageBoxState::Ready);
        QVERIFY(box.config().loadingErrorImage() == status);
        box.setConfig(box.config().loadingErrorImage({}).loadingErrorWidget(widgetFactory).build());
        replacement = box.findChild<QLabel*>("customStatus", Qt::FindDirectChildrenOnly);
        QVERIFY(replacement);
        box.setSource(":/missing-status-2.png");
        QTRY_COMPARE(box.state(), ImageBoxState::Error);
        QVERIFY(replacement->isVisible());

        const auto sharedConfig = ImageBoxConfig().loadingErrorWidget(widgetFactory).build();
        ImageBox first;
        ImageBox second;
        first.setConfig(sharedConfig);
        second.setConfig(sharedConfig);
        auto* firstWidget = first.findChild<QLabel*>("customStatus", Qt::FindDirectChildrenOnly);
        auto* secondWidget = second.findChild<QLabel*>("customStatus", Qt::FindDirectChildrenOnly);
        QVERIFY(firstWidget);
        QVERIFY(secondWidget);
        QVERIFY(firstWidget != secondWidget);
        QCOMPARE(firstWidget->parentWidget(), &first);
        QCOMPARE(secondWidget->parentWidget(), &second);

        ControlledPipeline deletionFixture;
        ImageBox deletionBox;
        deletionBox.resize(32, 32);
        deletionBox.setPipeline(deletionFixture.pipeline);
        deletionBox.setConfig(ImageBoxConfig().loadingErrorWidget(widgetFactory).loadingIndicator().build());
        deletionBox.show();
        deletionBox.setSource("https://example.test/deleted-widget");
        QTRY_COMPARE(deletionBox.state(), ImageBoxState::Loading);
        auto* deletedWidget = deletionBox.findChild<QLabel*>("customStatus", Qt::FindDirectChildrenOnly);
        QVERIFY(deletedWidget);
        delete deletedWidget;
        QTRY_VERIFY(deletionBox.isLoadingIndicatorActive());
        deletionFixture.loader->release("/deleted-widget");
        QTRY_COMPARE(deletionBox.state(), ImageBoxState::Ready);
    }

    void offscreenPolicies() {
        for (auto policy : {OffscreenPolicy::Keep, OffscreenPolicy::ReleaseHandle, OffscreenPolicy::ReleaseImage}) {
            ControlledPipeline fixture;
            fixture.loader->releaseAll();
            ImageBox box;
            box.resize(8, 8);
            box.setPipeline(fixture.pipeline);
            box.setConfig(ImageBoxConfig().offscreenPolicy(policy).build());
            QCOMPARE(box.config().offscreenPolicy(), policy);
            if (policy == OffscreenPolicy::Keep) {
                box.show();
                box.hide();
            }
            box.setSource("https://example.test/a");
            QCOMPARE(box.state(), ImageBoxState::Empty);
            QCOMPARE(fixture.loader->started("/a"), 0);
            box.show();
            QTRY_COMPARE(box.state(), ImageBoxState::Ready);
            flushDeletes();
            QCOMPARE(fixture.active->stats().entries, qint64(1));
            const auto hits = fixture.pipeline->cacheStats().renderedMemory.hits;
            QSignalSpy loaded(&box, &ImageBox::loaded);
            box.hide();
            flushDeletes();
            QCOMPARE(fixture.active->stats().entries, policy == OffscreenPolicy::Keep ? qint64(1) : qint64(0));
            QCOMPARE(box.image().isNull(), policy == OffscreenPolicy::ReleaseImage);
            box.show();
            if (policy != OffscreenPolicy::Keep)
                QTRY_COMPARE(loaded.count(), 1);
            QCoreApplication::processEvents();
            QTRY_COMPARE(box.state(), ImageBoxState::Ready);
            QCOMPARE(box.image().pixelColor(0, 0), QColor(Qt::red));
            QCOMPARE(loaded.count(), policy == OffscreenPolicy::Keep ? 0 : 1);
            if (policy != OffscreenPolicy::Keep)
                QVERIFY(fixture.pipeline->cacheStats().renderedMemory.hits > hits);
            flushDeletes();
            QCOMPARE(fixture.active->stats().entries, qint64(1));
            box.hide();
            box.setConfig(box.config().offscreenPolicy(OffscreenPolicy::ReleaseImage).build());
            QVERIFY(box.image().isNull());
            QCOMPARE(fixture.active->stats().entries, qint64(0));
            box.setSource("");
            box.show();
            QCOMPARE(box.state(), ImageBoxState::Empty);
        }
    }

    void hiddenRequestsAndReentrancy() {
        ControlledPipeline fixture;
        QWidget parent;
        ImageBox box(&parent);
        box.resize(8, 8);
        box.setPipeline(fixture.pipeline);
        box.setConfig(ImageBoxConfig().loadingIndicator().build());
        parent.show();
        box.setSource("https://example.test/a");
        QTRY_COMPARE(fixture.loader->started("/a"), 1);
        QSignalSpy loaded(&box, &ImageBox::loaded);
        QSignalSpy failed(&box, &ImageBox::loadFailed);
        parent.hide();
        QCOMPARE(box.state(), ImageBoxState::Empty);
        QVERIFY(!box.isLoadingIndicatorActive());
        QVERIFY(box.findChildren<ImageSubscription*>().isEmpty());
        fixture.loader->release("/a");
        QVERIFY(fixture.pipeline->waitForIdle());
        QCoreApplication::processEvents();
        QCOMPARE(loaded.count(), 0);
        QCOMPARE(failed.count(), 0);
        box.setSource("https://example.test/b");
        box.resize(12, 12);
        QCOMPARE(fixture.loader->started("/b"), 0);
        parent.show();
        QTRY_COMPARE(fixture.loader->started("/b"), 1);
        fixture.loader->release("/b");
        QTRY_COMPARE(box.state(), ImageBoxState::Ready);
        QCOMPARE(box.image().pixelColor(0, 0), QColor(Qt::blue));
        box.setConfig(box.config().resizeDebounce(500).build());
        box.resize(20, 20);
        QCOMPARE(box.state(), ImageBoxState::Loading);
        box.hide();
        box.cancelCurrentRequest();
        const auto calls = fixture.loader->started("/b");
        box.show();
        // The changed size still needs rendering, but the cancelled debounce never fires hidden.
        QTRY_COMPARE(box.state(), ImageBoxState::Ready);
        QVERIFY(fixture.loader->started("/b") <= calls + 1);

        QPointer<ImageBox> victim = new ImageBox;
        victim->setPipeline(fixture.pipeline);
        victim->show();
        connect(victim, &ImageBox::stateChanged, this, [victim](ImageBoxState state) {
            if (state == ImageBoxState::Empty)
                delete victim.data();
        });
        victim->setSource("https://example.test/pending");
        victim->hide();
        QTRY_VERIFY(victim.isNull());
        fixture.loader->releaseAll();
    }

    void rapidVisibility() {
        ControlledPipeline fixture;
        ImageBox box;
        box.resize(8, 8);
        box.setPipeline(fixture.pipeline);
        box.setConfig(ImageBoxConfig().offscreenPolicy(OffscreenPolicy::ReleaseImage).build());
        QSignalSpy loaded(&box, &ImageBox::loaded);
        for (int i = 0; i < 100; ++i) {
            box.show();
            box.setSource(QString("https://example.test/pending-%1").arg(i));
            box.hide();
            QVERIFY(box.findChildren<ImageSubscription*>().isEmpty());
            QCOMPARE(box.state(), ImageBoxState::Empty);
        }
        fixture.loader->releaseAll();
        QVERIFY(fixture.pipeline->waitForIdle());
        QCoreApplication::processEvents();
        QCOMPARE(loaded.count(), 0);
        QVERIFY(box.image().isNull());
        QCOMPARE(fixture.active->stats().entries, qint64(0));
        box.setSource("https://example.test/a");
        box.show();
        QTRY_COMPARE(box.state(), ImageBoxState::Ready);
        QCOMPARE(box.image().pixelColor(0, 0), QColor(Qt::red));
        QCOMPARE(loaded.count(), 1);
    }

    void largeOffscreenList() {
        constexpr int count = 1056;
        constexpr int visibleCount = 32;
        auto loader = QSharedPointer<ControlledLoader>::create();
        loader->releaseAll();
        auto active = QSharedPointer<ActiveResourceStore>::create(4 * 1024 * 1024);
        auto pipeline = QSharedPointer<ImagePipeline>::create(QSharedPointer<RenderedMemoryCache>::create(4 * 1024 * 1024), loader, decode, 4, EventSink{},
                                                              PipelineResources{nullptr, active});
        QWidget parent;
        parent.resize(256, 256);
        QVector<ImageBox*> boxes;
        for (int i = 0; i < count; ++i) {
            auto* box = new ImageBox(&parent);
            box->resize(8, 8);
            box->setConfig(ImageBoxConfig().offscreenPolicy(OffscreenPolicy::ReleaseImage).build());
            box->hide();
            box->setPipeline(pipeline);
            box->setSource(QString("https://example.test/image-%1").arg(i));
            boxes.push_back(box);
        }
        parent.show();
        QCOMPARE(pipeline->stats().renderInFlight, 0);
        for (int pass = 0; pass < 2; ++pass) {
            for (int start = 0; start < count; start += visibleCount) {
                for (int i = start; i < start + visibleCount; ++i)
                    boxes[i]->show();
                auto ready = [&] {
                    for (int i = start; i < start + visibleCount; ++i)
                        if (boxes[i]->state() != ImageBoxState::Ready)
                            return false;
                    return true;
                };
                QTRY_VERIFY(ready());
                flushDeletes();
                QVERIFY(active->stats().entries <= visibleCount);
                for (int i = start; i < start + visibleCount; ++i) {
                    QCOMPARE(boxes[i]->image().pixelColor(0, 0), QColor(Qt::blue));
                    boxes[i]->hide();
                    QVERIFY(boxes[i]->image().isNull());
                    QVERIFY(boxes[i]->findChildren<ImageSubscription*>().isEmpty());
                }
                flushDeletes();
                QCOMPARE(active->stats().entries, qint64(0));
            }
        }
        QVERIFY(pipeline->waitForIdle());
        QCOMPARE(pipeline->stats().renderInFlight, 0);
        QCOMPARE(active->stats().bytes, qint64(0));
        QVERIFY(pipeline->cacheStats().renderedMemory.totalBytes <= 4 * 1024 * 1024);
    }

    void presentationOwnership() {
        auto* box = new ImageBox;
        QPointer<detail::ImageBoxPresentation> presentation = box->findChild<detail::ImageBoxPresentation*>(QString(), Qt::FindDirectChildrenOnly);
        QVERIFY(presentation);
        QCOMPARE(presentation->parent(), box);
        QSignalSpy destroyed(presentation.data(), &QObject::destroyed);
        delete box;
        QVERIFY(presentation.isNull());
        QCOMPARE(destroyed.count(), 1);
    }

    void presentationAndTransitions() {
        ControlledPipeline fixture;
        QWidget host;
        ImageBox box(&host);
        box.resize(8, 8);
        box.setPipeline(fixture.pipeline);
        QImage placeholder(8, 8, QImage::Format_RGB32);
        placeholder.fill(Qt::yellow);
        QImage error(8, 8, QImage::Format_RGB32);
        error.fill(Qt::green);
        box.setConfig(ImageBoxConfig().placeholder(placeholder).errorImage(error).build());
        host.show();
        box.show();
        auto pixel = [&] {
            QImage canvas(8, 8, QImage::Format_RGB32);
            canvas.fill(Qt::black);
            box.render(&canvas);
            return canvas.pixelColor(box.rect().center());
        };
        QCOMPARE(pixel(), QColor(Qt::yellow));
        box.setConfig(box.config().loadingIndicator().build());
        box.setSource("https://example.test/a");
        QVERIFY(box.isLoadingIndicatorActive());
        box.hide();
        QVERIFY(!box.isLoadingIndicatorActive());
        box.show();
        QTRY_VERIFY(box.isLoadingIndicatorActive());
        fixture.loader->release("/a");
        QTRY_COMPARE(box.state(), ImageBoxState::Ready);
        QVERIFY(!box.isLoadingIndicatorActive());
        QVERIFY(!box.isTransitionRunning());
        QCOMPARE(pixel(), QColor(Qt::red));
        flushDeletes();
        QCOMPARE(fixture.active->stats().entries, qint64(1));

        box.setConfig(box.config().transition(ImageTransition::CrossFade).transitionDuration(1000).build());
        box.setSource("https://example.test/b");
        QVERIFY(!box.isLoadingIndicatorActive());
        box.setConfig(box.config().loadingOverlay().build());
        QVERIFY(box.isLoadingIndicatorActive());
        fixture.loader->release("/b");
        QTRY_COMPARE(box.state(), ImageBoxState::Ready);
        QVERIFY(box.isTransitionRunning());
        box.setConfig(box.config().cornerRadius(2).build());
        QVERIFY(box.isTransitionRunning());
        box.setConfig(box.config());
        QVERIFY(box.isTransitionRunning());
        flushDeletes();
        QCOMPARE(fixture.active->stats().entries, qint64(2));
        QTRY_VERIFY(box.transitionProgress() >= 0.4);
        const auto progress = box.transitionProgress();
        const auto blended = pixel();
        QVERIFY(qAbs(blended.red() - qRound(255 * (1 - progress))) <= 3);
        QVERIFY(qAbs(blended.blue() - qRound(255 * progress)) <= 3);
        QVERIFY(blended.green() <= 3);
        QTRY_VERIFY_WITH_TIMEOUT(!box.isTransitionRunning(), 2000);
        QCOMPARE(box.transitionProgress(), qreal(1));
        QCOMPARE(fixture.active->stats().entries, qint64(1));
        QCOMPARE(pixel(), QColor(Qt::blue));

        fixture.loader->releaseAll();
        for (auto transition : {ImageTransition::Fade, ImageTransition::CrossFade, ImageTransition::Slide, ImageTransition::Zoom, ImageTransition::FadeZoom}) {
            box.setConfig(box.config().transition(transition).transitionDuration(500).build());
            box.setSource(box.source().endsWith("/a") ? "https://example.test/b" : "https://example.test/a");
            QTRY_COMPARE(box.state(), ImageBoxState::Ready);
            QVERIFY(box.isTransitionRunning());
            pixel();
            box.reload();
            QTRY_COMPARE(box.state(), ImageBoxState::Ready);
            box.hide();
            QVERIFY(!box.isTransitionRunning());
            flushDeletes();
            QCOMPARE(fixture.active->stats().entries, qint64(1));
            box.show();
        }
        box.setConfig(box.config().transitionDuration(0).build());
        box.reload();
        QTRY_COMPARE(box.state(), ImageBoxState::Ready);
        QVERIFY(!box.isTransitionRunning());
        box.setPipeline(normalPipeline());
        box.setSource(":/missing.png");
        QTRY_COMPARE(box.state(), ImageBoxState::Error);
        QVERIFY(!box.isTransitionRunning());
        QCOMPARE(pixel(), box.image().pixelColor(0, 0));
        box.setConfig(box.config().errorReplacesImage().build());
        QCOMPARE(pixel(), QColor(Qt::green));
        box.setSource("");
        QCOMPARE(pixel(), QColor(Qt::yellow));
        flushDeletes();
        QCOMPARE(fixture.active->stats().entries, qint64(0));
        box.setSource(":/missing.png");
        QTRY_COMPARE(box.state(), ImageBoxState::Error);
        QCOMPARE(pixel(), QColor(Qt::green));

        auto* transient = new ImageBox;
        transient->resize(8, 8);
        transient->setPipeline(fixture.pipeline);
        transient->setConfig(ImageBoxConfig().transition(ImageTransition::CrossFade).transitionDuration(1000).build());
        transient->show();
        transient->setSource("https://example.test/a");
        QTRY_COMPARE(transient->state(), ImageBoxState::Ready);
        transient->setSource("https://example.test/b");
        QTRY_COMPARE(transient->state(), ImageBoxState::Ready);
        QVERIFY(transient->isTransitionRunning());
        delete transient;
        QVERIFY(fixture.pipeline->waitForIdle());
        flushDeletes();
        QCOMPARE(fixture.active->stats().entries, qint64(0));
    }

    void dimensionsAlgorithmsAndResize() {
        ImageBox configBox;
        configBox.resize(32, 32);
        configBox.setConfig(ImageBoxConfig()
                                    .fit(ImageFit::Cover)
                                    .scaleAlgorithm(ImageScaleAlgorithm::Lanczos3)
                                    .resizeDebounce(120)
                                    .sizeBucket(8)
                                    .cornerRadius(6)
                                    .transition(ImageTransition::Fade)
                                    .transitionDuration(300)
                                    .offscreenPolicy(OffscreenPolicy::ReleaseImage)
                                    .build());
        QCOMPARE(configBox.config().fit(), ImageFit::Cover);
        QCOMPARE(configBox.config().scaleAlgorithm(), ImageScaleAlgorithm::Lanczos3);
        QCOMPARE(configBox.config().resizeDebounce(), 120);
        QCOMPARE(configBox.config().sizeBucket(), 8);
        QCOMPARE(configBox.config().cornerRadius(), qreal(6));
        QCOMPARE(configBox.config().transition(), ImageTransition::Fade);
        QCOMPARE(configBox.config().transitionDuration(), 300);
        QCOMPARE(configBox.config().offscreenPolicy(), OffscreenPolicy::ReleaseImage);

        class DprBox : public ImageBox {
        public:
            qreal ratio = 1;

        protected:
            qreal requestDevicePixelRatio() const override {
                return ratio;
            }
        };

        auto loader = QSharedPointer<ControlledLoader>::create();
        loader->releaseAll();
        std::mutex mutex;
        QVector<RenderOptions> recorded;
        QThread* renderThread = nullptr;
        auto pipeline = QSharedPointer<ImagePipeline>::create(QSharedPointer<RenderedMemoryCache>::create(16 * 1024 * 1024), loader,
                                                              [&](const QByteArray& data, const RenderOptions& options, const std::atomic<bool>& token) {
                                                                  {
                                                                      std::lock_guard<std::mutex> lock(mutex);
                                                                      recorded.push_back(options);
                                                                      renderThread = QThread::currentThread();
                                                                  }
                                                                  return ImageRenderer{}(data, options, token);
                                                              });
        DprBox box;
        box.setPipeline(pipeline);
        box.setConfig(ImageBoxConfig().fit(ImageFit::Fill).build());
        box.resize(101, 51);
        QSignalSpy loaded(&box, &ImageBox::loaded);
        QSignalSpy started(&box, &ImageBox::loadingStarted);
        QCOMPARE(box.config().scaleAlgorithm(), ImageScaleAlgorithm::QtSmooth);
        box.setSource("https://example.test/a");
        QTRY_COMPARE(loaded.count(), 1);
        QCOMPARE(box.image().size(), QSize(101, 51));
        {
            std::lock_guard<std::mutex> lock(mutex);
            QCOMPARE(recorded.back().scaleAlgorithm, ImageScaleAlgorithm::QtSmooth);
            QVERIFY(renderThread != QThread::currentThread());
        }
        for (const qreal ratio : {1.25, 1.5, 2.0, 3.0}) {
            const int previous = loaded.count();
            box.ratio = ratio;
            QEvent change(QEvent::ScreenChangeInternal);
            QCoreApplication::sendEvent(&box, &change);
            QTRY_COMPARE(loaded.count(), previous + 1);
            QCOMPARE(box.image().size(), *physicalTargetSize(QSizeF(101, 51), ratio).value);
            QCOMPARE(box.image().devicePixelRatio(), ratio);
        }
        box.setConfig(box.config().scaleAlgorithm(ImageScaleAlgorithm::Lanczos4).build());
        QTRY_COMPARE(box.state(), ImageBoxState::Ready);
        {
            std::lock_guard<std::mutex> lock(mutex);
            QCOMPARE(recorded.back().scaleAlgorithm, ImageScaleAlgorithm::Lanczos4);
        }
        box.setConfig(box.config().scaleAlgorithm(ImageScaleAlgorithm::Bicubic).build());
        QTRY_COMPARE(box.state(), ImageBoxState::Ready);
        {
            std::lock_guard<std::mutex> lock(mutex);
            QCOMPARE(recorded.back().scaleAlgorithm, ImageScaleAlgorithm::Bicubic);
        }
        box.ratio = 1;
        box.setConfig(box.config().scaleAlgorithm(ImageScaleAlgorithm::QtSmooth).build());
        QTRY_COMPARE(box.state(), ImageBoxState::Ready);
        box.show();
        QCoreApplication::processEvents();
        const auto hiddenConfigCount = loaded.count();
        box.hide();
        box.setConfig(box.config().scaleAlgorithm(ImageScaleAlgorithm::Lanczos4).build());
        QCOMPARE(loaded.count(), hiddenConfigCount);
        box.show();
        QTRY_COMPARE(loaded.count(), hiddenConfigCount + 1);
        {
            std::lock_guard<std::mutex> lock(mutex);
            QCOMPARE(recorded.back().scaleAlgorithm, ImageScaleAlgorithm::Lanczos4);
        }
        box.setConfig(box.config().resizeDebounce(150).build());
        const auto count = started.count();
        for (int i = 0; i < 1000; ++i)
            box.resize(100 + i % 99, 60 + i % 47);
        box.resize(301, 201);
        QCOMPARE(started.count(), count);
        QTRY_COMPARE(box.state(), ImageBoxState::Ready);
        QCOMPARE(started.count(), count + 1);
        QCOMPARE(box.image().size(), QSize(301, 201));
        box.setConfig(box.config().sizeBucket(16).build());
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

    void sourcesAndPainting() {
        auto network = QSharedPointer<FakeNetwork>::create();
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

    void failuresAndEmpty() {
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

    void lateResultsAndHandles() {
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

    void cancellationAndDestruction() {
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

    void rapidSwitchAndPipelineReplacement() {
        ControlledPipeline fixture;
        ImageBox box;
        box.setPipeline(fixture.pipeline);
        fixture.loader->releaseAll();
        for (int i = 0; i < 1000; ++i) {
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

    void signalOrderAndReentrancy() {
        ImageBox box;
        box.setPipeline(normalPipeline());
        QStringList order;
        connect(&box, &ImageBox::stateChanged, &box, [&](ImageBoxState state) { order << (state == ImageBoxState::Loading ? "loading" : "ready"); });
        connect(&box, &ImageBox::loadingStarted, &box, [&] { order << "started"; });
        connect(&box, &ImageBox::loaded, &box, [&] { order << "loaded"; });
        box.setSource(":/aster-test/sample.ppm");
        QTRY_COMPARE(order.size(), 4);
        QCOMPARE(order, QStringList({"loading", "started", "ready", "loaded"}));

        auto connection = connect(&box, &ImageBox::stateChanged, &box, [&](ImageBoxState state) {
            if (state == ImageBoxState::Ready)
                box.setSource("");
        });
        QSignalSpy loaded(&box, &ImageBox::loaded);
        box.reload();
        QTRY_COMPARE(box.state(), ImageBoxState::Empty);
        QCOMPARE(loaded.count(), 0);
        disconnect(connection);

        connection = connect(&box, &ImageBox::loadingStarted, &box, [&] { box.cancelCurrentRequest(); });
        box.setSource(":/aster-test/sample.ppm");
        QCOMPARE(box.state(), ImageBoxState::Empty);
        QVERIFY(box.findChildren<ImageSubscription*>().isEmpty());
        disconnect(connection);

        for (const auto terminal : {false, true}) {
            QPointer<ImageBox> victim = new ImageBox;
            victim->setPipeline(box.pipeline());
            connect(victim, &ImageBox::stateChanged, this, [victim, terminal](ImageBoxState state) {
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
