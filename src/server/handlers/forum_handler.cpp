#include "server/psych_server.h"
#include "database/db_manager.h"
#include "utils/logger.h"
#include <QJsonArray>
#include <QDateTime>

// ============================================================
// Get Posts List
// ============================================================

void PsychServer::handleGetPosts(ClientSession* session, const Message& msg) {
    if (!session->isAuthenticated()) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::AUTH_FAILED, "请先登录"));
        return;
    }

    QJsonObject payload = msg.payload();
    QString category = payload["category"].toString();
    int page = payload.value("page").toInt(1);
    int pageSize = payload.value("pageSize").toInt(20);
    int offset = (page - 1) * pageSize;

    QString sql = "SELECT p.id, p.author_id, p.title, p.content, p.category, "
                  "p.view_count, p.like_count, p.reply_count, p.is_pinned, p.created_at, "
                  "u.nickname as author_name, u.avatar as author_avatar "
                  "FROM forum_posts p "
                  "JOIN users u ON p.author_id = u.id "
                  "WHERE p.status = 'active'";
    QVariantMap bindings;

    if (!category.isEmpty()) {
        sql += " AND p.category = :category";
        bindings["category"] = category;
    }

    sql += " ORDER BY p.is_pinned DESC, p.created_at DESC LIMIT :limit OFFSET :offset";
    bindings["limit"] = pageSize;
    bindings["offset"] = offset;

    auto results = DbManager::instance().executeQuery(sql, bindings);

    QJsonArray postsArray;
    for (const auto& row : results) {
        QJsonObject post;
        post["id"] = row["id"].toLongLong();
        post["authorId"] = row["author_id"].toLongLong();
        post["title"] = row["title"].toString();
        post["content"] = row["content"].toString();
        post["category"] = row["category"].toString();
        post["authorName"] = row["author_name"].toString();
        post["authorAvatar"] = row["author_avatar"].toString();
        post["viewCount"] = row["view_count"].toInt();
        post["likeCount"] = row["like_count"].toInt();
        post["replyCount"] = row["reply_count"].toInt();
        post["isPinned"] = row["is_pinned"].toBool();
        post["isLiked"] = false;
        post["createdAt"] = row["created_at"].toDateTime().toString(Qt::ISODate);
        postsArray.append(post);
    }

    QJsonObject responseData;
    responseData["posts"] = postsArray;
    responseData["page"] = page;

    sendMessage(session, Message::success(
        Protocol::MessageType::GET_POSTS_RESPONSE, msg.seq(), responseData));
}

// ============================================================
// Create Post
// ============================================================

void PsychServer::handleCreatePost(ClientSession* session, const Message& msg) {
    if (!session->isAuthenticated()) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::AUTH_FAILED, "请先登录"));
        return;
    }

    QJsonObject payload = msg.payload();
    QString title = payload["title"].toString().trimmed();
    QString content = payload["content"].toString().trimmed();
    QString category = payload.value("category").toString("general");

    if (title.isEmpty() || content.isEmpty()) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::INVALID_REQUEST, "标题和内容不能为空"));
        return;
    }

    int rows = DbManager::instance().executeUpdate(
        "INSERT INTO forum_posts (author_id, title, content, category) "
        "VALUES (:author_id, :title, :content, :category)",
        {
            {"author_id", session->userId()},
            {"title", title},
            {"content", content},
            {"category", category}
        });

    qint64 postId = DbManager::instance().lastInsertId();

    QJsonObject responseData;
    responseData["postId"] = postId;

    sendMessage(session, Message::success(
        Protocol::MessageType::CREATE_POST_RESPONSE, msg.seq(), responseData));

    logAction(session->userId(), "create_post", "post", postId);
}

// ============================================================
// Get Post Detail
// ============================================================

