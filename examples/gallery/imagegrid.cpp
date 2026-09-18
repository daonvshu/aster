#include "imagegrid.h"
#include "smoothscrollbar.h"

#include <QFileInfo>
#include <QLabel>
#include <QPainter>
#include <QResizeEvent>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <limits>

namespace aster::gallery {
namespace {
constexpr int columns = 6;
constexpr int gap = 12;
constexpr int caption = 30;

QImage statusImage(const QString& text, const QColor& color) {
    QImage image(240, 240, QImage::Format_RGB32);
    image.fill(color);
    QPainter painter(&image);
    painter.setPen(QColor("#aeb9c8"));
    painter.drawText(image.rect(), Qt::AlignCenter, text);
    return image;
}
} // namespace

ImageGrid::ImageGrid(QSharedPointer<cache::ImagePipeline> pipeline, QWidget* parent)
    : QAbstractScrollArea(parent)
    , pipeline_(std::move(pipeline)) {
    config_.offscreenPolicy(gui::OffscreenPolicy::ReleaseImage)
            .transitionDuration(220)
            .loadingIndicator()
            .cornerRadius(6)
            .placeholder(statusImage(tr("加载中"), QColor("#222c3b")))
            .errorImage(statusImage(tr("加载失败"), QColor("#3c2830")))
            .errorReplacesImage();
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    setVerticalScrollBar(new SmoothScrollbar);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setFrameShape(QFrame::NoFrame);
    setMinimumWidth(650);
}

void ImageGrid::setPipeline(QSharedPointer<cache::ImagePipeline> pipeline) {
    if (pipeline_ == pipeline)
        return;

    pipeline_ = std::move(pipeline);
    for (const auto& card : cards_)
        card.image->setPipeline(pipeline_);
}

void ImageGrid::clearItems() {
    for (const auto& card : cards_)
        delete card.widget;
    cards_.clear();
}

void ImageGrid::setFiles(QStringList files) {
    clearItems();
    files_ = std::move(files);
    {
        const QSignalBlocker block(verticalScrollBar());
        verticalScrollBar()->setValue(0);
    }
    updateItems();
}

int ImageGrid::imageCount() const {
    return files_.size();
}

int ImageGrid::visibleItemCount() const {
    return cards_.size();
}

void ImageGrid::setAlgorithm(gui::ImageScaleAlgorithm algorithm) {
    config_.scaleAlgorithm(algorithm);
    for (const auto& card : cards_)
        card.image->setConfig(config_);
}

void ImageGrid::setFit(gui::ImageFit fit) {
    config_.fit(fit);
    for (const auto& card : cards_)
        card.image->setConfig(config_);
}

void ImageGrid::setTransition(gui::ImageTransition transition) {
    config_.transition(transition);
    for (const auto& card : cards_)
        card.image->setConfig(config_);
}

void ImageGrid::setTransitionPolicy(gui::TransitionPolicy policy) {
    config_.transitionPolicy(policy);
    for (const auto& card : cards_)
        card.image->setConfig(config_);
}

void ImageGrid::setOffscreenPolicy(gui::OffscreenPolicy policy) {
    config_.offscreenPolicy(policy);
    for (const auto& card : cards_)
        card.image->setConfig(config_);
}

void ImageGrid::refresh() {
    for (const auto& card : cards_)
        card.image->reload();
}

void ImageGrid::runRace(const QString& slow, const QString& fast) {
    setFiles({slow});
    if (cards_.isEmpty())
        return;
    QPointer<gui::ImageBox> box = cards_.first().image;
    QTimer::singleShot(100, this, [this, box, fast] {
        if (!box)
            return;
        files_[0] = fast;
        box->parentWidget()->setToolTip(fast);
        box->setSource(fast);
    });
}

void ImageGrid::resizeEvent(QResizeEvent* event) {
    QAbstractScrollArea::resizeEvent(event);
    updateItems();
}

void ImageGrid::scrollContentsBy(int, int) {
    updateItems();
}

void ImageGrid::updateItems() {
    const int cellWidth = std::max(1, (viewport()->width() - gap * (columns + 1)) / columns);
    const int rowHeight = cellWidth + caption + gap;
    const qint64 rows = (qint64(files_.size()) + columns - 1) / columns;
    const int totalHeight = int(std::min<qint64>(std::numeric_limits<int>::max(), rows * rowHeight + gap));
    {
        const QSignalBlocker block(verticalScrollBar());
        verticalScrollBar()->setRange(0, std::max(0, totalHeight - viewport()->height()));
        verticalScrollBar()->setPageStep(viewport()->height());
        verticalScrollBar()->setSingleStep(std::max(20, rowHeight / 4));
    }
    const int offset = verticalScrollBar()->value();
    const qint64 viewportHeight = viewport()->height();
    const qint64 bufferedTop = std::max<qint64>(0, qint64(offset) - viewportHeight);
    const qint64 bufferedBottom = std::min<qint64>(totalHeight, qint64(offset) + viewportHeight * 2);
    const int first = int(bufferedTop / rowHeight * columns);
    const int last = int(std::min<qint64>(files_.size(), (bufferedBottom + rowHeight - 1) / rowHeight * columns));
    for (auto it = cards_.begin(); it != cards_.end();) {
        if (it.key() < first || it.key() >= last) {
            delete it->widget;
            it = cards_.erase(it);
        } else
            ++it;
    }
    for (int index = first; index < last; ++index) {
        if (!cards_.contains(index)) {
            auto* widget = new QWidget(viewport());
            auto* layout = new QVBoxLayout(widget);
            layout->setContentsMargins(0, 0, 0, 0);
            layout->setSpacing(4);
            auto* box = new gui::ImageBox(widget);
            box->setPipeline(pipeline_);
            box->setConfig(config_);
            const QUrl sourceUrl(files_[index]);
            const auto name = sourceUrl.scheme().startsWith("http") ? sourceUrl.path() : QFileInfo(files_[index]).fileName();
            auto* label = new QLabel(name, widget);
            label->setTextFormat(Qt::PlainText);
            label->setAlignment(Qt::AlignCenter);
            label->setFixedHeight(caption - 4);
            label->setMinimumWidth(0);
            label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
            widget->setToolTip(files_[index]);
            connect(box, &gui::ImageBox::stateChanged, label, [label, box](gui::ImageBoxState state) {
                const QStringList states = {QObject::tr("空"), QObject::tr("加载中"), QObject::tr("成功"), QObject::tr("失败")};
                const QUrl url(box->source());
                const auto title = url.scheme().startsWith("http") ? url.path() : QFileInfo(box->source()).fileName();
                label->setText(title + " · " + states[int(state)]);
                if (state == gui::ImageBoxState::Ready)
                    box->parentWidget()->setToolTip(box->source());
            });
            connect(box, &gui::ImageBox::loadFailed, widget, [widget, box](cache::ImageError error, const QString& message) {
                widget->setToolTip(box->source() + "\n" + QObject::tr("加载失败（%1）：%2").arg(int(error)).arg(message));
            });
            layout->addWidget(box, 1);
            layout->addWidget(label);
            box->setSource(files_[index]);
            cards_.insert(index, {widget, box});
        }
        auto& card = cards_[index];
        card.widget->setGeometry(gap + (index % columns) * (cellWidth + gap), gap + (index / columns) * rowHeight - offset, cellWidth, rowHeight - gap);
        card.widget->show();
    }
}
} // namespace aster::gallery
