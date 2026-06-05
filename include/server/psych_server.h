#ifndef PSYCH_SERVER_H
#define PSYCH_SERVER_H

#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QWebSocketServer>
#include <QWebSocket>
#include <QHash>
#include <QTimer>
#include <QJsonObject>
#include "message.h"

/**
 * @brief TCP 客户端会话
 *
 * 封装单个 TCP 连接的状态，包括缓冲区、用户ID、心跳等。
 */
class ClientSession : public QObject {
    Q_OBJECT
public:
    explicit ClientSession(QTcpSocket* socket, QObject* parent = nullptr);

    QTcpSocket* socket() const { return m_socket; }
    qint64 userId() const { return m_userId; }
    void setUserId(qint64 id) { m_userId = id; }
    QString username() const { return m_username; }
    void setUsername(const QString& name) { m_username = name; }
    QString token() const { return m_token; }
    void setToken(const QString& token) { m_token = token; }
    QString role() const { return m_role; }
    void setRole(const QString& role) { m_role = role; }
    bool isAuthenticated() const { return m_userId > 0; }
    void resetHeartbeat();

signals:
    void messageReceived(ClientSession* session, const Message& msg);
    void disconnected(ClientSession* session);

private slots:
    void onReadyRead();
    void onDisconnected();

private:
    void processBuffer();

    QTcpSocket* m_socket;
    QByteArray m_buffer;
    qint64 m_userId = -1;
    QString m_username;
    QString m_token;
    QString m_role;
    QTimer* m_heartbeatTimer;
};

/**
 * @brief 主服务器类
 *
 * 管理 TCP 和 WebSocket 连接，路由消息到对应的业务处理逻辑。
 */
class PsychServer : public QObject {
    Q_OBJECT
public:
    explicit PsychServer(QObject* parent = nullptr);
    ~PsychServer() override;

    bool start();
    void stop();

    int activeConnections() const { return m_sessions.size(); }

    void sendMessage(ClientSession* session, const Message& msg);
    void broadcastToUser(qint64 userId, const Message& msg);
    void broadcastOnlineStatus(qint64 userId, bool online);

private slots:
    void onNewTcpConnection();
    void onNewWsConnection();
    void onClientMessage(ClientSession* session, const Message& msg);
    void onClientDisconnected(ClientSession* session);
    void onWsMessageReceived(const QString& message);
    void onWsDisconnected();
    void cleanupExpiredSessions();

private:
    // Message handlers (will be implemented per module)
    void handleLogin(ClientSession* session, const Message& msg);
    void handleRegister(ClientSession* session, const Message& msg);
    void handleValidateToken(ClientSession* session, const Message& msg);
    void handleLogout(ClientSession* session, const Message& msg);
    void handleGetProfile(ClientSession* session, const Message& msg);
    void handleUpdateProfile(ClientSession* session, const Message& msg);
    void handleChangePassword(ClientSession* session, const Message& msg);
    void handleGetUserStats(ClientSession* session, const Message& msg);
    void handleHeartbeat(ClientSession* session, const Message& msg);
    void handleGetScales(ClientSession* session, const Message& msg);
    void handleGetScaleDetail(ClientSession* session, const Message& msg);
    void handleSubmitAssessment(ClientSession* session, const Message& msg);
    void handleGetAssessmentHistory(ClientSession* session, const Message& msg);
    void handleGetAssessmentStats(ClientSession* session, const Message& msg);
    void handleCreateScale(ClientSession* session, const Message& msg);
    void handleUpdateScale(ClientSession* session, const Message& msg);
    void handleDeleteScale(ClientSession* session, const Message& msg);
    void handleImportScale(ClientSession* session, const Message& msg);
    void handleSendMessage(ClientSession* session, const Message& msg);
    void handleGetContacts(ClientSession* session, const Message& msg);
    void handleGetMessageHistory(ClientSession* session, const Message& msg);
    void handleAiChat(ClientSession* session, const Message& msg);
    void handleAddContact(ClientSession* session, const Message& msg);
    void handleRemoveContact(ClientSession* session, const Message& msg);
    void handleSearchUsers(ClientSession* session, const Message& msg);
    void handleGetPendingRequests(ClientSession* session, const Message& msg);
    void handleAcceptContact(ClientSession* session, const Message& msg);
    void handleRejectContact(ClientSession* session, const Message& msg);
    void handleGetPosts(ClientSession* session, const Message& msg);
    void handleCreatePost(ClientSession* session, const Message& msg);
    void handleGetPostDetail(ClientSession* session, const Message& msg);
    void handleCreateReply(ClientSession* session, const Message& msg);
    void handleLikePost(ClientSession* session, const Message& msg);
    void handleGetCounselors(ClientSession* session, const Message& msg);
    void handleCreateAppointment(ClientSession* session, const Message& msg);
    void handleGetAppointments(ClientSession* session, const Message& msg);
    void handleGetResources(ClientSession* session, const Message& msg);
    void handleGetFavorites(ClientSession* session, const Message& msg);
    void handleAddFavorite(ClientSession* session, const Message& msg);
    void handleRemoveFavorite(ClientSession* session, const Message& msg);
    void handleGetDashboardStats(ClientSession* session, const Message& msg);
    void handleManageUser(ClientSession* session, const Message& msg);
    void handleGetSystemLogs(ClientSession* session, const Message& msg);
    void handleGetReports(ClientSession* session, const Message& msg);
    void handleResolveReport(ClientSession* session, const Message& msg);
    void handleDismissReport(ClientSession* session, const Message& msg);

    // Counselor/Publisher handlers (800-899)
    void handlePublishTask(ClientSession* session, const Message& msg);
    void handleGetMyTasks(ClientSession* session, const Message& msg);
    void handleGetPendingTasks(ClientSession* session, const Message& msg);
    void handleReviewTask(ClientSession* session, const Message& msg);
    void handleGetTaskReport(ClientSession* session, const Message& msg);
    void handleGrantReportAccess(ClientSession* session, const Message& msg);

    // TODO: 将 setInstance() 实现移到 cpp 文件
    // void setInstance() { g_serverInstance = this; }
    void logAction(qint64 userId, const QString& action,
                   const QString& targetType = {}, qint64 targetId = 0,
                   const QJsonObject& details = {});

    QTcpServer* m_tcpServer = nullptr;
    QWebSocketServer* m_wsServer = nullptr;
    QHash<QTcpSocket*, ClientSession*> m_sessions;
    QHash<qint64, ClientSession*> m_userSessions; // userId -> session
    QHash<QWebSocket*, qint64> m_wsSessions;      // ws -> userId
    QTimer* m_cleanupTimer;
};

#endif // PSYCH_SERVER_H
