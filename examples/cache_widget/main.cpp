#include "aster/cache/renderer/imagerenderer.h"
#include "aster/cache/service/imageservice.h"
#include "cachewidget.h"

#include <QApplication>
#include <QColor>
#include <QComboBox>
#include <QFileDialog>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    aster::cache::ImageServiceConfig config;
    config.renderer = aster::cache::ImageRenderer{};
    config.renderedMemoryBytes = 64 * 1024 * 1024;
    config.encodedMemoryBytes = 32 * 1024 * 1024;
    config.workerCount = 4;
    aster::cache::ImageService::configure(config);

    QMainWindow window;
    window.setWindowTitle(QStringLiteral("aster · cache 缩放算法对比"));
    window.resize(1280, 400);
    window.setStyleSheet("QWidget { background: #18212e; color: #e5eaf1; font-size: 13px; }"
                         "QLineEdit, QComboBox, QSpinBox { background: #253246; border: 1px solid "
                         "#435168; border-radius: 4px; padding: 7px; }"
                         "QPushButton { background: #30445e; border: 1px solid #506580; "
                         "border-radius: 4px; padding: 8px 12px; }"
                         "QPushButton:hover { background: #405a7c; }");
    auto* central = new QWidget(&window);
    auto* root = new QVBoxLayout(central);
    root->setContentsMargins(16, 16, 16, 12);
    root->setSpacing(12);
    auto* source = new QLineEdit(central);
    source->setPlaceholderText(QStringLiteral("图片路径，也可以粘贴路径后按 Enter"));
    auto* choose = new QPushButton(QStringLiteral("选择图片…"), central);
    auto* load = new QPushButton(QStringLiteral("读取"), central);
    auto* sourceRow = new QHBoxLayout;
    sourceRow->addWidget(source, 1);
    sourceRow->addWidget(choose);
    sourceRow->addWidget(load);
    root->addLayout(sourceRow);
    auto* options = new QHBoxLayout;
    auto* fit = new QComboBox(central);
    fit->addItem(QStringLiteral("适应 · Contain"), int(aster::cache::ImageFit::Contain));
    fit->addItem(QStringLiteral("铺满裁剪 · Cover"), int(aster::cache::ImageFit::Cover));
    fit->addItem(QStringLiteral("拉伸 · Fill"), int(aster::cache::ImageFit::Fill));
    fit->addItem(QStringLiteral("仅缩小 · ScaleDown"), int(aster::cache::ImageFit::ScaleDown));
    auto* effect = new QComboBox(central);
    effect->addItem(QStringLiteral("无效果"));
    effect->addItem(QStringLiteral("灰度"));
    effect->addItem(QStringLiteral("反色"));
    effect->addItem(QStringLiteral("半透明 · 50%"));
    effect->addItem(QStringLiteral("蓝色色调"));
    effect->addItem(QStringLiteral("模糊 · 4 px"));
    auto* size = new QSpinBox(central);
    size->setRange(120, 600);
    size->setValue(240);
    options->addWidget(new QLabel(QStringLiteral("显示方式"), central));
    options->addWidget(fit);
    options->addSpacing(12);
    options->addWidget(new QLabel(QStringLiteral("图片效果"), central));
    options->addWidget(effect);
    options->addSpacing(12);
    options->addWidget(new QLabel(QStringLiteral("控件尺寸"), central));
    options->addWidget(size);
    options->addStretch();
    root->addLayout(options);
    auto* grid = new QGridLayout;
    grid->setSpacing(12);
    const QList<QPair<QString, aster::cache::ImageScaleAlgorithm>> algorithms = {
            {"QtFast", aster::cache::ImageScaleAlgorithm::QtFast},     {"QtSmooth", aster::cache::ImageScaleAlgorithm::QtSmooth},
            {"Bilinear", aster::cache::ImageScaleAlgorithm::Bilinear}, {"Bicubic", aster::cache::ImageScaleAlgorithm::Bicubic},
            {"Lanczos3", aster::cache::ImageScaleAlgorithm::Lanczos3}, {"Lanczos4", aster::cache::ImageScaleAlgorithm::Lanczos4}};
    QList<CacheWidget*> widgets;
    for (int i = 0; i < algorithms.size(); ++i) {
        auto* panel = new QWidget(central);
        auto* panelLayout = new QVBoxLayout(panel);
        auto* label = new QLabel(algorithms[i].first, panel);
        label->setAlignment(Qt::AlignCenter);
        auto* box = new CacheWidget(aster::cache::ImageService::pipeline(), algorithms[i].second, panel);
        box->setFixedSize(240, 240);
        panelLayout->addWidget(label);
        panelLayout->addWidget(box);
        grid->addWidget(panel, 0, i);
        widgets.append(box);
    }
    root->addLayout(grid, 1);
    window.setCentralWidget(central);
    const auto transformations = [effect] {
        using aster::cache::ImageTransformation;
        QVector<QSharedPointer<ImageTransformation>> result;
        switch (effect->currentIndex()) {
            case 1:
                result.push_back(ImageTransformation::grayscale());
                break;
            case 2:
                result.push_back(ImageTransformation::invert());
                break;
            case 3:
                result.push_back(ImageTransformation::opacity(0.5));
                break;
            case 4:
                result.push_back(ImageTransformation::tint(QColor("#4a90e2"), 0.35));
                break;
            case 5:
                result.push_back(ImageTransformation::blur(4));
                break;
            default:
                break;
        }
        return result;
    };
    const auto apply = [&] {
        const auto selectedTransformations = transformations();
        for (auto* box : widgets) {
            box->setFixedSize(size->value(), size->value());
            box->setFit(aster::cache::ImageFit(fit->currentData().toInt()));
            box->setTransformations(selectedTransformations);
            box->setSource(source->text().trimmed());
        }
    };
    QObject::connect(load, &QPushButton::clicked, &window, apply);
    QObject::connect(source, &QLineEdit::returnPressed, &window, apply);
    QObject::connect(choose, &QPushButton::clicked, &window, [&] {
        const auto path = QFileDialog::getOpenFileName(&window, QStringLiteral("选择图片"));
        if (!path.isEmpty()) {
            source->setText(path);
            apply();
        }
    });
    QObject::connect(size, QOverload<int>::of(&QSpinBox::valueChanged), &window, [&](int) {
        if (!source->text().isEmpty())
            apply();
    });
    QObject::connect(fit, QOverload<int>::of(&QComboBox::currentIndexChanged), &window, [&](int) {
        if (!source->text().isEmpty())
            apply();
    });
    QObject::connect(effect, QOverload<int>::of(&QComboBox::currentIndexChanged), &window, [&](int) {
        const auto selectedTransformations = transformations();
        for (auto* box : widgets)
            box->setTransformations(selectedTransformations);
    });
    if (QCoreApplication::arguments().size() > 1) {
        source->setText(QCoreApplication::arguments().at(1));
        apply();
    }
    window.show();
    const int result = app.exec();
    aster::cache::ImageService::shutdown();
    return result;
}
