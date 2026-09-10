#include "imagecatalog.h"

#include <QCollator>
#include <QDirIterator>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QImageReader>
#include <QSet>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>

namespace aster::gallery
{
ImageCatalog::ImageCatalog(QObject* parent) : QObject(parent)
{
}

ImageCatalog::~ImageCatalog()
{
    cancel();
}

void ImageCatalog::cancel()
{
    ++generation_;
    if (cancelled_)
        cancelled_->store(true);
}

void ImageCatalog::loadFolder(const QString& directory, bool recursive)
{
    cancel();
    const QFileInfo info(directory);
    if (!info.isDir() || !info.isReadable())
    {
        Q_EMIT failed(tr("文件夹不存在或无法读取。"));
        return;
    }
    const auto generation = generation_;
    cancelled_ = QSharedPointer<std::atomic<bool>>::create(false);
    const auto cancelled = cancelled_;
    QSet<QString> suffixes;
    for (const auto& format : QImageReader::supportedImageFormats())
        suffixes.insert(QString::fromLatin1(format).toLower());
    suffixes.insert("jpg");
    suffixes.insert("jpeg");
    suffixes.insert("tif");
    suffixes.insert("tiff");
    auto* watcher = new QFutureWatcher<QStringList>(this);
    connect(watcher, &QFutureWatcher<QStringList>::finished, this,
            [this, watcher, generation]
            {
                const auto files = watcher->result();
                watcher->deleteLater();
                if (generation == generation_)
                    Q_EMIT ready(files);
            });
    watcher->setFuture(QtConcurrent::run(
        [directory = info.absoluteFilePath(), recursive, suffixes, cancelled]
        {
            QStringList files;
            QDirIterator it(directory, QDir::Files | QDir::Readable,
                            recursive ? QDirIterator::Subdirectories
                                      : QDirIterator::NoIteratorFlags);
            while (it.hasNext() && !cancelled->load())
            {
                const auto path = it.next();
                if (suffixes.contains(it.fileInfo().suffix().toLower()))
                    files.push_back(path);
            }
            if (cancelled->load())
                return QStringList{};
            QCollator collator;
            collator.setNumericMode(true);
            std::sort(files.begin(), files.end(),
                      [&collator](const QString& a, const QString& b)
                      {
                          return collator.compare(a, b) < 0;
                      });
            return files;
        }));
    Q_EMIT started();
}

void ImageCatalog::loadWebsite(const QUrl& url)
{
    cancel();
    if (!url.isValid() || url.host().isEmpty() ||
        (url.scheme() != "http" && url.scheme() != "https"))
        Q_EMIT failed(tr("请输入有效的 HTTP 或 HTTPS 网站地址。"));
    else
        Q_EMIT failed(tr("网站图片提取接口已预留，暂未实现。请先选择本地文件夹。"));
}
}
