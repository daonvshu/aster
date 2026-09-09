#include "aster/cache/decoder/boundedimagedecoder.h"

#include <QBuffer>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <vector>

using namespace aster::cache;

int decoderBenchmark()
{
    QImage image(512, 512, QImage::Format_ARGB32);
    image.fill(QColor(16, 64, 128, 128));
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    if (!image.save(&buffer, "PNG"))
        return 1;

    const std::atomic<bool> cancelled{false};
    std::cout << "operation,p50_us,p95_us\n";
    for (bool reject : {false, true})
    {
        DecodeLimits limits;
        if (reject)
            limits.maxSide = 128;
        BoundedImageDecoder decoder(limits);
        std::vector<double> samples;
        for (int i = 0; i < 110; ++i)
        {
            const auto started = std::chrono::steady_clock::now();
            auto result = decoder.decode(bytes, cancelled);
            const auto elapsed = std::chrono::duration<double, std::micro>(
                                     std::chrono::steady_clock::now() - started)
                                     .count();
            if (reject ? result.error != ImageError::ResourceLimit : !result)
                return 1;
            if (i >= 10)
                samples.push_back(elapsed);
        }
        std::sort(samples.begin(), samples.end());
        std::cout << (reject ? "oversize_header_reject" : "bounded_png_decode") << ','
                  << samples[50] << ',' << samples[94] << '\n';
    }
    return 0;
}
