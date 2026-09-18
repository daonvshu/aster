#pragma once

#include "aster/cache/core/imagetransformation.h"
#include "aster/cache/pipeline/imagepipeline.h"
#include "aster/cache/pipeline/imagesubscription.h"

#include <QImage>
#include <QPointer>
#include <QVector>
#include <QWidget>

class CacheWidget final : public QWidget {
    Q_OBJECT

public:
    explicit CacheWidget(QSharedPointer<aster::cache::ImagePipeline> pipeline, aster::cache::ImageScaleAlgorithm algorithm, QWidget* parent = nullptr);
    void setSource(const QString& source);
    void setFit(aster::cache::ImageFit fit);
    void setTransformations(QVector<QSharedPointer<aster::cache::ImageTransformation>> transformations);

protected:
    void paintEvent(QPaintEvent*) override;

private:
    QSharedPointer<aster::cache::ImagePipeline> pipeline_;
    QPointer<aster::cache::ImageSubscription> subscription_;
    QImage image_;
    QString source_;
    aster::cache::ImageScaleAlgorithm algorithm_;
    aster::cache::ImageFit fit_ = aster::cache::ImageFit::Contain;
    QVector<QSharedPointer<aster::cache::ImageTransformation>> transformations_;
};
