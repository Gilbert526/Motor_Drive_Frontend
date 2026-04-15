#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "SerialManager.h"
#include "DataParser.h"
#include <QMessageBox>
#include <QSerialPortInfo>
#include <QDialog>
#include <QDialogButtonBox>
#include <QListWidgetItem>

MainWindow::MainWindow(QWidget *parent):
    QMainWindow(parent),
    ui(new Ui::MainWindow),
    m_serialManager(nullptr),
    m_dataParser(nullptr),
    m_serialThread(nullptr),
    m_maxWavePoints(20000),     // 最多存储20000点
    m_currentMaxPoints(500),
    m_plotPaused(false),
    m_syncingFromMask(false),
    m_packetCounter(0),
    m_packetIntervalSec(1.0 / DEFAULT_PACKET_FREQ_HZ) {
        ui->setupUi(this);

        // 初始化串口管理线程
        m_serialManager = new SerialManager();
        m_serialThread = new QThread(this);
        m_serialManager->moveToThread(m_serialThread);
        connect(m_serialThread, &QThread::finished, m_serialManager, &QObject::deleteLater);

        // 创建数据解析器（主线程）
        m_dataParser = new DataParser(this);

        // 信号连接
        connect(m_serialManager, &SerialManager::portOpened, this, &MainWindow::handleSerialPortOpened);
        connect(m_serialManager, &SerialManager::portClosed, this, &MainWindow::handleSerialPortClosed);
        connect(m_serialManager, &SerialManager::rawDataReceived, m_dataParser, &DataParser::parseData);
        connect(m_dataParser, &DataParser::parsedData, this, &MainWindow::handleNewData);

        connect(m_dataParser, &DataParser::maskReceived, this, &MainWindow::onMaskReceived);

        // Display received message in text box
        connect(m_serialManager, &SerialManager::rawDataReceived, this, [this](const QByteArray &data) {
            // Translate into UTF8 text
            QString text = QString::fromUtf8(data);
            
            // Filtering condition 1: Check for presence of replacement character (indicates decoding issues)
            if (text.contains(QChar::ReplacementCharacter)) {
                return;
            }
            
            // Filtering condition 2: Check for non-printable characters (except common whitespace)
            bool hasNonPrintable = false;
            for (QChar ch : text) {
                if (ch.isPrint() || ch == '\n' || ch == '\r' || ch == '\t') {
                    continue;
                }
                hasNonPrintable = true;
                break;
            }
            if (hasNonPrintable) {
                return;
            }
            
            // Display if it passes filters
            if (!text.isEmpty()) {
                ui->plainTextEditReceive->appendPlainText(text);
            }
        });

        // 启动串口线程
        m_serialThread->start();

        // 初始化示波器区域
        setupPlottingArea();

        // 加载字段列表到左侧
        loadAvailableFields();

        // 定时器刷新波形
        m_plotTimer = new QTimer(this);
        connect(m_plotTimer, &QTimer::timeout, this, &MainWindow::updatePlot);
        m_plotTimer->start(50);

        // 串口UI初始化
        refreshSerialPorts();
        ui->comboBaud->addItems({"9600", "19200", "38400", "57600", "115200", "230400", "460800", "921600"});
        ui->comboBaud->setCurrentText("115200");

        ui->pushButtonRefresh->setText("Refresh");
        ui->pushButtonSend->setText("->");
        ui->pushButtonStart->setText("Start");
        ui->pushButtonStop->setText("Stop");
        ui->pushButtonAudible->setText("Audible");
        ui->pushButtonReset->setText("Reset");
        ui->pushButtonPause->setText("⏸");

        updateUiForSerialState(false);
    }

MainWindow::~MainWindow() {
    if (m_serialThread->isRunning()) {
        m_serialThread->quit();
        m_serialThread->wait();
    }
    delete ui;
}

