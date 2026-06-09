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

    const qint64 AI_USER_ID = 0;

    // Store user message
    DbManager::instance().executeUpdate(
        "INSERT INTO messages (sender_id, receiver_id, content, msg_type) "
        "VALUES (:sender, :receiver, :content, 'text')",
        {{"sender", session->userId()}, {"receiver", AI_USER_ID}, {"content", message}});

    LOG_INFO(QString("AI chat request from user %1: %2").arg(session->userId()).arg(message.left(50)));

    // Capture userId and seq instead of raw session pointer (session may disconnect before API responds)
    qint64 userId = session->userId();
    quint32 seq = msg.seq();

    DeepSeekClient::instance().sendMessage(message,
        [this, userId, seq, AI_USER_ID](const QString& aiResponse, bool isApiResponse) {
            LOG_INFO(QString("AI response for user %1 (api=%2): %3")
                .arg(userId).arg(isApiResponse).arg(aiResponse.left(50)));

            // Store AI response in DB
            DbManager::instance().executeUpdate(
                "INSERT INTO messages (sender_id, receiver_id, content, msg_type) "
                "VALUES (:sender, :receiver, :content, 'text')",
                {{"sender", AI_USER_ID}, {"receiver", userId}, {"content", aiResponse}});

            // Send response only once via direct response to original session
            if (m_userSessions.contains(userId)) {
                ClientSession* sess = m_userSessions[userId];
                QJsonObject directPayload;
                directPayload["content"] = aiResponse;
                directPayload["senderId"] = AI_USER_ID;
                directPayload["senderName"] = "心灵伙伴(AI)";
                sendMessage(sess, Message::success(
                    Protocol::MessageType::AI_CHAT_RESPONSE, seq, directPayload));
            }
        });
}
