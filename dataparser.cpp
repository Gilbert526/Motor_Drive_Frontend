#include "dataparser.h"
#include <QDebug>
#include <QtEndian>

DataParser::DataParser(QObject *parent)
    : QObject(parent)
    , m_syncBytes({0xAA, 0x55})
    , m_maxPoints(20000)
{
}

void DataParser::setSyncBytes(const QByteArray &sync)
{
    m_syncBytes = sync;
}

void DataParser::setFields(const QList<FieldDef> &fields)
{
    m_fields = fields;
    m_dataQueues.clear();
    for (const auto &field : m_fields) {
        m_dataQueues[field.name] = QVector<double>();
        m_dataQueues[field.name].reserve(m_maxPoints);
    }
}

void DataParser::setMaxDataPoints(int points)
{
    m_maxPoints = points;
    for (auto &queue : m_dataQueues) {
        if (queue.size() > m_maxPoints)
            queue = queue.mid(queue.size() - m_maxPoints);
        queue.reserve(m_maxPoints);
    }
}

void DataParser::parseReceivedData(const QByteArray &data)
{
    m_buffer.append(data);
    tryParsePackets();
}

void DataParser::tryParsePackets()
{
    int idx = 0;
    int bufferSize = m_buffer.size();
    int syncLen = m_syncBytes.size();

    while (idx + syncLen + 4 <= bufferSize) {
        // 查找帧头
        if (m_buffer.mid(idx, syncLen) != m_syncBytes) {
            // 不是帧头，可能是文本信息
            int end = m_buffer.indexOf(m_syncBytes, idx);
            if (end == -1) {
                // 没有更多帧头，将剩余数据当作文本输出
                QByteArray text = m_buffer.mid(idx);
                if (!text.isEmpty()) {
                    emit textMessageReceived(QString::fromUtf8(text).trimmed());
                }
                m_buffer.clear();
                return;
            } else {
                // 从 idx 到 end 之间的数据是文本
                QByteArray text = m_buffer.mid(idx, end - idx);
                if (!text.isEmpty()) {
                    emit textMessageReceived(QString::fromUtf8(text).trimmed());
                }
                idx = end;
                continue;
            }
        }

        // 读取 mask（4字节小端）
        quint32 mask = 0;
        for (int i = 0; i < 4; ++i) {
            mask |= (static_cast<quint8>(m_buffer[idx + syncLen + i]) << (8 * i));
        }

        int pos = idx + syncLen + 4;
        QMap<QString, double> packetData;

        for (const auto &field : m_fields) {
            if (mask & field.maskBit) {
                if (pos + field.size > bufferSize) {
                    // 数据不足，等待后续字节
                    return;
                }
                double value = 0.0;
                const char *raw = m_buffer.constData() + pos;
                if (field.fmt == 'B') {
                    value = static_cast<quint8>(*raw);
                } else if (field.fmt == 'H') {
                    value = qFromLittleEndian<quint16>(raw);
                } else if (field.fmt == 'f') {
                    value = qFromLittleEndian<float>(raw);
                } else {
                    qWarning() << "Unknown format" << field.fmt;
                }
                packetData[field.name] = value;
                appendToQueue(field.name, value);
                pos += field.size;
            }
        }

        if (!packetData.isEmpty()) {
            emit packetParsed(packetData);
        }
        idx = pos;
    }

    // 移除已处理的字节
    if (idx > 0)
        m_buffer = m_buffer.mid(idx);
}

void DataParser::appendToQueue(const QString &name, double value)
{
    if (!m_dataQueues.contains(name))
        return;
    auto &vec = m_dataQueues[name];
    vec.append(value);
    if (vec.size() > m_maxPoints)
        vec.removeFirst();
}

QVector<double> DataParser::getFieldData(const QString &fieldName) const
{
    return m_dataQueues.value(fieldName);
}

QByteArray DataParser::buildPacket(const QMap<QString, double> &values) const
{
    QByteArray packet;
    packet.append(m_syncBytes);

    // 计算 mask
    quint32 mask = 0;
    for (auto it = values.begin(); it != values.end(); ++it) {
        QString name = it.key();
        for (const auto &field : m_fields) {
            if (field.name == name) {
                mask |= field.maskBit;
                break;
            }
        }
    }

    // 追加 mask（小端）
    for (int i = 0; i < 4; ++i) {
        packet.append(static_cast<char>((mask >> (8 * i)) & 0xFF));
    }

    // 按字段顺序追加数据（根据mask存在性）
    for (const auto &field : m_fields) {
        if (mask & field.maskBit) {
            double val = values.value(field.name, 0.0);
            if (field.fmt == 'B') {
                quint8 byteVal = static_cast<quint8>(val);
                packet.append(reinterpret_cast<const char*>(&byteVal), 1);
            } else if (field.fmt == 'H') {
                quint16 shortVal = static_cast<quint16>(val);
                qToLittleEndian(shortVal, packet.data() + packet.size());
                packet.resize(packet.size() + 2);
            } else if (field.fmt == 'f') {
                float floatVal = static_cast<float>(val);
                qToLittleEndian(floatVal, packet.data() + packet.size());
                packet.resize(packet.size() + 4);
            }
        }
    }
    return packet;
}