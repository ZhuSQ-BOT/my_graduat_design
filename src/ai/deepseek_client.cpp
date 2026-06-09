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
    payload["temperature"] = 0.8;
    payload["max_tokens"] = 1024;

    QJsonArray messages;
    QJsonObject systemMsg;
    systemMsg["role"] = "system";
    systemMsg["content"] = QStringLiteral(
        "你是「心灵伙伴」，一个专业的心理支持AI助手，具备心理咨询师的沟通技巧和同理心。"
        "\n\n【核心原则】"
        "\n1. 积极倾听：专注理解用户的情绪和经历，不急于给出建议"
        "\n2. 共情回应：用温暖的语言表达理解和接纳，让用户感到被看见"
        "\n3. 开放提问：用开放式问题引导用户探索自己的感受"
        "\n4. 赋能支持：帮助用户发现自己的内在资源，而非直接给答案"
        "\n\n【沟通技巧】"
        "\n- 反映情感：「听起来你感到...」，「我注意到你说这话时...」"
        "\n- 正常化体验：「很多人遇到这种情况也会有类似的感受...」"
        "\n- 探索例外：「有没有什么时候这个情况会好一些？」"
        "\n- 肯定努力：「你已经尝试了很多方法，这很不容易...」"
        "\n\n【语言风格】"
        "\n- 温柔、耐心、不评判"
        "\n- 用「我们」而非「你」来建立同盟感"
        "\n- 适当使用比喻和意象（如「情绪像天气，会来也会走」）"
        "\n- 避免过于专业的术语，用生活化的语言"
        "\n\n【重要边界】"
        "\n- 不参与 Crisis 干预（如自伤、自杀），需引导寻求专业帮助"
        "\n- 不提供医疗诊断或药物建议"
        "\n- 不承诺「治愈」或「解决问题」，而是陪伴探索"
        "\n- 当用户提到测评结果时，帮助其理解含义，而非解读诊断"
        "\n\n【回复结构】"
        "\n1. 先共情（1-2句）"
        "\n2. 再探索（1个开放式问题）"
        "\n3. 后支持（1个肯定或建议）"
        "\n\n请用中文回复，保持温暖、专业、有边界感的对话风格。");
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
        LOG_DEBUG(QString("DeepSeek API raw response: %1").arg(QString::fromUtf8(data).left(500)));

        QJsonDocument doc = QJsonDocument::fromJson(data);

        if (!doc.isNull() && doc.isObject()) {
            QJsonObject obj = doc.object();

            if (obj.contains("error")) {
                QString errorMsg = obj["error"].toObject()["message"].toString();
                LOG_ERROR(QString("DeepSeek API returned error: %1").arg(errorMsg));
            }

            QJsonArray choices = obj["choices"].toArray();

            if (!choices.isEmpty()) {
                QJsonObject choice = choices[0].toObject();
                QJsonObject message = choice["message"].toObject();
                response = message["content"].toString().trimmed();
                success = true;
            } else {
                LOG_WARN("DeepSeek API response has no choices");
            }
        } else {
            LOG_WARN("DeepSeek API response is not valid JSON");
        }
    } else {
        LOG_ERROR(QString("DeepSeek API network error: %1").arg(reply->errorString()));
        LOG_ERROR(QString("HTTP status code: %1").arg(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()));
    }

    reply->deleteLater();

    if (success && !response.isEmpty()) {
        LOG_DEBUG(QString("DeepSeek API success, response length: %1").arg(response.length()));
        callback(response, true);
    } else {
        LOG_WARN("DeepSeek API failed, using fallback response");
        callback(QString("我理解你的感受，谢谢你愿意和我分享。如果你有任何困扰或问题，都可以随时告诉我。"), false);
    }
}
