#include "imagebox.h"

#include "private/imageboxpresentation.h"

#include <QEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSharedPointer>
#include <QThread>
#include <QTimer>

#include <algorithm>

namespace aster::gui {
namespace {
bool sameTransformations(const QVector<QSharedPointer<cache::ImageTransformation>>& first, const QVector<QSharedPointer<cache::ImageTransformation>>& second) {
    if (first.size() != second.size())
        return false;
    for (int index = 0; index < first.size(); ++index)
        if (!first[index] || !second[index] || !(first[index]->identity() == second[index]->identity()))
            return false;
    return true;
}
} // namespace

ImageBox::ImageBox(QWidget* parent)
    : QWidget(parent)
    , presentation_(new detail::ImageBoxPresentation(*this, config_)) {
    qRegisterMetaType<ImageBoxState>("aster::gui::ImageBoxState");
    qRegisterMetaType<cache::ImageError>("aster::cache::ImageError");
    resizeTimer_.setSingleShot(true);
    resizeTimer_.setInterval(config_.resizeDebounce_);
    connect(&resizeTimer_, &QTimer::timeout, this, &ImageBox::startRequest);
}

ImageBox::~ImageBox() {
    invalidateRequest();
}

void ImageBox::setPipeline(QSharedPointer<cache::ImagePipeline> pipeline) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (pipeline_ == pipeline)
        return;

    invalidateRequest();
    pipeline_ = std::move(pipeline);
    startRequest();
}

QSharedPointer<cache::ImagePipeline> ImageBox::pipeline() const {
    return pipeline_;
}

void ImageBox::setConfig(const ImageBoxConfig& config) {
    Q_ASSERT(QThread::currentThread() == thread());
    const bool requestChanged = config_.fit_ != config.fit_ || config_.scaleAlgorithm_ != config.scaleAlgorithm_ ||
            !sameTransformations(config_.transformations_, config.transformations_) || config_.sizeBucket_ != config.sizeBucket_;
    const bool offscreenPolicyChanged = config_.offscreenPolicy_ != config.offscreenPolicy_;
    const bool transitionChanged = config_.transition_ != config.transition_ || config_.transitionPolicy_ != config.transitionPolicy_ ||
            config_.transitionDuration_ != config.transitionDuration_;
    const bool widgetFactoryChanged = config_.loadingErrorWidgetFactory_ != config.loadingErrorWidgetFactory_;
    QWidget* replacement = nullptr;
    if (widgetFactoryChanged)
        replacement = presentation_->createLoadingErrorWidget(config);
    config_ = config;
    resizeTimer_.setInterval(config.resizeDebounce_);
    presentation_->syncConfig(transitionChanged, widgetFactoryChanged, replacement);
    if (requestChanged && !source_.isEmpty() && (suspended_ || !isVisible()))
        resumePending_ = true;
    if (suspended_ || (offscreenPolicyChanged && !isVisible())) {
        suspendForHide();
        return;
    }
    if (source_.isEmpty())
        setState(ImageBoxState::Empty);
    else if (requestChanged)
        startRequest();
    presentation_->syncState(state_);
    update();
}

ImageBoxConfig ImageBox::config() const {
    return config_;
}

QString ImageBox::source() const {
    return source_;
}

ImageBoxState ImageBox::state() const {
    return state_;
}

QImage ImageBox::image() const {
    return presentation_->image();
}

cache::ImageError ImageBox::error() const {
    return error_;
}

QString ImageBox::errorString() const {
    return errorString_;
}

void ImageBox::setSource(const QString& source) {
    Q_ASSERT(QThread::currentThread() == thread());
    if (source_ == source)
        return;

    invalidateRequest();
    source_ = source;
    const auto generation = generation_;
    QPointer<ImageBox> guard(this);
    const auto changedSource = source_;
    Q_EMIT sourceChanged(changedSource);
    if (guard && generation_ == generation)
        startRequest();
}

void ImageBox::reload() {
    Q_ASSERT(QThread::currentThread() == thread());
    startRequest();
}

void ImageBox::invalidateRequest() {
    presentation_->finishTransition();
    resizeTimer_.stop();
    ++generation_;
    if (subscription_) {
        auto* old = subscription_.data();
        subscription_.clear();
        disconnect(old, nullptr, this, nullptr);
        delete old;
    }
}

void ImageBox::cancelCurrentRequest() {
    Q_ASSERT(QThread::currentThread() == thread());
    resumePending_ = false;
    if (state_ != ImageBoxState::Loading)
        return;

    invalidateRequest();
    error_ = cache::ImageError::None;
    errorString_.clear();
    setState(presentation_->image().isNull() ? ImageBoxState::Empty : ImageBoxState::Ready);
}

