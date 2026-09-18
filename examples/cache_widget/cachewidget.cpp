#include "cachewidget.h"

#include <QFileInfo>
#include <QPainter>

namespace {
bool sameTransformations(const QVector<QSharedPointer<aster::cache::ImageTransformation>>& first,
                         const QVector<QSharedPointer<aster::cache::ImageTransformation>>& second) {
    if (first.size() != second.size())
        return false;
    for (int index = 0; index < first.size(); ++index)
        if (!first[index] || !second[index] || !(first[index]->identity() == second[index]->identity()))
            return false;
    return true;
}
} // namespace

CacheWidget::CacheWidget(QSharedPointer<aster::cache::ImagePipeline> pipeline, aster::cache::ImageScaleAlgorithm algorithm, QWidget* parent)
    : QWidget(parent)
    , pipeline_(std::move(pipeline))
    , algorithm_(algorithm) {
    setMinimumSize(180, 180);
    setAutoFillBackground(true);
}

void CacheWidget::setSource(const QString& source) {
    source_ = source;
    image_ = {};
    if (subscription_) {
        subscription_->cancel();
        subscription_.clear();
    }
    if (!pipeline_ || source.isEmpty()) {
        update();
        return;
    }

    aster::cache::RenderOptions options;
    options.physicalTargetSize = size() * devicePixelRatioF();
    options.dpr = devicePixelRatioF();
    options.fitMode = aster::cache::fitName(fit_);
    options.scaleAlgorithm = algorithm_;
    options.transformations = transformations_;
    auto* subscription = pipeline_->request(source, options, this);
    subscription_ = subscription;
    connect(subscription, &aster::cache::ImageSubscription::finished, this, [this, subscription](aster::cache::ImageResult result) {
        if (subscription_ != subscription)
            return;
        subscription_.clear();
        if (result)
            image_ = std::move(*result.value);
        update();
    });
    update();
}

void CacheWidget::setFit(aster::cache::ImageFit fit) {
    if (fit_ == fit)
        return;
    fit_ = fit;
    if (!source_.isEmpty())
        setSource(source_);
}

void CacheWidget::setTransformations(QVector<QSharedPointer<aster::cache::ImageTransformation>> transformations) {
    if (sameTransformations(transformations_, transformations))
        return;
    transformations_ = std::move(transformations);
    if (!source_.isEmpty())
        setSource(source_);
}

void CacheWidget::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), palette().color(QPalette::Window));
    if (!image_.isNull())
        painter.drawImage(rect(), image_);
    else
        painter.drawText(rect(), Qt::AlignCenter, QFileInfo(source_).fileName().isEmpty() ? QStringLiteral("Loading...") : QFileInfo(source_).fileName());
}