// ==================== 波形区域初始化 ====================
void MainWindow::setupPlottingArea() {
    // Obtain pointers to UI elements
    m_fieldList = ui->fieldListWidget;
    m_scrollArea = ui->scrollArea;
    m_oscContainer = ui->oscilloscopeContainer;
    m_sampleSlider = ui->sampleSlider;
    m_sampleLabel = ui->sampleLabel;

    // Set field list to single selection mode
    m_fieldList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_fieldList->setDragEnabled(false);
    m_fieldList->clear();
    // Get available fields from DataParser and populate the list with checkable items
    QStringList allFields = m_dataParser->getFieldNames();
    for (const QString &field : allFields) {
        QListWidgetItem *item = new QListWidgetItem(field);
        item->setText(field);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        item->setCheckState(Qt::Unchecked);
        m_fieldList->addItem(item);
    }
    connect(m_fieldList, &QListWidget::itemChanged, this, &MainWindow::onFieldCheckStateChanged);

    // Set up the oscilloscope container with a vertical layout
    if (m_oscContainer->layout() == nullptr) {
        m_oscLayout = new QVBoxLayout(m_oscContainer);
        m_oscContainer->setLayout(m_oscLayout);
    } else {
        m_oscLayout = qobject_cast<QVBoxLayout*>(m_oscContainer->layout());
    }
    m_oscLayout->setAlignment(Qt::AlignTop);

    // Sample slider configuration
    m_sampleSlider->setRange(100, 10000);
    m_sampleSlider->setSingleStep(100);
    m_sampleSlider->setPageStep(100);
    m_sampleSlider->setTickInterval(100);
    m_sampleSlider->setValue(m_currentMaxPoints);
    m_sampleLabel->setText(QString::number(m_currentMaxPoints));
    connect(m_sampleSlider, &QSlider::valueChanged, this, &MainWindow::on_sampleSlider_valueChanged);

    // Double click field to add new oscilloscope
    connect(m_fieldList, &QListWidget::itemDoubleClicked, this, &MainWindow::on_fieldList_itemDoubleClicked);

    // Add a scope by default
    addOscilloscope("Scope 1");
}

void MainWindow::addOscilloscope(const QString &title, int index) {
    OscilloscopeWidget *osc = new OscilloscopeWidget;
    if (!title.isEmpty())
        osc->setTitle(title);
    else
        osc->setTitle(QString("Scope %1").arg(m_oscilloscopes.size() + 1));

    // 连接配置请求信号（点击齿轮按钮时）
    connect(osc, &OscilloscopeWidget::fieldsChanged, this, [this, osc]() {
        on_oscilloscopeConfigRequested(osc);
    });
    connect(osc, &OscilloscopeWidget::removeRequested, this, [this, osc]() {
        removeOscilloscope(osc);
    });
    connect(osc, &OscilloscopeWidget::addBelowRequested, this, [this, osc]() {
        int idx = m_oscLayout->indexOf(osc);
        if (idx >= 0) {
            addOscilloscope(QString("Scope %1").arg(m_oscilloscopes.size() + 1), idx + 1);
        }
    });

    if (index < 0 || index > m_oscLayout->count()) {
        m_oscLayout->addWidget(osc);
        m_oscilloscopes.append(osc);
    } else {
        m_oscLayout->insertWidget(index, osc);
        m_oscilloscopes.insert(index, osc);
    }
}

void MainWindow::removeOscilloscope(OscilloscopeWidget *osc) {
    if (!osc) return;
    int idx = m_oscLayout->indexOf(osc);
    if (idx >= 0) {
        m_oscLayout->removeWidget(osc);
        m_oscilloscopes.removeAt(idx);
        osc->deleteLater();
    }
}

void MainWindow::loadAvailableFields() {
    if (!m_dataParser) return;
    QStringList allFields = m_dataParser->getFieldNames();
    m_fieldList->clear();
    for (const QString &field : allFields) {
        QListWidgetItem *item = new QListWidgetItem(field);
        item->setText(field);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        item->setCheckState(Qt::Unchecked);
        m_fieldList->addItem(item);
    }
}

void MainWindow::on_fieldList_itemDoubleClicked(QListWidgetItem *item) {
    // 双击字段：创建新示波器并添加该字段
    addOscilloscope();
    OscilloscopeWidget *newOsc = m_oscilloscopes.last();
    newOsc->setFields({item->text()});
}

