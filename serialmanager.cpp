#include "serialmanager.h"
#include <QDebug>

SerialManager::SerialManager(QObject *parent) : QObject(parent), m_serial(nullptr) {
    m_serial = new QSerialPort(this);
    connect(m_serial, &QSerialPort::readyRead, this, &SerialManager::handleReadyRead);
}

SerialManager::~SerialManager()
{
    if (m_serial->isOpen())
        m_serial->close();
}

void SerialManager::openSerialPort(const QString &portName, qint32 baudRate)
{
    if (m_serial->isOpen()) {
        emit portOpened(false, "Serial port already open");
        return;
    }
    m_serial->setPortName(portName);
    m_serial->setBaudRate(baudRate);
    m_serial->setDataBits(QSerialPort::Data8);
    m_serial->setParity(QSerialPort::NoParity);
    m_serial->setStopBits(QSerialPort::OneStop);
    m_serial->setFlowControl(QSerialPort::NoFlowControl);

    if (!m_serial->open(QIODevice::ReadWrite)) {
        emit portOpened(false, m_serial->errorString());
        return;
    }
    emit portOpened(true, "");
}

void SerialManager::closeSerialPort()
{
    if (m_serial->isOpen()) {
        m_serial->close();
    }
    emit portClosed();
}

void SerialManager::sendData(const QByteArray &data)
{
    if (m_serial->isOpen()) {
        m_serial->write(data);
    }
}

void SerialManager::handleReadyRead()
{
    QByteArray data = m_serial->readAll();
    if (!data.isEmpty()) {
        emit rawDataReceived(data);
    }
}