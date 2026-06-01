#include "server/psych_server.h"
#include "database/db_manager.h"
#include "utils/logger.h"
#include <QJsonArray>

// ============================================================
// Create Scale (210)
// ============================================================

void PsychServer::handleCreateScale(ClientSession* session, const Message& msg) {
    if (!session->isAuthenticated()) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::AUTH_FAILED, "请先登录"));
        return;
    }
    if (session->role() != "admin") {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::PERMISSION_DENIED, "权限不足"));
        return;
    }

    QJsonObject payload = msg.payload();
    QString name = payload["name"].toString().trimmed();
    QString description = payload["description"].toString().trimmed();
    QString category = payload["category"].toString().trimmed();
    QString scoringMethod = payload["scoringMethod"].toString().trimmed();
    int totalQuestions = payload["totalQuestions"].toInt();

    if (name.isEmpty()) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::INVALID_REQUEST, "量表名称不能为空"));
        return;
    }

    // Check for duplicate name
    auto existing = DbManager::instance().executeQuery(
        "SELECT id FROM scales WHERE name = :name", {{"name", name}});
    if (!existing.isEmpty()) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::INVALID_REQUEST, "量表名称已存在"));
        return;
    }

    QVariantMap bindings;
    bindings["name"] = name;
    bindings["description"] = description;
    bindings["category"] = category.isEmpty() ? "general" : category;
    bindings["totalQuestions"] = totalQuestions > 0 ? totalQuestions : 0;
    bindings["scoringMethod"] = scoringMethod.isEmpty() ? "sum" : scoringMethod;

    bool ok = DbManager::instance().executeUpdate(
        "INSERT INTO scales (name, description, category, total_questions, scoring_method) "
        "VALUES (:name, :description, :category, :totalQuestions, :scoringMethod)",
        bindings);

    if (!ok) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::DATABASE_ERROR, "创建量表失败"));
        return;
    }

    // Get the new scale ID
    auto newId = DbManager::instance().executeQuery("SELECT LAST_INSERT_ID() AS id", {});
    int scaleId = newId.isEmpty() ? 0 : newId[0]["id"].toInt();

    QJsonObject data;
    data["scaleId"] = scaleId;

    sendMessage(session, Message::success(
        Protocol::MessageType::CREATE_SCALE_RESPONSE, msg.seq(), data));

    logAction(session->userId(), "create_scale", "scale", scaleId);
    LOG_INFO(QString("Scale '%1' (id=%2) created by admin %3").arg(name).arg(scaleId).arg(session->userId()));
}

// ============================================================
// Update Scale (212)
// ============================================================

void PsychServer::handleUpdateScale(ClientSession* session, const Message& msg) {
    if (!session->isAuthenticated()) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::AUTH_FAILED, "请先登录"));
        return;
    }
    if (session->role() != "admin") {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::PERMISSION_DENIED, "权限不足"));
        return;
    }

    QJsonObject payload = msg.payload();
    int scaleId = payload["scaleId"].toInt();
    QString name = payload["name"].toString().trimmed();
    QString description = payload["description"].toString().trimmed();
    QString category = payload["category"].toString().trimmed();
    QString scoringMethod = payload["scoringMethod"].toString().trimmed();

    if (scaleId <= 0 || name.isEmpty()) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::INVALID_REQUEST, "参数无效"));
        return;
    }

    QVariantMap bindings;
    bindings["scaleId"] = scaleId;
    bindings["name"] = name;
    bindings["description"] = description;
    bindings["category"] = category;
    bindings["scoringMethod"] = scoringMethod;

    bool ok = DbManager::instance().executeUpdate(
        "UPDATE scales SET name = :name, description = :description, "
        "category = :category, scoring_method = :scoringMethod "
        "WHERE id = :scaleId",
        bindings);

    if (!ok) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::DATABASE_ERROR, "更新量表失败"));
        return;
    }

    sendMessage(session, Message::success(
        Protocol::MessageType::UPDATE_SCALE_RESPONSE, msg.seq(), {}));

    logAction(session->userId(), "update_scale", "scale", scaleId);
    LOG_INFO(QString("Scale %1 updated by admin %2").arg(scaleId).arg(session->userId()));
}

// ============================================================
// Delete Scale (214)
// ============================================================

