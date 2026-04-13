#include "serialmanager.h"
#include <QDebug>

SerialManager::SerialManager(QObject *parent)
    : QObject(parent)
    , m_serial(new QSerialPort(this))
{
    connect(m_serial, &QSerialPort::readyRead, this, &SerialManager::handleReadyRead);
    connect(m_serial, &QSerialPort::errorOccurred, this, &SerialManager::handleError);
}

SerialManager::~SerialManager()
{
    close();
}

bool SerialManager::open(const QString &portName, qint32 baudRate)
{
    if (m_serial->isOpen())
        close();

    m_serial->setPortName(portName);
    m_serial->setBaudRate(baudRate);
    m_serial->setDataBits(QSerialPort::Data8);
    m_serial->setParity(QSerialPort::NoParity);
    m_serial->setStopBits(QSerialPort::OneStop);
    m_serial->setFlowControl(QSerialPort::NoFlowControl);

    if (!m_serial->open(QIODevice::ReadWrite)) {
        emit errorOccurred(m_serial->errorString());
        return false;
    }
    return true;
}

void SerialManager::close()
{
    if (m_serial->isOpen()) {
        m_serial->close();
        emit portClosed();
    }
}

bool SerialManager::isOpen() const
{
    return m_serial->isOpen();
}

void SerialManager::sendData(const QByteArray &data)
{
    if (m_serial->isOpen()) {
        m_serial->write(data);
        if (!m_serial->waitForBytesWritten(100))
            emit errorOccurred("Write timeout");
    } else {
        emit errorOccurred("Serial port not open");
    }
}

void SerialManager::handleReadyRead()
{
    QByteArray data = m_serial->readAll();
    if (!data.isEmpty())
        emit dataReceived(data);
}

void SerialManager::handleError(QSerialPort::SerialPortError error)
{
    if (error != QSerialPort::NoError)
        emit errorOccurred(m_serial->errorString());
}