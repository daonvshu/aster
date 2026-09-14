#pragma once

#include "aster/cache/core/imageresult.h"
#include "aster/cache/core/imagetask.h"

#include <QObject>
#include <QSharedPointer>

Q_DECLARE_METATYPE(aster::cache::ImageResult)

namespace aster::cache {
class ImagePipeline;

class ImageSubscription final : public QObject {
    Q_OBJECT

public:
    ~ImageSubscription() override;
    bool isFinished() const;

public Q_SLOTS:
    void cancel();

Q_SIGNALS:
    void finished(aster::cache::ImageResult result);

private:
    friend class ImagePipeline;
    explicit ImageSubscription(QObject* parent);
    std::function<void(ImageResult)> completion() const;
    void attach(Subscription);
    void deliver(ImageResult);

    struct DeliveryState;
    QSharedPointer<DeliveryState> delivery_;
    Subscription subscription_;
    bool cancelled_ = false;
    bool finished_ = false;
};
} // namespace aster::cache
