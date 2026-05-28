#include "server/psych_server.h"
#include "database/db_manager.h"
#include "utils/logger.h"
#include <QJsonArray>
#include <QDateTime>

// ============================================================
// Get Scales List
// ============================================================

void PsychServer::handleGetScales(ClientSession* session, const Message& msg) {
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

    QString sql = "SELECT id, name, name_en, description, category, total_questions, "
                  "difficulty, time_limit_minutes FROM scales WHERE is_active = 1";
    QVariantMap bindings;

    if (!category.isEmpty()) {
        sql += " AND category = :category";
        bindings["category"] = category;
    }

    sql += " ORDER BY category, id LIMIT :limit OFFSET :offset";
    bindings["limit"] = pageSize;
    bindings["offset"] = offset;

    auto results = DbManager::instance().executeQuery(sql, bindings);

    QJsonArray scalesArray;
    for (const auto& row : results) {
        QJsonObject scale;
        scale["id"] = row["id"].toInt();
        scale["name"] = row["name"].toString();
        scale["nameEn"] = row["name_en"].toString();
        scale["description"] = row["description"].toString();
        scale["category"] = row["category"].toString();
        scale["totalQuestions"] = row["total_questions"].toInt();
        scale["difficulty"] = row["difficulty"].toInt();
        scale["timeLimitMinutes"] = row["time_limit_minutes"].isNull() ? -1 : row["time_limit_minutes"].toInt();
        scalesArray.append(scale);
    }

    // Get total count
    QString countSql = "SELECT COUNT(*) as cnt FROM scales WHERE is_active = 1";
    QVariantMap countBindings;
    if (!category.isEmpty()) {
        countSql += " AND category = :category";
        countBindings["category"] = category;
    }
    auto countResult = DbManager::instance().executeQuery(countSql, countBindings);
    int total = countResult.isEmpty() ? 0 : countResult.first()["cnt"].toInt();

    QJsonObject responseData;
    responseData["scales"] = scalesArray;
    responseData["total"] = total;
    responseData["page"] = page;
    responseData["pageSize"] = pageSize;

    sendMessage(session, Message::success(
        Protocol::MessageType::GET_SCALES_RESPONSE, msg.seq(), responseData));
}

// ============================================================
// Get Scale Detail (with questions)
// ============================================================

void PsychServer::handleGetScaleDetail(ClientSession* session, const Message& msg) {
    if (!session->isAuthenticated()) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::AUTH_FAILED, "请先登录"));
        return;
    }

    int scaleId = msg.payload()["scaleId"].toInt();
    if (scaleId <= 0) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::INVALID_REQUEST, "无效的量表ID"));
        return;
    }

    // Get scale info
    auto scaleResults = DbManager::instance().executeQuery(
        "SELECT id, name, name_en, description, category, total_questions, "
        "scoring_method, time_limit_minutes FROM scales WHERE id = :id AND is_active = 1",
        {{"id", scaleId}});

    if (scaleResults.isEmpty()) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::USER_NOT_FOUND, "量表不存在"));
        return;
    }

    auto scaleRow = scaleResults.first();
    QJsonObject scaleObj;
    scaleObj["id"] = scaleRow["id"].toInt();
    scaleObj["name"] = scaleRow["name"].toString();
    scaleObj["nameEn"] = scaleRow["name_en"].toString();
    scaleObj["description"] = scaleRow["description"].toString();
    scaleObj["category"] = scaleRow["category"].toString();
    scaleObj["totalQuestions"] = scaleRow["total_questions"].toInt();
    scaleObj["scoringMethod"] = scaleRow["scoring_method"].toString();
    scaleObj["timeLimitMinutes"] = scaleRow["time_limit_minutes"].isNull()
                                     ? -1 : scaleRow["time_limit_minutes"].toInt();

    // Get questions
    auto questionResults = DbManager::instance().executeQuery(
        "SELECT id, question_number, content, options, reverse_scored "
        "FROM scale_questions WHERE scale_id = :scale_id ORDER BY question_number",
        {{"scale_id", scaleId}});

    QJsonArray questionsArray;
    for (const auto& qRow : questionResults) {
        QJsonObject question;
        question["id"] = qRow["id"].toLongLong();
        question["questionNumber"] = qRow["question_number"].toInt();
        question["content"] = qRow["content"].toString();
        question["options"] = QJsonDocument::fromJson(
            qRow["options"].toString().toUtf8()).array();
        question["reverseScored"] = qRow["reverse_scored"].toBool();
        questionsArray.append(question);
    }

    scaleObj["questions"] = questionsArray;

    QJsonObject responseData;
    responseData["scale"] = scaleObj;

    sendMessage(session, Message::success(
        Protocol::MessageType::GET_SCALE_DETAIL_RESPONSE, msg.seq(), responseData));
}

