#include "database/db_manager.h"
#include "utils/logger.h"
#include <QSqlRecord>
#include <QThread>

DbManager& DbManager::instance() {
    static DbManager manager;
    return manager;
}

DbManager::DbManager(QObject* parent) : QObject(parent) {}

DbManager::~DbManager() {
    close();
}

bool DbManager::init(const QString& host, int port, const QString& dbName,
                     const QString& user, const QString& password, int poolSize) {
    QMutexLocker locker(&m_mutex);

    m_host = host;
    m_port = port;
    m_dbName = dbName;
    m_user = user;
    m_password = password;
    m_poolSize = poolSize;

    // Create initial connections
    for (int i = 0; i < poolSize; ++i) {
        QString connName = createConnection();
        if (connName.isEmpty()) {
            LOG_ERROR("Failed to create database connection pool");
            return false;
        }
        m_availableConns.enqueue(connName);
    }

    m_initialized = true;
    LOG_INFO(QString("Database pool initialized with %1 connections").arg(poolSize));
    return true;
}

QString DbManager::createConnection() {
    QString connName = QString("conn_%1_%2")
                           .arg(reinterpret_cast<qintptr>(QThread::currentThreadId()))
                           .arg(m_connCounter++);

    QSqlDatabase db = QSqlDatabase::addDatabase("QMYSQL", connName);
    db.setHostName(m_host);
    db.setPort(m_port);
    db.setDatabaseName(m_dbName);
    db.setUserName(m_user);
    db.setPassword(m_password);

    // Connection options
    db.setConnectOptions("MYSQL_OPT_RECONNECT=1;MYSQL_OPT_CONNECT_TIMEOUT=10");

    if (!db.open()) {
        LOG_ERROR(QString("Database connection failed: %1").arg(db.lastError().text()));
        QSqlDatabase::removeDatabase(connName);
        return QString();
    }

    // Set character set
    QSqlQuery query(db);
    query.exec("SET NAMES utf8mb4");
    query.exec("SET CHARACTER SET utf8mb4");

    LOG_DEBUG(QString("Created database connection: %1").arg(connName));
    return connName;
}

QString DbManager::acquireConnection() {
    QMutexLocker locker(&m_mutex);

    if (m_availableConns.isEmpty()) {
        // Create a new connection if pool is exhausted
        if (m_connCounter < m_poolSize * 2) {
            QString connName = createConnection();
            if (!connName.isEmpty()) {
                return connName;
            }
        }
        LOG_WARN("Connection pool exhausted, waiting...");
        return QString();
    }

    return m_availableConns.dequeue();
}

void DbManager::releaseConnection(const QString& connName) {
    QMutexLocker locker(&m_mutex);

    QSqlDatabase db = QSqlDatabase::database(connName);
    if (db.isOpen()) {
        m_availableConns.enqueue(connName);
    } else {
        // Reconnect if connection was lost
        db.close();
        QSqlDatabase::removeDatabase(connName);
        QString newConn = createConnection();
        if (!newConn.isEmpty()) {
            m_availableConns.enqueue(newConn);
        }
    }
}

QList<QVariantMap> DbManager::executeQuery(const QString& queryStr,
                                            const QVariantMap& bindings) {
    QList<QVariantMap> results;

    QString connName = acquireConnection();
    if (connName.isEmpty()) {
        LOG_ERROR("No available database connection");
        return results;
    }

    QSqlDatabase db = QSqlDatabase::database(connName);
    QSqlQuery query(db);
    query.prepare(queryStr);
    bindParameters(query, bindings);

    if (query.exec()) {
        QSqlRecord record = query.record();
        int fieldCount = record.count();

        while (query.next()) {
            QVariantMap row;
            for (int i = 0; i < fieldCount; ++i) {
                row[record.fieldName(i)] = query.value(i);
            }
            results.append(row);
        }
    } else {
        LOG_ERROR(QString("Query failed: %1\nSQL: %2")
                      .arg(query.lastError().text(), queryStr));
    }

    releaseConnection(connName);
    return results;
}

int DbManager::executeUpdate(const QString& queryStr,
                             const QVariantMap& bindings) {
    QString connName = acquireConnection();
    if (connName.isEmpty()) {
        LOG_ERROR("No available database connection");
        return -1;
    }

    QSqlDatabase db = QSqlDatabase::database(connName);
    QSqlQuery query(db);
    query.prepare(queryStr);
    bindParameters(query, bindings);

    int affectedRows = -1;
    if (query.exec()) {
        affectedRows = query.numRowsAffected();
        m_lastInsertId = query.lastInsertId().toLongLong();
    } else {
        LOG_ERROR(QString("Update failed: %1\nSQL: %2")
                      .arg(query.lastError().text(), queryStr));
    }

    releaseConnection(connName);
    return affectedRows;
}

qint64 DbManager::lastInsertId() {
    return m_lastInsertId;
}

bool DbManager::isConnected() const {
    return m_initialized;
}

void DbManager::close() {
    QMutexLocker locker(&m_mutex);

    while (!m_availableConns.isEmpty()) {
        QString connName = m_availableConns.dequeue();
        QSqlDatabase db = QSqlDatabase::database(connName);
        if (db.isOpen()) {
            db.close();
        }
        QSqlDatabase::removeDatabase(connName);
    }

    m_initialized = false;
    LOG_INFO("Database pool closed");
}

void DbManager::bindParameters(QSqlQuery& query, const QVariantMap& bindings) {
    for (auto it = bindings.begin(); it != bindings.end(); ++it) {
        query.bindValue(":" + it.key(), it.value());
    }
}
