#include "aster/cache/cache/renderedmemorycache.h"

#include <QCoreApplication>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

using namespace aster::cache;
using Timer = std::chrono::steady_clock;
using Samples = std::vector<double>;

int diskBenchmark();
int renderedBenchmark();
int decoderBenchmark();
int resamplerBenchmark();
int lutBenchmark();

template <class Function>
Samples measure(int count, Function operation) {
    Samples samples;
    samples.reserve(size_t(count));
    for (int i = 0; i < count; ++i) {
        const auto start = Timer::now();
        operation(i);
        samples.push_back(std::chrono::duration<double, std::micro>(Timer::now() - start).count());
    }
    return samples;
}

void report(int count, const char* name, Samples samples) {
    std::sort(samples.begin(), samples.end());
    std::cout << count << ',' << name << ',' << samples[samples.size() / 2] << ',' << samples[(samples.size() - 1) * 95 / 100] << '\n';
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (app.arguments().contains("--lut"))
        return lutBenchmark();
    if (app.arguments().contains("--resample"))
        return resamplerBenchmark();
    if (app.arguments().contains("--decode"))
        return decoderBenchmark();
    if (app.arguments().contains("--rendered"))
        return renderedBenchmark();
    if (app.arguments().contains("--disk"))
        return diskBenchmark();
    std::cout << "entries,operation,p50_us,p95_us\n";
    for (int count : {1000, 10000}) {
        QImage image(16, 16, QImage::Format_ARGB32);
        image.fill(Qt::red);
        const auto cost = RenderedMemoryCache::costOf(image);
        RenderedMemoryCache cache(cost * count);
        KeyBuilder builder;
        RenderOptions options;
        options.physicalTargetSize = image.size();
        std::vector<RenderKey> keys;
        for (int i = 0; i < count * 2; ++i) {
            const auto source = builder.network(QUrl(QString("https://benchmark.test/%1").arg(i)));
            keys.push_back(*builder.render(*source.value, options).value);
        }
        report(count, "put", measure(count, [&](int i) { cache.put(keys[i], image); }));
        report(count, "get", measure(count, [&](int i) { cache.get(keys[i]); }));
        report(count, "evict_put", measure(count, [&](int i) { cache.put(keys[count + i], image); }));
        std::vector<std::thread> workers;
        std::vector<Samples> concurrent(4);
        for (int n = 0; n < 4; ++n)
            workers.emplace_back([&, n] { concurrent[n] = measure(count, [&](int i) { cache.get(keys[count + (i + n) % count]); }); });
        for (auto& worker : workers)
            worker.join();
        Samples merged;
        for (const auto& samples : concurrent)
            merged.insert(merged.end(), samples.begin(), samples.end());
        report(count, "get_4_threads", std::move(merged));
        if (cache.stats().totalBytes != count * cost || cache.stats().evictions != quint64(count))
            return 1;
    }
}
