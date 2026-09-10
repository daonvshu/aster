#include "imagebox.h"

#include "private/imageboxpresentation.h"

#include <QEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSharedPointer>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace aster::gui
{
ImageBox::ImageBox(QWidget* parent)
    : QWidget(parent), presentation_(new detail::ImageBoxPresentation(*this))
{
    qRegisterMetaType<ImageBoxState>("aster::gui::ImageBoxState");
    qRegisterMetaType<cache::ImageError>("aster::cache::ImageError");
    resizeTimer_.setSingleShot(true);
    resizeTimer_.setInterval(75);
    connect(&resizeTimer_, &QTimer::timeout, this, &ImageBox::startRequest);
}

ImageBox::~ImageBox()
{
    invalidateRequest();
}

void ImageBox::setPipeline(QSharedPointer<cache::ImagePipeline> pipeline)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (pipeline_ == pipeline)
        return;

    invalidateRequest();
    pipeline_ = std::move(pipeline);
    startRequest();
}

QSharedPointer<cache::ImagePipeline> ImageBox::pipeline() const
{
    return pipeline_;
}

QString ImageBox::source() const
{
    return source_;
}

ImageBoxState ImageBox::state() const
{
    return state_;
}

QImage ImageBox::image() const
{
    return presentation_->image();
}

cache::ImageError ImageBox::error() const
{
    return error_;
}

QString ImageBox::errorString() const
{
    return errorString_;
}

void ImageBox::setSource(const QString& source)
{
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

void ImageBox::reload()
{
    Q_ASSERT(QThread::currentThread() == thread());
    startRequest();
}

void ImageBox::invalidateRequest()
{
    presentation_->finishTransition();
    resizeTimer_.stop();
    ++generation_;
    if (subscription_)
    {
        auto* old = subscription_.data();
        subscription_.clear();
        disconnect(old, nullptr, this, nullptr);
        delete old;
    }
}

void ImageBox::cancelCurrentRequest()
{
    Q_ASSERT(QThread::currentThread() == thread());
    resumePending_ = false;
    if (state_ != ImageBoxState::Loading)
        return;

    invalidateRequest();
    error_ = cache::ImageError::None;
    errorString_.clear();
    setState(presentation_->image().isNull() ? ImageBoxState::Empty : ImageBoxState::Ready);
}

void ImageBox::startRequest()
{
    invalidateRequest();
    resumePending_ = false;
    error_ = cache::ImageError::None;
    errorString_.clear();
    if (source_.isEmpty())
    {
        requestedTarget_ = {};
        presentation_->clear();
        update();
        setState(ImageBoxState::Empty);
        return;
    }

    if (suspended_)
    {
        resumePending_ = true;
        setState(presentation_->image().isNull() ? ImageBoxState::Empty : ImageBoxState::Ready);
        return;
    }

    const auto dpr = requestDevicePixelRatio();
    const auto target = cache::physicalTargetSize(contentsRect().size(), dpr, targetSizeBucket_);
    if (!target)
    {
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
    if (!pipeline)
    {
        QTimer::singleShot(0, this,
                           [this, generation]
                           {
                               applyResult(generation, cache::ImageResult::failure(
                                                           cache::ImageError::InvalidRequest,
                                                           "ImagePipeline is not configured"));
                           });
        return;
    }

    cache::RenderOptions options;
    options.physicalTargetSize = *target.value;
    options.dpr = dpr;
    options.fitMode = cache::fitName(fit_);
    options.scaleAlgorithm = scaleAlgorithm_;
    auto* subscription = pipeline->request(source_, options, this);
    if (!guard)
        return;
    if (generation_ != generation)
    {
        delete subscription;
        return;
    }

    subscription_ = subscription;
    connect(subscription, &cache::ImageSubscription::finished, this,
            [this, generation](cache::ImageResult result)
            {
                applyResult(generation, std::move(result));
            });
}

void ImageBox::applyResult(quint64 generation, cache::ImageResult result)
{
    if (generation != generation_)
        return;

    subscription_.clear();
    if (result && result.value->isNull())
        result = cache::ImageResult::failure(cache::ImageError::ProcessingError, "Empty image");

    QPointer<ImageBox> guard(this);
    if (result)
    {
        presentation_->accept(std::move(result), requestedTarget_, fit_, requestDevicePixelRatio());
        error_ = cache::ImageError::None;
        errorString_.clear();
        update();
        setState(ImageBoxState::Ready);
        if (guard && generation_ == generation)
            Q_EMIT loaded();
        return;
    }

    if (result.error == cache::ImageError::Cancelled)
    {
        setState(presentation_->image().isNull() ? ImageBoxState::Empty : ImageBoxState::Ready);
        return;
    }

    error_ = result.error;
    errorString_ = result.message;
    setState(ImageBoxState::Error);
    if (guard && generation_ == generation)
        Q_EMIT loadFailed(result.error, result.message);
}

void ImageBox::setState(ImageBoxState state, bool queuedNotification)
{
    if (state_ == state)
        return;

    state_ = state;
    presentation_->syncLoadingIndicator(state_ == ImageBoxState::Loading);
    update();
    if (queuedNotification)
    {
        const auto generation = generation_;
        QTimer::singleShot(0, this,
                           [this, state, generation]
                           {
                               if (generation_ == generation && state_ == state)
                                   Q_EMIT stateChanged(state);
                           });
        return;
    }
    Q_EMIT stateChanged(state);
}

void ImageBox::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    if (cornerRadius_ > 0)
    {
        const QRectF bounds(contentsRect());
        if (bounds.isEmpty())
            return;

        const qreal radius = std::min(cornerRadius_, std::min(bounds.width(), bounds.height()) / 2);
        QPainterPath clip;
        clip.addRoundedRect(bounds, radius, radius);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setClipPath(clip, Qt::IntersectClip);
    }
    presentation_->paint(painter, requestDevicePixelRatio(), fit_, state_ == ImageBoxState::Error);
}

qreal ImageBox::cornerRadius() const
{
    return cornerRadius_;
}

void ImageBox::setCornerRadius(qreal radius)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (!std::isfinite(radius) || radius < 0)
        throw std::invalid_argument("Invalid corner radius");
    if (cornerRadius_ == radius)
        return;

    cornerRadius_ = radius;
    update();
    Q_EMIT cornerRadiusChanged(radius);
}

