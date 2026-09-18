#pragma once

#include "aster/cache/core/imagerendergeometry.h"
#include "aster/cache/core/imagetransformation.h"
#include "aster/gui/imageboxtypes.h"

#include <QImage>
#include <QSharedPointer>
#include <QVector>

#include <functional>

class QWidget;

namespace aster::gui {
using ImageFit = cache::ImageFit;
using ImageScaleAlgorithm = cache::ImageScaleAlgorithm;

class ImageBoxConfig final {
public:
    using LoadingErrorWidgetFactory = std::function<QWidget*(QWidget*)>;

    /**
     * @brief Returns the fit mode. The default is Cover.
     * @return Fit mode.
     */
    cache::ImageFit fit() const;

    /**
     * @brief Sets the fit mode. The default is Cover.
     * @param value Fit mode.
     */
    ImageBoxConfig& fit(cache::ImageFit value);

    /**
     * @brief Returns the scaling algorithm. The default is QtSmooth.
     * @return Scaling algorithm.
     */
    cache::ImageScaleAlgorithm scaleAlgorithm() const;

    /**
     * @brief Sets the scaling algorithm. The default is QtSmooth.
     * @param value Scaling algorithm.
     */
    ImageBoxConfig& scaleAlgorithm(cache::ImageScaleAlgorithm value);

    /**
     * @brief Returns the image transformations. The default is empty.
     * @return Transformation sequence applied after scaling.
     */
    QVector<QSharedPointer<cache::ImageTransformation>> transformations() const;

    /**
     * @brief Adds an image transformation after the current sequence.
     * @param transformation Transformation to add. It must not be null.
     * @return This configuration for chained calls.
     */
    ImageBoxConfig& addTransformation(QSharedPointer<cache::ImageTransformation> transformation);

    /**
     * @brief Removes all image transformations.
     * @return This configuration for chained calls.
     */
    ImageBoxConfig& clearTransformations();

    /**
     * @brief Returns resize debounce. The default is 75 ms.
     * @return Debounce interval.
     */
    int resizeDebounce() const;

    /**
     * @brief Sets resize debounce. The default is 75 ms.
     * @param milliseconds Debounce interval.
     */
    ImageBoxConfig& resizeDebounce(int milliseconds);

    /**
     * @brief Returns the target size bucket. The default is 1 pixel.
     * @return Bucket size.
     */
    int sizeBucket() const;

    /**
     * @brief Sets the target size bucket. The default is 1 pixel.
     * @param pixels Bucket size.
     */
    ImageBoxConfig& sizeBucket(int pixels);

    /**
     * @brief Returns the loading placeholder. The default is empty.
     * @return Placeholder image.
     */
    QImage placeholder() const;

    /**
     * @brief Sets the loading placeholder. The default is an empty image.
     * @param image Placeholder image.
     */
    ImageBoxConfig& placeholder(const QImage& image);

    /**
     * @brief Returns the error image. The default is empty.
     * @return Error image.
     */
    QImage errorImage() const;

    /**
     * @brief Sets the error image. The default is an empty image.
     * @param image Error image.
     */
    ImageBoxConfig& errorImage(const QImage& image);

    /**
     * @brief Returns whether the error image replaces current content.
     * @return True when enabled.
     */
    bool errorReplacesImage() const;

    /**
     * @brief Controls whether the error image replaces the current image. The default is false.
     * @param enabled Enable replacement.
     */
    ImageBoxConfig& errorReplacesImage(bool enabled = true);

    /**
     * @brief Returns whether the built-in loading indicator is enabled.
     * @return True when enabled.
     */
    bool loadingIndicator() const;

    /**
     * @brief Enables the built-in loading indicator. The default is false.
     * @param enabled Enable the indicator.
     */
    ImageBoxConfig& loadingIndicator(bool enabled = true);

    /**
     * @brief Returns whether the loading overlay is enabled.
     * @return True when enabled.
     */
    bool loadingOverlay() const;

