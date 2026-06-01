#include "server/psych_server.h"
#include "database/db_manager.h"
#include "protocol.h"
#include "utils/logger.h"
#include <QJsonArray>
#include <QDateTime>
#include <QUuid>

// ============================================================
// Publish Assessment Task (800) - counselor / admin
// ============================================================

void PsychServer::handlePublishTask(ClientSession* session, const Message& msg) {
    QString role = session->role();
    if (role != "counselor" && role != "admin") {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::PERMISSION_DENIED, "需要咨询师或管理员权限"));
        return;
    }

    QJsonObject payload = msg.payload();
    int scaleId = payload["scaleId"].toInt();
    QString title = payload["title"].toString().trimmed();
    QString description = payload["description"].toString();
    QJsonArray targetUserIds = payload["targetUserIds"].toArray();
    QString dueDate = payload["dueDate"].toString();

    if (scaleId <= 0) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::INVALID_REQUEST, "无效的量表ID"));
        return;
    }
    if (title.isEmpty()) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::INVALID_REQUEST, "任务标题不能为空"));
        return;
    }

    // Verify scale exists and is active
    auto scaleCheck = DbManager::instance().executeQuery(
        "SELECT id FROM scales WHERE id = :id AND is_active = 1",
        {{"id", scaleId}});
    if (scaleCheck.isEmpty()) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::USER_NOT_FOUND, "量表不存在或未启用"));
        return;
    }

    // Build target_user_ids JSON
    QJsonDocument targetDoc(targetUserIds);
    QString targetJson = QString::fromUtf8(targetDoc.toJson(QJsonDocument::Compact));

    // Insert task (status = pending, needs admin review)
    DbManager::instance().executeUpdate(
        "INSERT INTO assessment_tasks "
        "(publisher_id, scale_id, title, description, target_user_ids, status, due_date) "
        "VALUES (:publisher, :scale, :title, :desc, :targets, 'pending', :due)",
        {
            {"publisher", session->userId()},
            {"scale", scaleId},
            {"title", title},
            {"desc", description},
            {"targets", targetJson},
            {"due", dueDate.isEmpty() ? QVariant(QString()) : QVariant(dueDate)}
        });

    // Get the inserted task ID
    auto result = DbManager::instance().executeQuery(
        "SELECT id FROM assessment_tasks WHERE publisher_id = :uid "
        "ORDER BY id DESC LIMIT 1",
        {{"uid", session->userId()}});

    qint64 newTaskId = result.isEmpty() ? -1 : result.first()["id"].toLongLong();

    logAction(session->userId(), "publish_task", "task", newTaskId);

    QJsonObject responseData;
    responseData["taskId"] = newTaskId;
    sendMessage(session, Message::success(
        Protocol::MessageType::PUBLISH_TASK_RESPONSE, msg.seq(), responseData));
}

// ============================================================
// Get My Tasks (802) - counselor / admin
// ============================================================

void PsychServer::handleGetMyTasks(ClientSession* session, const Message& msg) {
    QString role = session->role();
    if (role != "counselor" && role != "admin") {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::PERMISSION_DENIED, "需要咨询师或管理员权限"));
        return;
    }

    QJsonObject payload = msg.payload();
    QString statusFilter = payload["status"].toString();
    int page = payload.value("page").toInt(1);
    int pageSize = payload.value("pageSize").toInt(20);
    int offset = (page - 1) * pageSize;

    QString where = "WHERE t.publisher_id = :uid ";
    if (!statusFilter.isEmpty()) {
        where += "AND t.status = :status ";
    }

    QString sql =
        "SELECT t.id, t.scale_id, s.name as scale_name, t.title, "
        "t.status, t.target_user_ids, t.due_date, t.created_at, "
        "u.nickname as reviewer_name "
        "FROM assessment_tasks t "
        "LEFT JOIN scales s ON t.scale_id = s.id "
        "LEFT JOIN users u ON t.reviewed_by = u.id " +
        where +
        "ORDER BY t.created_at DESC LIMIT :limit OFFSET :offset";

    QVariantMap params = {
        {"uid", session->userId()},
        {"limit", pageSize},
        {"offset", offset}
    };
    if (!statusFilter.isEmpty()) {
        params["status"] = statusFilter;
    }

    auto results = DbManager::instance().executeQuery(sql, params);

    QJsonArray tasksArray;
    for (const auto& row : results) {
        QJsonObject t;
        t["id"] = row["id"].toLongLong();
        t["scaleId"] = row["scale_id"].toInt();
        t["scaleName"] = row["scale_name"].toString();
        t["title"] = row["title"].toString();
        t["status"] = row["status"].toString();
        t["reviewedByName"] = row["reviewer_name"].toString();

        // Parse target_user_ids to get count
        QJsonDocument doc = QJsonDocument::fromJson(row["target_user_ids"].toByteArray());
        int targetCount = 0;
        if (doc.isArray()) {
            targetCount = doc.array().size();
        }
        t["targetCount"] = targetCount;

        // Get submit count for this task's scale
        auto submitResult = DbManager::instance().executeQuery(
            "SELECT COUNT(DISTINCT user_id) as cnt FROM assessment_records "
            "WHERE scale_id = :sid",
            {{"sid", row["scale_id"].toInt()}});
        t["submitCount"] = submitResult.isEmpty() ? 0 : submitResult.first()["cnt"].toInt();

        t["dueDate"] = row["due_date"].isNull() ? "" : row["due_date"].toDateTime().toString(Qt::ISODate);
        t["createdAt"] = row["created_at"].toDateTime().toString(Qt::ISODate);
        tasksArray.append(t);
    }

    QJsonObject responseData;
    responseData["tasks"] = tasksArray;
    responseData["page"] = page;
    sendMessage(session, Message::success(
        Protocol::MessageType::GET_MY_TASKS_RESPONSE, msg.seq(), responseData));
}

