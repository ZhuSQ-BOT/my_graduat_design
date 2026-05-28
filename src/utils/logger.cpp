#include "logger.h"
#include <QDebug>
#include <QMutexLocker>

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

void Logger::init(const QString& logFile) {
    QMutexLocker locker(&m_mutex);
    m_file.setFileName(logFile);
    if (m_file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        m_stream.setDevice(&m_file);
        m_initialized = true;
    }
}

Logger::~Logger() {
    if (m_initialized) {
        m_file.close();
    }
}

void Logger::log(Level level, const QString& msg) {
    QMutexLocker locker(&m_mutex);
    QString timestamp = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss.zzz");
    QString logLine = QString("[%1] [%2] %3")
                          .arg(timestamp)
                          .arg(levelToString(level))
                          .arg(msg);

    qDebug().noquote() << logLine;

    if (m_initialized) {
        m_stream << logLine << "\n";
        m_stream.flush();
    }
}

void Logger::debug(const QString& msg) { log(Level::DEBUG, msg); }
void Logger::info(const QString& msg)  { log(Level::INFO, msg); }
void Logger::warn(const QString& msg)  { log(Level::WARN, msg); }
void Logger::error(const QString& msg) { log(Level::ERROR, msg); }
void Logger::fatal(const QString& msg) { log(Level::FATAL, msg); }

QString Logger::levelToString(Level level) const {
    switch (level) {
        case Level::DEBUG: return "DEBUG";
        case Level::INFO:  return "INFO ";
        case Level::WARN:  return "WARN ";
        case Level::ERROR: return "ERROR";
        case Level::FATAL: return "FATAL";
        default: return "?????";
    }
}