void ImageBox::startRequest() {
    invalidateRequest();
    resumePending_ = false;
    error_ = cache::ImageError::None;
    errorString_.clear();
    if (source_.isEmpty()) {
        requestedTarget_ = {};
        presentation_->clear();
        update();
        setState(ImageBoxState::Empty);
        return;
    }

    if (suspended_) {
        resumePending_ = true;
        setState(presentation_->image().isNull() ? ImageBoxState::Empty : ImageBoxState::Ready);
        return;
    }

    const auto dpr = requestDevicePixelRatio();
    const auto target = cache::physicalTargetSize(contentsRect().size(), dpr, config_.sizeBucket_);
    if (!target) {
        requestedTarget_ = {};
        setState(presentation_->image().isNull() ? ImageBoxState::Empty : ImageBoxState::Ready);
        return;
    }
    requestedTarget_ = *target.value;
    requestedDpr_ = dpr;

    const auto generation = generation_;
    QPointer<ImageBox> guard(this);
    setState(ImageBoxState::Loading);
    if (!guard || generation_ != generation)
        return;

    Q_EMIT loadingStarted();
    if (!guard || generation_ != generation)
        return;

    const auto pipeline = pipeline_;
    if (!pipeline) {
        QTimer::singleShot(0, this, [this, generation] {
            applyResult(generation, cache::ImageResult::failure(cache::ImageError::InvalidRequest, "ImagePipeline is not configured"));
        });
        return;
    }

    cache::RenderOptions options;
    options.physicalTargetSize = *target.value;
    options.dpr = dpr;
    options.fitMode = cache::fitName(config_.fit_);
    options.scaleAlgorithm = config_.scaleAlgorithm_;
    options.transformations = config_.transformations_;
    auto* subscription = pipeline->request(source_, options, this);
    if (!guard)
        return;
    if (generation_ != generation) {
        delete subscription;
        return;
    }

    subscription_ = subscription;
    connect(subscription, &cache::ImageSubscription::finished, this,
            [this, generation](cache::ImageResult result) { applyResult(generation, std::move(result)); });
}

void ImageBox::applyResult(quint64 generation, cache::ImageResult result) {
    if (generation != generation_)
        return;

    subscription_.clear();
    if (result && result.value->isNull())
        result = cache::ImageResult::failure(cache::ImageError::ProcessingError, "Empty image");

    QPointer<ImageBox> guard(this);
    if (result) {
        presentation_->accept(std::move(result), requestedTarget_, config_.fit_, requestDevicePixelRatio());
        error_ = cache::ImageError::None;
        errorString_.clear();
        update();
        setState(ImageBoxState::Ready);
        if (guard && generation_ == generation)
            Q_EMIT loaded();
        return;
    }

    if (result.error == cache::ImageError::Cancelled) {
        setState(presentation_->image().isNull() ? ImageBoxState::Empty : ImageBoxState::Ready);
        return;
    }

    error_ = result.error;
    errorString_ = result.message;
    setState(ImageBoxState::Error);
    if (guard && generation_ == generation)
        Q_EMIT loadFailed(result.error, result.message);
}

void ImageBox::setState(ImageBoxState state, bool queuedNotification) {
    if (state_ == state)
        return;

    state_ = state;
    presentation_->syncState(state);
    update();
    if (queuedNotification) {
        const auto generation = generation_;
        QTimer::singleShot(0, this, [this, state, generation] {
            if (generation_ == generation && state_ == state)
                Q_EMIT stateChanged(state);
        });
        return;
    }
    Q_EMIT stateChanged(state);
}

void ImageBox::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    if (config_.cornerRadius_ > 0) {
        const QRectF bounds(contentsRect());
        if (bounds.isEmpty())
            return;

        const qreal radius = std::min(config_.cornerRadius_, std::min(bounds.width(), bounds.height()) / 2);
        QPainterPath clip;
        clip.addRoundedRect(bounds, radius, radius);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setClipPath(clip, Qt::IntersectClip);
    }
    presentation_->paint(painter, requestDevicePixelRatio(), config_.fit_, state_ == ImageBoxState::Error);
}

qreal ImageBox::requestDevicePixelRatio() const {
    return devicePixelRatioF();
}

void ImageBox::scheduleSizeRequest() {
    if (source_.isEmpty())
        return;
    if (suspended_) {
        resumePending_ = true;
        return;
    }
    const auto dpr = requestDevicePixelRatio();
    const auto target = cache::physicalTargetSize(contentsRect().size(), dpr, config_.sizeBucket_);
    if (target && *target.value == requestedTarget_ && dpr == requestedDpr_ && !resizeTimer_.isActive())
        return;
    invalidateRequest();
    update();
    if (!target) {
        requestedTarget_ = {};
        setState(presentation_->image().isNull() ? ImageBoxState::Empty : ImageBoxState::Ready);
        return;
    }
    resizeTimer_.start();
    setState(ImageBoxState::Loading);
}

void ImageBox::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    presentation_->syncState(state_);
    scheduleSizeRequest();
}

bool ImageBox::event(QEvent* event) {
    const auto type = event->type();
    QPointer<ImageBox> guard(this);
    const bool result = QWidget::event(event);
    if (!guard)
        return result;
    if (type == QEvent::Hide) {
        suspendForHide();
        return result;
    }
    if (type == QEvent::Show) {
        suspended_ = false;
        const auto generation = generation_;
        QTimer::singleShot(0, this, [this, generation] {
            if (generation_ == generation && isVisible())
                resumeAfterShow();
        });
        return result;
    }
    if (type == QEvent::ScreenChangeInternal || type == QEvent::ContentsRectChange
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
        || type == QEvent::DevicePixelRatioChange
#endif
    )
        scheduleSizeRequest();
    return result;
}

bool ImageBox::isLoadingIndicatorActive() const {
    return presentation_->isLoadingIndicatorActive();
}

bool ImageBox::isTransitionRunning() const {
    return presentation_->isTransitionRunning();
}

qreal ImageBox::transitionProgress() const {
    return presentation_->transitionProgress();
}
} // namespace aster::gui
