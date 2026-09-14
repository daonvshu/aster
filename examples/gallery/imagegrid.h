#pragma once

#include "aster/gui/imagebox.h"

#include <QAbstractScrollArea>
#include <QMap>

namespace aster::gallery
{
class ImageGrid : public QAbstractScrollArea
{
    Q_OBJECT

public:
    explicit ImageGrid(QSharedPointer<cache::ImagePipeline> pipeline, QWidget* parent = nullptr);
    void setFiles(QStringList files);
    int imageCount() const;
    int visibleItemCount() const;
    void setAlgorithm(gui::ImageScaleAlgorithm algorithm);
    void setFit(gui::ImageFit fit);
    void setTransition(gui::ImageTransition transition);
    void refresh();
    void runRace(const QString& slow, const QString& fast);

protected:
    void resizeEvent(QResizeEvent* event) override;
    void scrollContentsBy(int dx, int dy) override;

private:
    struct Card
    {
        QWidget* widget;
        gui::ImageBox* image;
    };

    void updateItems();
    void clearItems();
    QSharedPointer<cache::ImagePipeline> pipeline_;
    QStringList files_;
    QMap<int, Card> cards_;
    gui::ImageBoxConfig config_;
};
}
