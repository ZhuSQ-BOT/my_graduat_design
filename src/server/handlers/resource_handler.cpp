#include "server/psych_server.h"
#include "database/db_manager.h"
#include "utils/logger.h"
#include <QJsonArray>

// ============================================================
// Get Resources
// ============================================================

void PsychServer::handleGetResources(ClientSession* session, const Message& msg) {
    if (!session->isAuthenticated()) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::AUTH_FAILED, "请先登录"));
        return;
    }

    QJsonObject payload = msg.payload();
    QString resourceType = payload["type"].toString();
    QString category = payload["category"].toString();
    int page = payload.value("page").toInt(1);
    int pageSize = payload.value("pageSize").toInt(20);
    int offset = (page - 1) * pageSize;

    QString sql = "SELECT id, title, description, resource_type, url, cover_image, "
                  "category, view_count, like_count, duration_seconds, created_at "
                  "FROM resources WHERE is_active = 1";
    QVariantMap bindings;

    if (!resourceType.isEmpty()) {
        sql += " AND resource_type = :type";
        bindings["type"] = resourceType;
    }
    if (!category.isEmpty()) {
        sql += " AND category = :category";
        bindings["category"] = category;
    }

    sql += " ORDER BY created_at DESC LIMIT :limit OFFSET :offset";
    bindings["limit"] = pageSize;
    bindings["offset"] = offset;

    auto results = DbManager::instance().executeQuery(sql, bindings);

    QJsonArray resourcesArray;
    for (const auto& row : results) {
        QJsonObject r;
        r["id"] = row["id"].toLongLong();
        r["title"] = row["title"].toString();
        r["description"] = row["description"].toString();
        r["resourceType"] = row["resource_type"].toString();
        r["url"] = row["url"].toString();
        r["coverImage"] = row["cover_image"].toString();
        r["category"] = row["category"].toString();
        r["viewCount"] = row["view_count"].toInt();
        r["likeCount"] = row["like_count"].toInt();
        r["durationSeconds"] = row["duration_seconds"].isNull() ? -1 : row["duration_seconds"].toInt();
        r["createdAt"] = row["created_at"].toDateTime().toString(Qt::ISODate);
        resourcesArray.append(r);
    }

    QJsonObject responseData;
    responseData["resources"] = resourcesArray;
    responseData["page"] = page;

    sendMessage(session, Message::success(
        Protocol::MessageType::GET_RESOURCES_RESPONSE, msg.seq(), responseData));
}
