#include "server/psych_server.h"
#include "database/db_manager.h"
#include "config.h"
#include "utils/logger.h"
#include <QCryptographicHash>
#include <QUuid>
#include <QDateTime>
#include <QJsonArray>

// ============================================================
// Auth Helpers
// ============================================================

static QString hashPassword(const QString& password, const QString& salt) {
    QByteArray data = (password + salt).toUtf8();
    // Multi-round SHA-256 for password hashing (in production, use bcrypt)
    for (int i = 0; i < Config::PASSWORD_HASH_ROUNDS; ++i) {
        data = QCryptographicHash::hash(data, QCryptographicHash::Sha256);
    }
    return QString::fromLatin1(data.toHex());
}

static QString generateToken() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

// ============================================================
// Login Handler
// ============================================================

void PsychServer::handleLogin(ClientSession* session, const Message& msg) {
    QJsonObject payload = msg.payload();
    QString username = payload["username"].toString().trimmed();
    QString password = payload["password"].toString();

    LOG_INFO(QString("Login attempt: %1").arg(username));

    // Validate input
    if (username.isEmpty() || password.isEmpty()) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::INVALID_REQUEST, "用户名和密码不能为空"));
        return;
    }

    // Query user
    auto results = DbManager::instance().executeQuery(
        "SELECT id, username, password_hash, salt, nickname, role, status, login_attempts, locked_until "
        "FROM users WHERE username = :username",
        {{"username", username}});

    if (results.isEmpty()) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::AUTH_FAILED, "用户名或密码错误"));
        logAction(0, "login_failed", "user", 0, {{"username", username}});
        return;
    }

    auto user = results.first();
    qint64 userId = user["id"].toLongLong();
    QString status = user["status"].toString();

    // Check if account is locked
    QVariant lockedUntil = user["locked_until"];
    if (!lockedUntil.isNull()) {
        QDateTime lockTime = lockedUntil.toDateTime();
        if (lockTime > QDateTime::currentDateTime()) {
            sendMessage(session, Message::error(msg.seq(),
                Protocol::ErrorCode::AUTH_FAILED,
                QString("账户已锁定，请在 %1 后重试").arg(lockTime.toString("HH:mm"))));
            return;
        }
        // Clear lock
        DbManager::instance().executeUpdate(
            "UPDATE users SET login_attempts = 0, locked_until = NULL WHERE id = :id",
            {{"id", userId}});
    }

    // Check status
    if (status == "banned") {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::PERMISSION_DENIED, "账户已被封禁"));
        return;
    }

    // Verify password
    QString salt = user["salt"].toString();
    QString storedHash = user["password_hash"].toString();
    QString inputHash = hashPassword(password, salt);

    if (inputHash != storedHash) {
        int attempts = user["login_attempts"].toInt() + 1;
        QVariantMap updateBindings = {{"id", userId}, {"attempts", attempts}};

        if (attempts >= Config::MAX_LOGIN_ATTEMPTS) {
            QDateTime lockUntil = QDateTime::currentDateTime().addSecs(Config::LOGIN_LOCK_MINUTES * 60);
            updateBindings["locked_until"] = lockUntil;
            DbManager::instance().executeUpdate(
                "UPDATE users SET login_attempts = :attempts, locked_until = :locked_until WHERE id = :id",
                updateBindings);
            sendMessage(session, Message::error(msg.seq(),
                Protocol::ErrorCode::AUTH_FAILED,
                QString("密码错误次数过多，账户已锁定 %1 分钟").arg(Config::LOGIN_LOCK_MINUTES)));
        } else {
            DbManager::instance().executeUpdate(
                "UPDATE users SET login_attempts = :attempts WHERE id = :id",
                updateBindings);
            sendMessage(session, Message::error(msg.seq(),
                Protocol::ErrorCode::AUTH_FAILED,
                QString("用户名或密码错误（剩余 %1 次尝试）")
                    .arg(Config::MAX_LOGIN_ATTEMPTS - attempts)));
        }
        logAction(userId, "login_failed", "user", userId);
        return;
    }

    // Login successful
    QString token = generateToken();
    QString role = user["role"].toString();
    session->setUserId(userId);
    session->setUsername(username);
    session->setToken(token);
    session->setRole(role);
    m_userSessions[userId] = session;

    // Update user record (write token to DB for auto-login)
    DbManager::instance().executeUpdate(
        "UPDATE users SET login_attempts = 0, locked_until = NULL, last_login = NOW(), token = :token WHERE id = :id",
        {{"id", userId}, {"token", token}});

    // Build response
    QJsonObject userData;
    userData["userId"] = userId;
    userData["username"] = username;
    userData["nickname"] = user["nickname"].toString();
    userData["role"] = user["role"].toString();
    userData["token"] = token;

    QJsonObject responseData;
    responseData["user"] = userData;

    sendMessage(session, Message::success(
        Protocol::MessageType::LOGIN_RESPONSE, msg.seq(), responseData));

    logAction(userId, "login", "user", userId);
    LOG_INFO(QString("User %1 (ID: %2) logged in successfully").arg(username).arg(userId));
}

