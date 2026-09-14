#include "imageboxconfig.h"

#include "aster/cache/core/imagerendergeometry.h"

#include <QWidget>

#include <cmath>
#include <stdexcept>

namespace aster::gui
{
cache::ImageFit ImageBoxConfig::fit() const
{
    return fit_;
}

ImageBoxConfig& ImageBoxConfig::fit(cache::ImageFit value)
{
    if (cache::fitName(value).isEmpty())
        throw std::invalid_argument("Invalid image fit");
    fit_ = value;
    return *this;
}

cache::ImageScaleAlgorithm ImageBoxConfig::scaleAlgorithm() const
{
    return scaleAlgorithm_;
}

ImageBoxConfig& ImageBoxConfig::scaleAlgorithm(cache::ImageScaleAlgorithm value)
{
    if (int(value) < 0 || int(value) > int(cache::ImageScaleAlgorithm::Lanczos4))
        throw std::invalid_argument("Invalid scale algorithm");
    scaleAlgorithm_ = value;
    return *this;
}

int ImageBoxConfig::resizeDebounce() const
{
    return resizeDebounce_;
}

ImageBoxConfig& ImageBoxConfig::resizeDebounce(int milliseconds)
{
    if (milliseconds < 0 || milliseconds > 60000)
        throw std::invalid_argument("Invalid resize debounce interval");
    resizeDebounce_ = milliseconds;
    return *this;
}

int ImageBoxConfig::sizeBucket() const
{
    return sizeBucket_;
}

ImageBoxConfig& ImageBoxConfig::sizeBucket(int pixels)
{
    if (pixels < 1 || pixels > 4096)
        throw std::invalid_argument("Invalid target size bucket");
    sizeBucket_ = pixels;
    return *this;
}

QImage ImageBoxConfig::placeholder() const
{
    return placeholder_;
}

ImageBoxConfig& ImageBoxConfig::placeholder(const QImage& image)
{
    placeholder_ = image.copy();
    return *this;
}

QImage ImageBoxConfig::errorImage() const
{
    return errorImage_;
}

ImageBoxConfig& ImageBoxConfig::errorImage(const QImage& image)
{
    errorImage_ = image.copy();
    return *this;
}

bool ImageBoxConfig::errorReplacesImage() const
{
    return errorReplacesImage_;
}

ImageBoxConfig& ImageBoxConfig::errorReplacesImage(bool enabled)
{
    errorReplacesImage_ = enabled;
    return *this;
}

bool ImageBoxConfig::loadingIndicator() const
{
    return loadingIndicator_;
}

ImageBoxConfig& ImageBoxConfig::loadingIndicator(bool enabled)
{
    loadingIndicator_ = enabled;
    return *this;
}

bool ImageBoxConfig::loadingOverlay() const
{
    return loadingOverlay_;
}

ImageBoxConfig& ImageBoxConfig::loadingOverlay(bool enabled)
{
    loadingOverlay_ = enabled;
    return *this;
}

QWidget* ImageBoxConfig::loadingErrorWidget() const
{
    return loadingErrorWidget_.data();
}

ImageBoxConfig& ImageBoxConfig::loadingErrorWidget(QWidget* widget)
{
    loadingErrorWidget_ = widget;
    return *this;
}

QImage ImageBoxConfig::loadingErrorImage() const
{
    return loadingErrorImage_;
}

ImageBoxConfig& ImageBoxConfig::loadingErrorImage(const QImage& image)
{
    loadingErrorImage_ = image.copy();
    return *this;
}

ImageTransition ImageBoxConfig::transition() const
{
    return transition_;
}

ImageBoxConfig& ImageBoxConfig::transition(ImageTransition value)
{
    if (int(value) < 0 || int(value) > int(ImageTransition::FadeZoom))
        throw std::invalid_argument("Invalid image transition");
    transition_ = value;
    return *this;
}

int ImageBoxConfig::transitionDuration() const
{
    return transitionDuration_;
}

ImageBoxConfig& ImageBoxConfig::transitionDuration(int milliseconds)
{
    if (milliseconds < 0 || milliseconds > 60000)
        throw std::invalid_argument("Invalid transition duration");
    transitionDuration_ = milliseconds;
    return *this;
}

qreal ImageBoxConfig::cornerRadius() const
{
    return cornerRadius_;
}

ImageBoxConfig& ImageBoxConfig::cornerRadius(qreal radius)
{
    if (!std::isfinite(radius) || radius < 0)
        throw std::invalid_argument("Invalid corner radius");
    cornerRadius_ = radius;
    return *this;
}

OffscreenPolicy ImageBoxConfig::offscreenPolicy() const
{
    return offscreenPolicy_;
}

ImageBoxConfig& ImageBoxConfig::offscreenPolicy(OffscreenPolicy value)
{
    if (int(value) < int(OffscreenPolicy::Keep) || int(value) > int(OffscreenPolicy::ReleaseImage))
        throw std::invalid_argument("Invalid offscreen policy");
    offscreenPolicy_ = value;
    return *this;
}

ImageBoxConfig ImageBoxConfig::build() const
{
    return *this;
}
}
