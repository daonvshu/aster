#include "aster/cache/cache/filediskcache.h"
#include "aster/cache/cache/rendereddiskcache.h"

#include <QTemporaryDir>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <vector>

using namespace aster::cache;

int renderedBenchmark()
{
    QTemporaryDir directory;
    auto disk =
        std::make_shared<FileDiskCache>(directory.path(), 16 * 1024 * 1024, 4 * 1024 * 1024);
    RenderedDiskCache cache(disk);
    RenderOptions options;
    options.physicalTargetSize = QSize(512, 512);
    options.processors = {{"qt-smooth-scale", 1, "2048-to-512"}};
    auto key = *KeyBuilder()
                    .render(*KeyBuilder().network(QUrl("https://benchmark/image")).value, options)
                    .value;
    QImage original(2048, 2048, QImage::Format_ARGB32);
    for (int y = 0; y < original.height(); ++y)
    {
        auto* line = reinterpret_cast<QRgb*>(original.scanLine(y));
        for (int x = 0; x < original.width(); ++x)
            line[x] = qRgb(x % 256, y % 256, (x + y) % 256);
    }
    auto resized = original.scaled(options.physicalTargetSize, Qt::IgnoreAspectRatio,
                                   Qt::SmoothTransformation);
    if (!cache.put(key, resized))
        return 1;
    std::cout << "operation,p50_us,p95_us\n";
    for (bool fromDisk : {false, true})
    {
        std::vector<double> times;
        for (int i = 0; i < 100; ++i)
        {
            auto before = std::chrono::steady_clock::now();
            auto image = fromDisk
                             ? cache.get(key).value.value_or(QImage())
                             : original.scaled(options.physicalTargetSize, Qt::IgnoreAspectRatio,
                                               Qt::SmoothTransformation);
            if (image.isNull())
                return 2;
            times.push_back(
                std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - before)
                    .count());
        }
        std::sort(times.begin(), times.end());
        std::cout << (fromDisk ? "rendered_png_read" : "qt_smooth_scale") << ',' << times[50] << ','
                  << times[94] << '\n';
    }
    return 0;
}
