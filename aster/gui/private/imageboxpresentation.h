#pragma once

#include "aster/gui/imagebox.h"

#include <QObject>
#include <QVariantAnimation>

namespace aster::gui::detail
{
class ImageBoxPresentation : public QObject
{
    Q_OBJECT

public:
    explicit ImageBoxPresentation(QWidget& owner);
    QImage image() const;
    void clear();
    void releaseHandle();
    void accept(cache::ImageResult result, QSize target, ImageFit fit, qreal dpr);
    void paint(QPainter& painter, qreal dpr, ImageFit fit, bool error);
    void finishTransition();
    void syncLoadingIndicator(bool loading);
    void setPlaceholder(const QImage& image);
    QImage placeholder() const;
    void setErrorImage(const QImage& image);
    QImage errorImage() const;
    void setErrorReplacesImage(bool enabled);
    bool errorReplacesImage() const;
    void setLoadingIndicatorEnabled(bool enabled);
    bool loadingIndicatorEnabled() const;
    void setLoadingOverlayEnabled(bool enabled);
    bool loadingOverlayEnabled() const;
    bool isLoadingIndicatorActive() const;
    void setTransition(ImageTransition transition);
    ImageTransition transition() const;
    void setTransitionDuration(int milliseconds);
    int transitionDuration() const;
    bool isTransitionRunning() const;
    qreal transitionProgress() const;

private:
    QWidget& owner_;
    bool loading_ = false;
    QImage currentImage_;
    cache::ImageHandle currentHandle_;
    QSize currentTarget_;
    ImageFit currentFit_ = ImageFit::Contain;
    QImage placeholder_;
    QImage errorImage_;
    bool errorReplacesImage_ = false;
    bool loadingIndicatorEnabled_ = false;
    bool loadingOverlayEnabled_ = false;
    QTimer loadingTimer_;
    int loadingAngle_ = 0;
    ImageTransition transition_ = ImageTransition::None;
    int transitionDuration_ = 200;
    QVariantAnimation animation_;
    qreal transitionProgress_ = 1;
    QImage previousImage_;
    QImage transitionCanvas_;
    QImage transitionIncoming_;
    cache::ImageHandle previousHandle_;
    QSize previousTarget_;
    ImageFit previousFit_ = ImageFit::Contain;
};
}