// ============================================================
// Submit Assessment
// ============================================================

void PsychServer::handleSubmitAssessment(ClientSession* session, const Message& msg) {
    if (!session->isAuthenticated()) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::AUTH_FAILED, "请先登录"));
        return;
    }

    QJsonObject payload = msg.payload();
    int scaleId = payload["scaleId"].toInt();
    QJsonArray answers = payload["answers"].toArray();
    qint64 startedAt = payload["startedAt"].toInteger();
    qint64 completedAt = payload["completedAt"].toInteger();

    if (scaleId <= 0 || answers.isEmpty()) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::INVALID_REQUEST, "无效的量表ID或答案"));
        return;
    }

    // Get questions for scoring
    auto questions = DbManager::instance().executeQuery(
        "SELECT id, question_number, scoring_rules, reverse_scored "
        "FROM scale_questions WHERE scale_id = :scale_id ORDER BY question_number",
        {{"scale_id", scaleId}});

    // Calculate score
    double totalScore = 0;
    QJsonObject factorScores;
    QString answersJson = QString::fromUtf8(
        QJsonDocument(answers).toJson(QJsonDocument::Compact));

    for (int i = 0; i < answers.size() && i < questions.size(); ++i) {
        int value = answers[i].toObject()["value"].toInt();

        // Handle reverse scoring
        if (questions[i]["reverse_scored"].toBool()) {
            // Assume 0-3 scale, reverse: 3->0, 2->1, 1->2, 0->3
            value = 3 - value;
        }

        totalScore += value;
    }

    // Determine result level (based on PHQ-9 / GAD-7 standard thresholds)
    QString resultLevel;
    QString interpretation;

    if (totalScore <= 4) {
        resultLevel = "normal";
        interpretation = "无抑郁/焦虑症状";
    } else if (totalScore <= 9) {
        resultLevel = "mild";
        interpretation = "轻度症状，建议关注自身状态";
    } else if (totalScore <= 14) {
        resultLevel = "moderate";
        interpretation = "中度症状，建议寻求专业帮助";
    } else if (totalScore <= 19) {
        resultLevel = "mod_severe";
        interpretation = "中重度症状，强烈建议寻求专业帮助";
    } else {
        resultLevel = "severe";
        interpretation = "重度症状，请尽快寻求专业治疗";
    }

    int durationSeconds = static_cast<int>((completedAt - startedAt) / 1000);

    // Save record
    int rows = DbManager::instance().executeUpdate(
        "INSERT INTO assessment_records "
        "(user_id, scale_id, total_score, factor_scores, result_level, "
        "result_interpretation, answers, started_at, completed_at, duration_seconds) "
        "VALUES (:user_id, :scale_id, :total_score, :factor_scores, :result_level, "
        ":result_interpretation, :answers, "
        "FROM_UNIXTIME(:started_at/1000), FROM_UNIXTIME(:completed_at/1000), :duration)",
        {
            {"user_id", session->userId()},
            {"scale_id", scaleId},
            {"total_score", totalScore},
            {"factor_scores", "{}"},
            {"result_level", resultLevel},
            {"result_interpretation", interpretation},
            {"answers", answersJson},
            {"started_at", startedAt},
            {"completed_at", completedAt},
            {"duration", durationSeconds}
        });

    qint64 recordId = DbManager::instance().lastInsertId();

    QJsonObject responseData;
    responseData["recordId"] = recordId;
    responseData["totalScore"] = totalScore;
    responseData["resultLevel"] = resultLevel;
    responseData["interpretation"] = interpretation;
    responseData["durationSeconds"] = durationSeconds;

    sendMessage(session, Message::success(
        Protocol::MessageType::SUBMIT_ASSESSMENT_RESPONSE, msg.seq(), responseData));

    logAction(session->userId(), "submit_assessment", "assessment", recordId,
              {{"scaleId", scaleId}, {"score", totalScore}});
}

// ============================================================
// Get Assessment History
// ============================================================

