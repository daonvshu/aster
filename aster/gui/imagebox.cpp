#include "imagebox.h"

#include <QEvent>
#include <QPainter>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <stdexcept>

namespace aster::gui
{
ImageBox::ImageBox(QWidget* parent) : QWidget(parent)
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

void ImageBox::setPipeline(std::shared_ptr<cache::ImagePipeline> pipeline)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (pipeline_ == pipeline)
        return;

    invalidateRequest();
    pipeline_ = std::move(pipeline);
    startRequest();
}

std::shared_ptr<cache::ImagePipeline> ImageBox::pipeline() const
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
    return currentImage_;
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
    if (state_ != ImageBoxState::Loading)
        return;

    invalidateRequest();
    error_ = cache::ImageError::None;
    errorString_.clear();
    setState(currentImage_.isNull() ? ImageBoxState::Empty : ImageBoxState::Ready);
}

void ImageBox::startRequest()
{
    invalidateRequest();
    error_ = cache::ImageError::None;
    errorString_.clear();
    if (source_.isEmpty())
    {
        requestedTarget_ = {};
        currentImage_ = {};
        currentHandle_.reset();
        update();
        setState(ImageBoxState::Empty);
        return;
    }

    const auto dpr = requestDevicePixelRatio();
    const auto target = cache::physicalTargetSize(contentsRect().size(), dpr, targetSizeBucket_);
    if (!target)
    {
        requestedTarget_ = {};
        setState(currentImage_.isNull() ? ImageBoxState::Empty : ImageBoxState::Ready);
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
        currentImage_ = std::move(*result.value);
        currentTarget_ = requestedTarget_;
        currentFit_ = fit_;
        currentHandle_ = std::move(result.handle);
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
        setState(currentImage_.isNull() ? ImageBoxState::Empty : ImageBoxState::Ready);
        return;
    }

    error_ = result.error;
    errorString_ = result.message;
    setState(ImageBoxState::Error);
    if (guard && generation_ == generation)
        Q_EMIT loadFailed(result.error, result.message);
}

void ImageBox::setState(ImageBoxState state)
{
    if (state_ == state)
        return;

    state_ = state;
    Q_EMIT stateChanged(state);
}

void ImageBox::paintEvent(QPaintEvent*)
{
    if (currentImage_.isNull())
        return;
    QPainter painter(this);
    painter.setClipRect(contentsRect());
    QSizeF size = QSizeF(currentImage_.size()) / currentImage_.devicePixelRatio();
    const auto exactTarget =
        cache::physicalTargetSize(contentsRect().size(), requestDevicePixelRatio());
    const bool preview = !exactTarget || currentTarget_ != *exactTarget.value ||
                         currentFit_ != fit_ ||
                         currentImage_.devicePixelRatio() != requestDevicePixelRatio();
    if (preview && fit_ != ImageFit::None)
    {
        const auto area = QSizeF(contentsRect().size());
        if (fit_ == ImageFit::Fill)
            size = area;
        else
        {
            const qreal sx = area.width() / size.width();
            const qreal sy = area.height() / size.height();
            qreal scale = fit_ == ImageFit::Cover ? std::max(sx, sy) : std::min(sx, sy);
            if (fit_ == ImageFit::ScaleDown)
                scale = std::min(qreal(1), scale);
            size *= scale;
        }
    }
    const QRectF destination(
        QPointF(contentsRect().x() + (contentsRect().width() - size.width()) / 2,
                contentsRect().y() + (contentsRect().height() - size.height()) / 2),
        size);
    painter.drawImage(destination, currentImage_);
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
        setState(currentImage_.isNull() ? ImageBoxState::Empty : ImageBoxState::Ready);
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
    if (type == QEvent::ScreenChangeInternal || type == QEvent::Show ||
        type == QEvent::ContentsRectChange
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
        || type == QEvent::DevicePixelRatioChange
#endif
    )
        scheduleSizeRequest();
    return result;
}
}
