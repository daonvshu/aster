#pragma once

#include <QObject>
#include <QSharedPointer>
#include <QStringList>
#include <QUrl>

#include <atomic>

namespace aster::gallery {
class ImageCatalog : public QObject {
    Q_OBJECT

public:
    explicit ImageCatalog(QObject* parent = nullptr);
    ~ImageCatalog() override;
    void loadFolder(const QString& directory, bool recursive);
    void loadWebsite(const QUrl& url);
    void cancel();

Q_SIGNALS:
    void started();
    void ready(const QStringList& files);
    void failed(const QString& message);

private:
    quint64 generation_ = 0;
    QSharedPointer<std::atomic<bool>> cancelled_;
};
} // namespace aster::gallery
