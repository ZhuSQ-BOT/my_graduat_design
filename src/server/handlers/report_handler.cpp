#include "server/psych_server.h"
#include "database/db_manager.h"
#include "utils/logger.h"
#include <QJsonArray>
#include <QDateTime>

// ============================================================
// Helper: Ensure content_reports table exists
// ============================================================
static void ensureReportsTable() {
    DbManager::instance().executeUpdate(
        "CREATE TABLE IF NOT EXISTS content_reports ("
        "  id BIGINT NOT NULL AUTO_INCREMENT PRIMARY KEY,"
        "  target_type VARCHAR(20) NOT NULL,"
        "  target_id BIGINT NOT NULL,"
        "  reporter_id BIGINT NOT NULL,"
        "  reason VARCHAR(500) NOT NULL,"
        "  status ENUM('pending','resolved','dismissed') NOT NULL DEFAULT 'pending',"
        "  resolved_by BIGINT DEFAULT NULL,"
        "  resolution_note VARCHAR(500) DEFAULT NULL,"
        "  created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,"
        "  resolved_at DATETIME DEFAULT NULL,"
        "  INDEX idx_status (status),"
        "  INDEX idx_target (target_type, target_id),"
        "  INDEX idx_reporter (reporter_id),"
        "  FOREIGN KEY (reporter_id) REFERENCES users(id) ON DELETE CASCADE,"
        "  FOREIGN KEY (resolved_by) REFERENCES users(id) ON DELETE SET NULL"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci",
        {});
}

// ============================================================
// Get Reports (706)
// ============================================================

void PsychServer::handleGetReports(ClientSession* session, const Message& msg) {
    if (!session->isAuthenticated()) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::AUTH_FAILED, "请先登录"));
        return;
    }

    ensureReportsTable();

    QString sql = "SELECT cr.id, cr.target_type, cr.target_id, cr.reason, cr.status, "
                  "cr.created_at, cr.resolved_at, cr.resolution_note, "
                  "u.username AS reporter_name "
                  "FROM content_reports cr "
                  "LEFT JOIN users u ON cr.reporter_id = u.id "
                  "ORDER BY "
                  "  CASE cr.status WHEN 'pending' THEN 0 ELSE 1 END, "
                  "  cr.created_at DESC";

    auto results = DbManager::instance().executeQuery(sql, {});

    QJsonArray reportsArray;
    for (const auto& row : results) {
        QJsonObject r;
        r["reportId"] = row["id"].toLongLong();
        r["targetType"] = row["target_type"].toString();
        r["targetId"] = row["target_id"].toLongLong();
        r["reporterName"] = row["reporter_name"].toString();
        r["reason"] = row["reason"].toString();
        r["status"] = row["status"].toString();
        r["createdAt"] = row["created_at"].toDateTime().toString("yyyy-MM-dd hh:mm:ss");
        if (!row["resolved_at"].isNull()) {
            r["resolvedAt"] = row["resolved_at"].toDateTime().toString("yyyy-MM-dd hh:mm:ss");
        }
        if (!row["resolution_note"].isNull()) {
            r["resolutionNote"] = row["resolution_note"].toString();
        }
        reportsArray.append(r);
    }

    // Count pending for dashboard
    auto pendingResults = DbManager::instance().executeQuery(
        "SELECT COUNT(*) AS cnt FROM content_reports WHERE status = 'pending'", {});
    int pendingCount = pendingResults.isEmpty() ? 0 : pendingResults[0]["cnt"].toInt();

    QJsonObject data;
    data["reports"] = reportsArray;
    data["pendingCount"] = pendingCount;

    sendMessage(session, Message::success(
        Protocol::MessageType::GET_REPORTS_RESPONSE, msg.seq(), data));

    LOG_INFO(QString("Reports listed (pending: %1) by admin %2").arg(pendingCount).arg(session->userId()));
}

// ============================================================
// Resolve Report (708)
// ============================================================

void PsychServer::handleResolveReport(ClientSession* session, const Message& msg) {
    if (!session->isAuthenticated()) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::AUTH_FAILED, "请先登录"));
        return;
    }

    QJsonObject payload = msg.payload();
    qint64 reportId = payload["reportId"].toInteger();

    if (reportId <= 0) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::INVALID_REQUEST, "无效的举报ID"));
        return;
    }

    QVariantMap bindings;
    bindings["reportId"] = reportId;
    bindings["resolvedBy"] = session->userId();

    bool ok = DbManager::instance().executeUpdate(
        "UPDATE content_reports SET status = 'resolved', resolved_by = :resolvedBy, "
        "resolved_at = NOW() WHERE id = :reportId AND status = 'pending'",
        bindings);

    if (!ok) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::DATABASE_ERROR, "处理举报失败"));
        return;
    }

    sendMessage(session, Message::success(
        Protocol::MessageType::RESOLVE_REPORT_RESPONSE, msg.seq(), {}));

    logAction(session->userId(), "resolve_report", "report", reportId);
    LOG_INFO(QString("Report %1 resolved by admin %2").arg(reportId).arg(session->userId()));
}

// ============================================================
// Dismiss Report (710)
// ============================================================

void PsychServer::handleDismissReport(ClientSession* session, const Message& msg) {
    if (!session->isAuthenticated()) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::AUTH_FAILED, "请先登录"));
        return;
    }

    QJsonObject payload = msg.payload();
    qint64 reportId = payload["reportId"].toInteger();

    if (reportId <= 0) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::INVALID_REQUEST, "无效的举报ID"));
        return;
    }

    QVariantMap bindings;
    bindings["reportId"] = reportId;
    bindings["resolvedBy"] = session->userId();

    bool ok = DbManager::instance().executeUpdate(
        "UPDATE content_reports SET status = 'dismissed', resolved_by = :resolvedBy, "
        "resolved_at = NOW() WHERE id = :reportId AND status = 'pending'",
        bindings);

    if (!ok) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::DATABASE_ERROR, "驳回举报失败"));
        return;
    }

    sendMessage(session, Message::success(
        Protocol::MessageType::DISMISS_REPORT_RESPONSE, msg.seq(), {}));

    logAction(session->userId(), "dismiss_report", "report", reportId);
    LOG_INFO(QString("Report %1 dismissed by admin %2").arg(reportId).arg(session->userId()));
}
