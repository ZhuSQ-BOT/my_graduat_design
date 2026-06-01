#include "server/psych_server.h"
#include "database/db_manager.h"
#include "utils/logger.h"
#include <QJsonArray>
#include <QCryptographicHash>
#include <QByteArray>

// ============================================================
// Get Profile (150)
// ============================================================

void PsychServer::handleGetProfile(ClientSession* session, const Message& msg) {
    if (!session->isAuthenticated()) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::AUTH_FAILED, "请先登录"));
        return;
    }

    qint64 userId = session->userId();
    QString sql = "SELECT id, username, nickname, email, phone, role, bio, "
                  "DATE_FORMAT(created_at, '%Y-%m-%d') AS join_date, "
                  "DATE_FORMAT(last_login, '%Y-%m-%d %H:%i:%s') AS last_login "
                  "FROM users WHERE id = :userId";

    QVariantMap bindings;
    bindings["userId"] = userId;

    auto results = DbManager::instance().executeQuery(sql, bindings);
    if (results.isEmpty()) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::USER_NOT_FOUND, "用户不存在"));
        return;
    }

    const auto& row = results[0];
    QJsonObject profile;
    profile["userId"] = row["id"].toLongLong();
    profile["username"] = row["username"].toString();
    profile["nickname"] = row["nickname"].toString();
    profile["email"] = row["email"].toString();
    profile["phone"] = row["phone"].toString();
    profile["role"] = row["role"].toString();
    profile["bio"] = row["bio"].toString();
    profile["createdAt"] = row["join_date"].toString();

    QJsonObject data;
    data["profile"] = profile;

    sendMessage(session, Message::success(
        Protocol::MessageType::GET_PROFILE_RESPONSE, msg.seq(), data));

    LOG_INFO(QString("Profile fetched for user %1").arg(userId));
}

// ============================================================
// Update Profile (152)
// ============================================================

void PsychServer::handleUpdateProfile(ClientSession* session, const Message& msg) {
    if (!session->isAuthenticated()) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::AUTH_FAILED, "请先登录"));
        return;
    }

    QJsonObject payload = msg.payload();
    QString nickname = payload["nickname"].toString().trimmed();
    QString phone = payload["phone"].toString().trimmed();
    QString email = payload["email"].toString().trimmed();
    QString bio = payload["bio"].toString().trimmed();

    if (nickname.isEmpty()) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::INVALID_REQUEST, "昵称不能为空"));
        return;
    }

    qint64 userId = session->userId();
    QString sql = "UPDATE users SET nickname = :nickname, phone = :phone, "
                  "email = :email, bio = :bio WHERE id = :userId";

    QVariantMap bindings;
    bindings["nickname"] = nickname;
    bindings["phone"] = phone;
    bindings["email"] = email;
    bindings["bio"] = bio;
    bindings["userId"] = userId;

    bool ok = DbManager::instance().executeUpdate(sql, bindings);
    if (!ok) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::DATABASE_ERROR, "更新失败，请稍后重试"));
        return;
    }

    // Update session's username (nickname)
    session->setUsername(nickname);

    QJsonObject data;
    data["nickname"] = nickname;
    data["phone"] = phone;
    data["email"] = email;
    data["bio"] = bio;

    sendMessage(session, Message::success(
        Protocol::MessageType::UPDATE_PROFILE_RESPONSE, msg.seq(), data));

    logAction(userId, "update_profile");
    LOG_INFO(QString("Profile updated for user %1").arg(userId));
}

// ============================================================
// Change Password (154)
// ============================================================

