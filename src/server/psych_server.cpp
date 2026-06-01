#include "server/psych_server.h"
#include "config.h"
#include "database/db_manager.h"
#include "utils/logger.h"
#include <QJsonDocument>
#include <QJsonArray>
#include <QCryptographicHash>
#include <QUuid>
#include <QDateTime>

// ============================================================
// ClientSession Implementation
// ============================================================

ClientSession::ClientSession(QTcpSocket* socket, QObject* parent)
    : QObject(parent), m_socket(socket)
{
    connect(m_socket, &QTcpSocket::readyRead, this, &ClientSession::onReadyRead);
    connect(m_socket, &QTcpSocket::disconnected, this, &ClientSession::onDisconnected);

    m_heartbeatTimer = new QTimer(this);
    m_heartbeatTimer->setInterval(Config::SESSION_TIMEOUT_MS);
    m_heartbeatTimer->setSingleShot(true);
    connect(m_heartbeatTimer, &QTimer::timeout, this, [this]() {
        LOG_WARN(QString("Session timeout for user %1").arg(m_userId));
        m_socket->disconnectFromHost();
    });
    m_heartbeatTimer->start();
}

void ClientSession::resetHeartbeat() {
    m_heartbeatTimer->start();
}

void ClientSession::onReadyRead() {
    m_buffer.append(m_socket->readAll());
    processBuffer();
}

void ClientSession::processBuffer() {
    while (m_buffer.size() >= Protocol::HEADER_SIZE) {
        // Peek at header to get body length
        QDataStream peekStream(m_buffer);
        peekStream.setByteOrder(QDataStream::BigEndian);
        quint32 magic, bodyLen;
        peekStream >> magic >> bodyLen;

        if (magic != Protocol::MAGIC_NUMBER) {
            LOG_WARN("Invalid magic number, closing connection");
            m_socket->disconnectFromHost();
            return;
        }

        int totalLen = Protocol::HEADER_SIZE + static_cast<int>(bodyLen);
        if (m_buffer.size() < totalLen) {
            break; // Wait for more data
        }

        QByteArray msgData = m_buffer.left(totalLen);
        m_buffer.remove(0, totalLen);

        Message msg;
        if (Message::deserialize(msgData, msg)) {
            emit messageReceived(this, msg);
        } else {
            LOG_WARN("Failed to deserialize message");
        }
    }
}

void ClientSession::onDisconnected() {
    emit disconnected(this);
}

// ============================================================
// PsychServer Implementation
// ============================================================

PsychServer::PsychServer(QObject* parent) : QObject(parent) {
    m_cleanupTimer = new QTimer(this);
    connect(m_cleanupTimer, &QTimer::timeout, this, &PsychServer::cleanupExpiredSessions);
}

PsychServer::~PsychServer() {
    stop();
}

bool PsychServer::start() {
    // Initialize database
    QString dbPassword = qEnvironmentVariable("PSYCH_DB_PASSWORD");
    if (dbPassword.isEmpty()) {
        dbPassword = Config::DB_PASSWORD;
    }

    if (!DbManager::instance().init(Config::DB_HOST, Config::DB_PORT,
                                     Config::DB_NAME, Config::DB_USER,
                                     dbPassword, 5)) {
        LOG_FATAL("Failed to initialize database connection pool");
        return false;
    }

    // Start TCP server
    m_tcpServer = new QTcpServer(this);
    connect(m_tcpServer, &QTcpServer::newConnection,
            this, &PsychServer::onNewTcpConnection);

    if (!m_tcpServer->listen(QHostAddress::Any, Config::TCP_PORT)) {
        LOG_FATAL(QString("TCP server failed to listen on port %1: %2")
                      .arg(Config::TCP_PORT)
                      .arg(m_tcpServer->errorString()));
        return false;
    }

    // Start WebSocket server
    m_wsServer = new QWebSocketServer("PsychWS", QWebSocketServer::NonSecureMode, this);
    connect(m_wsServer, &QWebSocketServer::newConnection,
            this, &PsychServer::onNewWsConnection);

    if (!m_wsServer->listen(QHostAddress::Any, Config::WS_PORT)) {
        LOG_FATAL(QString("WebSocket server failed to listen on port %1: %2")
                      .arg(Config::WS_PORT)
                      .arg(m_wsServer->errorString()));
        return false;
    }

    // Start cleanup timer (every 5 minutes)
    m_cleanupTimer->start(300000);

    LOG_INFO(QString("TCP server listening on port %1").arg(Config::TCP_PORT));
    LOG_INFO(QString("WebSocket server listening on port %1").arg(Config::WS_PORT));
    return true;
}

