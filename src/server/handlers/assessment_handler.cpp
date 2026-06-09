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

    LOG_INFO(QString("handleGetScales called by user %1, category=%2, page=%3, pageSize=%4")
        .arg(session->userId()).arg(category).arg(page).arg(pageSize));

    QString sql = "SELECT id, name, name_en, description, category, total_questions, "
                  "scoring_method, difficulty, time_limit_minutes FROM scales WHERE is_active = 1";
    QVariantMap bindings;

    if (!category.isEmpty()) {
        sql += " AND category = :category";
        bindings["category"] = category;
    }

    sql += " ORDER BY category, id LIMIT :limit OFFSET :offset";
    bindings["limit"] = pageSize;
    bindings["offset"] = offset;

    LOG_DEBUG(QString("Executing SQL: %1").arg(sql));
    auto results = DbManager::instance().executeQuery(sql, bindings);
    LOG_INFO(QString("Query returned %1 rows").arg(results.size()));

    QJsonArray scalesArray;
    for (const auto& row : results) {
        QJsonObject scale;
        scale["id"] = row["id"].toInt();
        scale["name"] = row["name"].toString();
        scale["nameEn"] = row["name_en"].toString();
        scale["description"] = row["description"].toString();
        scale["category"] = row["category"].toString();
        scale["totalQuestions"] = row["total_questions"].toInt();
        scale["questionCount"] = row["total_questions"].toInt();
        QString sm = row["scoring_method"].toString();
        int baseMax = row["total_questions"].toInt() * 3;
        scale["maxScore"] = (sm == "sum_multiply") ? static_cast<int>(baseMax * 1.25) : baseMax;
        scale["difficulty"] = row["difficulty"].toInt();
        scale["timeLimitMinutes"] = row["time_limit_minutes"].isNull() ? -1 : row["time_limit_minutes"].toInt();
        scalesArray.append(scale);
        
        LOG_DEBUG(QString("Loaded scale: %1 (id=%2)").arg(scale["name"].toString()).arg(scale["id"].toInt()));
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

    LOG_INFO(QString("Total scales in database: %1, returning %2 scales")
        .arg(total).arg(scalesArray.size()));

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

    // Get scale info
    auto scaleInfo = DbManager::instance().executeQuery(
        "SELECT scoring_method, total_questions FROM scales WHERE id = :id",
        {{"id", scaleId}});
    QString scoringMethod = scaleInfo.isEmpty() ? "sum" : scaleInfo.first()["scoring_method"].toString();
    int maxPerQ = 3; // default 0-3 scale

    // Get questions for scoring
    auto questions = DbManager::instance().executeQuery(
        "SELECT id, question_number, scoring_rules, reverse_scored, options "
        "FROM scale_questions WHERE scale_id = :scale_id ORDER BY question_number",
        {{"scale_id", scaleId}});

    // Calculate score
    double rawScore = 0;
    QJsonObject factorScores; // <-- 新增：因子得分
    QString answersJson = QString::fromUtf8(
        QJsonDocument(answers).toJson(QJsonDocument::Compact));

    for (int i = 0; i < answers.size() && i < questions.size(); ++i) {
        QJsonObject ansObj = answers[i].toObject();
        int value = ansObj.contains("score") ? ansObj["score"].toInt() : ansObj["value"].toInt();

        // Handle reverse scoring
        if (questions[i]["reverse_scored"].toBool()) {
            value = maxPerQ - value;
        }

        rawScore += value;
        
        // Calculate factor scores
        QString scoringRulesStr = questions[i]["scoring_rules"].toString();
        if (!scoringRulesStr.isEmpty()) {
            QJsonParseError error;
            QJsonDocument doc = QJsonDocument::fromJson(scoringRulesStr.toUtf8(), &error);
            if (error.error == QJsonParseError::NoError && doc.isObject()) {
                QJsonObject rules = doc.object();
                QJsonArray factors = rules["factors"].toArray();
                QJsonArray weights = rules["weights"].toArray();
                
                for (int j = 0; j < factors.size(); ++j) {
                    QString factor = factors[j].toString();
                    double weight = weights.isEmpty() ? 1.0 : weights[j].toDouble();
                    double factorScore = value * weight;
                    
                    if (factorScores.contains(factor)) {
                        factorScores[factor] = factorScores[factor].toDouble() + factorScore;
                    } else {
                        factorScores[factor] = factorScore;
                    }
                }
            } else {
                // If no valid scoring_rules, use "total" as default factor
                if (factorScores.contains("total")) {
                    factorScores["total"] = factorScores["total"].toDouble() + value;
                } else {
                    factorScores["total"] = value;
                }
            }
        } else {
            // If no scoring_rules, use "total" as default factor
            if (factorScores.contains("total")) {
                factorScores["total"] = factorScores["total"].toDouble() + value;
            } else {
                factorScores["total"] = value;
            }
        }
    }

    // Apply scoring method
    double totalScore = rawScore;
    if (scoringMethod == "sum_multiply") {
        totalScore = rawScore * 1.25;
    }

    // Determine result level based on scale-specific thresholds
    QString resultLevel;
    QString interpretation;

    // Use approximate percentile-based thresholds for general scales
    int maxPossible = static_cast<int>(questions.size() * maxPerQ * (scoringMethod == "sum_multiply" ? 1.25 : 1));
    double ratio = (maxPossible > 0) ? totalScore / maxPossible : 0;

    if (ratio <= 0.25) {
        resultLevel = "normal"; interpretation = "结果在正常范围内，请继续保持良好的心理状态。";
    } else if (ratio <= 0.40) {
        resultLevel = "mild"; interpretation = "轻度异常，建议关注自身状态，适当进行自我调节。";
    } else if (ratio <= 0.55) {
        resultLevel = "moderate"; interpretation = "中度异常，建议寻求专业心理咨询师的帮助。";
    } else if (ratio <= 0.70) {
        resultLevel = "mod_severe"; interpretation = "中重度异常，强烈建议尽快预约专业心理咨询。";
    } else {
        resultLevel = "severe"; interpretation = "重度异常，请尽快寻求专业治疗和帮助。";
    }

    int durationSeconds = static_cast<int>((completedAt - startedAt) / 1000);

    // Save record
    int rows = DbManager::instance().executeUpdate(
        "INSERT INTO assessment_records "
        "(user_id, scale_id, total_score, factor_scores, result_level, "
        "result_interpretation, answers, started_at, completed_at, duration_seconds) "
        "VALUES (:user_id, :scale_id, :total_score, :factor_scores, :result_level, "
        ":result_interpretation, :answers, "
        "FROM_UNIXTIME(:started_at), FROM_UNIXTIME(:completed_at), :duration)",
        {
            {"user_id", session->userId()},
            {"scale_id", scaleId},
            {"total_score", totalScore},
            {"factor_scores", QString::fromUtf8(QJsonDocument(factorScores).toJson(QJsonDocument::Compact))},
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
                  "ar.result_level, ar.result_interpretation, ar.completed_at, "
                  "ar.factor_scores "
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
        
        // Parse factor_scores (JSON field)
        QString factorScoresStr = row["factor_scores"].toString();
        if (!factorScoresStr.isEmpty() && factorScoresStr != "{}") {
            QJsonParseError error;
            QJsonDocument doc = QJsonDocument::fromJson(factorScoresStr.toUtf8(), &error);
            if (error.error == QJsonParseError::NoError && doc.isObject()) {
                record["factorScores"] = doc.object();
            } else {
                record["factorScores"] = QJsonObject();
            }
        } else {
            record["factorScores"] = QJsonObject();
        }
        
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
