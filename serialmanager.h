#ifndef SERIALMANAGER_H
#define SERIALMANAGER_H

#include <QObject>
#include <QSerialPort>
#include <QByteArray>

/**
 * @brief Thin QObject wrapper around QSerialPort used from the serial worker thread.
 *
 * The main window talks to this object exclusively through queued signals/slots.
 * That keeps blocking serial-port I/O and readyRead notifications away from the
 * UI thread while still forwarding raw bytes to DataParser as soon as they arrive.
 */
class SerialManager : public QObject {
    Q_OBJECT

public:
    explicit SerialManager(QObject *parent = nullptr);
    ~SerialManager();

public slots:
    /**
     * @brief Configure and open the named serial port.
     *
     * Emits portOpened(true, "") on success, or portOpened(false, reason) if the
     * port is already open or QSerialPort reports an error.
     */
    void openSerialPort(const QString &portName, qint32 baudRate);

    /**
     * @brief Close the active serial port, if any, and notify the UI layer.
     */
    void closeSerialPort();

    /**
     * @brief Write an already-framed command byte array to the device.
     *
     * The caller is responsible for adding command terminators such as CR/LF.
     * Data is ignored when the port is closed.
     */
    void sendData(const QByteArray &data);

signals:
    void rawDataReceived(const QByteArray &data);  // Sends raw bytes to the parser.
    void portOpened(bool success, const QString &errorMsg);
    void portClosed();

private slots:
    void handleReadyRead();

private:
    QSerialPort *m_serial;
};

#endif // SERIALMANAGER_H