    /**
     * @brief Enables the loading overlay over the current image. The default is false.
     * @param enabled Enable the overlay.
     */
    ImageBoxConfig& loadingOverlay(bool enabled = true);

    /**
     * @brief Returns the loading and failure widget factory.
     * @return Widget factory or an empty function.
     */
    const LoadingErrorWidgetFactory& loadingErrorWidgetFactory() const;

    /**
     * @brief Sets the loading and failure widget factory. The default is empty.
     * @param factory Factory used by each ImageBox to create its own replacement widget.
     */
    ImageBoxConfig& loadingErrorWidget(LoadingErrorWidgetFactory factory);

    /**
     * @brief Returns the loading and failure replacement image.
     * @return Replacement image.
     */
    QImage loadingErrorImage() const;

    /**
     * @brief Sets an image shown during loading and failure. The default is empty.
     * @param image Replacement image.
     */
    ImageBoxConfig& loadingErrorImage(const QImage& image);

    /**
     * @brief Returns the transition type. The default is CrossFade.
     * @return Transition type.
     */
    ImageTransition transition() const;

    /**
     * @brief Sets the transition type. The default is CrossFade.
     * @param value Transition type.
     */
    ImageBoxConfig& transition(ImageTransition value);

    /**
     * @brief Returns the transition trigger policy. The default is FirstLoadOrNonMemoryCache.
     * @return Transition policy.
     */
    TransitionPolicy transitionPolicy() const;

    /**
     * @brief Sets the transition trigger policy. The default is FirstLoadOrNonMemoryCache.
     * @param value Transition policy.
     */
    ImageBoxConfig& transitionPolicy(TransitionPolicy value);

    /**
     * @brief Returns transition duration. The default is 200 ms.
     * @return Duration in milliseconds.
     */
    int transitionDuration() const;

    /**
     * @brief Sets transition duration. The default is 200 ms.
     * @param milliseconds Duration in milliseconds.
     */
    ImageBoxConfig& transitionDuration(int milliseconds);

    /**
     * @brief Returns corner radius. The default is 0.
     * @return Radius in logical pixels.
     */
    qreal cornerRadius() const;

    /**
     * @brief Sets corner radius. The default is 0.
     * @param radius Radius in logical pixels.
     */
    ImageBoxConfig& cornerRadius(qreal radius);

    /**
     * @brief Returns offscreen handling. The default is Keep.
     * @return Offscreen policy.
     */
    OffscreenPolicy offscreenPolicy() const;

    /**
     * @brief Sets offscreen handling. The default is Keep.
     * @param value Offscreen policy.
     */
    ImageBoxConfig& offscreenPolicy(OffscreenPolicy value);

    /**
     * @brief Returns a copy for concise chained construction.
     * @return Configured options.
     */
    ImageBoxConfig build() const;

private:
    friend class ImageBox;
    cache::ImageFit fit_ = cache::ImageFit::Cover;
    cache::ImageScaleAlgorithm scaleAlgorithm_ = cache::ImageScaleAlgorithm::QtSmooth;
    QVector<QSharedPointer<cache::ImageTransformation>> transformations_;
    int resizeDebounce_ = 75;
    int sizeBucket_ = 1;
    QImage placeholder_;
    QImage errorImage_;
    bool errorReplacesImage_ = false;
    bool loadingIndicator_ = false;
    bool loadingOverlay_ = false;
    QSharedPointer<LoadingErrorWidgetFactory> loadingErrorWidgetFactory_;
    QImage loadingErrorImage_;
    ImageTransition transition_ = ImageTransition::CrossFade;
    TransitionPolicy transitionPolicy_ = TransitionPolicy::FirstLoadOrNonMemoryCache;
    int transitionDuration_ = 200;
    qreal cornerRadius_ = 0;
    OffscreenPolicy offscreenPolicy_ = OffscreenPolicy::Keep;
};
} // namespace aster::gui
