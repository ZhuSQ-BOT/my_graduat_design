#ifndef DB_MANAGER_H
#define DB_MANAGER_H

#include <QObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QMutex>
#include <QQueue>
#include <QString>
#include <QVariantMap>
#include <functional>

/**
 * @brief 数据库连接池管理器
 *
 * 提供线程安全的数据库连接池，支持连接复用和自动回收。
 * 所有数据库操作通过此单例类进行。
 */
class DbManager : public QObject {
    Q_OBJECT

public:
    static DbManager& instance();

    /**
     * @brief 初始化数据库连接池
     * @param host 数据库主机
     * @param port 端口
     * @param dbName 数据库名
     * @param user 用户名
     * @param password 密码
     * @param poolSize 连接池大小
     * @return 是否初始化成功
     */
    bool init(const QString& host, int port, const QString& dbName,
              const QString& user, const QString& password, int poolSize = 5);

    /**
     * @brief 获取一个数据库连接
     * @return 连接名（用于 QSqlDatabase::database()）
     */
    QString acquireConnection();

    /**
     * @brief 归还数据库连接
     * @param connName 连接名
     */
    void releaseConnection(const QString& connName);

    /**
     * @brief 执行查询并返回结果
     * @param queryStr SQL 语句
     * @param bindings 绑定参数
     * @return 查询结果列表
     */
    QList<QVariantMap> executeQuery(const QString& queryStr,
                                     const QVariantMap& bindings = {});

    /**
     * @brief 执行非查询语句（INSERT/UPDATE/DELETE）
     * @param queryStr SQL 语句
     * @param bindings 绑定参数
     * @return 受影响的行数，失败返回 -1
     */
    int executeUpdate(const QString& queryStr,
                      const QVariantMap& bindings = {});

    /**
     * @brief 获取最后插入的 ID
     * @return 自增 ID
     */
    qint64 lastInsertId();

    /**
     * @brief 检查数据库是否可用
     * @return 连接是否正常
     */
    bool isConnected() const;

    /**
     * @brief 关闭所有连接
     */
    void close();

private:
    explicit DbManager(QObject* parent = nullptr);
    ~DbManager() override;
    DbManager(const DbManager&) = delete;
    DbManager& operator=(const DbManager&) = delete;

    QString createConnection();
    void bindParameters(QSqlQuery& query, const QVariantMap& bindings);

    QString m_host;
    int m_port = 3306;
    QString m_dbName;
    QString m_user;
    QString m_password;
    int m_poolSize = 5;
    int m_connCounter = 0;

    QQueue<QString> m_availableConns;
    QMutex m_mutex;
    qint64 m_lastInsertId = -1;
    bool m_initialized = false;
};

#endif // DB_MANAGER_H