void PsychServer::stop() {
    if (m_tcpServer) {
        m_tcpServer->close();
    }
    if (m_wsServer) {
        m_wsServer->close();
    }
    DbManager::instance().close();
    LOG_INFO("Server stopped");
}

void PsychServer::onNewTcpConnection() {
    while (m_tcpServer->hasPendingConnections()) {
        QTcpSocket* socket = m_tcpServer->nextPendingConnection();

        if (m_sessions.size() >= Config::MAX_CONNECTIONS) {
            LOG_WARN("Max connections reached, rejecting new connection");
            socket->disconnectFromHost();
            socket->deleteLater();
            continue;
        }

        auto* session = new ClientSession(socket, this);
        connect(session, &ClientSession::messageReceived,
                this, &PsychServer::onClientMessage);
        connect(session, &ClientSession::disconnected,
                this, &PsychServer::onClientDisconnected);

        m_sessions[socket] = session;

        LOG_INFO(QString("New TCP connection from %1:%2")
                     .arg(socket->peerAddress().toString())
                     .arg(socket->peerPort()));
    }
}

void PsychServer::onNewWsConnection() {
    while (m_wsServer->hasPendingConnections()) {
        QWebSocket* ws = m_wsServer->nextPendingConnection();
        connect(ws, &QWebSocket::textMessageReceived,
                this, &PsychServer::onWsMessageReceived);
        connect(ws, &QWebSocket::disconnected,
                this, &PsychServer::onWsDisconnected);

        m_wsSessions[ws] = -1; // Not authenticated yet
        LOG_INFO("New WebSocket connection");
    }
}

