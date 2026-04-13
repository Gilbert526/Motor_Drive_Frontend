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

        connect(serial, &QSerialPort::readyRead, this, &MainWindow::readSerialData);
}

MainWindow::~MainWindow()
{
    if (serial->isOpen())
        serial->close();
    delete ui;
}

void MainWindow::refreshSerialPorts()
{
    ui->comboPort->clear();
    foreach (const QSerialPortInfo &info, QSerialPortInfo::availablePorts()) {
        ui->comboPort->addItem(info.portName());
    }
    if (ui->comboPort->count() == 0)
        ui->comboPort->addItem("No available ports");
}

void MainWindow::on_pushButtonStart_clicked()
{
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

    ui->pushButtonStart->setEnabled(true);
}

void MainWindow::on_pushButtonSend_clicked()
{
    if (!serial->isOpen()) {
        QMessageBox::warning(this, "Warning", "Please open the serial port first");
        return;
    }

    QString sendStr = ui->lineEditSend->text();
    if (sendStr.isEmpty())
        return;

    // 发送字符串（不自动添加换行，如果需要换行可以手动加 "\r\n"）
    QByteArray data = sendStr.toUtf8();
    qint64 written = serial->write(data);
    if (written != data.size()) {
        QMessageBox::warning(this, "Warning", "Incomplete data sent");
    } else {
        // 可选：将发送的内容也显示在接收区（回显）
        ui->plainTextEditReceive->appendPlainText(">> " + sendStr);
    }
}

void MainWindow::readSerialData()
{
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