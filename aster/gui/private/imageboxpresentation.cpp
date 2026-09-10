#include "imageboxpresentation.h"

#include "imageframepainter.h"

#include <QThread>

#include <algorithm>
#include <stdexcept>

namespace aster::gui::detail
{
ImageBoxPresentation::ImageBoxPresentation(QWidget& owner) : QObject(&owner), owner_(owner)
{
    loadingTimer_.setInterval(40);
    QObject::connect(&loadingTimer_, &QTimer::timeout, this,
                     [this]
                     {
                         loadingAngle_ = (loadingAngle_ + 20) % 360;
                         owner_.update();
                     });
    animation_.setStartValue(0.0);
    animation_.setEndValue(1.0);
    QObject::connect(&animation_, &QVariantAnimation::valueChanged, this,
                     [this](const QVariant& value)
                     {
                         transitionProgress_ = value.toReal();
                         owner_.update();
                     });
    QObject::connect(&animation_, &QVariantAnimation::finished, this,
                     [this]
                     {
                         finishTransition();
                     });
}

QImage ImageBoxPresentation::image() const
{
    return currentImage_;
}

void ImageBoxPresentation::clear()
{
    finishTransition();
    currentImage_ = {};
    currentHandle_.reset();
    owner_.update();
}

void ImageBoxPresentation::accept(cache::ImageResult result, QSize target, ImageFit fit, qreal dpr)
{
    finishTransition();
    bool animate =
        transition_ != ImageTransition::None && transitionDuration_ > 0 && owner_.isVisible();
    if (animate && transition_ == ImageTransition::CrossFade && !currentImage_.isNull())
    {
        const auto pixels = cache::physicalTargetSize(owner_.size(), dpr);
        if (pixels && qint64(pixels.value->width()) * pixels.value->height() <= 16 * 1024 * 1024)
        {
            transitionCanvas_ = QImage(*pixels.value, QImage::Format_ARGB32_Premultiplied);
            transitionCanvas_.setDevicePixelRatio(dpr);
            transitionIncoming_ = QImage(*pixels.value, QImage::Format_ARGB32_Premultiplied);
            transitionIncoming_.setDevicePixelRatio(dpr);
        }
        animate = !transitionCanvas_.isNull() && !transitionIncoming_.isNull();
    }
    if (animate && transition_ == ImageTransition::CrossFade)
    {
        previousImage_ = currentImage_;
        previousHandle_ = std::move(currentHandle_);
        previousTarget_ = currentTarget_;
        previousFit_ = currentFit_;
    }
    currentImage_ = std::move(*result.value);
    currentTarget_ = target;
    currentFit_ = fit;
    currentHandle_ = std::move(result.handle);
    if (animate)
    {
        animation_.setDuration(transitionDuration_);
        animation_.start();
    }

    owner_.update();
}

void ImageBoxPresentation::paint(QPainter& painter, qreal dpr, ImageFit fit, bool error)
{
    const ImageFramePainter framePainter(owner_.contentsRect(), dpr, fit);
    painter.setClipRect(owner_.contentsRect());
    const bool errorVisual =
        error && !errorImage_.isNull() && (currentImage_.isNull() || errorReplacesImage_);
    if (errorVisual || currentImage_.isNull())
        framePainter.draw(painter, errorVisual ? errorImage_ : placeholder_, {}, ImageFit::Contain);
    else
    {
        const qreal progress = transitionProgress_;
        if (!previousImage_.isNull() && !transitionCanvas_.isNull())
        {
            transitionCanvas_.fill(Qt::transparent);
            QPainter blend(&transitionCanvas_);
            blend.setClipRect(owner_.contentsRect());
            blend.setOpacity(1 - progress);
            framePainter.draw(blend, previousImage_, previousTarget_, previousFit_);
            transitionIncoming_.fill(Qt::transparent);
            QPainter incoming(&transitionIncoming_);
            incoming.setClipRect(owner_.contentsRect());
            incoming.setOpacity(progress);
            framePainter.draw(incoming, currentImage_, currentTarget_, currentFit_);
            incoming.end();
            blend.setCompositionMode(QPainter::CompositionMode_Plus);
            blend.setOpacity(1);
            blend.drawImage(QPointF(0, 0), transitionIncoming_);
            blend.end();
            painter.drawImage(QPointF(0, 0), transitionCanvas_);
        }
        else
        {
            painter.save();
            if (transition_ == ImageTransition::Fade || transition_ == ImageTransition::CrossFade ||
                transition_ == ImageTransition::FadeZoom)
                painter.setOpacity(progress);
            if (transition_ == ImageTransition::Slide)
                painter.translate((1 - progress) * owner_.contentsRect().width(), 0);
            if (transition_ == ImageTransition::Zoom || transition_ == ImageTransition::FadeZoom)
            {
                const auto center = QRectF(owner_.contentsRect()).center();
                painter.translate(center);
                painter.scale(0.85 + 0.15 * progress, 0.85 + 0.15 * progress);
                painter.translate(-center);
            }
            framePainter.draw(painter, currentImage_, currentTarget_, currentFit_);
            painter.restore();
        }
    }
    if (owner_.isVisible() && loading_ && loadingIndicatorEnabled_ &&
        (currentImage_.isNull() || loadingOverlayEnabled_))
    {
        const auto center = QRectF(owner_.contentsRect()).center();
        const qreal side =
            std::min(24, std::min(owner_.contentsRect().width(), owner_.contentsRect().height()));
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(owner_.palette().color(QPalette::Highlight), 2));
        painter.drawArc(QRectF(center.x() - side / 2 + 2, center.y() - side / 2 + 2,
                               std::max(qreal(0), side - 4), std::max(qreal(0), side - 4)),
                        loadingAngle_ * 16, 240 * 16);
    }
}

