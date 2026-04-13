#ifndef DATAPARSER_H
#define DATAPARSER_H

#include <QObject>
#include <QByteArray>
#include <QMap>
#include <QVector>
#include <QStringList>

struct FieldDef
{
    QString name;
    int size;          // 字节数
    char fmt;          // 'B', 'H', 'f'
    quint32 maskBit;
};

class DataParser : public QObject
{
    Q_OBJECT
public:
    explicit DataParser(QObject *parent = nullptr);

    // 协议配置
    void setSyncBytes(const QByteArray &sync);
    void setFields(const QList<FieldDef> &fields);

    // 解析接收数据
    void parseReceivedData(const QByteArray &data);

    // 获取某个字段的数据队列（用于绘图）
    QVector<double> getFieldData(const QString &fieldName) const;
    int getMaxDataPoints() const { return m_maxPoints; }
    void setMaxDataPoints(int points);

    // 构造发送包（根据字段掩码和值）
    QByteArray buildPacket(const QMap<QString, double> &values) const;

signals:
    void packetParsed(const QMap<QString, double> &fieldValues);
    void textMessageReceived(const QString &message);   // 非二进制数据当作文本显示
    void parseError(const QString &error);

private:
    QByteArray m_syncBytes;
    QList<FieldDef> m_fields;
    QMap<QString, QVector<double>> m_dataQueues;  // 环形队列（保留最近 N 点）
    int m_maxPoints;

    QByteArray m_buffer;   // 粘包缓冲区

    void appendToQueue(const QString &name, double value);
    void tryParsePackets();
};

#endif // DATAPARSER_H