// ============================================================
// Register Handler
// ============================================================

void PsychServer::handleRegister(ClientSession* session, const Message& msg) {
    QJsonObject payload = msg.payload();
    QString username = payload["username"].toString().trimmed();
    QString password = payload["password"].toString();
    QString email = payload["email"].toString().trimmed();
    QString nickname = payload["nickname"].toString().trimmed();

    LOG_INFO(QString("Register attempt: %1").arg(username));

    // Validate input
    if (username.isEmpty() || password.isEmpty()) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::INVALID_REQUEST, "用户名和密码不能为空"));
        return;
    }
    if (username.length() < 3 || username.length() > 50) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::INVALID_REQUEST, "用户名长度需在 3-50 个字符之间"));
        return;
    }
    if (password.length() < 6) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::INVALID_REQUEST, "密码长度至少为 6 个字符"));
        return;
    }

    // Check if username exists
    auto existing = DbManager::instance().executeQuery(
        "SELECT id FROM users WHERE username = :username",
        {{"username", username}});

    if (!existing.isEmpty()) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::USER_ALREADY_EXISTS, "用户名已被注册"));
        return;
    }

    // Check if email exists
    if (!email.isEmpty()) {
        auto emailCheck = DbManager::instance().executeQuery(
            "SELECT id FROM users WHERE email = :email",
            {{"email", email}});
        if (!emailCheck.isEmpty()) {
            sendMessage(session, Message::error(msg.seq(),
                Protocol::ErrorCode::USER_ALREADY_EXISTS, "邮箱已被注册"));
            return;
        }
    }

    // Generate salt and hash
    QString salt = QUuid::createUuid().toString(QUuid::WithoutBraces).left(32);
    QString passwordHash = hashPassword(password, salt);

    // Insert user
    if (nickname.isEmpty()) {
        nickname = username;
    }

    int rows = DbManager::instance().executeUpdate(
        "INSERT INTO users (username, password_hash, salt, email, nickname, role, status) "
        "VALUES (:username, :password_hash, :salt, :email, :nickname, 'user', 'active')",
        {
            {"username", username},
            {"password_hash", passwordHash},
            {"salt", salt},
            {"email", email.isEmpty() ? QVariant() : QVariant(email)},
            {"nickname", nickname}
        });

    if (rows <= 0) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::DATABASE_ERROR, "注册失败，请稍后重试"));
        return;
    }

    qint64 newUserId = DbManager::instance().lastInsertId();

    QJsonObject responseData;
    responseData["userId"] = newUserId;
    responseData["username"] = username;

    sendMessage(session, Message::success(
        Protocol::MessageType::REGISTER_RESPONSE, msg.seq(), responseData));

    logAction(newUserId, "register", "user", newUserId);
    LOG_INFO(QString("User registered: %1 (ID: %2)").arg(username).arg(newUserId));
}

