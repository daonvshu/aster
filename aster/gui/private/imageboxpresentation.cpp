#include "imageboxpresentation.h"

#include "imageframepainter.h"

#include <algorithm>

namespace aster::gui::detail
{
ImageBoxPresentation::ImageBoxPresentation(QWidget& owner, const ImageBoxConfig& config)
    : QObject(&owner), owner_(owner), config_(config)
{
    loadingTimer_.setInterval(40);
    connect(&loadingTimer_, &QTimer::timeout, this, [this] {
        loadingAngle_ = (loadingAngle_ + 20) % 360;
        owner_.update();
    });
    animation_.setStartValue(0.0);
    animation_.setEndValue(1.0);
    connect(&animation_, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
        transitionProgress_ = value.toReal();
        owner_.update();
    });
    connect(&animation_, &QVariantAnimation::finished, this, [this] {
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

void ImageBoxPresentation::releaseHandle()
{
    finishTransition();
    currentHandle_.reset();
}

void ImageBoxPresentation::accept(cache::ImageResult result, QSize target, ImageFit fit, qreal dpr)
{
    finishTransition();
    bool animate = config_.transition() != ImageTransition::None &&
                   config_.transitionDuration() > 0 && owner_.isVisible();
    if (animate && config_.transition() == ImageTransition::CrossFade && !currentImage_.isNull())
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
    if (animate && config_.transition() == ImageTransition::CrossFade)
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
        // Set the initial value before starting the animation so the first paint
        // cannot render the incoming frame at the completed state.
        transitionProgress_ = 0;
        animation_.setDuration(config_.transitionDuration());
        animation_.start();
    }

    owner_.update();
}

void ImageBoxPresentation::paint(QPainter& painter, qreal dpr, ImageFit fit, bool error)
{
    const ImageFramePainter framePainter(owner_.contentsRect(), dpr, fit);
    painter.setClipRect(owner_.contentsRect(), Qt::IntersectClip);
    auto* loadingErrorWidget = config_.loadingErrorWidget();
    if (loadingErrorWidget && loadingErrorWidget->isVisible())
        return;
    const bool statusImage = (state_ == ImageBoxState::Loading || state_ == ImageBoxState::Error) &&
                             !config_.loadingErrorImage().isNull();
    const bool errorVisual = error && !config_.errorImage().isNull() &&
                             (currentImage_.isNull() || config_.errorReplacesImage());
    if (statusImage)
        framePainter.draw(painter, config_.loadingErrorImage(), {}, ImageFit::Contain);
    else if (errorVisual || currentImage_.isNull())
        framePainter.draw(painter, errorVisual ? config_.errorImage() : config_.placeholder(), {},
                          ImageFit::Contain);
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
            if (config_.transition() == ImageTransition::Fade ||
                config_.transition() == ImageTransition::CrossFade ||
                config_.transition() == ImageTransition::FadeZoom)
                painter.setOpacity(progress);
            if (config_.transition() == ImageTransition::Slide)
                painter.translate((1 - progress) * owner_.contentsRect().width(), 0);
            if (config_.transition() == ImageTransition::Zoom ||
                config_.transition() == ImageTransition::FadeZoom)
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
    if (owner_.isVisible() && loading_ && config_.loadingIndicator() &&
        (currentImage_.isNull() || config_.loadingOverlay()))
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

void ImageBoxPresentation::syncState(ImageBoxState state)
{
    state_ = state;
    const bool loading = state == ImageBoxState::Loading;
    loading_ = loading;
    if (auto* widget = config_.loadingErrorWidget())
    {
        widget->setGeometry(owner_.contentsRect());
        widget->setVisible(owner_.isVisible() &&
                           (state == ImageBoxState::Loading || state == ImageBoxState::Error));
        widget->raise();
    }
    const bool active = owner_.isVisible() && loading_ && !config_.loadingErrorWidget() &&
                        config_.loadingErrorImage().isNull() && config_.loadingIndicator() &&
                        (currentImage_.isNull() || config_.loadingOverlay());
    if (active && !loadingTimer_.isActive())
        loadingTimer_.start();
    else if (!active)
        loadingTimer_.stop();
}

void ImageBoxPresentation::prepareConfigChange()
{
    if (auto* widget = config_.loadingErrorWidget())
    {
        widget->hide();
        widget->setParent(nullptr);
    }
}

void ImageBoxPresentation::syncConfig()
{
    finishTransition();
    if (auto* widget = config_.loadingErrorWidget())
    {
        widget->setParent(&owner_);
        widget->setGeometry(owner_.contentsRect());
    }
    syncState(state_);
    owner_.update();
}

bool ImageBoxPresentation::isLoadingIndicatorActive() const
{
    return loadingTimer_.isActive();
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