// ============================================================
// Get Pending Tasks (804) - admin only
// ============================================================

void PsychServer::handleGetPendingTasks(ClientSession* session, const Message& msg) {
    if (session->role() != "admin") {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::PERMISSION_DENIED, "需要管理员权限"));
        return;
    }

    QJsonObject payload = msg.payload();
    int page = payload.value("page").toInt(1);
    int pageSize = payload.value("pageSize").toInt(20);
    int offset = (page - 1) * pageSize;

    auto results = DbManager::instance().executeQuery(
        "SELECT t.id, t.title, t.description, t.target_user_ids, "
        "t.created_at, u.nickname as publisher_name, s.name as scale_name "
        "FROM assessment_tasks t "
        "LEFT JOIN users u ON t.publisher_id = u.id "
        "LEFT JOIN scales s ON t.scale_id = s.id "
        "WHERE t.status = 'pending' "
        "ORDER BY t.created_at ASC LIMIT :limit OFFSET :offset",
        {{"limit", pageSize}, {"offset", offset}});

    QJsonArray tasksArray;
    for (const auto& row : results) {
        QJsonObject t;
        t["id"] = row["id"].toLongLong();
        t["publisherName"] = row["publisher_name"].toString();
        t["scaleName"] = row["scale_name"].toString();
        t["title"] = row["title"].toString();
        t["description"] = row["description"].toString();

        QJsonDocument doc = QJsonDocument::fromJson(row["target_user_ids"].toByteArray());
        t["targetCount"] = doc.isArray() ? doc.array().size() : 0;
        t["createdAt"] = row["created_at"].toDateTime().toString(Qt::ISODate);
        tasksArray.append(t);
    }

    QJsonObject responseData;
    responseData["tasks"] = tasksArray;
    responseData["page"] = page;
    sendMessage(session, Message::success(
        Protocol::MessageType::GET_PENDING_TASKS_RESPONSE, msg.seq(), responseData));
}

// ============================================================
// Review Task (806) - admin only
// ============================================================

void PsychServer::handleReviewTask(ClientSession* session, const Message& msg) {
    if (session->role() != "admin") {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::PERMISSION_DENIED, "需要管理员权限"));
        return;
    }

    QJsonObject payload = msg.payload();
    qint64 taskId = payload["taskId"].toInteger();
    QString action = payload["action"].toString();  // "approve" or "reject"
    QString note = payload["note"].toString();

    if (taskId <= 0) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::INVALID_REQUEST, "无效的任务ID"));
        return;
    }

    auto taskCheck = DbManager::instance().executeQuery(
        "SELECT status FROM assessment_tasks WHERE id = :id",
        {{"id", taskId}});
    if (taskCheck.isEmpty()) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::USER_NOT_FOUND, "任务不存在"));
        return;
    }
    if (taskCheck.first()["status"].toString() != "pending") {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::INVALID_REQUEST, "任务已审核，不能重复操作"));
        return;
    }

    QString newStatus = (action == "approve") ? "active" : "closed";

    DbManager::instance().executeUpdate(
        "UPDATE assessment_tasks SET status = :status, reviewed_by = :reviewer, "
        "reviewed_at = NOW(), review_note = :note WHERE id = :id",
        {
            {"status", newStatus},
            {"reviewer", session->userId()},
            {"note", note},
            {"id", taskId}
        });

    logAction(session->userId(), "review_task", "task", taskId);

    QJsonObject responseData;
    responseData["success"] = true;
    sendMessage(session, Message::success(
        Protocol::MessageType::REVIEW_TASK_RESPONSE, msg.seq(), responseData));
}

// ============================================================
// Get Task Report (808)
// ============================================================

