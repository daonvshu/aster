#include "aster/cache/cache/rendereddiskcache.h"
#if ASTER_ENABLE_SQLITE_DISK_CACHE
#include "aster/cache/cache/sqlitediskcache.h"
#endif
#include "aster/cache/renderer/imagerenderer.h"
#include "aster/cache/service/imageservice.h"
#include "gallerywindow.h"
#ifdef ASTER_GALLERY_NETWORK
#include "aster/cache/source/qtnetworkservice.h"
#endif

#include <QApplication>
#include <QCommandLineParser>
#include <QMessageBox>
#include <QStandardPaths>

#include <exception>

namespace {
aster::cache::ImageServiceConfig makeGalleryConfig(aster::gallery::GalleryWindow::DiskCacheKind diskCache) {
    aster::cache::ImageServiceConfig config;
    config.renderer = aster::cache::ImageRenderer{};
    config.renderedMemoryBytes = 128 * 1024 * 1024;
    config.activeMemoryBytes = 64 * 1024 * 1024;
    config.workerCount = 4;
#if ASTER_ENABLE_SQLITE_DISK_CACHE
    if (diskCache == aster::gallery::GalleryWindow::DiskCacheKind::Sqlite) {
        const auto root = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
        constexpr qint64 sourceBudget = 256 * 1024 * 1024;
        constexpr qint64 renderedBudget = 256 * 1024 * 1024;
        constexpr qint64 maxEntry = 32 * 1024 * 1024;
        config.enableDefaultDiskCache = false;
        config.sourceDisk = QSharedPointer<aster::cache::SqliteDiskCache>::create(root + "/http-source.sqlite", sourceBudget, maxEntry);
        auto rendered = QSharedPointer<aster::cache::SqliteDiskCache>::create(root + "/rendered.sqlite", renderedBudget, maxEntry);
        config.renderedDisk = QSharedPointer<aster::cache::RenderedDiskCache>::create(rendered);
    }
#else
    Q_UNUSED(diskCache)
#endif
#ifdef ASTER_GALLERY_NETWORK
    config.network = QSharedPointer<aster::cache::QtNetworkService>::create(10000);
#endif
    return config;
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication::setApplicationName("aster_gallery");
    QCoreApplication::setOrganizationName("aster");
    QApplication app(argc, argv);
    QCommandLineParser parser;
    parser.setApplicationDescription("aster ImageBox gallery");
    parser.addHelpOption();
    parser.addPositionalArgument("folder", "Image folder to open", "[folder]");
    parser.process(app);
    try {
        const auto config = makeGalleryConfig(aster::gallery::GalleryWindow::DiskCacheKind::File);
        aster::cache::ImageService::configure(config);
        int result;
        {
            aster::gallery::GalleryWindow window(aster::cache::ImageService::pipeline());
            window.setDiskCacheFactory([](auto diskCache) { return aster::cache::ImageService::createPipeline(makeGalleryConfig(diskCache)); });
            window.show();
            if (!parser.positionalArguments().isEmpty())
                window.openFolder(parser.positionalArguments().first());
            result = app.exec();
        }
        aster::cache::ImageService::shutdown();
        return result;
    } catch (const std::exception& error) {
        QMessageBox::critical(nullptr, "aster", QString::fromUtf8(error.what()));
        return 1;
    }
}
