#include <QCoreApplication>
#include "config.h"
#include "utils/logger.h"
#include "server/psych_server.h"

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);
    app.setApplicationName("PsychServer");
    app.setApplicationVersion("1.0.0");

    Logger::instance().init("psych_server.log");
    LOG_INFO("=== Psychology Counseling Server Starting ===");
    LOG_INFO(QString("Version: %1").arg(app.applicationVersion()));

    PsychServer server;
    if (!server.start()) {
        LOG_FATAL("Failed to start server!");
        return 1;
    }

    LOG_INFO(QString("Server listening on TCP port %1, WebSocket port %2")
                 .arg(Config::TCP_PORT).arg(Config::WS_PORT));

    return app.exec();
}
