#include "server/psych_server.h"
#include "database/db_manager.h"
#include "utils/logger.h"
#include <QJsonArray>

// ============================================================
// Add Contact (320) - send friend request
// ============================================================
void PsychServer::handleAddContact(ClientSession* session, const Message& msg) {
    if (!session->isAuthenticated()) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::AUTH_FAILED, "请先登录"));
        return;
    }
    qint64 contactId = msg.payload()["contactId"].toInteger();
    if (contactId <= 0) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::INVALID_REQUEST, "无效的用户ID"));
        return;
    }

    qint64 uid = session->userId();
    // INSERT IGNORE: skip if already exists
    DbManager::instance().executeUpdate(
        "INSERT IGNORE INTO user_contacts (user_id, contact_id, status) "
        "VALUES (:uid, :cid, :status)",
        {{"uid", uid}, {"cid", contactId}, {"status", "pending"}});

    // If the other user already sent a request, auto-accept both
    auto existing = DbManager::instance().executeQuery(
        "SELECT status FROM user_contacts WHERE user_id = :cid AND contact_id = :uid",
        {{"cid", contactId}, {"uid", uid}});
    if (!existing.isEmpty() && existing[0]["status"].toString() == "pending") {
        DbManager::instance().executeUpdate(
            "UPDATE user_contacts SET status='accepted' WHERE user_id=:cid AND contact_id=:uid",
            {{"cid", contactId}, {"uid", uid}});
        DbManager::instance().executeUpdate(
            "UPDATE user_contacts SET status='accepted' WHERE user_id=:uid AND contact_id=:cid",
            {{"uid", uid}, {"cid", contactId}});
    }

    sendMessage(session, Message::success(
        Protocol::MessageType::ADD_CONTACT_RESPONSE, msg.seq(), {}));
    logAction(uid, "add_contact", "user", contactId);

    // Notify the target user about the friend request
    QJsonObject notify;
    notify["fromUserId"] = uid;
    notify["fromNickname"] = session->username();
    broadcastToUser(contactId,
        Message(Protocol::MessageType::FRIEND_REQUEST_NOTIFY, 0, notify));
}

// ============================================================
// Remove Contact (322)
// ============================================================
void PsychServer::handleRemoveContact(ClientSession* session, const Message& msg) {
    if (!session->isAuthenticated()) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::AUTH_FAILED, "请先登录"));
        return;
    }
    qint64 contactId = msg.payload()["contactId"].toInteger();
    qint64 uid = session->userId();
    DbManager::instance().executeUpdate(
        "DELETE FROM user_contacts WHERE user_id = :uid AND contact_id = :cid",
        {{"uid", uid}, {"cid", contactId}});
    sendMessage(session, Message::success(
        Protocol::MessageType::REMOVE_CONTACT_RESPONSE, msg.seq(), {}));
}

// ============================================================
// Search Users (324)
// ============================================================
void PsychServer::handleSearchUsers(ClientSession* session, const Message& msg) {
    if (!session->isAuthenticated()) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::AUTH_FAILED, "请先登录"));
        return;
    }
    QString keyword = msg.payload()["keyword"].toString().trimmed();
    if (keyword.isEmpty()) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::INVALID_REQUEST, "请输入搜索关键词"));
        return;
    }

    auto results = DbManager::instance().executeQuery(
        "SELECT id, nickname, role, avatar FROM users "
        "WHERE (nickname LIKE :kw OR username LIKE :kw2) "
        "AND id != :uid AND status = 'active' LIMIT 20",
        {{"kw", QString("%%1%").arg(keyword)},
         {"kw2", QString("%%1%").arg(keyword)},
         {"uid", session->userId()}});

    QJsonArray usersArr;
    for (const auto& r : results) {
        QJsonObject u;
        u["userId"] = r["id"].toLongLong();
        u["nickname"] = r["nickname"].toString();
        u["role"] = r["role"].toString();
        u["avatar"] = r["avatar"].toString();
        usersArr.append(u);
    }

    QJsonObject data;
    data["users"] = usersArr;
    sendMessage(session, Message::success(
        Protocol::MessageType::SEARCH_USERS_RESPONSE, msg.seq(), data));
}

// ============================================================
// Get Pending Requests (326)
// ============================================================
void PsychServer::handleGetPendingRequests(ClientSession* session, const Message& msg) {
    if (!session->isAuthenticated()) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::AUTH_FAILED, "请先登录"));
        return;
    }
    qint64 uid = session->userId();

    auto results = DbManager::instance().executeQuery(
        "SELECT uc.user_id, u.nickname, u.role, u.avatar, uc.created_at "
        "FROM user_contacts uc JOIN users u ON uc.user_id = u.id "
        "WHERE uc.contact_id = :uid AND uc.status = 'pending'",
        {{"uid", uid}});

    QJsonArray pendingArr;
    for (const auto& row : results) {
        QJsonObject r;
        r["userId"] = row["user_id"].toLongLong();
        r["nickname"] = row["nickname"].toString();
        r["role"] = row["role"].toString();
        r["avatar"] = row["avatar"].toString();
        r["createdAt"] = row["created_at"].toDateTime().toString("yyyy-MM-dd hh:mm:ss");
        pendingArr.append(r);
    }

    QJsonObject data;
    data["pendingRequests"] = pendingArr;
    sendMessage(session, Message::success(
        Protocol::MessageType::GET_PENDING_REQUESTS_RESPONSE, msg.seq(), data));
}

// ============================================================
// Accept Contact (328) - accept incoming friend request
// ============================================================
void PsychServer::handleAcceptContact(ClientSession* session, const Message& msg) {
    if (!session->isAuthenticated()) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::AUTH_FAILED, "请先登录"));
        return;
    }
    qint64 fromUserId = msg.payload()["userId"].toInteger();
    qint64 uid = session->userId();

    if (fromUserId <= 0) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::INVALID_REQUEST, "无效的用户ID"));
        return;
    }

    // Accept: update the incoming pending request + insert reciprocal accepted row
    DbManager::instance().executeUpdate(
        "UPDATE user_contacts SET status = 'accepted' "
        "WHERE user_id = :from AND contact_id = :uid AND status = 'pending'",
        {{"from", fromUserId}, {"uid", uid}});

    DbManager::instance().executeUpdate(
        "INSERT IGNORE INTO user_contacts (user_id, contact_id, status) "
        "VALUES (:uid, :from, 'accepted')",
        {{"uid", uid}, {"from", fromUserId}});

    sendMessage(session, Message::success(
        Protocol::MessageType::ACCEPT_CONTACT_RESPONSE, msg.seq(), {}));
    logAction(uid, "accept_contact", "user", fromUserId);
}

// ============================================================
// Reject Contact (330) - decline incoming friend request
// ============================================================
void PsychServer::handleRejectContact(ClientSession* session, const Message& msg) {
    if (!session->isAuthenticated()) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::AUTH_FAILED, "请先登录"));
        return;
    }
    qint64 fromUserId = msg.payload()["userId"].toInteger();
    qint64 uid = session->userId();

    if (fromUserId <= 0) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::INVALID_REQUEST, "无效的用户ID"));
        return;
    }

    DbManager::instance().executeUpdate(
        "DELETE FROM user_contacts "
        "WHERE user_id = :from AND contact_id = :uid AND status = 'pending'",
        {{"from", fromUserId}, {"uid", uid}});

    sendMessage(session, Message::success(
        Protocol::MessageType::REJECT_CONTACT_RESPONSE, msg.seq(), {}));
    logAction(uid, "reject_contact", "user", fromUserId);
}
