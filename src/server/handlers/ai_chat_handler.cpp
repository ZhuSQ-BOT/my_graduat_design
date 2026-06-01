#include "server/psych_server.h"
#include "database/db_manager.h"
#include "utils/logger.h"
#include <QJsonArray>
#include <QDateTime>

// Forward declaration
QString generateAiResponse(const QString& message);

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

    QString aiResponse = generateAiResponse(message);

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
}

QString generateAiResponse(const QString& message) {
    QString lowerMsg = message.toLower();
    
    if (lowerMsg.contains("抑郁") || lowerMsg.contains("不开心") || lowerMsg.contains("难过")) {
        return QStringLiteral("我理解你现在可能感到很困难。请记住，你不是一个人在面对这些情绪。试着深呼吸，给自己一些时间和空间。如果你愿意，可以和我多说说你的感受。");
    }
    else if (lowerMsg.contains("焦虑") || lowerMsg.contains("紧张") || lowerMsg.contains("压力")) {
        return QStringLiteral("焦虑是很常见的情绪，很多人都会经历。可以试试一些放松的方法，比如正念呼吸、冥想或者散步。如果你愿意，可以告诉我更多关于是什么让你感到焦虑。");
    }
    else if (lowerMsg.contains("睡眠") || lowerMsg.contains("失眠")) {
        return QStringLiteral("睡眠问题确实会影响我们的身心健康。建议你建立一个规律的作息时间，睡前避免使用电子设备，营造一个舒适的睡眠环境。如果问题持续存在，建议咨询专业人士。");
    }
    else if (lowerMsg.contains("自杀") || lowerMsg.contains("想死")) {
        return QStringLiteral("你的生命是宝贵的，现在的困难只是暂时的。请立即联系你身边信任的人、心理咨询师或拨打心理援助热线。你值得被关心和帮助。");
    }
    else if (lowerMsg.contains("谢谢") || lowerMsg.contains("感谢")) {
        return QStringLiteral("不客气！能帮到你我很开心。如果你还有其他问题或需要倾诉，随时都可以来找我。");
    }
    else if (lowerMsg.contains("你好") || lowerMsg.contains("嗨") || lowerMsg.contains("您好")) {
        return QStringLiteral("你好！我是你的心灵伙伴AI，很高兴能为你提供帮助。请问有什么我可以帮到你的吗？");
    }
    else if (lowerMsg.contains("爱") || lowerMsg.contains("感情") || lowerMsg.contains("恋爱")) {
        return QStringLiteral("爱是一种很美好的情感，但也可能带来困惑和痛苦。重要的是要学会爱自己，建立健康的边界。如果你愿意，可以和我分享你的故事。");
    }
    else if (lowerMsg.contains("孤独") || lowerMsg.contains("孤单")) {
        return QStringLiteral("孤独感是很多人都会有的体验。试着参与一些你感兴趣的活动，结识志同道合的朋友。记住，即使你感觉孤独，你也不是真正的孤独。");
    }
    else if (lowerMsg.contains("压力") || lowerMsg.contains("工作") || lowerMsg.contains("学习")) {
        return QStringLiteral("压力是生活中常见的一部分。试着分解任务，给自己设定合理的目标，不要对自己太苛刻。记得也要给自己一些休息和放松的时间。");
    }
    else {
        return QStringLiteral("谢谢你愿意和我分享。我在这里倾听，如果你有任何困扰或问题，都可以随时告诉我。");
    }
}
