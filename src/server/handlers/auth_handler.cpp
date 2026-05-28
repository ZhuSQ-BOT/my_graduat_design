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
    session->setUserId(userId);
    session->setUsername(username);
    session->setToken(token);
    m_userSessions[userId] = session;

    // Update user record
    DbManager::instance().executeUpdate(
        "UPDATE users SET login_attempts = 0, locked_until = NULL, last_login = NOW() WHERE id = :id",
        {{"id", userId}});

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
            {"email", email.isEmpty() ? QVariant(QVariant::String) : QVariant(email)},
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