void PsychServer::onClientMessage(ClientSession* session, const Message& msg) {
    session->resetHeartbeat();

    LOG_INFO(QString("[onClientMessage] Received message type: %1 from user: %2 role: %3")
                 .arg(static_cast<quint32>(msg.type()))
                 .arg(session->userId())
                 .arg(session->role()));

    // ============================================================
    // 权限校验：未登录只能访问认证相关接口
    // ============================================================
    bool isAuthMessage =
        msg.type() == Protocol::MessageType::LOGIN_REQUEST ||
        msg.type() == Protocol::MessageType::REGISTER_REQUEST ||
        msg.type() == Protocol::MessageType::VALIDATE_TOKEN_REQUEST ||
        msg.type() == Protocol::MessageType::HEARTBEAT;

    if (!session->isAuthenticated() && !isAuthMessage) {
        LOG_WARN(QString("Unauthenticated access attempt: msgType=%1 from %2")
                     .arg(static_cast<quint32>(msg.type()))
                     .arg(session->socket()->peerAddress().toString()));
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::AUTH_FAILED, "请先登录"));
        return;
    }

    // ============================================================
    // 权限校验：管理员专属接口
    // ============================================================
    bool isAdminOnly =
        msg.type() == Protocol::MessageType::GET_DASHBOARD_STATS_REQUEST ||
        msg.type() == Protocol::MessageType::MANAGE_USER_REQUEST ||
        msg.type() == Protocol::MessageType::GET_SYSTEM_LOGS_REQUEST;

    if (isAdminOnly && session->role() != "admin") {
        LOG_WARN(QString("Permission denied: user %1 (role=%2) attempted admin action %3")
                     .arg(session->userId()).arg(session->role()).arg(static_cast<quint32>(msg.type())));
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::PERMISSION_DENIED, "权限不足，需要管理员身份"));
        return;
    }

    switch (msg.type()) {
    // Auth
    case Protocol::MessageType::LOGIN_REQUEST:
        handleLogin(session, msg);
        break;
    case Protocol::MessageType::REGISTER_REQUEST:
        handleRegister(session, msg);
        break;
    case Protocol::MessageType::VALIDATE_TOKEN_REQUEST:
        handleValidateToken(session, msg);
        break;
    case Protocol::MessageType::LOGOUT_REQUEST:
        handleLogout(session, msg);
        break;

    // Assessment
    case Protocol::MessageType::GET_SCALES_REQUEST:
        handleGetScales(session, msg);
        break;
    case Protocol::MessageType::GET_SCALE_DETAIL_REQUEST:
        handleGetScaleDetail(session, msg);
        break;
    case Protocol::MessageType::SUBMIT_ASSESSMENT_REQUEST:
        handleSubmitAssessment(session, msg);
        break;
    case Protocol::MessageType::GET_ASSESSMENT_HISTORY_REQUEST:
        handleGetAssessmentHistory(session, msg);
        break;
    case Protocol::MessageType::GET_ASSESSMENT_STATS_REQUEST:
        handleGetAssessmentStats(session, msg);
        break;

    // Messaging
    case Protocol::MessageType::SEND_MESSAGE:
        handleSendMessage(session, msg);
        break;
    case Protocol::MessageType::GET_CONTACTS_REQUEST:
        handleGetContacts(session, msg);
        break;
    case Protocol::MessageType::GET_MESSAGE_HISTORY_REQUEST:
        handleGetMessageHistory(session, msg);
        break;

    // Forum
    case Protocol::MessageType::GET_POSTS_REQUEST:
        handleGetPosts(session, msg);
        break;
    case Protocol::MessageType::CREATE_POST_REQUEST:
        handleCreatePost(session, msg);
        break;
    case Protocol::MessageType::GET_POST_DETAIL_REQUEST:
        handleGetPostDetail(session, msg);
        break;
    case Protocol::MessageType::CREATE_REPLY_REQUEST:
        handleCreateReply(session, msg);
        break;

    // Appointment
    case Protocol::MessageType::GET_COUNSELORS_REQUEST:
        handleGetCounselors(session, msg);
        break;
    case Protocol::MessageType::CREATE_APPOINTMENT_REQUEST:
        handleCreateAppointment(session, msg);
        break;
    case Protocol::MessageType::GET_APPOINTMENTS_REQUEST:
        handleGetAppointments(session, msg);
        break;

    // Resources
    case Protocol::MessageType::GET_RESOURCES_REQUEST:
        handleGetResources(session, msg);
        break;

    // Admin
    case Protocol::MessageType::GET_DASHBOARD_STATS_REQUEST:
        handleGetDashboardStats(session, msg);
        break;
    case Protocol::MessageType::MANAGE_USER_REQUEST:
        handleManageUser(session, msg);
        break;
    case Protocol::MessageType::GET_SYSTEM_LOGS_REQUEST:
        handleGetSystemLogs(session, msg);
        break;

    // Counselor/Publisher (800-899)
    case Protocol::MessageType::PUBLISH_TASK_REQUEST:
        handlePublishTask(session, msg);
        break;
    case Protocol::MessageType::GET_MY_TASKS_REQUEST:
        handleGetMyTasks(session, msg);
        break;
    case Protocol::MessageType::GET_PENDING_TASKS_REQUEST:
        handleGetPendingTasks(session, msg);
        break;
    case Protocol::MessageType::REVIEW_TASK_REQUEST:
        handleReviewTask(session, msg);
        break;
    case Protocol::MessageType::GET_TASK_REPORT_REQUEST:
        handleGetTaskReport(session, msg);
        break;
    case Protocol::MessageType::GRANT_REPORT_ACCESS_REQUEST:
        handleGrantReportAccess(session, msg);
        break;

    // System
    case Protocol::MessageType::HEARTBEAT:
        handleHeartbeat(session, msg);
        break;

    default:
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::INVALID_REQUEST, "Unknown message type"));
        break;
    }
}

