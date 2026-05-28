#include "server/psych_server.h"
#include "database/db_manager.h"
#include "utils/logger.h"
#include <QJsonArray>
#include <QDateTime>

// ============================================================
// Get Dashboard Stats (Admin)
// ============================================================

void PsychServer::handleGetDashboardStats(ClientSession* session, const Message& msg) {
    if (!session->isAuthenticated()) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::AUTH_FAILED, "请先登录"));
        return;
    }

    // Check admin role
    auto userCheck = DbManager::instance().executeQuery(
        "SELECT role FROM users WHERE id = :id",
        {{"id", session->userId()}});
    if (userCheck.isEmpty() || userCheck.first()["role"].toString() != "admin") {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::PERMISSION_DENIED, "需要管理员权限"));
        return;
    }

    QJsonObject responseData;

    // User stats
    auto userStats = DbManager::instance().executeQuery(
        "SELECT COUNT(*) as total, "
        "SUM(role = 'user') as users, "
        "SUM(role = 'counselor') as counselors, "
        "SUM(status = 'active') as active "
        "FROM users");
    if (!userStats.isEmpty()) {
        QJsonObject users;
        users["total"] = userStats.first()["total"].toInt();
        users["users"] = userStats.first()["users"].toInt();
        users["counselors"] = userStats.first()["counselors"].toInt();
        users["active"] = userStats.first()["active"].toInt();
        responseData["userStats"] = users;
    }

    // Assessment stats
    auto assessStats = DbManager::instance().executeQuery(
        "SELECT COUNT(*) as total, "
        "COUNT(DISTINCT user_id) as unique_users, "
        "AVG(total_score) as avg_score "
        "FROM assessment_records");
    if (!assessStats.isEmpty()) {
        QJsonObject assess;
        assess["total"] = assessStats.first()["total"].toInt();
        assess["uniqueUsers"] = assessStats.first()["unique_users"].toInt();
        assess["avgScore"] = assessStats.first()["avg_score"].toDouble();
        responseData["assessmentStats"] = assess;
    }

    // Forum stats
    auto forumStats = DbManager::instance().executeQuery(
        "SELECT COUNT(*) as posts, "
        "(SELECT COUNT(*) FROM forum_replies WHERE status = 'active') as replies "
        "FROM forum_posts WHERE status = 'active'");
    if (!forumStats.isEmpty()) {
        QJsonObject forum;
        forum["posts"] = forumStats.first()["posts"].toInt();
        forum["replies"] = forumStats.first()["replies"].toInt();
        responseData["forumStats"] = forum;
    }

    // Recent assessments (last 7 days trend)
    auto trend = DbManager::instance().executeQuery(
        "SELECT DATE(completed_at) as date, COUNT(*) as count, AVG(total_score) as avg_score "
        "FROM assessment_records "
        "WHERE completed_at >= DATE_SUB(NOW(), INTERVAL 7 DAY) "
        "GROUP BY DATE(completed_at) ORDER BY date");

    QJsonArray trendArray;
    for (const auto& row : trend) {
        QJsonObject point;
        point["date"] = row["date"].toDate().toString("yyyy-MM-dd");
        point["count"] = row["count"].toInt();
        point["avgScore"] = row["avg_score"].toDouble();
        trendArray.append(point);
    }
    responseData["assessmentTrend"] = trendArray;

    // Online users
    responseData["onlineUsers"] = m_userSessions.size();

    sendMessage(session, Message::success(
        Protocol::MessageType::GET_DASHBOARD_STATS_RESPONSE, msg.seq(), responseData));
}

// ============================================================
// Manage User (Admin)
// ============================================================