void PsychServer::handleDeleteScale(ClientSession* session, const Message& msg) {
    if (!session->isAuthenticated()) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::AUTH_FAILED, "请先登录"));
        return;
    }
    if (session->role() != "admin") {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::PERMISSION_DENIED, "权限不足"));
        return;
    }

    QJsonObject payload = msg.payload();
    int scaleId = payload["scaleId"].toInt();

    if (scaleId <= 0) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::INVALID_REQUEST, "参数无效"));
        return;
    }

    // Soft delete: set is_active = 0
    bool ok = DbManager::instance().executeUpdate(
        "UPDATE scales SET is_active = 0 WHERE id = :scaleId",
        {{"scaleId", scaleId}});

    if (!ok) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::DATABASE_ERROR, "删除量表失败"));
        return;
    }

    sendMessage(session, Message::success(
        Protocol::MessageType::DELETE_SCALE_RESPONSE, msg.seq(), {}));

    logAction(session->userId(), "delete_scale", "scale", scaleId);
    LOG_INFO(QString("Scale %1 deleted by admin %2").arg(scaleId).arg(session->userId()));
}

// ============================================================
// Import Scale (216) - include scale metadata + questions
// ============================================================

void PsychServer::handleImportScale(ClientSession* session, const Message& msg) {
    if (!session->isAuthenticated()) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::AUTH_FAILED, "请先登录"));
        return;
    }
    if (session->role() != "admin") {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::PERMISSION_DENIED, "权限不足"));
        return;
    }

    QJsonObject payload = msg.payload();
    QString name = payload["name"].toString().trimmed();
    QString description = payload["description"].toString().trimmed();
    QString category = payload["category"].toString().trimmed();
    QString scoringMethod = payload["scoringMethod"].toString().trimmed();
    QJsonArray questions = payload["questions"].toArray();

    if (name.isEmpty() || questions.isEmpty()) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::INVALID_REQUEST, "量表名称和题目不能为空"));
        return;
    }

    // Insert scale
    QVariantMap scaleBindings;
    scaleBindings["name"] = name;
    scaleBindings["description"] = description;
    scaleBindings["category"] = category.isEmpty() ? "general" : category;
    scaleBindings["totalQuestions"] = questions.size();
    scaleBindings["scoringMethod"] = scoringMethod.isEmpty() ? "sum" : scoringMethod;

    bool ok = DbManager::instance().executeUpdate(
        "INSERT INTO scales (name, description, category, total_questions, scoring_method) "
        "VALUES (:name, :description, :category, :totalQuestions, :scoringMethod)",
        scaleBindings);

    if (!ok) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::DATABASE_ERROR, "创建量表失败"));
        return;
    }

    // Get the new scale ID
    auto newId = DbManager::instance().executeQuery("SELECT LAST_INSERT_ID() AS id", {});
    int scaleId = newId.isEmpty() ? 0 : newId[0]["id"].toInt();

    // Insert questions
    int questionNumber = 1;
    for (const auto& qVal : questions) {
        QJsonObject qObj = qVal.toObject();
        QString content = qObj["content"].toString().trimmed();
        QJsonArray options = qObj["options"].toArray();
        QJsonObject scoringRules = qObj["scoringRules"].toObject();

        if (content.isEmpty()) {
            questionNumber++;
            continue;
        }

        QVariantMap qBindings;
        qBindings["scaleId"] = scaleId;
        qBindings["questionNumber"] = questionNumber;
        qBindings["content"] = content;
        qBindings["options"] = QString::fromUtf8(QJsonDocument(options).toJson(QJsonDocument::Compact));
        qBindings["scoringRules"] = QString::fromUtf8(QJsonDocument(scoringRules).toJson(QJsonDocument::Compact));
        qBindings["reverseScored"] = qObj["reverseScored"].toBool() ? 1 : 0;

        DbManager::instance().executeUpdate(
            "INSERT INTO scale_questions (scale_id, question_number, content, options, scoring_rules, reverse_scored) "
            "VALUES (:scaleId, :questionNumber, :content, :options, :scoringRules, :reverseScored)",
            qBindings);

        questionNumber++;
    }

    QJsonObject data;
    data["scaleId"] = scaleId;
    data["questionsCount"] = questionNumber - 1;

    sendMessage(session, Message::success(
        Protocol::MessageType::IMPORT_SCALE_RESPONSE, msg.seq(), data));

    logAction(session->userId(), "import_scale", "scale", scaleId);
    LOG_INFO(QString("Scale '%1' (id=%2) with %3 questions imported by admin %4")
                 .arg(name).arg(scaleId).arg(questionNumber - 1).arg(session->userId()));
}
