#include "server/psych_server.h"
#include "database/db_manager.h"
#include "utils/logger.h"
#include <QJsonArray>
#include <QDateTime>

// ============================================================
// Get Counselors
// ============================================================

void PsychServer::handleGetCounselors(ClientSession* session, const Message& msg) {
    if (!session->isAuthenticated()) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::AUTH_FAILED, "请先登录"));
        return;
    }

    auto results = DbManager::instance().executeQuery(
        "SELECT id, nickname, avatar, email FROM users "
        "WHERE role = 'counselor' AND status = 'active' ORDER BY nickname");

    QJsonArray counselorArray;
    for (const auto& row : results) {
        QJsonObject c;
        c["id"] = row["id"].toLongLong();
        c["nickname"] = row["nickname"].toString();
        c["avatar"] = row["avatar"].toString();
        c["email"] = row["email"].toString();
        counselorArray.append(c);
    }

    QJsonObject responseData;
    responseData["counselors"] = counselorArray;

    sendMessage(session, Message::success(
        Protocol::MessageType::GET_COUNSELORS_RESPONSE, msg.seq(), responseData));
}

// ============================================================
// Create Appointment
// ============================================================

void PsychServer::handleCreateAppointment(ClientSession* session, const Message& msg) {
    if (!session->isAuthenticated()) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::AUTH_FAILED, "请先登录"));
        return;
    }

    QJsonObject payload = msg.payload();
    qint64 counselorId = payload["counselorId"].toInteger();
    QString scheduledAt = payload["scheduledAt"].toString();
    int durationMinutes = payload.value("durationMinutes").toInt(60);
    QString notes = payload["notes"].toString();

    if (counselorId <= 0 || scheduledAt.isEmpty()) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::INVALID_REQUEST, "无效的咨询师或时间"));
        return;
    }

    // Check if counselor exists
    auto check = DbManager::instance().executeQuery(
        "SELECT id FROM users WHERE id = :id AND role = 'counselor'",
        {{"id", counselorId}});
    if (check.isEmpty()) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::USER_NOT_FOUND, "咨询师不存在"));
        return;
    }

    // Check time conflict
    auto conflict = DbManager::instance().executeQuery(
        "SELECT id FROM appointments "
        "WHERE counselor_id = :cid AND scheduled_at = :time "
        "AND status IN ('pending', 'confirmed')",
        {{"cid", counselorId}, {"time", scheduledAt}});
    if (!conflict.isEmpty()) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::INVALID_REQUEST, "该时段已被预约"));
        return;
    }

    int rows = DbManager::instance().executeUpdate(
        "INSERT INTO appointments (user_id, counselor_id, scheduled_at, duration_minutes, notes) "
        "VALUES (:user_id, :counselor_id, :scheduled_at, :duration, :notes)",
        {
            {"user_id", session->userId()},
            {"counselor_id", counselorId},
            {"scheduled_at", scheduledAt},
            {"duration", durationMinutes},
            {"notes", notes.isEmpty() ? QVariant(QVariant::String) : QVariant(notes)}
        });

    qint64 appointmentId = DbManager::instance().lastInsertId();

    QJsonObject responseData;
    responseData["appointmentId"] = appointmentId;

    sendMessage(session, Message::success(
        Protocol::MessageType::CREATE_APPOINTMENT_RESPONSE, msg.seq(), responseData));

    logAction(session->userId(), "create_appointment", "appointment", appointmentId);
}

// ============================================================
// Get Appointments
// ============================================================

void PsychServer::handleGetAppointments(ClientSession* session, const Message& msg) {
    if (!session->isAuthenticated()) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::AUTH_FAILED, "请先登录"));
        return;
    }

    QJsonObject payload = msg.payload();
    QString status = payload["status"].toString();

    QString sql = "SELECT a.id, a.scheduled_at, a.duration_minutes, a.status, a.notes, "
                  "u.nickname as counselor_name, u.avatar as counselor_avatar "
                  "FROM appointments a "
                  "JOIN users u ON a.counselor_id = u.id "
                  "WHERE a.user_id = :user_id";
    QVariantMap bindings = {{"user_id", session->userId()}};

    if (!status.isEmpty()) {
        sql += " AND a.status = :status";
        bindings["status"] = status;
    }

    sql += " ORDER BY a.scheduled_at DESC";

    auto results = DbManager::instance().executeQuery(sql, bindings);

    QJsonArray appointmentsArray;
    for (const auto& row : results) {
        QJsonObject apt;
        apt["id"] = row["id"].toLongLong();
        apt["counselorName"] = row["counselor_name"].toString();
        apt["counselorAvatar"] = row["counselor_avatar"].toString();
        apt["scheduledAt"] = row["scheduled_at"].toDateTime().toString(Qt::ISODate);
        apt["durationMinutes"] = row["duration_minutes"].toInt();
        apt["status"] = row["status"].toString();
        apt["notes"] = row["notes"].toString();
        appointmentsArray.append(apt);
    }

    QJsonObject responseData;
    responseData["appointments"] = appointmentsArray;

    sendMessage(session, Message::success(
        Protocol::MessageType::GET_APPOINTMENTS_RESPONSE, msg.seq(), responseData));
}
