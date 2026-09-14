#include "imagesubscription.h"

#include <QMetaObject>
#include <QSharedPointer>
#include <QThread>
#include <QWeakPointer>

#include <mutex>
#include <stdexcept>

namespace aster::cache {
struct ImageSubscription::DeliveryState {
    std::mutex mutex;
    ImageSubscription* target = nullptr;
};

ImageSubscription::ImageSubscription(QObject* parent)
    : QObject(nullptr)
    , delivery_(QSharedPointer<DeliveryState>::create()) {
    if (parent && parent->thread() != QThread::currentThread())
        throw std::invalid_argument("Subscription parent must belong to the calling thread");

    setParent(parent);
    delivery_->target = this;
    qRegisterMetaType<aster::cache::ImageResult>("aster::cache::ImageResult");
}

ImageSubscription::~ImageSubscription() {
    {
        std::lock_guard<std::mutex> lock(delivery_->mutex);
        delivery_->target = nullptr;
    }
    subscription_.cancel();
}

std::function<void(ImageResult)> ImageSubscription::completion() const {
    return [weak = QWeakPointer<DeliveryState>(delivery_)](ImageResult result) {
        if (auto state = weak.toStrongRef()) {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (auto* target = state->target) {
                // Destruction takes the same lock before QObject removes queued calls.
                QMetaObject::invokeMethod(target, [target, result = std::move(result)]() mutable { target->deliver(std::move(result)); }, Qt::QueuedConnection);
            }
        }
    };
}

void ImageSubscription::attach(Subscription subscription) {
    subscription_ = std::move(subscription);
    if (cancelled_)
        subscription_.cancel();
}

bool ImageSubscription::isFinished() const {
    return finished_;
}

void ImageSubscription::cancel() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (finished_ || cancelled_)
        return;

    cancelled_ = true;
    subscription_.cancel();
    completion()(ImageResult::failure(ImageError::Cancelled));
}

void ImageSubscription::deliver(ImageResult result) {
    if (finished_)
        return;

    finished_ = true;
    if (cancelled_)
        result = ImageResult::failure(ImageError::Cancelled);

    // No member access after emission: a direct slot may destroy the sender.
    deleteLater();
    Q_EMIT finished(std::move(result));
}
} // namespace aster::cache
