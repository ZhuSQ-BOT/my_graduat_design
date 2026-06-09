#include "server/psych_server.h"
#include "database/db_manager.h"
#include "utils/logger.h"
#include <QJsonArray>
#include <QDateTime>

// ============================================================
// Send Message
// ============================================================
void PsychServer::handleSendMessage(ClientSession* session, const Message& msg) {
    if (!session->isAuthenticated()) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::AUTH_FAILED, "请先登录"));
        return;
    }
    QJsonObject payload = msg.payload();
    qint64 receiverId = payload["receiverId"].toInteger();
    QString content = payload["content"].toString().trimmed();
    QString msgType = payload.value("msgType").toString("text");

    if (receiverId <= 0 || content.isEmpty()) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::INVALID_REQUEST, "无效的接收者或内容为空"));
        return;
    }

    // Store message
    DbManager::instance().executeUpdate(
        "INSERT INTO messages (sender_id, receiver_id, content, msg_type) "
        "VALUES (:sender_id, :receiver_id, :content, :msg_type)",
        {{"sender_id", session->userId()}, {"receiver_id", receiverId},
         {"content", content}, {"msg_type", msgType}});
    qint64 msgId = DbManager::instance().lastInsertId();

    // Forward to receiver if online
    QJsonObject fwdPayload;
    fwdPayload["msgId"] = msgId;
    fwdPayload["senderId"] = session->userId();
    fwdPayload["senderName"] = session->username();
    fwdPayload["content"] = content;
    fwdPayload["msgType"] = msgType;
    fwdPayload["timestamp"] = QDateTime::currentMSecsSinceEpoch();
    broadcastToUser(receiverId,
        Message(Protocol::MessageType::RECEIVE_MESSAGE, 0, fwdPayload));

    QJsonObject responseData;
    responseData["msgId"] = msgId;
    responseData["senderId"] = session->userId();
    responseData["senderName"] = session->username();
    sendMessage(session, Message::success(
        Protocol::MessageType::RECEIVE_MESSAGE, msg.seq(), responseData));
}

// ============================================================
// Get Contacts - based on user_contacts table + AI bot
// ============================================================
void PsychServer::handleGetContacts(ClientSession* session, const Message& msg) {
    if (!session->isAuthenticated()) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::AUTH_FAILED, "请先登录"));
        return;
    }

    qint64 uid = session->userId();
    QJsonArray contactsArr;

    // AI bot is always available
    QJsonObject aiContact;
    aiContact["userId"] = 0;
    aiContact["nickname"] = "AI助手";
    aiContact["username"] = "AI助手";
    aiContact["role"] = "ai";
    aiContact["avatar"] = "";
    aiContact["isOnline"] = true;
    contactsArr.append(aiContact);

    // Get accepted contacts from user_contacts table
    auto results = DbManager::instance().executeQuery(
        "SELECT u.id, u.nickname, u.avatar, u.role, uc.created_at "
        "FROM user_contacts uc JOIN users u ON uc.contact_id = u.id "
        "WHERE uc.user_id = :uid AND uc.status = 'accepted' AND u.status = 'active' "
        "ORDER BY u.role DESC, u.nickname",
        {{"uid", uid}});

    for (const auto& row : results) {
        QJsonObject c;
        qint64 cid = row["id"].toLongLong();
        c["userId"] = cid;
        c["nickname"] = row["nickname"].toString();
        c["username"] = row["nickname"].toString();
        c["role"] = row["role"].toString();
        c["avatar"] = row["avatar"].toString();
        c["isOnline"] = m_userSessions.contains(cid);
        contactsArr.append(c);
    }

    QJsonObject responseData;
    responseData["contacts"] = contactsArr;
    sendMessage(session, Message::success(
        Protocol::MessageType::GET_CONTACTS_RESPONSE, msg.seq(), responseData));
}

