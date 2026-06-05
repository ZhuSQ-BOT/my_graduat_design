#include "server/psych_server.h"
#include "database/db_manager.h"
#include "utils/logger.h"
#include "ai/deepseek_client.h"
#include <QJsonArray>
#include <QDateTime>
#include <QRandomGenerator>

// ============================================================
// AI Chat Handler (400)
// ============================================================

void PsychServer::handleAiChat(ClientSession* session, const Message& msg) {
    if (!session->isAuthenticated()) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::AUTH_FAILED, "请先登录"));
        return;
    }

    QJsonObject payload = msg.payload();
    QString message = payload["content"].toString().trimmed();

    if (message.isEmpty()) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::INVALID_REQUEST, "消息内容不能为空"));
        return;
    }

    // Store user message
    DbManager::instance().executeUpdate(
        "INSERT INTO messages (sender_id, receiver_id, content, msg_type) "
        "VALUES (:sender, :receiver, :content, 'text')",
        {{"sender", session->userId()}, {"receiver", -1}, {"content", message}});

    // Use DeepSeek API
    DeepSeekClient::instance().sendMessage(message,
        [this, session, msg](const QString& aiResponse, bool isApiResponse) {
            // Store AI response
            DbManager::instance().executeUpdate(
                "INSERT INTO messages (sender_id, receiver_id, content, msg_type) "
                "VALUES (:sender, :receiver, :content, 'text')",
                {{"sender", -1}, {"receiver", session->userId()}, {"content", aiResponse}});

            QJsonObject responsePayload;
            responsePayload["content"] = aiResponse;
            responsePayload["senderId"] = -1;
            responsePayload["senderName"] = "心灵伙伴(AI)";
            sendMessage(session, Message::success(
                Protocol::MessageType::AI_CHAT_RESPONSE, msg.seq(), responsePayload));

            // Also push to user via RECEIVE_MESSAGE if online
            QJsonObject pushPayload;
            pushPayload["msgId"] = -1;
            pushPayload["senderId"] = -1;
            pushPayload["senderName"] = "心灵伙伴(AI)";
            pushPayload["content"] = aiResponse;
            pushPayload["msgType"] = "text";
            pushPayload["timestamp"] = QDateTime::currentMSecsSinceEpoch();
            broadcastToUser(session->userId(),
                Message(Protocol::MessageType::RECEIVE_MESSAGE, 0, pushPayload));
        });
}