void MainWindow::on_oscilloscopeConfigRequested(OscilloscopeWidget *osc) {
    // 弹出多选对话框，让用户选择要显示的字段
    QDialog dialog(this);
    dialog.setWindowTitle("Select Fields to Display");
    QListWidget *list = new QListWidget(&dialog);
    list->setSelectionMode(QAbstractItemView::MultiSelection);
    QStringList allFields = m_dataParser->getFieldNames();
    QStringList currentFields = osc->getFields();

    for (const QString &field : allFields) {
        QListWidgetItem *item = new QListWidgetItem(field);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(currentFields.contains(field) ? Qt::Checked : Qt::Unchecked);
        list->addItem(item);
    }

    QDialogButtonBox *buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttonBox, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    QVBoxLayout *layout = new QVBoxLayout(&dialog);
    layout->addWidget(list);
    layout->addWidget(buttonBox);

    if (dialog.exec() == QDialog::Accepted) {
        QStringList selected;
        for (int i = 0; i < list->count(); ++i) {
            if (list->item(i)->checkState() == Qt::Checked)
                selected << list->item(i)->text();
        }
        osc->setFields(selected);
    }
}

void MainWindow::on_sampleSlider_valueChanged(int value) {
    // 1. Calculate the "snapped" value
    int snappedValue = (value / 100) * 100;

    // 2. Block signals temporarily to prevent infinite loops when we reset the value
    m_sampleSlider->blockSignals(true);
    m_sampleSlider->setValue(snappedValue);
    m_sampleSlider->blockSignals(false);

    // 3. Update your label and internal variables
    m_currentMaxPoints = snappedValue;
    m_sampleLabel->setText(QString::number(snappedValue));
    updateAllPlots();
}

void MainWindow::updateAllPlots() {
    for (OscilloscopeWidget *osc : m_oscilloscopes) {
        osc->updatePlot(m_waveData, m_timeStamps, m_currentMaxPoints);
    }
}

void MainWindow::updatePlot() {
    if (!m_plotPaused) {
        updateAllPlots();
    }
}

// ==================== 数据处理 ====================
void MainWindow::handleNewData(const QHash<QString, double> &values) {
    // Calculate current time
    double currentTime = m_packetCounter * m_packetIntervalSec;
    m_packetCounter++;
    addTimeStamp(currentTime);
    // 追加到波形缓冲区
    for (auto it = values.begin(); it != values.end(); ++it) {
        const QString &field = it.key();
        double val = it.value();
        QVector<double> &vec = m_waveData[field];
        vec.append(val);
        if (vec.size() > m_maxWavePoints) {
            vec.remove(0, vec.size() - m_maxWavePoints);
        }
    }
    // 可选：在接收区显示关键数值（调试用，可注释）
    // if (values.contains("RPM")) {
    //     ui->plainTextEditReceive->appendPlainText(QString("RPM: %1").arg(values["RPM"], 0, 'f', 1));
    // }
}

void MainWindow::onMaskReceived(quint32 mask) {
    if (m_syncingFromMask) return;
    m_syncingFromMask = true;

    static quint32 lastMask = 0;
    if (mask == lastMask) {
        m_syncingFromMask = false;   // 关键：必须重置标志
        return;
    }
    lastMask = mask;

    // 遍历字段列表中的每个项
    for (int i = 0; i < m_fieldList->count(); ++i) {
        QListWidgetItem *item = m_fieldList->item(i);
        if (!item) continue;
        // 获取字段名对应的掩码位（需要字段定义信息）
        QString fieldName = item->text();
        quint32 bitMask = m_dataParser->getMaskForField(fieldName);
        bool shouldCheck = (mask & bitMask) != 0;
        if (shouldCheck != (item->checkState() == Qt::Checked)) {
            item->setCheckState(shouldCheck ? Qt::Checked : Qt::Unchecked);
        }
    }

    m_syncingFromMask = false;
}

