#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "SerialManager.h"
#include "DataParser.h"
#include <QMessageBox>
#include <QDebug>
#include <QSerialPortInfo>

MainWindow::MainWindow(QWidget *parent):
    QMainWindow(parent),
    ui(new Ui::MainWindow),
    m_serialManager(nullptr),
    m_dataParser(nullptr),
    m_serialThread(nullptr),
    m_maxWavePoints(2000) {
        ui->setupUi(this);

        // Initialize SerialManager and move it to a separate thread
        m_serialManager = new SerialManager();
        m_serialThread = new QThread(this);
        m_serialManager->moveToThread(m_serialThread);
        connect(m_serialThread, &QThread::finished, m_serialManager, &QObject::deleteLater);

        // Create DataParser in the main thread
        m_dataParser = new DataParser(this);

        // Connect signals and slots
        // SerialManager signals
        connect(m_serialManager, &SerialManager::portOpened, this, &MainWindow::handleSerialPortOpened);
        connect(m_serialManager, &SerialManager::portClosed, this, &MainWindow::handleSerialPortClosed);

        // Connect raw data signal to DataParser's parseData slot
        connect(m_serialManager, &SerialManager::rawDataReceived, m_dataParser, &DataParser::parseData);

        // Connect DataParser's parsedData signal to MainWindow's handleNewData slot
        connect(m_dataParser, &DataParser::parsedData, this, &MainWindow::handleNewData);

        // Start the serial thread
        m_serialThread->start();

        // Setup the plot
        setupPlot();

        // Start the plot update timer (50ms interval)
        m_plotTimer = new QTimer(this);
        connect(m_plotTimer, &QTimer::timeout, this, &MainWindow::updatePlot);
        m_plotTimer->start(50);

        refreshSerialPorts();

        ui->comboBaud->addItems({"9600", "19200", "38400", "57600", "115200", "230400", "460800", "921600"});
        ui->comboBaud->setCurrentText("115200");

        ui->pushButtonRefresh->setText("Refresh");
        ui->pushButtonSend->setText("->");
        ui->pushButtonStart->setText("Start");
        ui->pushButtonStop->setText("Stop");
        ui->pushButtonAudible->setText("Audible");
        ui->pushButtonReset->setText("Reset");

        m_plotFields = {"RPM", "IA", "IB", "IC"};
        refreshPlotFields();

        updateUiForSerialState(false);
}

MainWindow::~MainWindow() {
    if (m_serialThread->isRunning()) {
        m_serialThread->quit();
        m_serialThread->wait();
    }
    delete ui;
}

/*---------Plotting---------*/
void MainWindow::setupPlot() {
    // 获取 UI 中的 QCustomPlot 控件（已在 Designer 中提升）
    m_customPlot = ui->customPlot;
    m_customPlot->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);
    m_customPlot->xAxis->setLabel("Sample");
    m_customPlot->yAxis->setLabel("Value");
    m_customPlot->legend->setVisible(true);
}

void MainWindow::addGraphForField(const QString &fieldName, const QColor &color) {
    QCPGraph *graph = m_customPlot->addGraph();
    graph->setName(fieldName);
    graph->setPen(QPen(color, 1.5));
    m_graphs[fieldName] = graph;
}

void MainWindow::refreshPlotFields() {
    // 简单起见，根据 m_plotFields 列表添加曲线
    // 实际应用中可以提供复选框让用户选择
    m_customPlot->clearGraphs();
    m_graphs.clear();

    QList<QColor> colors = {Qt::red, Qt::green, Qt::blue, Qt::magenta, Qt::cyan, Qt::darkYellow};
    for (int i = 0; i < m_plotFields.size(); ++i) {
        addGraphForField(m_plotFields[i], colors[i % colors.size()]);
    }
}

void MainWindow::handleNewData(const QHash<QString, double> &values) {
    // 将新数据追加到波形缓冲区
    for (auto it = values.begin(); it != values.end(); ++it) {
        const QString &field = it.key();
        double val = it.value();
        QVector<double> &vec = m_waveData[field];
        vec.append(val);
        if (vec.size() > m_maxWavePoints) {
            vec.remove(0, vec.size() - m_maxWavePoints);
        }
    }

    //// 可选：在接收区显示关键字段数值（调试用）
    //if (values.contains("RPM")) {
    //    ui->plainTextEditReceive->appendPlainText(QString("RPM: %1").arg(values["RPM"], 0, 'f', 1));
    //}
}