void PsychServer::handleGetAssessmentHistory(ClientSession* session, const Message& msg) {
    if (!session->isAuthenticated()) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::AUTH_FAILED, "请先登录"));
        return;
    }

    QJsonObject payload = msg.payload();
    int scaleId = payload.value("scaleId").toInt(0);
    int page = payload.value("page").toInt(1);
    int pageSize = payload.value("pageSize").toInt(20);
    int offset = (page - 1) * pageSize;

    QString sql = "SELECT ar.id, ar.scale_id, s.name as scale_name, ar.total_score, "
                  "ar.result_level, ar.result_interpretation, ar.completed_at "
                  "FROM assessment_records ar "
                  "JOIN scales s ON ar.scale_id = s.id "
                  "WHERE ar.user_id = :user_id";
    QVariantMap bindings = {{"user_id", session->userId()}};

    if (scaleId > 0) {
        sql += " AND ar.scale_id = :scale_id";
        bindings["scale_id"] = scaleId;
    }

    sql += " ORDER BY ar.completed_at DESC LIMIT :limit OFFSET :offset";
    bindings["limit"] = pageSize;
    bindings["offset"] = offset;

    auto results = DbManager::instance().executeQuery(sql, bindings);

    QJsonArray recordsArray;
    for (const auto& row : results) {
        QJsonObject record;
        record["id"] = row["id"].toLongLong();
        record["scaleId"] = row["scale_id"].toInt();
        record["scaleName"] = row["scale_name"].toString();
        record["totalScore"] = row["total_score"].toDouble();
        record["resultLevel"] = row["result_level"].toString();
        record["interpretation"] = row["result_interpretation"].toString();
        record["completedAt"] = row["completed_at"].toDateTime().toString(Qt::ISODate);
        recordsArray.append(record);
    }

    QJsonObject responseData;
    responseData["records"] = recordsArray;
    responseData["page"] = page;
    responseData["pageSize"] = pageSize;

    sendMessage(session, Message::success(
        Protocol::MessageType::GET_ASSESSMENT_HISTORY_RESPONSE, msg.seq(), responseData));
}

// ============================================================
// Get Assessment Stats (for data visualization)
// ============================================================

void PsychServer::handleGetAssessmentStats(ClientSession* session, const Message& msg) {
    if (!session->isAuthenticated()) {
        sendMessage(session, Message::error(msg.seq(),
            Protocol::ErrorCode::AUTH_FAILED, "请先登录"));
        return;
    }

    QJsonObject payload = msg.payload();
    int scaleId = payload.value("scaleId").toInt(0);

    // Get per-scale stats
    QString sql = "SELECT s.name, s.category, COUNT(*) as count, "
                  "AVG(ar.total_score) as avg_score, "
                  "MIN(ar.total_score) as min_score, "
                  "MAX(ar.total_score) as max_score "
                  "FROM assessment_records ar "
                  "JOIN scales s ON ar.scale_id = s.id "
                  "WHERE ar.user_id = :user_id";
    QVariantMap bindings = {{"user_id", session->userId()}};

    if (scaleId > 0) {
        sql += " AND ar.scale_id = :scale_id";
        bindings["scale_id"] = scaleId;
    }

    sql += " GROUP BY ar.scale_id";

    auto results = DbManager::instance().executeQuery(sql, bindings);

    QJsonArray statsArray;
    for (const auto& row : results) {
        QJsonObject stat;
        stat["scaleName"] = row["name"].toString();
        stat["category"] = row["category"].toString();
        stat["count"] = row["count"].toInt();
        stat["avgScore"] = row["avg_score"].toDouble();
        stat["minScore"] = row["min_score"].toDouble();
        stat["maxScore"] = row["max_score"].toDouble();
        statsArray.append(stat);
    }

    // Get trend data (last 10 assessments per scale)
    QString trendSql = "SELECT scale_id, total_score, completed_at "
                       "FROM assessment_records "
                       "WHERE user_id = :user_id";
    QVariantMap trendBindings = {{"user_id", session->userId()}};

    if (scaleId > 0) {
        trendSql += " AND scale_id = :scale_id";
        trendBindings["scale_id"] = scaleId;
    }

    trendSql += " ORDER BY completed_at ASC";

    auto trendResults = DbManager::instance().executeQuery(trendSql, trendBindings);

    QJsonArray trendArray;
    for (const auto& row : trendResults) {
        QJsonObject point;
        point["scaleId"] = row["scale_id"].toInt();
        point["score"] = row["total_score"].toDouble();
        point["date"] = row["completed_at"].toDateTime().toString("yyyy-MM-dd");
        trendArray.append(point);
    }

    // Overall summary
    QString summarySql = "SELECT COUNT(*) as total_assessments, "
                         "COUNT(DISTINCT scale_id) as scales_used "
                         "FROM assessment_records WHERE user_id = :user_id";
    auto summaryResult = DbManager::instance().executeQuery(summarySql,
        {{"user_id", session->userId()}});

    QJsonObject responseData;
    responseData["scaleStats"] = statsArray;
    responseData["trend"] = trendArray;
    if (!summaryResult.isEmpty()) {
        responseData["totalAssessments"] = summaryResult.first()["total_assessments"].toInt();
        responseData["scalesUsed"] = summaryResult.first()["scales_used"].toInt();
    }

    sendMessage(session, Message::success(
        Protocol::MessageType::GET_ASSESSMENT_STATS_RESPONSE, msg.seq(), responseData));
}
