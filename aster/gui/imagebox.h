#pragma once

#include "aster/cache/pipeline/imagepipeline.h"
#include "aster/gui/imageboxconfig.h"

#include <QPointer>
#include <QSharedPointer>
#include <QTimer>
#include <QWidget>

namespace aster::gui
{
Q_NAMESPACE
Q_ENUM_NS(ImageTransition)
Q_ENUM_NS(OffscreenPolicy)

enum class ImageBoxState
{
    Empty,
    Loading,
    Ready,
    Error
};
Q_ENUM_NS(ImageBoxState)

namespace detail
{
class ImageBoxPresentation;
}

class ImageBox : public QWidget
{
    Q_OBJECT
    Q_PROPERTY(QString source READ source WRITE setSource NOTIFY sourceChanged)
    Q_PROPERTY(aster::gui::ImageBoxState state READ state NOTIFY stateChanged)

public:
    /**
     * @brief Creates an ImageBox.
     * @param parent Optional widget parent.
     */
    explicit ImageBox(QWidget* parent = nullptr);

    /**
     * @brief Destroys the ImageBox and cancels its active request.
     */
    ~ImageBox() override;

    /**
     * @brief Sets the image pipeline. The default is null.
     * @param pipeline Pipeline to use.
     */
    void setPipeline(QSharedPointer<cache::ImagePipeline> pipeline);

    /**
     * @brief Returns the configured pipeline.
     * @return Pipeline or null.
     */
    QSharedPointer<cache::ImagePipeline> pipeline() const;

    /**
     * @brief Applies all display settings in one operation.
     * @param config Configuration values and defaults.
     */
    void setConfig(const ImageBoxConfig& config);

    /**
     * @brief Returns the current display configuration.
     * @return Configuration snapshot.
     */
    ImageBoxConfig config() const;

    /**
     * @brief Returns the source string. The default is empty.
     * @return Source string.
     */
    QString source() const;

    /**
     * @brief Returns the current state. The default is Empty.
     * @return ImageBox state.
     */
    ImageBoxState state() const;

    /**
     * @brief Returns the current image. The default is null.
     * @return Current image.
     */
    QImage image() const;

    /**
     * @brief Returns the last image error. The default is None.
     * @return Error code.
     */
    cache::ImageError error() const;

    /**
     * @brief Returns the last error message. The default is empty.
     * @return Error message.
     */
    QString errorString() const;

    /**
     * @brief Returns whether the built-in loading animation timer is active.
     * @return True when active.
     */
    bool isLoadingIndicatorActive() const;

    /**
     * @brief Returns whether a transition is running.
     * @return True when running.
     */
    bool isTransitionRunning() const;

    /**
     * @brief Returns transition progress. The default is 1.
     * @return Progress from 0 to 1.
     */
    qreal transitionProgress() const;

public Q_SLOTS:
    /**
     * @brief Sets a source and starts loading.
     * @param source Path, URL, resource, or data string.
     */
    void setSource(const QString& source);

    /**
     * @brief Reloads the current source.
     */
    void reload();

    /**
     * @brief Cancels the current request while retaining the source and current image.
     */
    void cancelCurrentRequest();

Q_SIGNALS:
    /**
     * @brief Emitted when the source changes.
     * @param source New source.
     */
    void sourceChanged(const QString& source);

    /**
     * @brief Emitted when state changes.
     * @param state New state.
     */
    void stateChanged(ImageBoxState state);

    /**
     * @brief Emitted when a request starts.
     */
    void loadingStarted();

    /**
     * @brief Emitted after a request succeeds.
     */
    void loaded();

    /**
     * @brief Emitted after a request fails.
     * @param error Error code.
     * @param message Error message.
     */
    void loadFailed(cache::ImageError error, const QString& message);

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
    ImageBoxConfig config_;
    QTimer resizeTimer_;
    QSize requestedTarget_;
    qreal requestedDpr_ = 0;
    bool suspended_ = false;
    bool resumePending_ = false;
    detail::ImageBoxPresentation* presentation_;
};
}

Q_DECLARE_METATYPE(aster::gui::ImageBoxState)
Q_DECLARE_METATYPE(aster::cache::ImageError)
