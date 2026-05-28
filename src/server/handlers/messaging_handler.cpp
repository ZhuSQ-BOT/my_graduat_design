#include "server/psych_server.h"
#include "database/db_manager.h"
#include "utils/logger.h"
#include <QJsonArray>
#include <QDateTime>

// ============================================================
// Send Message (IM)
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
    int rows = DbManager::instance().executeUpdate(
        "INSERT INTO messages (sender_id, receiver_id, content, msg_type) "
        "VALUES (:sender_id, :receiver_id, :content, :msg_type)",
        {
            {"sender_id", session->userId()},
            {"receiver_id", receiverId},
            {"content", content},
            {"msg_type", msgType}
        });

    qint64 msgId = DbManager::instance().lastInsertId();

    // Build forwarded message
    QJsonObject fwdPayload;
    fwdPayload["msgId"] = msgId;
    fwdPayload["senderId"] = session->userId();
    fwdPayload["senderName"] = session->username();
    fwdPayload["content"] = content;
    fwdPayload["msgType"] = msgType;
    fwdPayload["timestamp"] = QDateTime::currentMSecsSinceEpoch();

    // Forward to receiver if online
    broadcastToUser(receiverId, Message(
        Protocol::MessageType::RECEIVE_MESSAGE, 0, fwdPayload));

    // Confirm to sender
    QJsonObject responseData;
    responseData["msgId"] = msgId;
    responseData["timestamp"] = fwdPayload["timestamp"];

    sendMessage(session, Message::success(
        Protocol::MessageType::RECEIVE_MESSAGE, msg.seq(), responseData));
}

// ============================================================
// Get Contacts (online users / counselors)
// ============================================================

void PsychServer::handleGetContacts(ClientSession* session, const Message& msg) {
    if (!session->isAuthenticated()) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::AUTH_FAILED, "请先登录"));
        return;
    }

    // Get counselors and recent contacts
    auto counselors = DbManager::instance().executeQuery(
        "SELECT id, nickname, avatar FROM users WHERE role = 'counselor' AND status = 'active'");

    QJsonArray counselorArray;
    for (const auto& row : counselors) {
        QJsonObject c;
        c["userId"] = row["id"].toLongLong();
        c["nickname"] = row["nickname"].toString();
        c["avatar"] = row["avatar"].toString();
        c["role"] = "counselor";
        // Check if online
        qint64 uid = row["id"].toLongLong();
        c["isOnline"] = m_userSessions.contains(uid);
        counselorArray.append(c);
    }

    // Get users this user has messaged with
    auto contacts = DbManager::instance().executeQuery(
        "SELECT DISTINCT u.id, u.nickname, u.avatar, u.role, "
        "(SELECT MAX(m2.created_at) FROM messages m2 "
        "WHERE (m2.sender_id = u.id AND m2.receiver_id = :uid) "
        "OR (m2.sender_id = :uid AND m2.receiver_id = u.id)) as last_msg_time "
        "FROM users u "
        "WHERE u.id IN ("
        "  SELECT DISTINCT CASE WHEN sender_id = :uid THEN receiver_id ELSE sender_id END "
        "  FROM messages WHERE sender_id = :uid OR receiver_id = :uid"
        ") ORDER BY last_msg_time DESC",
        {{"uid", session->userId()}});

    QJsonArray contactArray;
    for (const auto& row : contacts) {
        QJsonObject c;
        qint64 uid = row["id"].toLongLong();
        c["userId"] = uid;
        c["nickname"] = row["nickname"].toString();
        c["avatar"] = row["avatar"].toString();
        c["role"] = row["role"].toString();
        c["isOnline"] = m_userSessions.contains(uid);
        c["lastMsgTime"] = row["last_msg_time"].toDateTime().toString(Qt::ISODate);
        contactArray.append(c);
    }

    QJsonObject responseData;
    responseData["counselors"] = counselorArray;
    responseData["contacts"] = contactArray;

    sendMessage(session, Message::success(
        Protocol::MessageType::GET_CONTACTS_RESPONSE, msg.seq(), responseData));
}

// ============================================================
// Get Message History
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
    int pageSize = payload.value("pageSize").toInt(50);
    int offset = (page - 1) * pageSize;

    if (otherUserId <= 0) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::INVALID_REQUEST, "无效的用户ID"));
        return;
    }

    qint64 myId = session->userId();

    auto results = DbManager::instance().executeQuery(
        "SELECT id, sender_id, receiver_id, content, msg_type, is_read, created_at "
        "FROM messages "
        "WHERE (sender_id = :my_id AND receiver_id = :other_id) "
        "OR (sender_id = :other_id AND receiver_id = :my_id) "
        "ORDER BY created_at DESC LIMIT :limit OFFSET :offset",
        {
            {"my_id", myId},
            {"other_id", otherUserId},
            {"limit", pageSize},
            {"offset", offset}
        });

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
        m["receiverId"] = row["receiver_id"].toLongLong();
        m["content"] = row["content"].toString();
        m["msgType"] = row["msg_type"].toString();
        m["isRead"] = row["is_read"].toBool();
        m["createdAt"] = row["created_at"].toDateTime().toString(Qt::ISODate);
        messagesArray.append(m);
    }

    QJsonObject responseData;
    responseData["messages"] = messagesArray;
    responseData["page"] = page;

    sendMessage(session, Message::success(
        Protocol::MessageType::GET_MESSAGE_HISTORY_RESPONSE, msg.seq(), responseData));
}
