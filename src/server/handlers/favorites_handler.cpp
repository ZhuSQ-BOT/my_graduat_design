#include "server/psych_server.h"
#include "database/db_manager.h"
#include "utils/logger.h"
#include <QJsonArray>

// ============================================================
// Get Favorites (604)
// Returns the IDs of resources the current user has bookmarked
// ============================================================

void PsychServer::handleGetFavorites(ClientSession* session, const Message& msg) {
    if (!session->isAuthenticated()) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::AUTH_FAILED, "请先登录"));
        return;
    }

    auto results = DbManager::instance().executeQuery(
        "SELECT resource_id FROM user_favorites WHERE user_id = :userId",
        {{"userId", session->userId()}});

    QJsonArray favIds;
    for (const auto& row : results) {
        favIds.append(row["resource_id"].toLongLong());
    }

    QJsonObject data;
    data["favoriteIds"] = favIds;

    sendMessage(session, Message::success(
        Protocol::MessageType::GET_FAVORITES_RESPONSE, msg.seq(), data));
}

// ============================================================
// Add Favorite (606)
// ============================================================

void PsychServer::handleAddFavorite(ClientSession* session, const Message& msg) {
    if (!session->isAuthenticated()) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::AUTH_FAILED, "请先登录"));
        return;
    }

    qint64 resourceId = msg.payload()["resourceId"].toInteger();
    if (resourceId <= 0) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::INVALID_REQUEST, "无效的资源ID"));
        return;
    }

    // INSERT IGNORE to handle duplicate safely
    DbManager::instance().executeUpdate(
        "INSERT IGNORE INTO user_favorites (user_id, resource_id) VALUES (:userId, :resourceId)",
        {{"userId", session->userId()}, {"resourceId", resourceId}});

    sendMessage(session, Message::success(
        Protocol::MessageType::ADD_FAVORITE_RESPONSE, msg.seq(), {}));

    logAction(session->userId(), "add_favorite", "resource", resourceId);
}

// ============================================================
// Remove Favorite (608)
// ============================================================

void PsychServer::handleRemoveFavorite(ClientSession* session, const Message& msg) {
    if (!session->isAuthenticated()) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::AUTH_FAILED, "请先登录"));
        return;
    }

    qint64 resourceId = msg.payload()["resourceId"].toInteger();
    if (resourceId <= 0) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::INVALID_REQUEST, "无效的资源ID"));
        return;
    }

    DbManager::instance().executeUpdate(
        "DELETE FROM user_favorites WHERE user_id = :userId AND resource_id = :resourceId",
        {{"userId", session->userId()}, {"resourceId", resourceId}});

    sendMessage(session, Message::success(
        Protocol::MessageType::REMOVE_FAVORITE_RESPONSE, msg.seq(), {}));

    logAction(session->userId(), "remove_favorite", "resource", resourceId);
}