// ============================================================
// Get Message History - with pagination
// ============================================================
void PsychServer::handleGetMessageHistory(ClientSession* session, const Message& msg) {
    if (!session->isAuthenticated()) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::AUTH_FAILED, "请先登录"));
        return;
    }

    QJsonObject payload = msg.payload();
    qint64 otherUserId = payload["otherUserId"].toInteger();
    int page = payload.value("page").toInt(1);
    int pageSize = payload.value("pageSize").toInt(20);
    int offset = (page - 1) * pageSize;

    if (otherUserId < 0) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::INVALID_REQUEST, "无效的用户ID"));
        return;
    }

    qint64 myId = session->userId();
    QString senderName;
    
    // 如果是AI聊天（otherUserId = 0），特殊处理
    const qint64 AI_USER_ID = 0;
    if (otherUserId == AI_USER_ID) {
        // 查询用户发送给AI的消息和AI的回复
        auto countResult = DbManager::instance().executeQuery(
            "SELECT COUNT(*) AS total FROM messages "
            "WHERE (sender_id = :my_id AND receiver_id = :ai_id) "
            "OR (sender_id = :ai_id AND receiver_id = :my_id)",
            {{"my_id", myId}, {"ai_id", AI_USER_ID}});
        int total = countResult.isEmpty() ? 0 : countResult[0]["total"].toInt();
        
        auto results = DbManager::instance().executeQuery(
            "SELECT m.id, m.sender_id, m.receiver_id, m.content, m.msg_type, m.is_read, m.created_at, "
            "CASE WHEN m.sender_id = :ai_id THEN '心灵伙伴(AI)' ELSE COALESCE(u.nickname, '未知用户') END AS sender_name "
            "FROM messages m LEFT JOIN users u ON m.sender_id = u.id "
            "WHERE (m.sender_id = :my_id AND m.receiver_id = :ai_id) "
            "OR (m.sender_id = :ai_id AND m.receiver_id = :my_id) "
            "ORDER BY m.created_at ASC LIMIT :limit OFFSET :offset",
            {{"my_id", myId}, {"ai_id", AI_USER_ID}, {"limit", pageSize}, {"offset", offset}});
        
        // Mark unread as read
        DbManager::instance().executeUpdate(
            "UPDATE messages SET is_read = 1 "
            "WHERE sender_id = :ai_id AND receiver_id = :my_id AND is_read = 0",
            {{"ai_id", AI_USER_ID}, {"my_id", myId}});
        
        QJsonArray messagesArray;
        for (const auto& row : results) {
            QJsonObject m;
            m["id"] = row["id"].toLongLong();
            m["senderId"] = row["sender_id"].toLongLong();
            m["senderName"] = row["sender_name"].toString();
            m["receiverId"] = row["receiver_id"].toLongLong();
            m["content"] = row["content"].toString();
            m["msgType"] = row["msg_type"].toString();
            m["isRead"] = row["is_read"].toBool();
            m["createdAt"] = row["created_at"].toDateTime().toString("yyyy-MM-dd hh:mm:ss");
            messagesArray.append(m);
        }
        
        QJsonObject responseData;
        responseData["messages"] = messagesArray;
        responseData["page"] = page;
        responseData["pageSize"] = pageSize;
        responseData["total"] = total;
        responseData["hasMore"] = (offset + pageSize) < total;
        
        sendMessage(session, Message::success(
            Protocol::MessageType::GET_MESSAGE_HISTORY_RESPONSE, msg.seq(), responseData));
        return;
    }

    // Count total
    auto countResult = DbManager::instance().executeQuery(
        "SELECT COUNT(*) AS total FROM messages "
        "WHERE (sender_id = :my_id AND receiver_id = :other_id) "
        "OR (sender_id = :other_id AND receiver_id = :my_id)",
        {{"my_id", myId}, {"other_id", otherUserId}});
    int total = countResult.isEmpty() ? 0 : countResult[0]["total"].toInt();

    // Fetch messages with sender name (oldest first for chat display)
    auto results = DbManager::instance().executeQuery(
        "SELECT m.id, m.sender_id, m.receiver_id, m.content, m.msg_type, m.is_read, m.created_at, "
        "COALESCE(u.nickname, '未知用户') AS sender_name "
        "FROM messages m LEFT JOIN users u ON m.sender_id = u.id "
        "WHERE (m.sender_id = :my_id AND m.receiver_id = :other_id) "
        "OR (m.sender_id = :other_id AND m.receiver_id = :my_id) "
        "ORDER BY m.created_at ASC LIMIT :limit OFFSET :offset",
        {{"my_id", myId}, {"other_id", otherUserId},
         {"limit", pageSize}, {"offset", offset}});

    // Mark unread as read
    DbManager::instance().executeUpdate(
        "UPDATE messages SET is_read = 1 "
        "WHERE sender_id = :other_id AND receiver_id = :my_id AND is_read = 0",
        {{"other_id", otherUserId}, {"my_id", myId}});

    QJsonArray messagesArray;
    for (const auto& row : results) {
        QJsonObject m;
        m["id"] = row["id"].toLongLong();
        m["senderId"] = row["sender_id"].toLongLong();
        m["senderName"] = row["sender_name"].toString();
        m["receiverId"] = row["receiver_id"].toLongLong();
        m["content"] = row["content"].toString();
        m["msgType"] = row["msg_type"].toString();
        m["isRead"] = row["is_read"].toBool();
        m["createdAt"] = row["created_at"].toDateTime().toString("yyyy-MM-dd hh:mm:ss");
        messagesArray.append(m);
    }

    QJsonObject responseData;
    responseData["messages"] = messagesArray;
    responseData["page"] = page;
    responseData["pageSize"] = pageSize;
    responseData["total"] = total;
    responseData["hasMore"] = (offset + pageSize) < total;

    sendMessage(session, Message::success(
        Protocol::MessageType::GET_MESSAGE_HISTORY_RESPONSE, msg.seq(), responseData));
}
