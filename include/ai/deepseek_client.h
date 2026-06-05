#ifndef DEEPSEEK_CLIENT_H
#define DEEPSEEK_CLIENT_H

#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonObject>
#include <QJsonDocument>
#include <QString>
#include <functional>

class DeepSeekClient : public QObject {
    Q_OBJECT

public:
    static DeepSeekClient& instance();

    void setApiKey(const QString& apiKey);
    void setApiUrl(const QString& url);

    void sendMessage(const QString& message, 
                     std::function<void(const QString&, bool)> callback);

private:
    DeepSeekClient(QObject* parent = nullptr);
    ~DeepSeekClient() override = default;

    DeepSeekClient(const DeepSeekClient&) = delete;
    DeepSeekClient& operator=(const DeepSeekClient&) = delete;

    void handleResponse(QNetworkReply* reply, 
                        std::function<void(const QString&, bool)> callback);

    QNetworkAccessManager* m_networkManager;
    QString m_apiKey;
    QString m_apiUrl;
};

#endif // DEEPSEEK_CLIENT_H
