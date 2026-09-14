#include "aster/cache/cache/encodedmemorycache.h"
#include "aster/cache/cache/filediskcache.h"

#include <QCryptographicHash>
#include <QTemporaryDir>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <stdexcept>

using namespace aster::cache;

int diskBenchmark() {
    using Timer = std::chrono::steady_clock;
    QTemporaryDir root;
    if (!root.isValid())
        return 1;
    FileDiskCache writer(root.path(), 512 * 1024 * 1024, 4096);
    std::vector<QByteArray> keys;
    keys.reserve(100000);
    std::cout << "entries,operation,p50_us,p95_us\n" << std::flush;
    for (int count : {10000, 100000}) {
        for (int i = int(keys.size()); i < count; ++i) {
            auto key = QCryptographicHash::hash(QByteArray::number(i), QCryptographicHash::Sha256);
            if (!writer.put(key, {"benchmark", {}}))
                return 2;
            keys.push_back(key);
            if ((i + 1) % 10000 == 0)
                std::cerr << "Prepared " << i + 1 << " entries\n";
        }
        const auto start = Timer::now();
        FileDiskCache reader(root.path(), 512 * 1024 * 1024, 4096);
        const auto construction = std::chrono::duration<double, std::micro>(Timer::now() - start).count();
        std::cout << count << ",lazy_construct," << construction << ',' << construction << '\n';
        std::vector<double> samples;
        for (int i = 0; i < 1000; ++i) {
            const auto before = Timer::now();
            if (!reader.get(keys[(i * 97) % count]))
                return 3;
            samples.push_back(std::chrono::duration<double, std::micro>(Timer::now() - before).count());
        }
        std::sort(samples.begin(), samples.end());
        std::cout << count << ",lookup," << samples[500] << ',' << samples[949] << '\n';
        const auto recovery = Timer::now();
        if (!reader.recover() || reader.stats().entryCount != count)
            return 4;
        const auto recovered = std::chrono::duration<double, std::micro>(Timer::now() - recovery).count();
        std::cout << count << ",recover," << recovered << ',' << recovered << '\n' << std::flush;
    }
    return 0;
}
