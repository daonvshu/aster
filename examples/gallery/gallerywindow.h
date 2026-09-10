#pragma once

#include "imagecatalog.h"
#include "imagegrid.h"

#include <QMainWindow>

class QCheckBox;
class QLabel;
class QLineEdit;

namespace aster::gallery
{
class GalleryWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit GalleryWindow(QSharedPointer<cache::ImagePipeline> pipeline,
                           QWidget* parent = nullptr);
    void openFolder(const QString& path);

private:
    ImageCatalog* catalog_;
    ImageGrid* grid_;
    QLineEdit* directory_;
    QCheckBox* recursive_;
    QLabel* message_;
};
}