void PsychServer::handleManageUser(ClientSession* session, const Message& msg) {
    if (!session->isAuthenticated()) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::AUTH_FAILED, "请先登录"));
        return;
    }

    // Check admin role
    auto adminCheck = DbManager::instance().executeQuery(
        "SELECT role FROM users WHERE id = :id",
        {{"id", session->userId()}});
    if (adminCheck.isEmpty() || adminCheck.first()["role"].toString() != "admin") {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::PERMISSION_DENIED, "需要管理员权限"));
        return;
    }

    QJsonObject payload = msg.payload();
    qint64 targetUserId = payload["userId"].toInteger();
    QString action = payload["action"].toString(); // ban, unban, set_role, list

    if (action == "list") {
        int page = payload.value("page").toInt(1);
        int pageSize = payload.value("pageSize").toInt(20);
        int offset = (page - 1) * pageSize;

        auto results = DbManager::instance().executeQuery(
            "SELECT id, username, nickname, email, role, status, created_at, last_login "
            "FROM users ORDER BY created_at DESC LIMIT :limit OFFSET :offset",
            {{"limit", pageSize}, {"offset", offset}});

        QJsonArray usersArray;
        for (const auto& row : results) {
            QJsonObject u;
            u["id"] = row["id"].toLongLong();
            u["username"] = row["username"].toString();
            u["nickname"] = row["nickname"].toString();
            u["email"] = row["email"].toString();
            u["role"] = row["role"].toString();
            u["status"] = row["status"].toString();
            u["createdAt"] = row["created_at"].toDateTime().toString(Qt::ISODate);
            u["lastLogin"] = row["last_login"].isNull() ? "" : row["last_login"].toDateTime().toString(Qt::ISODate);
            u["isOnline"] = m_userSessions.contains(row["id"].toLongLong());
            usersArray.append(u);
        }

        QJsonObject responseData;
        responseData["users"] = usersArray;
        responseData["page"] = page;

        sendMessage(session, Message::success(
            Protocol::MessageType::MANAGE_USER_RESPONSE, msg.seq(), responseData));
        return;
    }

    if (targetUserId <= 0) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::INVALID_REQUEST, "无效的用户ID"));
        return;
    }

    if (action == "ban") {
        DbManager::instance().executeUpdate(
            "UPDATE users SET status = 'banned' WHERE id = :id",
            {{"id", targetUserId}});

        // Disconnect user if online
        if (m_userSessions.contains(targetUserId)) {
            m_userSessions[targetUserId]->socket()->disconnectFromHost();
        }
    } else if (action == "unban") {
        DbManager::instance().executeUpdate(
            "UPDATE users SET status = 'active' WHERE id = :id",
            {{"id", targetUserId}});
    } else if (action == "set_role") {
        QString newRole = payload["role"].toString();
        if (newRole != "user" && newRole != "counselor" && newRole != "admin") {
            sendMessage(session, Message::error(msg.seq(),
                Protocol::ErrorCode::INVALID_REQUEST, "无效的角色"));
            return;
        }
        DbManager::instance().executeUpdate(
            "UPDATE users SET role = :role WHERE id = :id",
            {{"role", newRole}, {"id", targetUserId}});
    } else {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::INVALID_REQUEST, "无效的操作"));
        return;
    }

    QJsonObject responseData;
    responseData["success"] = true;

    sendMessage(session, Message::success(
        Protocol::MessageType::MANAGE_USER_RESPONSE, msg.seq(), responseData));

    logAction(session->userId(), "manage_user_" + action, "user", targetUserId);
}

// ============================================================
// Get System Logs (Admin)
// ============================================================

void PsychServer::handleGetSystemLogs(ClientSession* session, const Message& msg) {
    if (!session->isAuthenticated()) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::AUTH_FAILED, "请先登录"));
        return;
    }

    // Check admin role
    auto adminCheck = DbManager::instance().executeQuery(
        "SELECT role FROM users WHERE id = :id",
        {{"id", session->userId()}});
    if (adminCheck.isEmpty() || adminCheck.first()["role"].toString() != "admin") {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::PERMISSION_DENIED, "需要管理员权限"));
        return;
    }

    QJsonObject payload = msg.payload();
    int page = payload.value("page").toInt(1);
    int pageSize = payload.value("pageSize").toInt(50);
    int offset = (page - 1) * pageSize;

    auto results = DbManager::instance().executeQuery(
        "SELECT sl.id, sl.user_id, sl.action, sl.target_type, sl.target_id, "
        "sl.ip_address, sl.details, sl.created_at, u.username "
        "FROM system_logs sl "
        "LEFT JOIN users u ON sl.user_id = u.id "
        "ORDER BY sl.created_at DESC LIMIT :limit OFFSET :offset",
        {{"limit", pageSize}, {"offset", offset}});

    QJsonArray logsArray;
    for (const auto& row : results) {
        QJsonObject log;
        log["id"] = row["id"].toLongLong();
        log["userId"] = row["user_id"].isNull() ? -1 : row["user_id"].toLongLong();
        log["username"] = row["username"].toString();
        log["action"] = row["action"].toString();
        log["targetType"] = row["target_type"].toString();
        log["targetId"] = row["target_id"].isNull() ? -1 : row["target_id"].toLongLong();
        log["ipAddress"] = row["ip_address"].toString();
        log["details"] = row["details"].toString();
        log["createdAt"] = row["created_at"].toDateTime().toString(Qt::ISODate);
        logsArray.append(log);
    }

    QJsonObject responseData;
    responseData["logs"] = logsArray;
    responseData["page"] = page;

    sendMessage(session, Message::success(
        Protocol::MessageType::GET_SYSTEM_LOGS_RESPONSE, msg.seq(), responseData));
}
