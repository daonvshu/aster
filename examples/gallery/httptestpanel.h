#pragma once

#include <QWidget>

class QLineEdit;
class QComboBox;

namespace aster::gallery {
class HttpTestPanel : public QWidget {
    Q_OBJECT

public:
    explicit HttpTestPanel(QWidget* parent = nullptr);

Q_SIGNALS:
    void sourcesRequested(const QStringList& sources);
    void raceRequested(const QString& slow, const QString& fast);
    void message(const QString& text);

private:
    QString baseUrl();
    QString imagePath(QLineEdit* edit);
    QLineEdit* server_;
    QLineEdit* image_;
    QLineEdit* second_;
    QComboBox* scenario_;
};
} // namespace aster::gallery