void MainWindow::updatePlot() {
    // 更新每条曲线的数据
    for (const QString &field : m_plotFields) {
        QCPGraph *graph = m_graphs.value(field);
        if (!graph) continue;
        const QVector<double> &data = m_waveData[field];
        if (data.isEmpty()) continue;

        // 构造 X 轴坐标（0,1,2,...）
        QVector<double> x(data.size());
        for (int i = 0; i < data.size(); ++i) x[i] = i;

        graph->setData(x, data);
    }

    // 自动调整 X 轴范围（显示最近 500 点）
    int maxPoints = 0;
    for (const QString &field : m_plotFields) {
        maxPoints = qMax(maxPoints, m_waveData[field].size());
    }
    if (maxPoints > 500) {
        m_customPlot->xAxis->setRange(maxPoints - 500, maxPoints);
    } else {
        m_customPlot->xAxis->setRange(0, 500);
    }

    // 自动调整 Y 轴范围（可选，根据当前所有曲线的值）
    m_customPlot->rescaleAxes(true);
    m_customPlot->replot();
}


/*---------Serial Port Handling---------*/
void MainWindow::refreshSerialPorts() {
    ui->comboPort->clear();
    foreach (const QSerialPortInfo &info, QSerialPortInfo::availablePorts()) {
        ui->comboPort->addItem(info.portName());
    }
    if (ui->comboPort->count() == 0)
        ui->comboPort->addItem("No available ports");
}

void MainWindow::updateUiForSerialState(bool isOpen)
{
    if (isOpen) {
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
    if (!m_serialManager) return;
    QByteArray data = cmd.toUtf8();
    // 通过信号发送到 SerialManager 线程
    QMetaObject::invokeMethod(m_serialManager, "sendData",
                              Qt::QueuedConnection,
                              Q_ARG(QByteArray, data));
    ui->plainTextEditReceive->appendPlainText(">> " + cmd);
}

void MainWindow::on_pushButtonStartToggle_clicked() {
    if (m_serialManager->thread() == nullptr) return;

    if (ui->pushButtonStartToggle->text() == "Stop") {
        QMetaObject::invokeMethod(m_serialManager, "closeSerialPort", Qt::QueuedConnection);
    }
    else {
        QString portName = ui->comboPort->currentText();
        qint32 baudRate = ui->comboBaud->currentText().toInt();
        
        if (portName == "No available ports") {
            QMessageBox::warning(this, "Warning", "No available ports, please refresh the list");
            return;
        }
        
        QMetaObject::invokeMethod(m_serialManager, "openSerialPort",
                                  Qt::QueuedConnection,
                                  Q_ARG(QString, portName),
                                  Q_ARG(qint32, baudRate));

        ui->pushButtonStartToggle->setEnabled(false);
    }
}

void MainWindow::on_pushButtonRefresh_clicked() {
    refreshSerialPorts();
}

void MainWindow::on_pushButtonSend_clicked() {
    QString sendStr = ui->lineEditSend->text();
    if (sendStr.isEmpty()) return;

    if (!sendStr.endsWith("\r\n"))
        sendStr += "\r\n";
    QByteArray data = sendStr.toUtf8();

    // 发送到串口线程
    QMetaObject::invokeMethod(m_serialManager, "sendData",
                              Qt::QueuedConnection,
                              Q_ARG(QByteArray, data));
    ui->plainTextEditReceive->appendPlainText(">> " + sendStr.trimmed());
}

void MainWindow::on_pushButtonStart_clicked() { sendCommand("start\r\n"); }

void MainWindow::on_pushButtonStop_clicked() { sendCommand("stop\r\n"); }

void MainWindow::on_pushButtonAlign_clicked() { sendCommand("align\r\n"); }

void MainWindow::on_pushButtonAudible_clicked() { sendCommand("audible\r\n"); }

void MainWindow::on_pushButtonReset_clicked() { sendCommand("reset\r\n"); }

void MainWindow::on_pushButtonPreset1_clicked() { sendCommand("log preset 1\r\n"); }

void MainWindow::on_pushButtonPreset2_clicked() { sendCommand("log preset 2\r\n"); }

void MainWindow::on_pushButtonPreset3_clicked() { sendCommand("log preset 3\r\n"); }

void MainWindow::on_pushButtonPreset4_clicked() { sendCommand("log preset 4\r\n"); }

/*--------Serial Port Status Handlers--------*/
void MainWindow::handleSerialPortOpened(bool success, const QString &errorMsg) {
    ui->pushButtonStartToggle->setEnabled(true);
    if (success) {
        updateUiForSerialState(true);
        statusBar()->showMessage("Serial port opened", 3000);
    } else {
        updateUiForSerialState(false);
        QMessageBox::critical(this, "Error", "Failed to open serial port: " + errorMsg);
    }
}

void MainWindow::handleSerialPortClosed() {
    updateUiForSerialState(false);
    statusBar()->showMessage("Serial port closed", 3000);
}