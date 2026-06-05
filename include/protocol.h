#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <QString>
#include <QJsonObject>
#include <QJsonDocument>
#include <QByteArray>
#include <QDataStream>

namespace Protocol {
    // Message header: 4 bytes magic + 4 bytes length + 4 bytes type + 4 bytes seq
    constexpr quint32 MAGIC_NUMBER = 0x50535943; // "PSYC"
    constexpr int HEADER_SIZE = 16;

    enum class MessageType : quint32 {
        // Auth (100-199)
        LOGIN_REQUEST = 100,
        LOGIN_RESPONSE = 101,
        REGISTER_REQUEST = 102,
        REGISTER_RESPONSE = 103,
        LOGOUT_REQUEST = 104,
        LOGOUT_RESPONSE = 105,
        VALIDATE_TOKEN_REQUEST = 106,
        VALIDATE_TOKEN_RESPONSE = 107,

        // User Profile (150-199)
        GET_PROFILE_REQUEST = 150,
        GET_PROFILE_RESPONSE = 151,
        UPDATE_PROFILE_REQUEST = 152,
        UPDATE_PROFILE_RESPONSE = 153,
        CHANGE_PASSWORD_REQUEST = 154,
        CHANGE_PASSWORD_RESPONSE = 155,
        GET_USER_STATS_REQUEST = 156,
        GET_USER_STATS_RESPONSE = 157,

        // Assessment (200-299)
        GET_SCALES_REQUEST = 200,
        GET_SCALES_RESPONSE = 201,
        GET_SCALE_DETAIL_REQUEST = 202,
        GET_SCALE_DETAIL_RESPONSE = 203,
        SUBMIT_ASSESSMENT_REQUEST = 204,
        SUBMIT_ASSESSMENT_RESPONSE = 205,
        GET_ASSESSMENT_HISTORY_REQUEST = 206,
        GET_ASSESSMENT_HISTORY_RESPONSE = 207,
        GET_ASSESSMENT_STATS_REQUEST = 208,
        GET_ASSESSMENT_STATS_RESPONSE = 209,
        CREATE_SCALE_REQUEST = 210,
        CREATE_SCALE_RESPONSE = 211,
        UPDATE_SCALE_REQUEST = 212,
        UPDATE_SCALE_RESPONSE = 213,
        DELETE_SCALE_REQUEST = 214,
        DELETE_SCALE_RESPONSE = 215,
        IMPORT_SCALE_REQUEST = 216,
        IMPORT_SCALE_RESPONSE = 217,

        // Messaging (300-399)
        SEND_MESSAGE = 300,
        RECEIVE_MESSAGE = 301,
        GET_CONTACTS_REQUEST = 302,
        GET_CONTACTS_RESPONSE = 303,
        GET_MESSAGE_HISTORY_REQUEST = 304,
        GET_MESSAGE_HISTORY_RESPONSE = 305,
        ONLINE_STATUS_UPDATE = 306,
        FRIEND_REQUEST_NOTIFY = 307,
        AI_CHAT_REQUEST = 310,
        AI_CHAT_RESPONSE = 311,
        ADD_CONTACT_REQUEST = 320,
        ADD_CONTACT_RESPONSE = 321,
        REMOVE_CONTACT_REQUEST = 322,
        REMOVE_CONTACT_RESPONSE = 323,
        SEARCH_USERS_REQUEST = 324,
        SEARCH_USERS_RESPONSE = 325,
        GET_PENDING_REQUESTS_REQUEST = 326,
        GET_PENDING_REQUESTS_RESPONSE = 327,
        ACCEPT_CONTACT_REQUEST = 328,
        ACCEPT_CONTACT_RESPONSE = 329,
        REJECT_CONTACT_REQUEST = 330,
        REJECT_CONTACT_RESPONSE = 331,

        // Forum (400-499)
        GET_POSTS_REQUEST = 400,
        GET_POSTS_RESPONSE = 401,
        CREATE_POST_REQUEST = 402,
        CREATE_POST_RESPONSE = 403,
        GET_POST_DETAIL_REQUEST = 404,
        GET_POST_DETAIL_RESPONSE = 405,
        CREATE_REPLY_REQUEST = 406,
        CREATE_REPLY_RESPONSE = 407,
        LIKE_POST_REQUEST = 408,
        LIKE_POST_RESPONSE = 409,