void PsychServer::handleChangePassword(ClientSession* session, const Message& msg) {
    if (!session->isAuthenticated()) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::AUTH_FAILED, "请先登录"));
        return;
    }

    QJsonObject payload = msg.payload();
    QString oldPassword = payload["oldPassword"].toString();
    QString newPassword = payload["newPassword"].toString();

    if (newPassword.trimmed().isEmpty()) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::INVALID_REQUEST, "新密码不能为空"));
        return;
    }

    if (newPassword.length() < 6) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::INVALID_REQUEST, "新密码长度至少为6位"));
        return;
    }

    qint64 userId = session->userId();

    // Fetch current password hash and salt
    QString fetchSql = "SELECT password_hash, salt FROM users WHERE id = :userId";
    QVariantMap fetchBindings;
    fetchBindings["userId"] = userId;

    auto results = DbManager::instance().executeQuery(fetchSql, fetchBindings);
    if (results.isEmpty()) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::USER_NOT_FOUND, "用户不存在"));
        return;
    }

    QString storedHash = results[0]["password_hash"].toString();
    QString salt = results[0]["salt"].toString();

    // Verify old password
    QByteArray oldSalted = (oldPassword.toUtf8() + salt.toUtf8());
    QString oldHash = QString::fromUtf8(
        QCryptographicHash::hash(oldSalted, QCryptographicHash::Sha256).toHex());

    if (oldHash != storedHash) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::AUTH_FAILED, "当前密码错误"));
        return;
    }

    // Generate new salt and hash for new password
    QString newSalt = QUuid::createUuid().toString(QUuid::WithoutBraces).left(16);
    QByteArray newSalted = (newPassword.toUtf8() + newSalt.toUtf8());
    QString newHash = QString::fromUtf8(
        QCryptographicHash::hash(newSalted, QCryptographicHash::Sha256).toHex());

    // Update password
    QString updateSql = "UPDATE users SET password_hash = :hash, salt = :salt WHERE id = :userId";
    QVariantMap updateBindings;
    updateBindings["hash"] = newHash;
    updateBindings["salt"] = newSalt;
    updateBindings["userId"] = userId;

    bool ok = DbManager::instance().executeUpdate(updateSql, updateBindings);
    if (!ok) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::DATABASE_ERROR, "密码修改失败，请稍后重试"));
        return;
    }

    sendMessage(session, Message::success(
        Protocol::MessageType::CHANGE_PASSWORD_RESPONSE, msg.seq(), {}));

    logAction(userId, "change_password");
    LOG_INFO(QString("Password changed for user %1").arg(userId));
}

// ============================================================
// Get User Stats (156)
// ============================================================

void PsychServer::handleGetUserStats(ClientSession* session, const Message& msg) {
    if (!session->isAuthenticated()) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::AUTH_FAILED, "请先登录"));
        return;
    }

    qint64 userId = session->userId();

    // Assessment count
    auto assessmentResults = DbManager::instance().executeQuery(
        "SELECT COUNT(*) AS cnt FROM assessment_records WHERE user_id = :userId",
        {{"userId", userId}});
    int assessmentCount = assessmentResults.isEmpty() ? 0 : assessmentResults[0]["cnt"].toInt();

    // Appointment count
    auto appointmentResults = DbManager::instance().executeQuery(
        "SELECT COUNT(*) AS cnt FROM appointments WHERE user_id = :userId",
        {{"userId", userId}});
    int appointmentCount = appointmentResults.isEmpty() ? 0 : appointmentResults[0]["cnt"].toInt();

    // Post count
    auto postResults = DbManager::instance().executeQuery(
        "SELECT COUNT(*) AS cnt FROM forum_posts WHERE author_id = :userId",
        {{"userId", userId}});
    int postCount = postResults.isEmpty() ? 0 : postResults[0]["cnt"].toInt();

    // Reply count
    auto replyResults = DbManager::instance().executeQuery(
        "SELECT COUNT(*) AS cnt FROM forum_replies WHERE author_id = :userId",
        {{"userId", userId}});
    int replyCount = replyResults.isEmpty() ? 0 : replyResults[0]["cnt"].toInt();

    // Message count (sent)
    auto messageResults = DbManager::instance().executeQuery(
        "SELECT COUNT(*) AS cnt FROM messages WHERE sender_id = :userId",
        {{"userId", userId}});
    int messageCount = messageResults.isEmpty() ? 0 : messageResults[0]["cnt"].toInt();

    QJsonObject data;
    data["assessmentCount"] = assessmentCount;
    data["appointmentCount"] = appointmentCount;
    data["postCount"] = postCount;
    data["replyCount"] = replyCount;
    data["messageCount"] = messageCount;

    sendMessage(session, Message::success(
        Protocol::MessageType::GET_USER_STATS_RESPONSE, msg.seq(), data));

    LOG_INFO(QString("Stats fetched for user %1").arg(userId));
}