void PsychServer::handleGetPostDetail(ClientSession* session, const Message& msg) {
    if (!session->isAuthenticated()) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::AUTH_FAILED, "请先登录"));
        return;
    }

    qint64 postId = msg.payload()["postId"].toInteger();
    if (postId <= 0) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::INVALID_REQUEST, "无效的帖子ID"));
        return;
    }

    // Increment view count
    DbManager::instance().executeUpdate(
        "UPDATE forum_posts SET view_count = view_count + 1 WHERE id = :id",
        {{"id", postId}});

    // Get post
    auto postResults = DbManager::instance().executeQuery(
        "SELECT p.*, u.nickname as author_name, u.avatar as author_avatar "
        "FROM forum_posts p JOIN users u ON p.author_id = u.id "
        "WHERE p.id = :id AND p.status = 'active'",
        {{"id", postId}});

    if (postResults.isEmpty()) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::USER_NOT_FOUND, "帖子不存在"));
        return;
    }

    auto row = postResults.first();
    QJsonObject postObj;
    postObj["id"] = row["id"].toLongLong();
    postObj["title"] = row["title"].toString();
    postObj["content"] = row["content"].toString();
    postObj["category"] = row["category"].toString();
    postObj["authorId"] = row["author_id"].toLongLong();
    postObj["authorName"] = row["author_name"].toString();
    postObj["authorAvatar"] = row["author_avatar"].toString();
    postObj["viewCount"] = row["view_count"].toInt();
    postObj["likeCount"] = row["like_count"].toInt();
    postObj["replyCount"] = row["reply_count"].toInt();
    postObj["isLiked"] = false;
    postObj["createdAt"] = row["created_at"].toDateTime().toString(Qt::ISODate);

    // Get replies
    auto replyResults = DbManager::instance().executeQuery(
        "SELECT r.id, r.author_id, r.content, r.parent_reply_id, r.like_count, r.created_at, "
        "u.nickname as author_name, u.avatar as author_avatar "
        "FROM forum_replies r "
        "JOIN users u ON r.author_id = u.id "
        "WHERE r.post_id = :post_id AND r.status = 'active' "
        "ORDER BY r.created_at ASC",
        {{"post_id", postId}});

    QJsonArray repliesArray;
    for (const auto& rRow : replyResults) {
        QJsonObject reply;
        reply["id"] = rRow["id"].toLongLong();
        reply["authorId"] = rRow["author_id"].toLongLong();
        reply["authorName"] = rRow["author_name"].toString();
        reply["authorAvatar"] = rRow["author_avatar"].toString();
        reply["content"] = rRow["content"].toString();
        reply["parentReplyId"] = rRow["parent_reply_id"].isNull() ? -1 : rRow["parent_reply_id"].toLongLong();
        reply["likeCount"] = rRow["like_count"].toInt();
        reply["isLiked"] = false;
        reply["createdAt"] = rRow["created_at"].toDateTime().toString(Qt::ISODate);
        repliesArray.append(reply);
    }

    QJsonObject responseData;
    responseData["post"] = postObj;
    responseData["replies"] = repliesArray;

    sendMessage(session, Message::success(
        Protocol::MessageType::GET_POST_DETAIL_RESPONSE, msg.seq(), responseData));
}

// ============================================================
// Create Reply
// ============================================================

void PsychServer::handleCreateReply(ClientSession* session, const Message& msg) {
    if (!session->isAuthenticated()) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::AUTH_FAILED, "请先登录"));
        return;
    }

    QJsonObject payload = msg.payload();
    qint64 postId = payload["postId"].toInteger();
    QString content = payload["content"].toString().trimmed();
    qint64 parentReplyId = payload.value("parentReplyId").toInteger(-1);

    if (postId <= 0 || content.isEmpty()) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::INVALID_REQUEST, "无效的帖子ID或内容为空"));
        return;
    }

    QVariantMap bindings = {
        {"post_id", postId},
        {"author_id", session->userId()},
        {"content", content}
    };

    QString sql;
    if (parentReplyId > 0) {
        sql = "INSERT INTO forum_replies (post_id, author_id, content, parent_reply_id) "
              "VALUES (:post_id, :author_id, :content, :parent_reply_id)";
        bindings["parent_reply_id"] = parentReplyId;
    } else {
        sql = "INSERT INTO forum_replies (post_id, author_id, content) "
              "VALUES (:post_id, :author_id, :content)";
    }

    DbManager::instance().executeUpdate(sql, bindings);
    qint64 replyId = DbManager::instance().lastInsertId();

    // Update reply count
    DbManager::instance().executeUpdate(
        "UPDATE forum_posts SET reply_count = reply_count + 1 WHERE id = :id",
        {{"id", postId}});

    QJsonObject responseData;
    responseData["replyId"] = replyId;

    sendMessage(session, Message::success(
        Protocol::MessageType::CREATE_REPLY_RESPONSE, msg.seq(), responseData));

    logAction(session->userId(), "create_reply", "reply", replyId, {{"postId", postId}});
}

// ============================================================
// Like Post (408)
// ============================================================

void PsychServer::handleLikePost(ClientSession* session, const Message& msg) {
    if (!session->isAuthenticated()) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::AUTH_FAILED, "请先登录"));
        return;
    }

    qint64 postId = msg.payload()["postId"].toInteger();
    if (postId <= 0) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::INVALID_REQUEST, "无效的帖子ID"));
        return;
    }

    // Increment like count
    DbManager::instance().executeUpdate(
        "UPDATE forum_posts SET like_count = like_count + 1 WHERE id = :id",
        {{"id", postId}});

    // Get updated count
    auto results = DbManager::instance().executeQuery(
        "SELECT like_count FROM forum_posts WHERE id = :id", {{"id", postId}});
    int likeCount = results.isEmpty() ? 0 : results[0]["like_count"].toInt();

    QJsonObject data;
    data["likeCount"] = likeCount;

    sendMessage(session, Message::success(
        Protocol::MessageType::LIKE_POST_RESPONSE, msg.seq(), data));

    logAction(session->userId(), "like_post", "post", postId);
    LOG_INFO(QString("User %1 liked post %2 (count=%3)")
                 .arg(session->userId()).arg(postId).arg(likeCount));
}