        // Appointment (500-599)
        GET_COUNSELORS_REQUEST = 500,
        GET_COUNSELORS_RESPONSE = 501,
        CREATE_APPOINTMENT_REQUEST = 502,
        CREATE_APPOINTMENT_RESPONSE = 503,
        GET_APPOINTMENTS_REQUEST = 504,
        GET_APPOINTMENTS_RESPONSE = 505,
        CANCEL_APPOINTMENT_REQUEST = 506,
        CANCEL_APPOINTMENT_RESPONSE = 507,
        UPDATE_APPOINTMENT_REQUEST = 508,
        UPDATE_APPOINTMENT_RESPONSE = 509,

        // Resources (600-699)
        GET_RESOURCES_REQUEST = 600,
        GET_RESOURCES_RESPONSE = 601,
        GET_RESOURCE_DETAIL_REQUEST = 602,
        GET_RESOURCE_DETAIL_RESPONSE = 603,
        GET_FAVORITES_REQUEST = 604,
        GET_FAVORITES_RESPONSE = 605,
        ADD_FAVORITE_REQUEST = 606,
        ADD_FAVORITE_RESPONSE = 607,
        REMOVE_FAVORITE_REQUEST = 608,
        REMOVE_FAVORITE_RESPONSE = 609,

        // Admin (700-799)
        GET_DASHBOARD_STATS_REQUEST = 700,
        GET_DASHBOARD_STATS_RESPONSE = 701,
        MANAGE_USER_REQUEST = 702,
        MANAGE_USER_RESPONSE = 703,
        GET_SYSTEM_LOGS_REQUEST = 704,
        GET_SYSTEM_LOGS_RESPONSE = 705,
        GET_REPORTS_REQUEST = 706,
        GET_REPORTS_RESPONSE = 707,
        RESOLVE_REPORT_REQUEST = 708,
        RESOLVE_REPORT_RESPONSE = 709,
        DISMISS_REPORT_REQUEST = 710,
        DISMISS_REPORT_RESPONSE = 711,

        // Counselor / Publisher (800-899)
        PUBLISH_TASK_REQUEST = 800,
        PUBLISH_TASK_RESPONSE = 801,
        GET_MY_TASKS_REQUEST = 802,
        GET_MY_TASKS_RESPONSE = 803,
        GET_PENDING_TASKS_REQUEST = 804,
        GET_PENDING_TASKS_RESPONSE = 805,
        REVIEW_TASK_REQUEST = 806,
        REVIEW_TASK_RESPONSE = 807,
        GET_TASK_REPORT_REQUEST = 808,
        GET_TASK_REPORT_RESPONSE = 809,
        GRANT_REPORT_ACCESS_REQUEST = 810,
        GRANT_REPORT_ACCESS_RESPONSE = 811,

        // System (900-999)
        HEARTBEAT = 900,
        HEARTBEAT_ACK = 901,
        ERROR_RESPONSE = 999
    };

    enum class ErrorCode : int {
        SUCCESS = 0,
        INVALID_REQUEST = 1001,
        AUTH_FAILED = 1002,
        TOKEN_EXPIRED = 1003,
        PERMISSION_DENIED = 1004,
        USER_NOT_FOUND = 1005,
        USER_ALREADY_EXISTS = 1006,
        DATABASE_ERROR = 2001,
        SERVER_ERROR = 9999
    };

    inline QByteArray serialize(MessageType type, quint32 seq, const QJsonObject& payload) {
        QByteArray body = QJsonDocument(payload).toJson(QJsonDocument::Compact);
        QByteArray result;
        QDataStream stream(&result, QIODevice::WriteOnly);
        stream.setByteOrder(QDataStream::BigEndian);
        stream << MAGIC_NUMBER
               << static_cast<quint32>(body.size())
               << static_cast<quint32>(type)
               << seq;
        result.append(body);
        return result;
    }
}

#endif // PROTOCOL_H
