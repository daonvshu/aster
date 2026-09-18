#include "aster/cache/cache/filediskcache.h"
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

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName("aster_gallery");
    QCoreApplication::setOrganizationName("aster");
    QCommandLineParser parser;
    parser.setApplicationDescription("aster ImageBox gallery");
    parser.addHelpOption();
    parser.addPositionalArgument("folder", "Image folder to open", "[folder]");
    parser.process(app);
    try {
        aster::cache::ImageServiceConfig config;
        config.renderer = aster::cache::ImageRenderer{};
        config.renderedMemoryBytes = 128 * 1024 * 1024;
        config.activeMemoryBytes = 64 * 1024 * 1024;
        config.workerCount = 4;
#ifdef ASTER_GALLERY_NETWORK
        config.network = QSharedPointer<aster::cache::QtNetworkService>::create(10000);
        config.sourceDisk = QSharedPointer<aster::cache::FileDiskCache>::create(
                QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/http-source", 256 * 1024 * 1024, 32 * 1024 * 1024);
#endif
        aster::cache::ImageService::configure(config);
        int result;
        {
            aster::gallery::GalleryWindow window(aster::cache::ImageService::pipeline());
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