void PsychServer::onClientDisconnected(ClientSession* session) {
    qint64 userId = session->userId();
    if (userId > 0) {
        m_userSessions.remove(userId);
        LOG_INFO(QString("User %1 (%2) disconnected")
                     .arg(userId).arg(session->username()));
        logAction(userId, "logout");
    }

    m_sessions.remove(session->socket());
    session->deleteLater();
}

void PsychServer::onWsMessageReceived(const QString& message) {
    auto* ws = qobject_cast<QWebSocket*>(sender());
    if (!ws) return;

    QJsonDocument doc = QJsonDocument::fromJson(message.toUtf8());
    if (!doc.isObject()) return;

    QJsonObject obj = doc.object();
    QString type = obj["type"].toString();

    if (type == "login") {
        // Authenticate WebSocket connection
        qint64 userId = obj["userId"].toInteger();
        m_wsSessions[ws] = userId;
        LOG_INFO(QString("WebSocket authenticated for user %1").arg(userId));
    } else if (type == "message") {
        // Forward message via WebSocket
        qint64 toUserId = obj["to"].toInteger();
        QJsonObject fwdPayload;
        fwdPayload["type"] = "message";
        fwdPayload["from"] = m_wsSessions.value(ws);
        fwdPayload["to"] = toUserId;
        fwdPayload["content"] = obj["content"];
        fwdPayload["timestamp"] = QDateTime::currentMSecsSinceEpoch();

        QString fwdMsg = QString::fromUtf8(QJsonDocument(fwdPayload).toJson(QJsonDocument::Compact));
        broadcastToUser(toUserId, Message(Protocol::MessageType::RECEIVE_MESSAGE, 0, fwdPayload));
    }
}

void PsychServer::onWsDisconnected() {
    auto* ws = qobject_cast<QWebSocket*>(sender());
    if (ws) {
        m_wsSessions.remove(ws);
        ws->deleteLater();
    }
}

void PsychServer::cleanupExpiredSessions() {
    LOG_DEBUG(QString("Active sessions: %1, WS sessions: %2")
                  .arg(m_sessions.size()).arg(m_wsSessions.size()));
}

// ============================================================
// Utility Methods
// ============================================================

void PsychServer::sendMessage(ClientSession* session, const Message& msg) {
    if (session && session->socket() && session->socket()->isOpen()) {
        session->socket()->write(msg.serialize());
    }
}

void PsychServer::broadcastToUser(qint64 userId, const Message& msg) {
    // Send via TCP if connected
    if (m_userSessions.contains(userId)) {
        sendMessage(m_userSessions[userId], msg);
    }

    // Send via WebSocket if connected
    for (auto it = m_wsSessions.begin(); it != m_wsSessions.end(); ++it) {
        if (it.value() == userId && it.key()->isValid()) {
            QJsonObject payload = msg.payload();
            payload["msgType"] = static_cast<int>(msg.type());
            it.key()->sendTextMessage(
                QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact)));
        }
    }
}

void PsychServer::logAction(qint64 userId, const QString& action,
                             const QString& targetType, qint64 targetId,
                             const QJsonObject& details) {
    QVariantMap bindings;
    if (userId > 0) bindings["user_id"] = userId;
    bindings["action"] = action;
    if (!targetType.isEmpty()) bindings["target_type"] = targetType;
    if (targetId > 0) bindings["target_id"] = targetId;
    if (!details.isEmpty()) {
        bindings["details"] = QString::fromUtf8(
            QJsonDocument(details).toJson(QJsonDocument::Compact));
    }

    QString sql = "INSERT INTO system_logs (user_id, action, target_type, target_id, details) "
                  "VALUES (:user_id, :action, :target_type, :target_id, :details)";
    DbManager::instance().executeUpdate(sql, bindings);
}
