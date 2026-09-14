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
    explicit ImageBoxPresentation(QWidget& owner, const ImageBoxConfig& config);
    QImage image() const;
    void clear();
    void releaseHandle();
    void accept(cache::ImageResult result, QSize target, ImageFit fit, qreal dpr);
    void paint(QPainter& painter, qreal dpr, ImageFit fit, bool error);
    void finishTransition();
    void syncState(ImageBoxState state);
    void prepareConfigChange();
    void syncConfig();
    bool isLoadingIndicatorActive() const;
    bool isTransitionRunning() const;
    qreal transitionProgress() const;

private:
    QWidget& owner_;
    const ImageBoxConfig& config_;
    bool loading_ = false;
    ImageBoxState state_ = ImageBoxState::Empty;
    QImage currentImage_;
    cache::ImageHandle currentHandle_;
    QSize currentTarget_;
    ImageFit currentFit_ = ImageFit::Contain;
    QTimer loadingTimer_;
    int loadingAngle_ = 0;
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