// ==================== 串口处理 ====================
void MainWindow::refreshSerialPorts() {
    ui->comboPort->clear();
    foreach (const QSerialPortInfo &info, QSerialPortInfo::availablePorts()) {
        ui->comboPort->addItem(info.portName());
    }
    if (ui->comboPort->count() == 0)
        ui->comboPort->addItem("No available ports");
}

void MainWindow::updateUiForSerialState(bool isOpen) {
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
    QMetaObject::invokeMethod(m_serialManager, "sendData",
                              Qt::QueuedConnection,
                              Q_ARG(QByteArray, data));
    ui->plainTextEditReceive->appendPlainText(">> " + cmd);
}

void MainWindow::on_pushButtonStartToggle_clicked() {
    if (m_serialManager->thread() == nullptr) return;

    if (ui->pushButtonStartToggle->text() == "Stop") {
        QMetaObject::invokeMethod(m_serialManager, "closeSerialPort", Qt::QueuedConnection);
    } else {
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
    QMetaObject::invokeMethod(m_serialManager, "sendData",
                              Qt::QueuedConnection,
                              Q_ARG(QByteArray, data));
    ui->plainTextEditReceive->appendPlainText(">> " + sendStr.trimmed());
}

void MainWindow::on_pushButtonStart_clicked()   { sendCommand("start\r\n"); }
void MainWindow::on_pushButtonStop_clicked()    { sendCommand("stop\r\n"); }
void MainWindow::on_pushButtonAlign_clicked()   { sendCommand("align\r\n"); }
void MainWindow::on_pushButtonAudible_clicked() { sendCommand("audible\r\n"); }
void MainWindow::on_pushButtonReset_clicked()   { sendCommand("reset\r\n"); }
void MainWindow::on_pushButtonFocManual_clicked() { sendCommand("foc manual\r\n"); }
void MainWindow::on_pushButtonPreset1_clicked() { sendCommand("log preset 1\r\n"); }
void MainWindow::on_pushButtonPreset2_clicked() { sendCommand("log preset 2\r\n"); }
void MainWindow::on_pushButtonPreset3_clicked() { sendCommand("log preset 3\r\n"); }
void MainWindow::on_pushButtonPreset4_clicked() { sendCommand("log preset 4\r\n"); }
void MainWindow::on_pushButtonBin_clicked()     { sendCommand("log bin\r\n"); }
void MainWindow::on_pushButtonUtf8_clicked()    { sendCommand("log utf8\r\n"); }

void MainWindow::on_pushButtonPause_clicked() {
    m_plotPaused = !m_plotPaused;
    ui->pushButtonPause->setText(m_plotPaused ? "▶" : "⏸");
    if (!m_plotPaused) {
        // 如果从暂停恢复，立即刷新一次
        updateAllPlots();
    }
}

void MainWindow::onFieldCheckStateChanged(QListWidgetItem *item) {
    if (m_syncingFromMask) return;
    if (!item) return;
    QString fieldName = item->text();
    bool checked = (item->checkState() == Qt::Checked);
    
    // 构造命令字符串
    QString cmdName = m_dataParser->getCommandNameForField(fieldName);
    QString cmd = checked ? QString("log add %1\r\n").arg(cmdName)
                          : QString("log rm %1\r\n").arg(cmdName);
    sendCommand(cmd);   // 复用已有的 sendCommand
}

void MainWindow::handleSerialPortOpened(bool success, const QString &errorMsg) {
    ui->pushButtonStartToggle->setEnabled(true);
    if (success) {
        updateUiForSerialState(true);
        syncFieldCheckStates();
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

void MainWindow::syncFieldCheckStates()
{
    for (int i = 0; i < m_fieldList->count(); ++i) {
        QListWidgetItem *item = m_fieldList->item(i);
        if (item->checkState() == Qt::Checked) {
            sendCommand(QString("log add %1\r\n").arg(m_dataParser->getCommandNameForField(item->text())));
        }
    }
}

void MainWindow::addTimeStamp(double offsetSec)
{
    m_timeStamps.append(offsetSec);
    if (m_timeStamps.size() > m_maxWavePoints) {
        m_timeStamps.remove(0, m_timeStamps.size() - m_maxWavePoints);
    }
}