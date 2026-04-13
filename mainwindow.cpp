#include "mainwindow.h"
#include "ui_mainwindow.h"
#include <QMessageBox>
#include <QDebug>

MainWindow::MainWindow(QWidget *parent):
    QMainWindow(parent),
    ui(new Ui::MainWindow),
    serial(new QSerialPort(this)) {
        ui->setupUi(this);

        refreshSerialPorts();

        ui->comboBaud->addItems({"9600", "19200", "38400", "57600", "115200", "230400", "460800", "921600"});
        ui->comboBaud->setCurrentText("115200");

        ui->pushButtonRefresh->setText("Refresh");
        ui->pushButtonSend->setText("->");
        ui->pushButtonStart->setText("Start");
        ui->pushButtonStop->setText("Stop");
        ui->pushButtonAudible->setText("Audible");
        ui->pushButtonReset->setText("Reset");

        connect(serial, &QSerialPort::readyRead, this, &MainWindow::readSerialData);
        connect(serial, &QSerialPort::errorOccurred, this, &MainWindow::handleSerialError);

        updateToggleButtonState(false);
}

MainWindow::~MainWindow() {
    if (serial->isOpen())
        serial->close();
    delete ui;
}

void MainWindow::refreshSerialPorts() {
    ui->comboPort->clear();
    foreach (const QSerialPortInfo &info, QSerialPortInfo::availablePorts()) {
        ui->comboPort->addItem(info.portName());
    }
    if (ui->comboPort->count() == 0)
        ui->comboPort->addItem("No available ports");
}

void MainWindow::updateToggleButtonState(bool isOpen) {
    if (isOpen) {
        // Disable port/baud selection and refresh button when serial is open
        ui->pushButtonStartToggle->setText("Stop");
        ui->comboPort->setEnabled(false);
        ui->comboBaud->setEnabled(false);
        ui->pushButtonRefresh->setEnabled(false);
    } else {
        ui->pushButtonStartToggle->setText("Start");
        ui->comboPort->setEnabled(true);
        ui->comboBaud->setEnabled(true);
        ui->pushButtonRefresh->setEnabled(true);
    }
}

void MainWindow::sendCommand(const QString &cmd) {
    if (!serial->isOpen()) {
        QMessageBox::warning(this, "Warning", "Please open the serial port first");
        return;
    }
    QByteArray data = cmd.toUtf8();
    serial->write(data);
    ui->plainTextEditReceive->appendPlainText(">> " + cmd);
}

void MainWindow::on_pushButtonStartToggle_clicked() {
    if (serial->isOpen()) {
        QMessageBox::warning(this, "Warning", "Serial port is already open");
        return;
    }

    QString portName = ui->comboPort->currentText();
    qint32 baudRate = ui->comboBaud->currentText().toInt();

    serial->setPortName(portName);
    serial->setBaudRate(baudRate);
    serial->setDataBits(QSerialPort::Data8);
    serial->setParity(QSerialPort::NoParity);
    serial->setStopBits(QSerialPort::OneStop);
    serial->setFlowControl(QSerialPort::NoFlowControl);

    if (!serial->open(QIODevice::ReadWrite)) {
        QMessageBox::critical(this, "Error", "Failed to open serial port: " + serial->errorString());
        return;
    }

    ui->pushButtonStartToggle->setEnabled(true);
}

void MainWindow::on_pushButtonRefresh_clicked() {
    if (serial->isOpen()) {
        // On -> Off
        serial->close();
        updateToggleButtonState(false);
        statusBar()->showMessage("Serial port closed", 3000);
    } else {
        // Off -> On
        QString portName = ui->comboPort->currentText();
        qint32 baudRate = ui->comboBaud->currentText().toInt();

        if (portName == "No available ports") {
            QMessageBox::warning(this, "Warning", "No available ports, please refresh the list");
            return;
        }

        serial->setPortName(portName);
        serial->setBaudRate(baudRate);
        serial->setDataBits(QSerialPort::Data8);
        serial->setParity(QSerialPort::NoParity);
        serial->setStopBits(QSerialPort::OneStop);
        serial->setFlowControl(QSerialPort::NoFlowControl);

        if (!serial->open(QIODevice::ReadWrite)) {
            QMessageBox::critical(this, "Error", "Failed to open serial port: " + serial->errorString());
            updateToggleButtonState(false);
            return;
        }

        updateToggleButtonState(true);
        statusBar()->showMessage(QString("Opened on %1, Baud Rate %2").arg(portName).arg(baudRate), 3000);
    }
}

void MainWindow::on_pushButtonSend_clicked() {
    if (!serial->isOpen()) {
        QMessageBox::warning(this, "Warning", "Please open the serial port first");
        return;
    }

    QString sendStr = ui->lineEditSend->text();
    if (sendStr.isEmpty())
        return;

    if (!sendStr.endsWith("\r\n")) {
        sendStr += "\r\n";
    }

    // Send the string as UTF-8 encoded bytes
    QByteArray data = sendStr.toUtf8();
    qint64 written = serial->write(data);
    if (written != data.size()) {
        QMessageBox::warning(this, "Warning", "Incomplete data sent");
    } else {
        // Echo the sent command in the receive area for confirmation
        ui->plainTextEditReceive->appendPlainText(">> " + sendStr);
    }
}

void MainWindow::on_pushButtonStart_clicked() { sendCommand("Start\r\n"); }

void MainWindow::on_pushButtonStop_clicked() { sendCommand("Stop\r\n"); }

void MainWindow::on_pushButtonAudible_clicked() { sendCommand("Audible\r\n"); }

void MainWindow::on_pushButtonReset_clicked() { sendCommand("Reset\r\n"); }

void MainWindow::readSerialData() {
    if (!serial->isOpen())
        return;

    QByteArray data = serial->readAll();
    if (data.isEmpty())
        return;

    // 将收到的原始数据以字符串形式显示到接收区
    // 注意：如果下位机发送的是二进制，这里可能会显示乱码，但当前我们只测试文本
    QString received = QString::fromUtf8(data);
    ui->plainTextEditReceive->appendPlainText("<< " + received);
}

void MainWindow::handleSerialError(QSerialPort::SerialPortError error) {
    if (error == QSerialPort::ResourceError) {

        if (serial->isOpen()) {
            serial->close();
        }
        updateToggleButtonState(false);
        statusBar()->showMessage("Device lost, automatically closed", 5000);
    }
}