// ============================================================
// Heartbeat Handler
// ============================================================

void PsychServer::handleHeartbeat(ClientSession* session, const Message& msg) {
    QJsonObject responseData;
    responseData["timestamp"] = QDateTime::currentMSecsSinceEpoch();
    responseData["serverTime"] = QDateTime::currentDateTime().toString(Qt::ISODate);

    sendMessage(session, Message::success(
        Protocol::MessageType::HEARTBEAT_ACK, msg.seq(), responseData));
}

// ============================================================
// Token Validation Handler
// ============================================================

void PsychServer::handleValidateToken(ClientSession* session, const Message& msg) {
    LOG_DEBUG(QString("[handleValidateToken] ENTRY seq=%1").arg(msg.seq()));
    QJsonObject payload = msg.payload();
    qint64 userId = payload["userId"].toInteger();
    QString token = payload["token"].toString();
    LOG_DEBUG(QString("[handleValidateToken] userId=%1 token=%2").arg(userId).arg(token.left(8)));

    // 检查用户是否存在且token匹配
    auto results = DbManager::instance().executeQuery(
        "SELECT id, username, nickname, role, status, token FROM users WHERE id = :id",
        {{"id", userId}});
    LOG_DEBUG(QString("[handleValidateToken] query returned %1 rows").arg(results.size()));

    if (results.isEmpty()) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::USER_NOT_FOUND, "用户不存在"));
        return;
    }

    auto user = results.first();
    QString status = user["status"].toString();

    if (status == "banned") {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::PERMISSION_DENIED, "账户已被封禁"));
        return;
    }

    // 验证 token 是否匹配
    QString storedToken = user["token"].toString();
    if (storedToken.isEmpty() || storedToken != token) {
        LOG_WARN(QString("Token validation failed for user %1 (ID: %2) - token mismatch")
                     .arg(user["username"].toString()).arg(userId));
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::AUTH_FAILED, "登录已过期，请重新登录"));
        return;
    }

    // Token验证成功，恢复会话
    session->setUserId(userId);
    session->setUsername(user["username"].toString());
    session->setToken(token);
    session->setRole(user["role"].toString());
    m_userSessions[userId] = session;

    // 返回用户信息
    QJsonObject userData;
    userData["userId"] = userId;
    userData["username"] = user["username"].toString();
    userData["nickname"] = user["nickname"].toString();
    userData["role"] = user["role"].toString();
    userData["token"] = token;

    QJsonObject responseData;
    responseData["user"] = userData;
    responseData["valid"] = true;

    sendMessage(session, Message::success(
        Protocol::MessageType::VALIDATE_TOKEN_RESPONSE, msg.seq(), responseData));

    LOG_INFO(QString("Token validated for user %1 (ID: %2)").arg(user["username"].toString()).arg(userId));
}

// ============================================================
// Logout Handler
// ============================================================

void PsychServer::handleLogout(ClientSession* session, const Message& msg) {
    qint64 userId = session->userId();

    if (userId > 0) {
        // 清除数据库中该用户的 token，使自动登录失效
        DbManager::instance().executeUpdate(
            "UPDATE users SET token = NULL WHERE id = :id",
            {{"id", userId}});
        LOG_INFO(QString("User %1 (ID: %2) logged out, token cleared")
                     .arg(session->username()).arg(userId));
    }

    // 从内存会话表中移除
    m_userSessions.remove(userId);
    session->setUserId(-1);
    session->setToken("");
    session->setRole("");

    sendMessage(session, Message::success(
        Protocol::MessageType::LOGOUT_RESPONSE, msg.seq(),
        QJsonObject{{"success", true}}));

    // 断开连接（延迟删除，避免在信号处理中直接 delete）
    QTimer::singleShot(0, session, &QObject::deleteLater);
}

