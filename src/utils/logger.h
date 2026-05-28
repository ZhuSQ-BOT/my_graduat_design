#ifndef LOGGER_H
#define LOGGER_H

#include <QString>
#include <QFile>
#include <QTextStream>
#include <QMutex>
#include <QDateTime>

class Logger {
public:
    enum class Level { DEBUG, INFO, WARN, ERROR, FATAL };

    static Logger& instance();

    void init(const QString& logFile = "server.log");
    void debug(const QString& msg);
    void info(const QString& msg);
    void warn(const QString& msg);
    void error(const QString& msg);
    void fatal(const QString& msg);

private:
    Logger() = default;
    ~Logger();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    void log(Level level, const QString& msg);
    QString levelToString(Level level) const;

    QFile m_file;
    QTextStream m_stream;
    QMutex m_mutex;
    bool m_initialized = false;
};

#define LOG_DEBUG(msg) Logger::instance().debug(msg)
#define LOG_INFO(msg) Logger::instance().info(msg)
#define LOG_WARN(msg) Logger::instance().warn(msg)
#define LOG_ERROR(msg) Logger::instance().error(msg)
#define LOG_FATAL(msg) Logger::instance().fatal(msg)

#endif // LOGGER_H
