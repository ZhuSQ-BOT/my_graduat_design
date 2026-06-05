#include "ai/deepseek_client.h"
#include "utils/logger.h"
#include <QJsonArray>

DeepSeekClient::DeepSeekClient(QObject* parent) : QObject(parent) {
    m_networkManager = new QNetworkAccessManager(this);
    m_apiUrl = "https://api.deepseek.com/v1/chat/completions";
}

DeepSeekClient& DeepSeekClient::instance() {
    static DeepSeekClient instance;
    return instance;
}

void DeepSeekClient::setApiKey(const QString& apiKey) {
    m_apiKey = apiKey;
}

void DeepSeekClient::setApiUrl(const QString& url) {
    m_apiUrl = url;
}

void DeepSeekClient::sendMessage(const QString& message, 
                                 std::function<void(const QString&, bool)> callback) {
    if (m_apiKey.isEmpty()) {
        LOG_WARN("DeepSeek API key not set, using fallback response");
        callback(QString("我理解你的感受，谢谢你愿意和我分享。如果你有任何困扰或问题，都可以随时告诉我。"), false);
        return;
    }

    QNetworkRequest request{QUrl(m_apiUrl)};
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Authorization", QString("Bearer %1").arg(m_apiKey).toUtf8());

    QJsonObject payload;
    payload["model"] = "deepseek-chat";
    payload["temperature"] = 0.7;
    payload["max_tokens"] = 1024;

    QJsonArray messages;
    QJsonObject systemMsg;
    systemMsg["role"] = "system";
    systemMsg["content"] = QStringLiteral(
        "你是一个温暖、有同理心的心理支持AI助手。请用温和、关怀的语气回应用户。"
        "你的目标是倾听和支持，而不是提供专业的医疗建议。"
        "如果用户有严重的心理危机，请引导他们寻求专业帮助。"
        "请用中文回复。"
    );
    messages.append(systemMsg);

    QJsonObject userMsg;
    userMsg["role"] = "user";
    userMsg["content"] = message;
    messages.append(userMsg);

    payload["messages"] = messages;

    QJsonDocument doc(payload);
    QByteArray data = doc.toJson(QJsonDocument::Compact);

    QNetworkReply* reply = m_networkManager->post(request, data);
    connect(reply, &QNetworkReply::finished, this, 
            [this, reply, callback]() { handleResponse(reply, callback); });
}

void DeepSeekClient::handleResponse(QNetworkReply* reply, 
                                    std::function<void(const QString&, bool)> callback) {
    QString response;
    bool success = false;

    if (reply->error() == QNetworkReply::NoError) {
        QByteArray data = reply->readAll();
        QJsonDocument doc = QJsonDocument::fromJson(data);
        
        if (!doc.isNull() && doc.isObject()) {
            QJsonObject obj = doc.object();
            QJsonArray choices = obj["choices"].toArray();
            
            if (!choices.isEmpty()) {
                QJsonObject choice = choices[0].toObject();
                QJsonObject message = choice["message"].toObject();
                response = message["content"].toString().trimmed();
                success = true;
            }
        }
    } else {
        LOG_ERROR("DeepSeek API error: " + reply->errorString());
    }

    reply->deleteLater();

    if (success && !response.isEmpty()) {
        callback(response, true);
    } else {
        LOG_WARN("DeepSeek API failed, using fallback response");
        callback(QString("我理解你的感受，谢谢你愿意和我分享。如果你有任何困扰或问题，都可以随时告诉我。"), false);
    }
}
