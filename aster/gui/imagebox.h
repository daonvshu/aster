#pragma once

#include "aster/cache/pipeline/imagepipeline.h"

#include <QPointer>
#include <QSharedPointer>
#include <QTimer>
#include <QWidget>

namespace aster::gui
{
Q_NAMESPACE

using ImageFit = cache::ImageFit;
using ImageScaleAlgorithm = cache::ImageScaleAlgorithm;

enum class ImageBoxState
{
    Empty,
    Loading,
    Ready,
    Error
};
Q_ENUM_NS(ImageBoxState)

enum class ImageTransition
{
    None,
    Fade,
    CrossFade,
    Slide,
    Zoom,
    FadeZoom
};
Q_ENUM_NS(ImageTransition)

enum class OffscreenPolicy
{
    Keep,
    ReleaseHandle,
    ReleaseImage
};
Q_ENUM_NS(OffscreenPolicy)

namespace detail
{
class ImageBoxPresentation;
}

class ImageBox : public QWidget
{
    Q_OBJECT
    Q_PROPERTY(QString source READ source WRITE setSource NOTIFY sourceChanged)
    Q_PROPERTY(aster::gui::ImageBoxState state READ state NOTIFY stateChanged)
    Q_PROPERTY(
        qreal cornerRadius READ cornerRadius WRITE setCornerRadius NOTIFY cornerRadiusChanged)

public:
    explicit ImageBox(QWidget* parent = nullptr);
    ~ImageBox() override;

    void setPipeline(QSharedPointer<cache::ImagePipeline> pipeline);
    QSharedPointer<cache::ImagePipeline> pipeline() const;
    QString source() const;
    ImageBoxState state() const;
    QImage image() const;
    cache::ImageError error() const;
    QString errorString() const;
    ImageFit fit() const;
    void setFit(ImageFit fit);
    qreal cornerRadius() const;
    void setCornerRadius(qreal radius);
    ImageScaleAlgorithm scaleAlgorithm() const;
    void setScaleAlgorithm(ImageScaleAlgorithm algorithm);
    int resizeDebounceInterval() const;
    void setResizeDebounceInterval(int milliseconds);
    int targetSizeBucket() const;
    void setTargetSizeBucket(int pixels);
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
    void setOffscreenPolicy(OffscreenPolicy policy);
    OffscreenPolicy offscreenPolicy() const;

public Q_SLOTS:
    void setSource(const QString& source);
    void reload();
    void cancelCurrentRequest();

Q_SIGNALS:
    void sourceChanged(const QString& source);
    void cornerRadiusChanged(qreal radius);
    void stateChanged(aster::gui::ImageBoxState state);
    void loadingStarted();
    void loaded();
    void loadFailed(aster::cache::ImageError error, const QString& message);

protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    bool event(QEvent*) override;
    virtual qreal requestDevicePixelRatio() const;

private:
    void invalidateRequest();
    void startRequest();
    void applyResult(quint64 generation, cache::ImageResult result);
    void setState(ImageBoxState state, bool queuedNotification = false);
    void scheduleSizeRequest();
    void suspendForHide();
    void resumeAfterShow();

    QSharedPointer<cache::ImagePipeline> pipeline_;
    QString source_;
    quint64 generation_ = 0;
    QPointer<cache::ImageSubscription> subscription_;
    ImageBoxState state_ = ImageBoxState::Empty;
    cache::ImageError error_ = cache::ImageError::None;
    QString errorString_;
    ImageFit fit_ = ImageFit::Contain;
    qreal cornerRadius_ = 0;
    ImageScaleAlgorithm scaleAlgorithm_ = ImageScaleAlgorithm::QtSmooth;
    QTimer resizeTimer_;
    int targetSizeBucket_ = 1;
    QSize requestedTarget_;
    qreal requestedDpr_ = 0;
    OffscreenPolicy offscreenPolicy_ = OffscreenPolicy::Keep;
    bool suspended_ = false;
    bool resumePending_ = false;
    detail::ImageBoxPresentation* presentation_;
};
}

Q_DECLARE_METATYPE(aster::gui::ImageBoxState)
Q_DECLARE_METATYPE(aster::cache::ImageError)