void PsychServer::handleGetTaskReport(ClientSession* session, const Message& msg) {
    if (!session->isAuthenticated()) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::AUTH_FAILED, "请先登录"));
        return;
    }

    QJsonObject payload = msg.payload();
    qint64 taskId = payload.value("taskId").toInteger(0);
    qint64 userId = payload.value("userId").toInteger(0);
    int page = payload.value("page").toInt(1);
    int pageSize = payload.value("pageSize").toInt(20);

    QString role = session->role();
    qint64 requesterId = session->userId();

    if (userId <= 0) {
        userId = requesterId;  // Default: view own records
    }

    // If viewing someone else's report
    if (userId != requesterId && role != "admin") {
        // Check if counselor has been granted access
        if (role == "counselor") {
            auto grantCheck = DbManager::instance().executeQuery(
                "SELECT id FROM report_access_grants "
                "WHERE user_id = :uid AND counselor_id = :cid "
                "AND (expires_at IS NULL OR expires_at > NOW())",
                {{"uid", userId}, {"cid", requesterId}});
            if (grantCheck.isEmpty()) {
                sendMessage(session, Message::error(msg.seq(),
                    Protocol::ErrorCode::PERMISSION_DENIED, "无权查看该用户的报告"));
                return;
            }
        } else {
            sendMessage(session, Message::error(msg.seq(),
                Protocol::ErrorCode::PERMISSION_DENIED, "无权查看该用户的报告"));
            return;
        }
    }

    // Build query
    QString where = "WHERE ar.user_id = :uid ";
    QVariantMap params = {{"uid", userId}, {"limit", pageSize}, {"offset", (page-1)*pageSize}};
    if (taskId > 0) {
        where += "AND ar.scale_id = (SELECT scale_id FROM assessment_tasks WHERE id = :tid) ";
        params["tid"] = taskId;
    }

    QString sql =
        "SELECT ar.id, ar.total_score, ar.result_level, ar.completed_at, "
        "u.nickname as user_name, s.name as scale_name "
        "FROM assessment_records ar "
        "LEFT JOIN users u ON ar.user_id = u.id "
        "LEFT JOIN scales s ON ar.scale_id = s.id " +
        where +
        "ORDER BY ar.completed_at DESC LIMIT :limit OFFSET :offset";

    auto results = DbManager::instance().executeQuery(sql, params);

    QJsonArray recordsArray;
    for (const auto& row : results) {
        QJsonObject r;
        r["id"] = row["id"].toLongLong();
        r["userName"] = row["user_name"].toString();
        r["scaleName"] = row["scale_name"].toString();
        r["totalScore"] = row["total_score"].toDouble();
        r["resultLevel"] = row["result_level"].toString();
        r["completedAt"] = row["completed_at"].toDateTime().toString(Qt::ISODate);
        recordsArray.append(r);
    }

    QJsonObject responseData;
    responseData["records"] = recordsArray;
    responseData["page"] = page;
    sendMessage(session, Message::success(
        Protocol::MessageType::GET_TASK_REPORT_RESPONSE, msg.seq(), responseData));
}

// ============================================================
// Grant Report Access (810) - user grants counselor access
// ============================================================

void PsychServer::handleGrantReportAccess(ClientSession* session, const Message& msg) {
    if (!session->isAuthenticated()) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::AUTH_FAILED, "请先登录"));
        return;
    }

    QString role = session->role();
    if (role != "user") {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::PERMISSION_DENIED, "只有普通用户可以授权"));
        return;
    }

    QJsonObject payload = msg.payload();
    qint64 counselorId = payload["counselorId"].toInteger();
    qint64 recordId = payload.value("recordId").toInteger(0);
    QString expiresAt = payload["expiresAt"].toString();

    if (counselorId <= 0) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::INVALID_REQUEST, "无效的咨询师ID"));
        return;
    }

    // Verify counselorId is a counselor
    auto counselorCheck = DbManager::instance().executeQuery(
        "SELECT role FROM users WHERE id = :id AND status = 'active'",
        {{"id", counselorId}});
    if (counselorCheck.isEmpty() || counselorCheck.first()["role"].toString() != "counselor") {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::INVALID_REQUEST, "目标用户不是咨询师"));
        return;
    }

    // Upsert grant
    DbManager::instance().executeUpdate(
        "INSERT INTO report_access_grants "
        "(user_id, counselor_id, record_id, expires_at) "
        "VALUES (:uid, :cid, :rid, :exp) "
        "ON DUPLICATE KEY UPDATE expires_at = VALUES(expires_at), granted_at = NOW()",
        {
            {"uid", session->userId()},
            {"cid", counselorId},
            {"rid", recordId > 0 ? QVariant(recordId) : QVariant(0)},
            {"exp", expiresAt.isEmpty() ? QVariant(QString()) : QVariant(expiresAt)}
        });

    logAction(session->userId(), "grant_report_access", "counselor", counselorId);

    QJsonObject responseData;
    responseData["granted"] = true;
    sendMessage(session, Message::success(
        Protocol::MessageType::GRANT_REPORT_ACCESS_RESPONSE, msg.seq(), responseData));
}
