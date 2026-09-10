#include "aster/cache/renderer/imageresampler.h"

#include <QElapsedTimer>

#include <algorithm>
#include <array>
#include <iostream>
#include <vector>

using namespace aster::cache;

int lutBenchmark()
{
    const std::atomic<bool> running{false};
    std::array<ImageResampler, 2> resamplers = {
        ImageResampler({}, ResampleExecution::Automatic, KernelEvaluation::Exact),
        ImageResampler({}, ResampleExecution::Automatic, KernelEvaluation::Lut)};
    std::cout << "kernel,algorithm,source,median_ms,weights_ms\n";
    for (auto size : {QSize(256, 256), QSize(1024, 1024), QSize(3840, 2160)})
    {
        QImage input(size, QImage::Format_RGB32);
        input.fill(QColor(80, 120, 160));
        const QSize target = size.width() == 3840 ? QSize(1920, 1080) : size / 4;
        for (auto algorithm : {ImageScaleAlgorithm::Lanczos3, ImageScaleAlgorithm::Lanczos4})
        {
            std::array<std::vector<double>, 2> total, weights;
            for (int round = -1; round < 9; ++round)
                for (int position = 0; position < 2; ++position)
                {
                    const int index = round >= 0 && round % 2 ? 1 - position : position;
                    ResampleMetrics metrics;
                    QElapsedTimer timer;
                    timer.start();
                    const auto result = resamplers[index].resize(
                        input, {target, QRect(QPoint(), target)}, algorithm, running, &metrics);
                    const auto ms = timer.nsecsElapsed() / 1e6;
                    if (!result)
                        return 1;
                    if (round >= 0)
                    {
                        total[index].push_back(ms);
                        weights[index].push_back(metrics.weightGenerationNs / 1e6);
                    }
                }
            for (int index = 0; index < 2; ++index)
            {
                std::sort(total[index].begin(), total[index].end());
                std::sort(weights[index].begin(), weights[index].end());
                std::cout << (index ? "lut" : "exact") << ',' << int(algorithm) << ','
                          << size.width() << 'x' << size.height() << ',' << total[index][4] << ','
                          << weights[index][4] << std::endl;
            }
        }
    }
    return 0;
}

int resamplerBenchmark()
{
    const std::atomic<bool> running{false};
    std::array<ImageResampler, 2> resamplers = {ImageResampler({}, ResampleExecution::Scalar),
                                                ImageResampler{}};
    std::cout << "backend,algorithm,source,target,median_ms,output_megapixels_per_sec,working_"
                 "bytes_bound\n";
    for (const auto sourceSize : {QSize(256, 256), QSize(1024, 1024), QSize(3840, 2160)})
    {
        const QSize target = sourceSize.width() == 3840 ? QSize(1920, 1080) : sourceSize / 4;
        QImage image(sourceSize, QImage::Format_RGB32);
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x)
                image.setPixel(x, y, qRgb(x % 256, y % 256, (x + y) % 256));
        for (const auto algorithm : {ImageScaleAlgorithm::QtFast, ImageScaleAlgorithm::QtSmooth,
                                     ImageScaleAlgorithm::Bilinear, ImageScaleAlgorithm::Bicubic,
                                     ImageScaleAlgorithm::Lanczos3, ImageScaleAlgorithm::Lanczos4})
        {
            std::array<std::vector<double>, 2> samples;
            ResampleMetrics metrics;
            // Warm both paths, then alternate order to reduce drift and first-run bias.
            for (int round = -1; round < 5; ++round)
                for (int position = 0; position < 2; ++position)
                {
                    const int backend = (round >= 0 && round % 2) ? 1 - position : position;
                    QElapsedTimer timer;
                    timer.start();
                    const auto result = resamplers[backend].resize(
                        image, {target, QRect(QPoint(), target)}, algorithm, running, &metrics);
                    const double ms = timer.nsecsElapsed() / 1e6;
                    if (!result)
                        return 1;
                    if (round >= 0)
                        samples[backend].push_back(ms);
                }
            for (int backend = 0; backend < 2; ++backend)
            {
                std::sort(samples[backend].begin(), samples[backend].end());
                const double ms = samples[backend][2];
                const char* name = backend == 0                      ? "scalar"
                                   : ImageResampler::simdAvailable() ? "sse2"
                                                                     : "scalar-fallback";
                std::cout << name << ',' << int(algorithm) << ',' << sourceSize.width() << 'x'
                          << sourceSize.height() << ',' << target.width() << 'x' << target.height()
                          << ',' << ms << ','
                          << (double(target.width()) * target.height() / (ms * 1000)) << ','
                          << metrics.workingBytes << std::endl;
            }
        }
    }
    return 0;
}