ImageFit ImageBox::fit() const
{
    return fit_;
}

void ImageBox::setFit(ImageFit fit)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (cache::fitName(fit).isEmpty())
        throw std::invalid_argument("Invalid image fit");
    if (fit_ == fit)
        return;
    fit_ = fit;
    update();
    startRequest();
}

ImageScaleAlgorithm ImageBox::scaleAlgorithm() const
{
    return scaleAlgorithm_;
}

void ImageBox::setScaleAlgorithm(ImageScaleAlgorithm algorithm)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (int(algorithm) < 0 || int(algorithm) > int(ImageScaleAlgorithm::Lanczos4))
        throw std::invalid_argument("Invalid scale algorithm");
    if (scaleAlgorithm_ == algorithm)
        return;
    scaleAlgorithm_ = algorithm;
    startRequest();
}

int ImageBox::resizeDebounceInterval() const
{
    return resizeTimer_.interval();
}

void ImageBox::setResizeDebounceInterval(int milliseconds)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (milliseconds < 0 || milliseconds > 60000)
        throw std::invalid_argument("Invalid resize debounce interval");
    resizeTimer_.setInterval(milliseconds);
}

int ImageBox::targetSizeBucket() const
{
    return targetSizeBucket_;
}

void ImageBox::setTargetSizeBucket(int pixels)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (pixels < 1 || pixels > 4096)
        throw std::invalid_argument("Invalid target size bucket");
    if (pixels == targetSizeBucket_)
        return;
    targetSizeBucket_ = pixels;
    scheduleSizeRequest();
}

qreal ImageBox::requestDevicePixelRatio() const
{
    return devicePixelRatioF();
}

void ImageBox::scheduleSizeRequest()
{
    if (source_.isEmpty())
        return;
    if (suspended_)
    {
        resumePending_ = true;
        return;
    }
    const auto dpr = requestDevicePixelRatio();
    const auto target = cache::physicalTargetSize(contentsRect().size(), dpr, targetSizeBucket_);
    if (target && *target.value == requestedTarget_ && dpr == requestedDpr_ &&
        !resizeTimer_.isActive())
        return;
    invalidateRequest();
    update();
    if (!target)
    {
        requestedTarget_ = {};
        setState(presentation_->image().isNull() ? ImageBoxState::Empty : ImageBoxState::Ready);
        return;
    }
    resizeTimer_.start();
    setState(ImageBoxState::Loading);
}

void ImageBox::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    scheduleSizeRequest();
}

bool ImageBox::event(QEvent* event)
{
    const auto type = event->type();
    QPointer<ImageBox> guard(this);
    const bool result = QWidget::event(event);
    if (!guard)
        return result;
    if (type == QEvent::Hide)
    {
        suspendForHide();
        return result;
    }
    if (type == QEvent::Show)
    {
        suspended_ = false;
        const auto generation = generation_;
        QTimer::singleShot(0, this,
                           [this, generation]
                           {
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

void ImageBox::setPlaceholder(const QImage& image)
{
    presentation_->setPlaceholder(image);
}

QImage ImageBox::placeholder() const
{
    return presentation_->placeholder();
}

void ImageBox::setErrorImage(const QImage& image)
{
    presentation_->setErrorImage(image);
}

QImage ImageBox::errorImage() const
{
    return presentation_->errorImage();
}

void ImageBox::setErrorReplacesImage(bool enabled)
{
    presentation_->setErrorReplacesImage(enabled);
}

bool ImageBox::errorReplacesImage() const
{
    return presentation_->errorReplacesImage();
}

void ImageBox::setLoadingIndicatorEnabled(bool enabled)
{
    presentation_->setLoadingIndicatorEnabled(enabled);
}

bool ImageBox::loadingIndicatorEnabled() const
{
    return presentation_->loadingIndicatorEnabled();
}

void ImageBox::setLoadingOverlayEnabled(bool enabled)
{
    presentation_->setLoadingOverlayEnabled(enabled);
}

bool ImageBox::loadingOverlayEnabled() const
{
    return presentation_->loadingOverlayEnabled();
}

bool ImageBox::isLoadingIndicatorActive() const
{
    return presentation_->isLoadingIndicatorActive();
}

void ImageBox::setTransition(ImageTransition transition)
{
    presentation_->setTransition(transition);
}

ImageTransition ImageBox::transition() const
{
    return presentation_->transition();
}

void ImageBox::setTransitionDuration(int milliseconds)
{
    presentation_->setTransitionDuration(milliseconds);
}

int ImageBox::transitionDuration() const
{
    return presentation_->transitionDuration();
}

bool ImageBox::isTransitionRunning() const
{
    return presentation_->isTransitionRunning();
}

qreal ImageBox::transitionProgress() const
{
    return presentation_->transitionProgress();
}
}
