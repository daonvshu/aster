#include "httptestpanel.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>

namespace aster::gallery {
HttpTestPanel::HttpTestPanel(QWidget* parent)
    : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* address = new QHBoxLayout;
    server_ = new QLineEdit(this);
    server_->setObjectName("httpServer");
    server_->setText(qEnvironmentVariable("ASTER_IMAGE_TEST_URL", "http://127.0.0.1:8765"));
    image_ = new QLineEdit("avatar.png", this);
    image_->setObjectName("httpImage");
    second_ = new QLineEdit("landscape.png", this);
    second_->setObjectName("httpSecondImage");
    address->addWidget(new QLabel(tr("测试服务器"), this));
    address->addWidget(server_, 2);
    address->addWidget(new QLabel(tr("图片 A"), this));
    address->addWidget(image_, 1);
    address->addWidget(new QLabel(tr("图片 B"), this));
    address->addWidget(second_, 1);
    layout->addLayout(address);
    auto* actions = new QHBoxLayout;
    scenario_ = new QComboBox(this);
    scenario_->setObjectName("httpScenario");
    const QStringList routes = {"image",   "delay/3",  "stream",  "status/404", "status/500", "redirect", "etag", "cache", "no-store", "wrong-content-type",
                                "corrupt", "truncate", "oversize"};
    const QStringList names = {tr("正常图片"),  tr("延迟 3 秒"), tr("分块慢下载"),        "404",          "500",          tr("302 重定向"),    "ETag / 304",
                               "Cache-Control", "no-store",      tr("错误 Content-Type"), tr("损坏数据"), tr("中途断流"), tr("超大响应 40MiB")};
    for (int i = 0; i < routes.size(); ++i)
        scenario_->addItem(names[i], routes[i]);
    auto* load = new QPushButton(tr("加载此场景"), this);
    load->setObjectName("httpLoad");
    auto* all = new QPushButton(tr("加载全部接口"), this);
    all->setObjectName("httpAll");
    auto* race = new QPushButton(tr("竞态：慢 A → 快 B"), this);
    race->setObjectName("httpRace");
    actions->addWidget(scenario_);
    actions->addWidget(load);
    actions->addWidget(all);
    actions->addWidget(race);
    actions->addStretch();
    layout->addLayout(actions);
    const auto request = [this, routes](bool allRoutes) {
        const auto base = baseUrl();
        const auto file = imagePath(image_);
        if (base.isEmpty() || file.isEmpty())
            return;
        QStringList sources;
        for (const auto& route : allRoutes ? routes : QStringList{scenario_->currentData().toString()})
            sources.append(base + "/" + route + (route.startsWith("status/") ? "" : "/" + file));
        Q_EMIT sourcesRequested(sources);
    };
    connect(load, &QPushButton::clicked, this, [request] { request(false); });
    connect(all, &QPushButton::clicked, this, [request] { request(true); });
    connect(race, &QPushButton::clicked, this, [this] {
        const auto base = baseUrl();
        const auto a = imagePath(image_);
        const auto b = imagePath(second_);
        if (!base.isEmpty() && !a.isEmpty() && !b.isEmpty())
            Q_EMIT raceRequested(base + "/delay/3/" + a, base + "/image/" + b);
    });
}

QString HttpTestPanel::baseUrl() {
    const QUrl url(server_->text().trimmed());
    if (!url.isValid() || url.host().isEmpty() || !url.userInfo().isEmpty() || (url.scheme() != "http" && url.scheme() != "https") || url.hasQuery() ||
        url.hasFragment() || (!url.path().isEmpty() && url.path() != "/")) {
        Q_EMIT message(tr("请输入服务器根地址，例如 http://127.0.0.1:8765"));
        return {};
    }
    auto result = url.toString(QUrl::FullyEncoded);
    while (result.endsWith('/'))
        result.chop(1);
    return result;
}

QString HttpTestPanel::imagePath(QLineEdit* edit) {
    const auto path = edit->text().trimmed();
    if (path.isEmpty() || path.startsWith('/') || path.contains('\\') || path.split('/').contains("..")) {
        Q_EMIT message(tr("请输入图片目录内的文件名或相对路径，例如 avatar.png。"));
        return {};
    }
    return QString::fromLatin1(QUrl::toPercentEncoding(path, "/"));
}
} // namespace aster::gallery