void ImageBoxPresentation::finishTransition()
{
    animation_.stop();
    transitionProgress_ = 1;
    previousImage_ = {};
    transitionCanvas_ = {};
    transitionIncoming_ = {};
    previousHandle_.reset();
    owner_.update();
}

void ImageBoxPresentation::syncLoadingIndicator(bool loading)
{
    loading_ = loading;
    const bool active = owner_.isVisible() && loading_ && loadingIndicatorEnabled_ &&
                        (currentImage_.isNull() || loadingOverlayEnabled_);
    if (active && !loadingTimer_.isActive())
        loadingTimer_.start();
    else if (!active)
        loadingTimer_.stop();
}

void ImageBoxPresentation::setPlaceholder(const QImage& image)
{
    Q_ASSERT(QThread::currentThread() == owner_.thread());
    placeholder_ = image.copy();
    owner_.update();
}

QImage ImageBoxPresentation::placeholder() const
{
    return placeholder_;
}

void ImageBoxPresentation::setErrorImage(const QImage& image)
{
    Q_ASSERT(QThread::currentThread() == owner_.thread());
    errorImage_ = image.copy();
    owner_.update();
}

QImage ImageBoxPresentation::errorImage() const
{
    return errorImage_;
}

void ImageBoxPresentation::setErrorReplacesImage(bool enabled)
{
    Q_ASSERT(QThread::currentThread() == owner_.thread());
    errorReplacesImage_ = enabled;
    owner_.update();
}

bool ImageBoxPresentation::errorReplacesImage() const
{
    return errorReplacesImage_;
}

void ImageBoxPresentation::setLoadingIndicatorEnabled(bool enabled)
{
    Q_ASSERT(QThread::currentThread() == owner_.thread());
    loadingIndicatorEnabled_ = enabled;
    syncLoadingIndicator(loading_);
    owner_.update();
}

bool ImageBoxPresentation::loadingIndicatorEnabled() const
{
    return loadingIndicatorEnabled_;
}

void ImageBoxPresentation::setLoadingOverlayEnabled(bool enabled)
{
    Q_ASSERT(QThread::currentThread() == owner_.thread());
    loadingOverlayEnabled_ = enabled;
    syncLoadingIndicator(loading_);
    owner_.update();
}

bool ImageBoxPresentation::loadingOverlayEnabled() const
{
    return loadingOverlayEnabled_;
}

bool ImageBoxPresentation::isLoadingIndicatorActive() const
{
    return loadingTimer_.isActive();
}

void ImageBoxPresentation::setTransition(ImageTransition transition)
{
    Q_ASSERT(QThread::currentThread() == owner_.thread());
    if (int(transition) < 0 || int(transition) > int(ImageTransition::FadeZoom))
        throw std::invalid_argument("Invalid image transition");
    finishTransition();
    transition_ = transition;
}

ImageTransition ImageBoxPresentation::transition() const
{
    return transition_;
}

void ImageBoxPresentation::setTransitionDuration(int milliseconds)
{
    Q_ASSERT(QThread::currentThread() == owner_.thread());
    if (milliseconds < 0 || milliseconds > 60000)
        throw std::invalid_argument("Invalid transition duration");
    finishTransition();
    transitionDuration_ = milliseconds;
}

int ImageBoxPresentation::transitionDuration() const
{
    return transitionDuration_;
}

bool ImageBoxPresentation::isTransitionRunning() const
{
    return animation_.state() == QAbstractAnimation::Running;
}

qreal ImageBoxPresentation::transitionProgress() const
{
    return transitionProgress_;
}
}
