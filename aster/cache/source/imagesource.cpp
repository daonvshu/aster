#include "iimagesourceloader.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>

namespace aster::cache {
Result<ImageSource> ImageSource::fromString(const QString& input, const KeyContext& context) {
    using R = Result<ImageSource>;
    if (input.trimmed().isEmpty() || input.contains(QChar('\0')))
        return R::failure(ImageError::InvalidRequest, "Empty or invalid source path");

    ImageSource source;
    source.context = context;

    auto local = [&](const QString& path) {
        source.kind = Kind::Local;
        source.url = QUrl::fromLocalFile(QFileInfo(path).absoluteFilePath());
        return R::success(source);
    };

    if (input.startsWith(":/")) {
        source.kind = Kind::Resource;
        source.url.setScheme("qrc");
        source.url.setPath(QDir::cleanPath(input.mid(1)));
        return R::success(std::move(source));
    }

    const bool drive = QRegularExpression("\\A[A-Za-z]:[/\\\\]").match(input).hasMatch();
    if (drive || input.startsWith("\\\\") || input.startsWith("//"))
        return local(QDir::fromNativeSeparators(input));

    const QUrl url(input, QUrl::StrictMode);
    if (url.scheme().isEmpty())
        return local(input);

    if (!url.isValid())
        return R::failure(ImageError::InvalidRequest, "Invalid source URL");

    if (url.scheme() == "http" || url.scheme() == "https") {
        if (url.host().isEmpty())
            return R::failure(ImageError::InvalidRequest, "Missing HTTP host");

        source.kind = Kind::Network;
        source.url = url;
        return R::success(std::move(source));
    }

    if (url.hasQuery() || url.hasFragment())
        return R::failure(ImageError::InvalidRequest, "File and resource URLs cannot have query or fragment");

    if (url.isLocalFile() && !url.toLocalFile().isEmpty() && url.userInfo().isEmpty() && url.port() == -1)
        return local(url.toLocalFile());

    if (url.scheme() == "qrc" && url.authority().isEmpty() && url.path().startsWith('/')) {
        source.kind = Kind::Resource;
        source.url.setScheme("qrc");
        source.url.setPath(QDir::cleanPath(url.path()));
        return R::success(std::move(source));
    }

    return R::failure(ImageError::InvalidRequest, "Unsupported source scheme");
}
} // namespace aster::cache
