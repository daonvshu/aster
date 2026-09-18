#pragma once

#include "imagecatalog.h"
#include "imagegrid.h"

#include <QMainWindow>

#include <functional>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;

namespace aster::gallery {
class GalleryWindow : public QMainWindow {
    Q_OBJECT

public:
    enum class DiskCacheKind { File, Sqlite };
    using PipelineFactory = std::function<QSharedPointer<cache::ImagePipeline>(DiskCacheKind)>;

    explicit GalleryWindow(QSharedPointer<cache::ImagePipeline> pipeline, QWidget* parent = nullptr);
    void setDiskCacheFactory(PipelineFactory factory, DiskCacheKind current = DiskCacheKind::File);
    void openFolder(const QString& path);

private:
    void applyDiskCache(DiskCacheKind kind);

    QSharedPointer<cache::ImagePipeline> pipeline_;
    ImageCatalog* catalog_;
    ImageGrid* grid_;
    QLineEdit* directory_;
    QCheckBox* recursive_;
    QComboBox* diskCache_;
    QLabel* diskCacheSize_;
    QLabel* message_;
    PipelineFactory pipelineFactory_;
    DiskCacheKind diskCacheKind_ = DiskCacheKind::File;
};
} // namespace aster::gallery
