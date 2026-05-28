#ifndef MESSAGE_H
#define MESSAGE_H

#include <QJsonObject>
#include <QJsonDocument>
#include <QByteArray>
#include <QDataStream>
#include "protocol.h"

class Message {
public:
    Message() = default;

    Message(Protocol::MessageType type, quint32 seq, const QJsonObject& payload = {})
        : m_type(type), m_seq(seq), m_payload(payload) {}

    Protocol::MessageType type() const { return m_type; }
    quint32 seq() const { return m_seq; }
    QJsonObject payload() const { return m_payload; }

    QByteArray serialize() const {
        return Protocol::serialize(m_type, m_seq, m_payload);
    }

    static bool deserialize(const QByteArray& data, Message& msg) {
        if (data.size() < Protocol::HEADER_SIZE) return false;

        QDataStream stream(data);
        stream.setByteOrder(QDataStream::BigEndian);

        quint32 magic, bodyLen, type, seq;
        stream >> magic >> bodyLen >> type >> seq;

        if (magic != Protocol::MAGIC_NUMBER) return false;
        if (data.size() < Protocol::HEADER_SIZE + static_cast<int>(bodyLen)) return false;

        QByteArray body = data.mid(Protocol::HEADER_SIZE, bodyLen);
        QJsonDocument doc = QJsonDocument::fromJson(body);
        if (!doc.isObject()) return false;

        msg.m_type = static_cast<Protocol::MessageType>(type);
        msg.m_seq = seq;
        msg.m_payload = doc.object();
        return true;
    }

    static Message error(quint32 seq, Protocol::ErrorCode code, const QString& message) {
        QJsonObject payload;
        payload["code"] = static_cast<int>(code);
        payload["message"] = message;
        return Message(Protocol::MessageType::ERROR_RESPONSE, seq, payload);
    }

    static Message success(Protocol::MessageType type, quint32 seq, const QJsonObject& data = {}) {
        QJsonObject payload;
        payload["code"] = static_cast<int>(Protocol::ErrorCode::SUCCESS);
        payload["data"] = data;
        return Message(type, seq, payload);
    }

private:
    Protocol::MessageType m_type = Protocol::MessageType::ERROR_RESPONSE;
    quint32 m_seq = 0;
    QJsonObject m_payload;
};

#endif // MESSAGE_H
