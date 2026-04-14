#ifndef SERIALMANAGER_H
#define SERIALMANAGER_H

#include <QObject>
#include <QSerialPort>
#include <QByteArray>

class SerialManager : public QObject {
    Q_OBJECT

public:
    explicit SerialManager(QObject *parent = nullptr);
    ~SerialManager();

public slots:
    void openSerialPort(const QString &portName, qint32 baudRate);
    void closeSerialPort();
    void sendData(const QByteArray &data);

signals:
    void rawDataReceived(const QByteArray &data);  // 发送原始数据给解析器
    void portOpened(bool success, const QString &errorMsg);
    void portClosed();

private slots:
    void handleReadyRead();

private:
    QSerialPort *m_serial;
};

#endif // SERIALMANAGER_H
