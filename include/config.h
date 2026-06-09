#ifndef CONFIG_H
#define CONFIG_H

#include <QString>

namespace Config {
    // Server settings
    constexpr int TCP_PORT = 9527;
    constexpr int WS_PORT = 9528;
    constexpr int MAX_CONNECTIONS = 100;
    constexpr int HEARTBEAT_INTERVAL_MS = 30000;
    constexpr int SESSION_TIMEOUT_MS = 1800000; // 30 minutes

    // Database settings
    const QString DB_HOST = "localhost";
    constexpr int DB_PORT = 3306;
    const QString DB_NAME = "psychology_system";
    const QString DB_USER = "root";
    const QString DB_PASSWORD = "Z.s.q.5.1.3.";

    // DeepSeek AI API
    const QString DEEPSEEK_API_KEY = "";

    // Security
    constexpr int PASSWORD_HASH_ROUNDS = 12;
    constexpr int TOKEN_EXPIRY_HOURS = 24;
    constexpr int MAX_LOGIN_ATTEMPTS = 5;
    constexpr int LOGIN_LOCK_MINUTES = 15;
}

#endif // CONFIG_H
