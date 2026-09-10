#pragma once

#include "aster/cache/pipeline/imagepipeline.h"

#include <QPointer>
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

class ImageBox : public QWidget
{
    Q_OBJECT
    Q_PROPERTY(QString source READ source WRITE setSource NOTIFY sourceChanged)
    Q_PROPERTY(aster::gui::ImageBoxState state READ state NOTIFY stateChanged)

public:
    explicit ImageBox(QWidget* parent = nullptr);
    ~ImageBox() override;

    void setPipeline(std::shared_ptr<cache::ImagePipeline> pipeline);
    std::shared_ptr<cache::ImagePipeline> pipeline() const;
    QString source() const;
    ImageBoxState state() const;
    QImage image() const;
    cache::ImageError error() const;
    QString errorString() const;
    ImageFit fit() const;
    void setFit(ImageFit fit);
    ImageScaleAlgorithm scaleAlgorithm() const;
    void setScaleAlgorithm(ImageScaleAlgorithm algorithm);
    int resizeDebounceInterval() const;
    void setResizeDebounceInterval(int milliseconds);
    int targetSizeBucket() const;
    void setTargetSizeBucket(int pixels);

public Q_SLOTS:
    void setSource(const QString& source);
    void reload();
    void cancelCurrentRequest();

Q_SIGNALS:
    void sourceChanged(const QString& source);
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
    void setState(ImageBoxState state);
    void scheduleSizeRequest();

    std::shared_ptr<cache::ImagePipeline> pipeline_;
    QString source_;
    quint64 generation_ = 0;
    QPointer<cache::ImageSubscription> subscription_;
    QImage currentImage_;
    cache::ImageHandle currentHandle_;
    ImageBoxState state_ = ImageBoxState::Empty;
    cache::ImageError error_ = cache::ImageError::None;
    QString errorString_;
    ImageFit fit_ = ImageFit::Contain;
    ImageScaleAlgorithm scaleAlgorithm_ = ImageScaleAlgorithm::QtSmooth;
    QTimer resizeTimer_;
    int targetSizeBucket_ = 1;
    QSize requestedTarget_;
    qreal requestedDpr_ = 0;
    QSize currentTarget_;
    ImageFit currentFit_ = ImageFit::Contain;
};
}

Q_DECLARE_METATYPE(aster::gui::ImageBoxState)
Q_DECLARE_METATYPE(aster::cache::ImageError)
