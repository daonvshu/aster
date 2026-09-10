#include "gallerywindow.h"

#include "httptestpanel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStatusBar>
#include <QTimer>
#include <QVBoxLayout>

namespace aster::gallery
{
GalleryWindow::GalleryWindow(QSharedPointer<cache::ImagePipeline> pipeline, QWidget* parent)
    : QMainWindow(parent), catalog_(new ImageCatalog(this)), grid_(new ImageGrid(pipeline, this)),
      directory_(new QLineEdit(this)), recursive_(new QCheckBox(tr("包含子目录"), this)),
      message_(new QLabel(tr("选择一个文件夹，开始浏览图片。"), this))
{
    setStyleSheet(
        "QWidget { background: #18212e; color: #e5eaf1; font-size: 13px; }"
        "QLineEdit, QComboBox { background: #253246; border: 1px solid #435168; border-radius: "
        "4px; padding: 7px; }"
        "QPushButton { background: #30445e; border: 1px solid #506580; border-radius: 4px; "
        "padding: 8px 12px; }"
        "QPushButton:hover { background: #405a7c; }"
        "QPushButton:focus, QLineEdit:focus, QComboBox:focus { border: 1px solid #80b8ff; }"
        "QToolTip { color: #18212e; background: #f3f6fa; border: 1px solid #8090a0; }");
    setWindowTitle(tr("aster · ImageBox 图片浏览测试"));
    resize(1280, 850);
    setMinimumSize(800, 500);
    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);
    layout->setContentsMargins(16, 16, 16, 12);
    layout->setSpacing(12);
    auto* sources = new QHBoxLayout;
    auto* choose = new QPushButton(tr("选择文件夹…"), this);
    auto* load = new QPushButton(tr("读取"), this);
    directory_->setPlaceholderText(tr("文件夹路径，也可以粘贴路径后按 Enter"));
    directory_->setClearButtonEnabled(true);
    recursive_->setChecked(true);
    sources->addWidget(choose);
    sources->addWidget(directory_, 1);
    sources->addWidget(load);
    sources->addWidget(recursive_);
    layout->addLayout(sources);
    auto* http = new HttpTestPanel(this);
    layout->addWidget(http);
    connect(http, &HttpTestPanel::message, message_, &QLabel::setText);
    connect(http, &HttpTestPanel::sourcesRequested, this,
            [this](const QStringList& sources)
            {
                catalog_->cancel();
                grid_->setFiles(sources);
                message_->setText(
                    tr("HTTP 测试：重新加载可观察 304/缓存；302 按当前手动重定向策略显示失败。"));
            });
    connect(http, &HttpTestPanel::raceRequested, this,
            [this](const QString& slow, const QString& fast)
            {
                catalog_->cancel();
                grid_->runRace(slow, fast);
                message_->setText(tr("同一个 ImageBox：加载慢 A，100ms 后切换 B；最终应保持 B。"));
            });

    auto* options = new QHBoxLayout;
    auto* algorithm = new QComboBox(this);
    algorithm->addItems({"QtFast", "QtSmooth", "Bilinear", "Bicubic", "Lanczos3", "Lanczos4"});
    algorithm->setCurrentIndex(1);
    auto* fit = new QComboBox(this);
    fit->addItem(tr("适应 · Contain"), int(gui::ImageFit::Contain));
    fit->addItem(tr("铺满裁剪 · Cover"), int(gui::ImageFit::Cover));
    fit->addItem(tr("拉伸 · Fill"), int(gui::ImageFit::Fill));
    fit->addItem(tr("仅缩小 · ScaleDown"), int(gui::ImageFit::ScaleDown));
    fit->addItem(tr("原始大小 · None"), int(gui::ImageFit::None));
    auto* transition = new QComboBox(this);
    transition->addItems({"None", "Fade", "CrossFade", "Slide", "Zoom", "FadeZoom"});
    auto* refresh = new QPushButton(tr("重新加载当前图片"), this);
    options->addWidget(new QLabel(tr("缩放算法"), this));
    options->addWidget(algorithm);
    options->addSpacing(12);
    options->addWidget(new QLabel(tr("显示方式"), this));
    options->addWidget(fit);
    options->addSpacing(12);
    options->addWidget(new QLabel(tr("过渡动画"), this));
    options->addWidget(transition);
    options->addStretch();
    options->addWidget(refresh);
    layout->addLayout(options);
    message_->setTextFormat(Qt::PlainText);
    message_->setWordWrap(true);
    layout->addWidget(message_);
    layout->addWidget(grid_, 1);
    setCentralWidget(central);

    connect(choose, &QPushButton::clicked, this,
            [this]
            {
                const auto folder = QFileDialog::getExistingDirectory(this, tr("选择图片文件夹"),
                                                                      directory_->text());
                if (!folder.isEmpty())
                    openFolder(folder);
            });
    connect(load, &QPushButton::clicked, this,
            [this]
            {
                openFolder(directory_->text());
            });
    connect(directory_, &QLineEdit::returnPressed, this,
            [this]
            {
                openFolder(directory_->text());
            });
    connect(algorithm, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int index)
            {
                grid_->setAlgorithm(gui::ImageScaleAlgorithm(index));
            });
    connect(fit, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this, fit](int index)
            {
                grid_->setFit(gui::ImageFit(fit->itemData(index).toInt()));
            });
    connect(transition, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int index)
            {
                grid_->setTransition(gui::ImageTransition(index));
            });
    connect(refresh, &QPushButton::clicked, grid_, &ImageGrid::refresh);
    connect(catalog_, &ImageCatalog::started, this,
            [this]
            {
                message_->setText(tr("正在扫描文件夹… 可以随时选择其他文件夹。"));
            });
    connect(catalog_, &ImageCatalog::failed, message_, &QLabel::setText);
    connect(catalog_, &ImageCatalog::ready, this,
            [this](const QStringList& files)
            {
                grid_->setFiles(files);
                message_->setText(
                    files.isEmpty()
                        ? tr("此文件夹中没有找到支持的图片文件。")
                        : tr("共 %1 张图片 · 每行 6 张 · 滚动浏览 · 悬停可查看路径或加载错误")
                              .arg(files.size()));
            });
    auto* stats = new QTimer(this);
    stats->setInterval(500);
    connect(stats, &QTimer::timeout, this,
            [this, pipeline]
            {
                const auto value = pipeline->cacheStats();
                statusBar()->showMessage(
                    tr("图片 %1 | 控件 %2 | Active %3 | 缓存 %4 MiB | 命中 %5 | HTTP %6 | 304 %7")
                        .arg(grid_->imageCount())
                        .arg(grid_->visibleItemCount())
                        .arg(value.active.entries)
                        .arg(value.renderedMemory.totalBytes / (1024.0 * 1024), 0, 'f', 1)
                        .arg(value.renderedMemory.hits)
                        .arg(value.network.requests)
                        .arg(value.network.validations));
            });
    stats->start();
}

void GalleryWindow::openFolder(const QString& path)
{
    const auto folder = QDir::cleanPath(path.trimmed());
    if (path.trimmed().isEmpty())
    {
        message_->setText(tr("请先选择或输入文件夹路径。"));
        return;
    }
    directory_->setText(QDir::toNativeSeparators(folder));
    catalog_->loadFolder(folder, recursive_->isChecked());
}
}
