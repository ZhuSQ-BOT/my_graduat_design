#include <QCoreApplication>
#include "config.h"
#include "utils/logger.h"
#include "server/psych_server.h"
#include "ai/deepseek_client.h"

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    app.setApplicationName("PsychServer");
    app.setApplicationVersion("1.0.0");

    Logger::instance().init("psych_server.log");
    LOG_INFO("=== Psychology Counseling Server Starting ===");
    LOG_INFO(QString("Version: %1").arg(app.applicationVersion()));

    // Initialize DeepSeek AI
    DeepSeekClient::instance().setApiKey(Config::DEEPSEEK_API_KEY);
    LOG_INFO("DeepSeek AI client initialized");

    PsychServer server;
    if (!server.start()) {
        LOG_FATAL("Failed to start server!");
        return 1;
    }

    LOG_INFO(QString("Server listening on TCP port %1, WebSocket port %2")
                 .arg(Config::TCP_PORT).arg(Config::WS_PORT));

    return app.exec();
}
