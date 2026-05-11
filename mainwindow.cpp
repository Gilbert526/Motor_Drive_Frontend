#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "audiolevelmeter.h"
#include "SerialManager.h"
#include "DataParser.h"
#include <QMessageBox>
#include <QSerialPortInfo>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QListWidgetItem>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QSet>
#include <QWheelEvent>
#include <cmath>
#include <utility>

MainWindow::MainWindow(QWidget *parent):
    QMainWindow(parent),
    ui(new Ui::MainWindow),
    m_serialManager(nullptr),
    m_dataParser(nullptr),
    m_serialThread(nullptr),
    m_historyIndex(-1),
    m_maxWavePoints(20000),     // 最多存储20000点
    m_currentMaxPoints(500),
    m_plotPaused(false),
    m_plotDirty(false),
    m_faultAutoCaptureTriggerMask(0),
    m_faultAutoCaptureDisplayPoints(5000),
    m_faultAutoCapturePacketsAfterTrigger(50),
    m_faultAutoCapturePending(false),
    m_faultAutoCaptureSkipCurrentPacket(false),
    m_faultAutoCapturePacketsRemaining(0),
    m_isLogging(false),
    m_syncingFromMask(false),
    m_lastSpeedValue(0.0),
    m_lastTorqueValue(0.0),
    m_targetManuallyEdited(false),
    m_timeManuallyEdited(false),
    m_updatingTargetType(false),
    m_recordHistory(true),
    m_gaugeTimer(nullptr),
    m_plotUpdatesSuspended(false),
    m_lastTimestampTicks(0),
    m_hasLastTimestamp(false) {
        ui->setupUi(this);
        setWindowTitle("Tuning Master");

        // 初始化串口管理线程
        m_serialManager = new SerialManager();
        m_serialThread = new QThread(this);
        m_serialManager->moveToThread(m_serialThread);
        connect(m_serialThread, &QThread::finished, m_serialManager, &QObject::deleteLater);

        // 创建数据解析器（主线程）
        m_dataParser = new DataParser();
        m_dataParser->moveToThread(m_serialThread);
        connect(m_serialThread, &QThread::finished, m_dataParser, &QObject::deleteLater);
        updateFaultAutoCaptureMask();

        // 信号连接
        connect(m_serialManager, &SerialManager::portOpened, this, &MainWindow::handleSerialPortOpened);
        connect(m_serialManager, &SerialManager::portClosed, this, &MainWindow::handleSerialPortClosed);
        connect(m_serialManager, &SerialManager::rawDataReceived, m_dataParser, &DataParser::parseData);
        connect(m_dataParser, &DataParser::parsedData, this, &MainWindow::handleNewData);
        connect(m_dataParser, &DataParser::packetStatusReceived, this, &MainWindow::handlePacketStatus);
        connect(m_dataParser, &DataParser::configurationChanged, this, [this]() {
            updateFaultAutoCaptureMask();
            loadAvailableFields();
            setupGaugeArea();
            syncFieldCheckStates();
            updateStatusIndicators();
        });

        connect(m_dataParser, &DataParser::maskReceived, this, &MainWindow::onMaskReceived);

        // Display received message in text box
        connect(m_serialManager, &SerialManager::rawDataReceived, this, [this](const QByteArray &data) {
            const QByteArray syncBytes = QByteArray::fromHex("AA55");
            static QByteArray buffer;           // Buffer for original data
            static QByteArray lineBuffer;       // Accumulated incomplete text lines
            buffer.append(data);

            int idx = 0;
            while (idx < buffer.size()) {
                // Look for the next frame header (0xAA 0x55)
                int headerPos = buffer.indexOf(syncBytes, idx);
                if (headerPos == -1) {
                    // Preserve a partial sync header so the next chunk can complete a binary frame.
                    int endOfText = buffer.size();
                    const int partialHeaderLen = syncBytes.size() - 1;
                    if (partialHeaderLen > 0 &&
                        buffer.size() >= partialHeaderLen &&
                        buffer.right(partialHeaderLen) == syncBytes.left(partialHeaderLen)) {
                        endOfText -= partialHeaderLen;
                    }
                    if (endOfText > idx) {
                        processReceiveTextChunk(buffer.mid(idx, endOfText - idx), lineBuffer);
                        idx = endOfText;
                    }
                    break;
                }

                // Text data before the frame header (possibly incomplete line)
                if (headerPos > idx) {
                    processReceiveTextChunk(buffer.mid(idx, headerPos - idx), lineBuffer);
                }

                if (buffer.size() >= headerPos + m_dataParser->minimumFrameSize() &&
                    !m_dataParser->hasValidFrameMetadata(buffer, headerPos)) {
                    processReceiveTextChunk(syncBytes.left(1), lineBuffer);
                    idx = headerPos + 1;
                    continue;
                }

                // Try to get the binary frame length
                int frameLen = m_dataParser->getFrameLength(buffer, headerPos);
                if (frameLen == -1) {
                    // Insufficient data, retain the unprocessed portion from headerPos (incomplete frame header)
                    idx = headerPos;
                    break;
                }

                // Skip the entire binary frame
                idx = headerPos + frameLen;
            }

            // If the loop ends normally (idx reaches the end), clear the buffer
            if (idx >= buffer.size()) {
                buffer.clear();
            } else {
                // Retain the unprocessed portion (possibly an incomplete binary frame header)
                buffer = buffer.mid(idx);
            }
        });

        // 启动串口线程
        m_serialThread->start(QThread::HighPriority);

        // 初始化示波器区域
        setupPlottingArea();
        setupGaugeArea();
        updateStatusIndicators();

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

        ui->pushButtonRefresh->setText("⭮");
        ui->pushButtonRefresh->setToolTip("Refresh serial ports");
        ui->pushButtonSend->setText("➢");
        ui->pushButtonSend->setToolTip("Send command");
        ui->pushButtonFoc->setText("FOC");
        ui->pushButtonFoc->setToolTip("Start motor with FOC");
        ui->pushButtonVvvf->setText("VVVF");
        ui->pushButtonVvvf->setToolTip("Start motor with VVVF ramp-up");
        ui->pushButtonSixstep->setText("Sixstep");
        ui->pushButtonSixstep->setToolTip("Start motor with six-step commutation");
        ui->pushButtonAlign->setText("Align");
        ui->pushButtonAlign->setToolTip("Align motor electrical and mechanical zero positions");
        ui->pushButtonStop->setText("Stop");
        ui->pushButtonStop->setToolTip("Stop motor");
        ui->pushButtonAudible->setText("Audible");
        ui->pushButtonAudible->setToolTip("Toggle audible PWM frequencies");
        ui->pushButtonReset->setText("Reset");
        ui->pushButtonReset->setToolTip("Reset motor state");
        ui->pushButtonPause->setText("⏸️");
        ui->pushButtonSave->setCheckable(true);
        ui->pushButtonSave->setToolTip("Start or stop saving telemetry to CSV");
        ui->pushButtonSave->setStyleSheet(
            "QPushButton:checked {"
            " background-color: #31e63a;"
            " border: 1px solid #0a8a1f;"
            "}"
        );
        ui->plainTextEditReceive->setReadOnly(true);

        // Command line features
        ui->lineEditSend->installEventFilter(this);
        ui->targetSlider->installEventFilter(this);
        ui->timeSlider->installEventFilter(this);
        ui->incrementSlider->installEventFilter(this);

        updateUiForSerialState(false);

        /*--- Target Setting ---*/
        // Initialize target selection
        ui->comboBoxTargetSelection->addItems({"Speed", "Torque"});
        m_currentTargetType = ui->comboBoxTargetSelection->currentText();

        ui->comboBoxTargetSelection->setToolTip("Select target type for motor control (Speed or Torque)");
        ui->targetSlider->setToolTip("Adjust target value using slider");
        ui->timeSlider->setToolTip("Adjust time duration for ramping to target value, set to 0 for immediate change");
        ui->lineEditTarget->setToolTip("Manually enter target value (overrides slider)");
        ui->lineEditTime->setToolTip("Manually enter time duration in seconds (overrides time slider)");
        ui->pushButtonTargetSend->setToolTip("Send target command");
        connect(ui->lineEditTarget, &QLineEdit::returnPressed,
                this, &MainWindow::on_pushButtonTargetSend_clicked);

        // Initialize target slider
        updateTargetSliderLimits();
        on_comboBoxTargetSelection_currentIndexChanged(0);
        setTargetValue(0.0, true);
        // Initialize time slider
        ui->timeSlider->setRange(0, 60);
        ui->timeSlider->setSingleStep(1);
        ui->timeSlider->setValue(0);
        on_timeSlider_valueChanged(0);

        /*--- Tuning ---*/
        // Populate subsystem combo box
        ui->comboBoxTuneSubsystem->addItems({"speed", "id", "iq", "fw", "gain", "offset"});

        // Connect tuning parameter signals
        connect(ui->comboBoxTuneSubsystem, QOverload<int>::of(&QComboBox::currentIndexChanged),
        this, &MainWindow::on_comboBoxTuneSubsystem_currentIndexChanged);

        // Initialize tuning parameters for the first subsystem
        on_comboBoxTuneSubsystem_currentIndexChanged(0);

        // Initialize increment slider with predefined step values
        const QVector<double> stepValues = {0.00001, 0.00005, 0.0001, 0.0005, 0.001, 0.005, 0.01, 0.05, 0.1, 0.5, 1.0, 5.0, 10.0, 50.0, 100.0, 500.0, 1000.0, 5000.0, 10000.0};
        ui->incrementSlider->setRange(0, stepValues.size() - 1);
        ui->incrementSlider->setValue(10); // 默认 1.0
        on_incrementSlider_valueChanged(10);

        ui->pushButtonTuneEnquire->setToolTip("Enquire current parameter value and display in tuning value box");
        ui->pushButtonIncrement->setToolTip("Increment parameter");
        ui->pushButtonDecrement->setToolTip("Decrement parameter");
        ui->pushButtonTuneSend->setToolTip("Send tuning command");
        ui->pushButtonTuneUndo->setToolTip("Undo last tuning change");
        ui->incrementLabel->setText("1");
        ui->incrementSlider->setToolTip("Adjust tuning increment step");

        m_stepValues = {0.00001, 0.00005, 0.0001, 0.0005, 0.001, 0.005, 0.01, 0.05, 0.1, 0.5, 1.0, 5.0, 10.0, 50.0, 100.0, 500.0, 1000.0, 5000.0, 10000.0};
    }

MainWindow::~MainWindow() {
    stopTelemetryLogging();
    if (m_serialThread->isRunning()) {
        m_serialThread->quit();
        m_serialThread->wait();
    }
    delete ui;
}

QList<QColor> MainWindow::getPresetColors()
{
    return {
        Qt::red, Qt::green, Qt::blue, Qt::magenta,
        Qt::cyan, Qt::darkYellow, Qt::darkCyan, Qt::darkMagenta,
        QColor(240, 131, 0),
        QColor(106, 45, 151),
        QColor(255, 143, 191)
    };
}

QStringList MainWindow::getColorNames()
{
    return {"Red", "Green", "Blue", "Magenta", "Cyan", "Dark Yellow", "Dark Cyan", "Dark Magenta",
            "Mikan", "Purple", "Pink"};
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
    m_fieldList->setDragEnabled(true);
    m_fieldList->setDragDropMode(QAbstractItemView::DragOnly);
    m_fieldList->setDefaultDropAction(Qt::CopyAction);
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
    updateAllMoveButtons();
}

void MainWindow::setupGaugeArea()
{
    QLayout *existingLayout = ui->gaugeArea->layout();
    QHBoxLayout *gaugeLayout = qobject_cast<QHBoxLayout*>(existingLayout);
    if (!gaugeLayout) {
        delete existingLayout;
        gaugeLayout = new QHBoxLayout(ui->gaugeArea);
        gaugeLayout->setContentsMargins(6, 0, 6, 0);
        gaugeLayout->setSpacing(18);
        ui->gaugeArea->setLayout(gaugeLayout);
    }

    gaugeLayout->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    m_gaugeBindings.clear();
    while (QLayoutItem *item = gaugeLayout->takeAt(0)) {
        if (QWidget *widget = item->widget()) {
            widget->deleteLater();
        }
        delete item;
    }

    gaugeLayout->setAlignment(Qt::AlignCenter);
    gaugeLayout->addStretch(1);
    for (const GaugeDef &gauge : m_dataParser->getGauges()) {
        addGauge(gauge);
    }
    gaugeLayout->addStretch(1);

    if (!m_gaugeTimer) {
        m_gaugeTimer = new QTimer(this);
        connect(m_gaugeTimer, &QTimer::timeout, this, &MainWindow::flushGaugeUpdates);
        m_gaugeTimer->start(16);
    }
}

void MainWindow::addGauge(const GaugeDef &gauge)
{
    QHBoxLayout *gaugeLayout = qobject_cast<QHBoxLayout*>(ui->gaugeArea->layout());
    if (!gaugeLayout) {
        return;
    }

    double warningThreshold = gauge.minimum + (gauge.maximum - gauge.minimum) * 0.65;
    double criticalThreshold = gauge.minimum + (gauge.maximum - gauge.minimum) * 0.85;
    deriveGaugeThresholds(gauge, &warningThreshold, &criticalThreshold);

    AudioLevelMeter *meter = new AudioLevelMeter(ui->gaugeArea);
    meter->setTitle(gauge.name);
    meter->setUnit(gauge.topDisplayUnit);
    meter->setRange(gauge.minimum, gauge.maximum);
    meter->setThresholds(warningThreshold, criticalThreshold);
    meter->setThresholdHysteresisPercent(gauge.hysteresis);
    meter->setDivisionCount(gauge.divisions);
    meter->setPeakHoldMs(1000);

    gaugeLayout->addWidget(meter, 0);
    m_gaugeBindings.append({gauge.dataSource, meter});
}

void MainWindow::deriveGaugeThresholds(const GaugeDef &gauge,
                                       double *warningThreshold,
                                       double *criticalThreshold) const
{
    auto lowerBoundMagnitudeForColor = [&gauge](const QString &color) -> std::optional<double> {
        std::optional<double> best;
        for (const GaugeThresholdDef &threshold : gauge.thresholds) {
            if (threshold.color.compare(color, Qt::CaseInsensitive) != 0 || !threshold.hasLowerBound) {
                continue;
            }
            const double magnitude = qAbs(threshold.lowerBound);
            if (!best.has_value() || magnitude < best.value()) {
                best = magnitude;
            }
        }
        return best;
    };

    const std::optional<double> warning = lowerBoundMagnitudeForColor("yellow");
    const std::optional<double> critical = lowerBoundMagnitudeForColor("red");
    if (warning.has_value()) {
        *warningThreshold = warning.value();
    }
    if (critical.has_value()) {
        *criticalThreshold = critical.value();
    }
}

void MainWindow::updateGauges(const QHash<QString, double> &values)
{
    for (GaugeBinding &binding : m_gaugeBindings) {
        if (values.contains(binding.fieldName)) {
            binding.pendingValue = values.value(binding.fieldName);
            binding.hasPendingValue = true;
        }
    }
}

void MainWindow::flushGaugeUpdates()
{
    for (GaugeBinding &binding : m_gaugeBindings) {
        if (binding.meter && binding.hasPendingValue) {
            binding.meter->setValue(binding.pendingValue);
            binding.hasPendingValue = false;
        }
    }

    updateStatusIndicators();
}

void MainWindow::updateStatusIndicators()
{
    if (!m_dataParser) {
        return;
    }

    QSet<int> configuredIndicators;
    for (const IndicatorDef &indicator : m_dataParser->getIndicators()) {
        configuredIndicators.insert(indicator.indicator);
        QLabel *label = findChild<QLabel*>(QString("statusLed%1").arg(indicator.indicator));
        if (!label) {
            continue;
        }

        const IndicatorStatusDef *status = resolveIndicatorStatus(indicator);
        if (!status) {
            status = defaultIndicatorStatus(indicator);
        }

        if (status) {
            applyIndicatorStatus(label, status->displayText, status->color);
        } else {
            applyIndicatorStatus(label, indicator.name, "off");
        }
    }

    for (int i = 0; i <= 8; ++i) {
        if (!configuredIndicators.contains(i)) {
            QLabel *label = findChild<QLabel*>(QString("statusLed%1").arg(i));
            applyIndicatorStatus(label, "Reserve", "off");
        }
    }
}

void MainWindow::updateFaultAutoCaptureMask()
{
    if (!m_dataParser) {
        m_faultAutoCaptureTriggerMask = 0;
        return;
    }

    m_faultAutoCaptureTriggerMask = m_dataParser->getErrorMaskForType("overcurrent") |
                                    m_dataParser->getErrorMaskForType("undervoltage");
}

void MainWindow::applyIndicatorStatus(QLabel *label, const QString &text, const QString &colorName)
{
    if (!label) {
        return;
    }

    const QString normalizedColor = colorName.trimmed().toLower();
    const QString stateKey = text + "\n" + normalizedColor;
    if (label->property("indicatorStateKey").toString() == stateKey) {
        return;
    }
    label->setProperty("indicatorStateKey", stateKey);

    label->setText(text);

    QColor activeColor;
    if (normalizedColor == "green") {
        activeColor = QColor(36, 166, 78);
    } else if (normalizedColor == "yellow") {
        activeColor = QColor(228, 171, 48);
    } else if (normalizedColor == "red") {
        activeColor = QColor(210, 64, 55);
    } else if (normalizedColor != "off") {
        activeColor = QColor(colorName);
    }

    if (!activeColor.isValid()) {
        label->setStyleSheet(
            "QLabel {"
            " background-color: #d7d9dc;"
            " color: #555;"
            " border: 1px solid #aeb4ba;"
            " border-radius: 4px;"
            " font-weight: normal;"
            "}"
        );
        return;
    }

    const QColor textColor = activeColor.lightness() > 170 ? QColor(35, 35, 35) : QColor(Qt::white);
    label->setStyleSheet(QString(
        "QLabel {"
        " background-color: %1;"
        " color: %2;"
        " border: 1px solid %3;"
        " border-radius: 4px;"
        " font-weight: bold;"
        "}"
    ).arg(activeColor.name(), textColor.name(), activeColor.darker(130).name()));
}

const IndicatorStatusDef* MainWindow::resolveIndicatorStatus(const IndicatorDef &indicator) const
{
    if (indicator.type == "mode") {
        return resolveModeIndicatorStatus(indicator);
    }
    if (indicator.type == "condition") {
        return resolveConditionIndicatorStatus(indicator);
    }
    if (indicator.type == "bitwise") {
        return resolveBitwiseIndicatorStatus(indicator);
    }
    return defaultIndicatorStatus(indicator);
}

const IndicatorStatusDef* MainWindow::resolveModeIndicatorStatus(const IndicatorDef &indicator) const
{
    const QString source = indicator.dataSource.trimmed();
    const bool isControlModeSource =
        source.compare("modes", Qt::CaseInsensitive) == 0 ||
        source.compare("mode", Qt::CaseInsensitive) == 0 ||
        source.compare("control_mode", Qt::CaseInsensitive) == 0 ||
        source.compare("controlMode", Qt::CaseInsensitive) == 0;

    double sourceValue = 0.0;
    if (!resolveIndicatorDataSourceValue(source, &sourceValue)) {
        return defaultIndicatorStatus(indicator);
    }

    for (const IndicatorStatusDef &status : indicator.statuses) {
        if (!status.hasValue) {
            continue;
        }

        if (status.numericValue.has_value()) {
            if (qFuzzyCompare(sourceValue + 1.0, status.numericValue.value() + 1.0)) {
                return &status;
            }
            continue;
        }

        if (!isControlModeSource || !m_dataParser) {
            continue;
        }

        const std::optional<quint8> modeValue = m_dataParser->getControlModeValueForName(status.valueName);
        if (modeValue.has_value() &&
            qFuzzyCompare(sourceValue + 1.0, static_cast<double>(modeValue.value()) + 1.0)) {
            return &status;
        }
    }

    return defaultIndicatorStatus(indicator);
}

const IndicatorStatusDef* MainWindow::resolveConditionIndicatorStatus(const IndicatorDef &indicator) const
{
    double value = 0.0;
    if (!resolveIndicatorDataSourceValue(indicator.dataSource, &value)) {
        return defaultIndicatorStatus(indicator);
    }

    for (const IndicatorStatusDef &status : indicator.statuses) {
        if (status.numericValue.has_value() &&
            !status.hasLowerBound &&
            !status.hasUpperBound &&
            qFuzzyCompare(value + 1.0, status.numericValue.value() + 1.0)) {
            return &status;
        }
    }

    for (const IndicatorStatusDef &status : indicator.statuses) {
        if (status.hasLowerBound && status.hasUpperBound &&
            qFuzzyCompare(status.lowerBound + 1.0, status.upperBound + 1.0) &&
            qFuzzyCompare(value + 1.0, status.lowerBound + 1.0)) {
            return &status;
        }
    }

    for (const IndicatorStatusDef &status : indicator.statuses) {
        if (!status.hasLowerBound && !status.hasUpperBound) {
            continue;
        }
        const bool aboveLower = !status.hasLowerBound || value >= status.lowerBound;
        const bool belowUpper = !status.hasUpperBound || value < status.upperBound;
        if (aboveLower && belowUpper) {
            return &status;
        }
    }

    return defaultIndicatorStatus(indicator);
}

const IndicatorStatusDef* MainWindow::resolveBitwiseIndicatorStatus(const IndicatorDef &indicator) const
{
    double numericSourceValue = 0.0;
    if (!resolveIndicatorDataSourceValue(indicator.dataSource, &numericSourceValue)) {
        return defaultIndicatorStatus(indicator);
    }
    const quint64 sourceValue = static_cast<quint64>(numericSourceValue);

    QList<const IndicatorStatusDef*> matches;
    for (const IndicatorStatusDef &status : indicator.statuses) {
        if (status.hasBit && (sourceValue & (1ULL << status.bit))) {
            matches.append(&status);
        }
    }

    if (matches.isEmpty()) {
        return defaultIndicatorStatus(indicator);
    }
    if (matches.size() == 1) {
        return matches.first();
    }

    double totalDuration = 0.0;
    for (const IndicatorStatusDef *status : matches) {
        totalDuration += qMax(0.05, status->timeSec);
    }
    if (totalDuration <= 0.0) {
        return matches.first();
    }

    const double cycleTime = std::fmod(QDateTime::currentMSecsSinceEpoch() / 1000.0, totalDuration);
    double accumulator = 0.0;
    for (const IndicatorStatusDef *status : matches) {
        accumulator += qMax(0.05, status->timeSec);
        if (cycleTime < accumulator) {
            return status;
        }
    }

    return matches.first();
}

const IndicatorStatusDef* MainWindow::defaultIndicatorStatus(const IndicatorDef &indicator) const
{
    for (const IndicatorStatusDef &status : indicator.statuses) {
        if (!status.hasValue && !status.hasBit && !status.hasLowerBound && !status.hasUpperBound) {
            return &status;
        }
    }
    return nullptr;
}

bool MainWindow::resolveIndicatorDataSourceValue(const QString &dataSource, double *value) const
{
    if (!value) {
        return false;
    }

    const QString source = dataSource.trimmed();
    if (source.isEmpty() || source.compare("Null", Qt::CaseInsensitive) == 0) {
        return false;
    }

    if (source.compare("errors", Qt::CaseInsensitive) == 0 ||
        source.compare("error", Qt::CaseInsensitive) == 0 ||
        source.compare("error_code", Qt::CaseInsensitive) == 0 ||
        source.compare("errorCode", Qt::CaseInsensitive) == 0) {
        *value = static_cast<double>(m_telemetryStatus.errorCode);
        return true;
    }

    if (source.compare("modes", Qt::CaseInsensitive) == 0 ||
        source.compare("mode", Qt::CaseInsensitive) == 0 ||
        source.compare("control_mode", Qt::CaseInsensitive) == 0 ||
        source.compare("controlMode", Qt::CaseInsensitive) == 0) {
        if (!m_telemetryStatus.controlModeKnown) {
            return false;
        }
        *value = static_cast<double>(m_telemetryStatus.controlMode);
        return true;
    }

    if (!m_latestTelemetryValues.contains(source)) {
        return false;
    }

    *value = m_latestTelemetryValues.value(source);
    return true;
}

void MainWindow::addOscilloscope(const QString &title, int index) {
    OscilloscopeWidget *osc = new OscilloscopeWidget;
    osc->setColorList(getPresetColors());
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

    connect(osc, &OscilloscopeWidget::moveUpRequested, this, &MainWindow::onMoveUpRequested);
    connect(osc, &OscilloscopeWidget::moveDownRequested, this, &MainWindow::onMoveDownRequested);

    connect(osc, &OscilloscopeWidget::refreshRequested, this, &MainWindow::updateAllPlots);

    if (index < 0 || index > m_oscLayout->count()) {
        m_oscLayout->addWidget(osc);
        m_oscilloscopes.append(osc);
    } else {
        m_oscLayout->insertWidget(index, osc);
        m_oscilloscopes.insert(index, osc);
    }

    updateAllMoveButtons();
}

void MainWindow::removeOscilloscope(OscilloscopeWidget *osc) {
    if (!osc) return;
    int idx = m_oscLayout->indexOf(osc);
    if (idx >= 0) {
        m_oscLayout->removeWidget(osc);
        m_oscilloscopes.removeAt(idx);
        osc->deleteLater();
    }

    updateAllMoveButtons();
}

void MainWindow::updateAllMoveButtons() {
    int count = m_oscilloscopes.size();
    for (int i = 0; i < count; ++i) {
        bool upEnabled = (i > 0);
        bool downEnabled = (i < count - 1);
        m_oscilloscopes[i]->setMoveButtonsEnabled(upEnabled, downEnabled);
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
    QDialog dialog(this);
    dialog.setWindowTitle("Configure Fields to Plot");
    dialog.setMinimumWidth(350);

    QTableWidget *table = new QTableWidget(&dialog);
    table->setColumnCount(2);
    table->setHorizontalHeaderLabels({"Field", "Color"});
    table->setColumnWidth(0, 180); // 设置第一列宽度为 180 像素
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table->verticalHeader()->setVisible(false);

    QStringList allFields = m_dataParser->getFieldNames();
    QStringList currentFields = osc->getFields();

    // 预设颜色列表（与 OscilloscopeWidget 中一致）
    QList<QColor> presetColors = MainWindow::getPresetColors();
    QStringList colorNames = MainWindow::getColorNames();

    // 存储每个字段的颜色下拉框指针
    QHash<int, QComboBox*> colorCombos;

    table->setRowCount(allFields.size());
    for (int i = 0; i < allFields.size(); ++i) {
        QString field = allFields[i];
        bool checked = currentFields.contains(field);

        // 第一列：字段名 + 复选框（不可编辑）
        QTableWidgetItem *item = new QTableWidgetItem(field);
        item->setFlags(item->flags() & ~Qt::ItemIsEditable);   // 禁止编辑
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
        table->setItem(i, 0, item);

        // 第二列：如果已勾选，创建颜色下拉框；否则留空
        if (checked) {
            QComboBox *combo = new QComboBox();
            // 添加预设颜色（带图标）
            for (int j = 0; j < presetColors.size(); ++j) {
                QPixmap pixmap(16, 16);
                pixmap.fill(presetColors[j]);
                combo->addItem(QIcon(pixmap), colorNames[j], presetColors[j]);
            }
            combo->addItem("Custom...");   // 自定义选项

            // 尝试获取当前字段的现有颜色
            QColor currentColor = osc->getFieldColor(field);
            if (currentColor.isValid()) {
                int index = presetColors.indexOf(currentColor);
                if (index >= 0)
                    combo->setCurrentIndex(index);
                else
                    combo->setCurrentIndex(presetColors.size()); // Custom...
            } else {
                // 默认：根据字段在 currentFields 中的索引分配预设颜色
                int idx = currentFields.indexOf(field);
                if (idx >= 0 && idx < presetColors.size())
                    combo->setCurrentIndex(idx);
            }

            // 处理 Custom... 选项：弹出颜色对话框
            connect(combo, QOverload<int>::of(&QComboBox::activated), this, [combo, field, this](int index) {
                if (index == combo->count() - 1) { // 最后一项是 "Custom..."
                    QColor newColor = QColorDialog::getColor(combo->palette().button().color(), nullptr,
                                                             QString("Select Color for %1").arg(field));
                    if (newColor.isValid()) {
                        // 将自定义颜色存储为用户数据，并改变按钮图标显示
                        QPixmap pixmap(16, 16);
                        pixmap.fill(newColor);
                        combo->setItemIcon(index, QIcon(pixmap));
                        combo->setItemData(index, newColor, Qt::UserRole);
                        combo->setCurrentIndex(index);
                    } else {
                        // 取消选择，恢复之前的选择
                        int prev = combo->property("prevIndex").toInt();
                        combo->setCurrentIndex(prev);
                    }
                }
                combo->setProperty("prevIndex", combo->currentIndex());
            });

            // 存储当前索引以便取消时恢复
            combo->setProperty("prevIndex", combo->currentIndex());

            table->setCellWidget(i, 1, combo);
            colorCombos[i] = combo;
        } else {
            table->setCellWidget(i, 1, nullptr);
        }
    }

    // 双击字段行：切换勾选状态
    connect(table, &QTableWidget::cellDoubleClicked, this, [table](int row, int column) {
        Q_UNUSED(column);
        QTableWidgetItem *item = table->item(row, 0);
        if (item) {
            Qt::CheckState newState = (item->checkState() == Qt::Checked) ? Qt::Unchecked : Qt::Checked;
            item->setCheckState(newState);
        }
    });

    // 响应复选框状态变化：动态添加/删除颜色下拉框
    connect(table, &QTableWidget::itemChanged, this, [table, &colorCombos, presetColors, colorNames](QTableWidgetItem *item) {
        if (item->column() != 0) return;
        int row = item->row();
        bool checked = (item->checkState() == Qt::Checked);
        QString field = item->text();

        if (checked && !colorCombos.contains(row)) {
            // 创建颜色下拉框
            QComboBox *combo = new QComboBox();
            for (int j = 0; j < presetColors.size(); ++j) {
                QPixmap pixmap(16, 16);
                pixmap.fill(presetColors[j]);
                combo->addItem(QIcon(pixmap), colorNames[j], presetColors[j]);
            }
            combo->addItem("Custom...");
            combo->setCurrentIndex(0);
            combo->setProperty("prevIndex", 0);

            // 处理 Custom... 选项
            connect(combo, QOverload<int>::of(&QComboBox::activated), [combo, field](int index) {
                if (index == combo->count() - 1) {
                    QColor newColor = QColorDialog::getColor(Qt::white, nullptr,
                                                            QString("Select Color for %1").arg(field));
                    if (newColor.isValid()) {
                        QPixmap pixmap(16, 16);
                        pixmap.fill(newColor);
                        combo->setItemIcon(index, QIcon(pixmap));
                        combo->setItemData(index, newColor, Qt::UserRole);
                        combo->setCurrentIndex(index);
                    } else {
                        int prev = combo->property("prevIndex").toInt();
                        combo->setCurrentIndex(prev);
                    }
                }
                combo->setProperty("prevIndex", combo->currentIndex());
            });

            table->setCellWidget(row, 1, combo);
            colorCombos[row] = combo;
        } else if (!checked && colorCombos.contains(row)) {
            QWidget *oldWidget = table->cellWidget(row, 1);
            if (oldWidget) {
                table->removeCellWidget(row, 1);
                oldWidget->deleteLater();
            }
            colorCombos.remove(row);
        }
    });

    QDialogButtonBox *buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttonBox, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    QVBoxLayout *layout = new QVBoxLayout(&dialog);
    layout->addWidget(table);
    layout->addWidget(buttonBox);

    if (dialog.exec() == QDialog::Accepted) {
        QStringList selectedFields;
        QHash<QString, QColor> colorOverrides;

        for (int i = 0; i < allFields.size(); ++i) {
            QTableWidgetItem *item = table->item(i, 0);
            if (item->checkState() == Qt::Checked) {
                QString field = allFields[i];
                selectedFields << field;

                QComboBox *combo = qobject_cast<QComboBox*>(table->cellWidget(i, 1));
                if (combo) {
                    int idx = combo->currentIndex();
                    QColor color;
                    if (idx >= 0 && idx < presetColors.size()) {
                        color = presetColors[idx];
                    } else if (idx == presetColors.size()) {
                        // Custom... 选项：从 item data 中取自定义颜色
                        color = combo->itemData(idx, Qt::UserRole).value<QColor>();
                        if (!color.isValid())
                            color = Qt::black; // fallback
                    }
                    if (color.isValid())
                        colorOverrides[field] = color;
                }
            }
        }

        osc->setFields(selectedFields);
        for (auto it = colorOverrides.begin(); it != colorOverrides.end(); ++it) {
            osc->setFieldColor(it.key(), it.value());
        }
    }
}

void MainWindow::onMoveUpRequested() {
    OscilloscopeWidget *osc = qobject_cast<OscilloscopeWidget*>(sender());
    if (!osc) return;
    int idx = m_oscilloscopes.indexOf(osc);
    if (idx <= 0) return;

    // 交换列表中的指针
    m_oscilloscopes.swapItemsAt(idx, idx - 1);  // 或 qSwap(m_oscilloscopes[idx], m_oscilloscopes[idx-1]);

    // 交换布局中的位置
    // 获取两个 widget 在布局中的索引（实际上与列表顺序一致，但布局可能因隐藏等原因不同，此处假设一致）
    // 更可靠：直接取布局中的两个 item，交换它们的位置
    QLayoutItem *itemUp = m_oscLayout->takeAt(idx - 1);
    QLayoutItem *itemDown = m_oscLayout->takeAt(idx - 1); // 注意：取走第一个后，原 idx 位置变为 idx-1
    if (itemUp && itemDown) {
        // 重新插入，顺序互换
        m_oscLayout->insertWidget(idx - 1, itemDown->widget());
        m_oscLayout->insertWidget(idx, itemUp->widget());
    }
    // 删除临时 item 对象（不删除 widget）
    delete itemUp;
    delete itemDown;

    updateAllMoveButtons();
}

void MainWindow::onMoveDownRequested() {
    OscilloscopeWidget *osc = qobject_cast<OscilloscopeWidget*>(sender());
    if (!osc) return;
    int idx = m_oscilloscopes.indexOf(osc);
    if (idx < 0 || idx >= m_oscilloscopes.size() - 1) return;

    m_oscilloscopes.swapItemsAt(idx, idx + 1);

    // 交换布局中的位置
    QLayoutItem *itemCurrent = m_oscLayout->takeAt(idx);
    QLayoutItem *itemNext = m_oscLayout->takeAt(idx); // 此时原 idx+1 移动到 idx
    if (itemCurrent && itemNext) {
        m_oscLayout->insertWidget(idx, itemNext->widget());
        m_oscLayout->insertWidget(idx + 1, itemCurrent->widget());
    }
    delete itemCurrent;
    delete itemNext;

    updateAllMoveButtons();
}

bool MainWindow::eventFilter(QObject *obj, QEvent *event) {
    if (obj == ui->lineEditSend && event->type() == QEvent::KeyPress) {
        QKeyEvent *keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->key() == Qt::Key_Up) {
            // 上键：浏览上一条历史
            if (!m_sendHistory.isEmpty() && m_historyIndex > 0) {
                m_historyIndex--;
                ui->lineEditSend->setText(m_sendHistory[m_historyIndex]);
            }
            return true;
        } else if (keyEvent->key() == Qt::Key_Down) {
            // 下键：浏览下一条历史
            if (!m_sendHistory.isEmpty() && m_historyIndex < m_sendHistory.size() - 1) {
                m_historyIndex++;
                ui->lineEditSend->setText(m_sendHistory[m_historyIndex]);
            } else if (m_historyIndex == m_sendHistory.size() - 1) {
                // 已经到最后一条，清空输入框
                ui->lineEditSend->clear();
                m_historyIndex = m_sendHistory.size(); // 指向末尾之后
            }
            return true;
        }
    }

    QSlider *slider = qobject_cast<QSlider*>(obj);
    if (slider &&
        (slider == ui->targetSlider || slider == ui->timeSlider || slider == ui->incrementSlider) &&
        event->type() == QEvent::Wheel) {
        QWheelEvent *wheelEvent = static_cast<QWheelEvent*>(event);
        const int deltaY = wheelEvent->angleDelta().y();
        if (deltaY == 0) {
            return true;
        }

        const int direction = (deltaY > 0) ? 1 : -1;
        const int step = qMax(1, slider->singleStep());
        const int nextValue = qBound(slider->minimum(),
                                     slider->value() + direction * step,
                                     slider->maximum());
        slider->setValue(nextValue);
        wheelEvent->accept();
        return true;
    }

    return QMainWindow::eventFilter(obj, event);
}

void MainWindow::changeEvent(QEvent *event)
{
    QMainWindow::changeEvent(event);

    if (event->type() == QEvent::ActivationChange ||
        event->type() == QEvent::WindowStateChange) {
        updatePlotRefreshState();
    }
}

void MainWindow::updatePlotRefreshState()
{
    const bool shouldSuspend = !isActiveWindow() || isMinimized();
    if (m_plotUpdatesSuspended == shouldSuspend) {
        return;
    }

    m_plotUpdatesSuspended = shouldSuspend;
    if (!m_plotTimer) {
        return;
    }

    if (m_plotUpdatesSuspended) {
        m_plotTimer->stop();
    } else {
        if (!m_plotTimer->isActive()) {
            m_plotTimer->start(50);
        }
        if (!m_plotPaused && m_plotDirty) {
            updateAllPlots();
        }
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
    m_plotDirty = true;
    updateAllPlots();
}

void MainWindow::updateAllPlots() {
    const QHash<QString, QVector<double>> &plotData = m_plotPaused ? m_pausedWaveData : m_waveData;
    const QVector<double> &plotTimeStamps = m_plotPaused ? m_pausedTimeStamps : m_timeStamps;

    for (OscilloscopeWidget *osc : m_oscilloscopes) {
        osc->updatePlot(plotData, plotTimeStamps, m_currentMaxPoints);
    }
    m_plotDirty = false;
}

void MainWindow::updatePlot() {
    if (m_plotUpdatesSuspended || m_plotPaused || !m_plotDirty) return;
    updateAllPlots();
}

// ==================== 数据处理 ====================
void MainWindow::handleNewData(const QHash<QString, double> &values) {
    const quint64 timestampTicks = static_cast<quint64>(values.value(DataParser::TIMESTAMP_FIELD, 0.0));

    if (m_hasLastTimestamp && timestampTicks < m_lastTimestampTicks) {
        m_timeStamps.clear();
        m_waveData.clear();
        m_pausedTimeStamps.clear();
        m_pausedWaveData.clear();
        m_latestTelemetryValues.clear();
    }

    m_lastTimestampTicks = timestampTicks;
    m_hasLastTimestamp = true;

    const double currentTime = static_cast<double>(timestampTicks) / 275000000.0;
    addTimeStamp(currentTime);
    for (auto it = values.cbegin(); it != values.cend(); ++it) {
        if (it.key() != DataParser::TIMESTAMP_FIELD) {
            m_latestTelemetryValues[it.key()] = it.value();
        }
    }
    // 追加到波形缓冲区
    for (auto it = values.begin(); it != values.end(); ++it) {
        const QString &field = it.key();
        if (field == DataParser::TIMESTAMP_FIELD) {
            continue;
        }
        double val = it.value();
        QVector<double> &vec = m_waveData[field];
        vec.append(val);
        if (vec.size() > m_maxWavePoints) {
            vec.remove(0, vec.size() - m_maxWavePoints);
        }
    }
    m_plotDirty = true;
    advanceFaultAutoCapture();
    updateGauges(values);
    // 可选：在接收区显示关键数值（调试用，可注释）
    writeTelemetryLogRow(values);
    // if (values.contains("RPM")) {
    //     ui->plainTextEditReceive->appendPlainText(QString("RPM: %1").arg(values["RPM"], 0, 'f', 1));
    // }
}

void MainWindow::capturePausedPlotSnapshot()
{
    m_pausedTimeStamps.clear();
    m_pausedWaveData.clear();

    if (m_timeStamps.isEmpty()) {
        return;
    }

    const int totalPoints = m_timeStamps.size();
    const int startIdx = qMax(0, totalPoints - m_currentMaxPoints);
    const int pointsToShow = totalPoints - startIdx;
    if (pointsToShow <= 0) {
        return;
    }

    m_pausedTimeStamps = m_timeStamps.mid(startIdx, pointsToShow);

    for (auto it = m_waveData.cbegin(); it != m_waveData.cend(); ++it) {
        const QVector<double> &series = it.value();
        if (series.size() < totalPoints) {
            continue;
        }

        m_pausedWaveData.insert(it.key(), series.mid(startIdx, pointsToShow));
    }
}

void MainWindow::setPlotPaused(bool paused)
{
    if (m_plotPaused == paused) {
        return;
    }

    m_plotPaused = paused;
    ui->pushButtonPause->setText(m_plotPaused ? "▶️" : "⏸️");

    if (m_plotPaused) {
        capturePausedPlotSnapshot();
    } else {
        m_pausedTimeStamps.clear();
        m_pausedWaveData.clear();
    }

    updateAllPlots();
}

void MainWindow::startFaultAutoCapture(quint32 triggeredMask)
{
    Q_UNUSED(triggeredMask);

    if (m_plotPaused) {
        return;
    }

    const int sliderMinimum = m_sampleSlider->minimum();
    const int sliderMaximum = m_sampleSlider->maximum();
    const int requestedPoints = qBound(sliderMinimum, m_faultAutoCaptureDisplayPoints, sliderMaximum);

    m_faultAutoCapturePending = true;
    m_faultAutoCaptureSkipCurrentPacket = true;
    m_faultAutoCapturePacketsRemaining = qMax(0, m_faultAutoCapturePacketsAfterTrigger);

    if (m_currentMaxPoints != requestedPoints) {
        m_sampleSlider->setValue(requestedPoints);
    } else {
        m_plotDirty = true;
        updateAllPlots();
    }
}

void MainWindow::advanceFaultAutoCapture()
{
    if (!m_faultAutoCapturePending || m_plotPaused) {
        return;
    }

    if (m_faultAutoCaptureSkipCurrentPacket) {
        m_faultAutoCaptureSkipCurrentPacket = false;
        return;
    }

    if (m_faultAutoCapturePacketsRemaining > 0) {
        --m_faultAutoCapturePacketsRemaining;
    }

    if (m_faultAutoCapturePacketsRemaining <= 0) {
        m_faultAutoCapturePending = false;
        m_faultAutoCaptureSkipCurrentPacket = false;
        setPlotPaused(true);
    }
}

void MainWindow::handlePacketStatus(quint32 errorCode,
                                    const QStringList &errorNames,
                                    quint8 controlMode,
                                    const QString &controlModeName,
                                    bool controlModeKnown) {
    QStringList normalizedErrorNames = errorNames;
    if (errorCode != 0 && normalizedErrorNames.isEmpty()) {
        normalizedErrorNames << "UNKNOWN_ERROR_BITS";
    }

    const quint32 previousErrorCode = m_telemetryStatus.errorCode;
    const bool statusChanged = m_telemetryStatus.errorCode != errorCode ||
                               m_telemetryStatus.errorNames != normalizedErrorNames ||
                               m_telemetryStatus.controlMode != controlMode ||
                               m_telemetryStatus.controlModeName != controlModeName ||
                               m_telemetryStatus.controlModeKnown != controlModeKnown;

    m_telemetryStatus.errorCode = errorCode;
    m_telemetryStatus.errorNames = normalizedErrorNames;
    m_telemetryStatus.controlMode = controlMode;
    m_telemetryStatus.controlModeName = controlModeName;
    m_telemetryStatus.controlModeKnown = controlModeKnown;

    if (statusChanged) {
        updateStatusIndicators();
    }

    const quint32 newlyTriggeredErrors = (~previousErrorCode) & errorCode & m_faultAutoCaptureTriggerMask;
    if (newlyTriggeredErrors != 0) {
        startFaultAutoCapture(newlyTriggeredErrors);
    }

    if (errorCode == 0) {
        return;
    }

    if (errorCode == previousErrorCode && !statusChanged) {
        return;
    }

    const QString message = QString("Error 0x%1: %2")
        .arg(errorCode, 8, 16, QLatin1Char('0'))
        .arg(m_telemetryStatus.errorNames.join(" | "));
    statusBar()->showMessage(message, 5000);
}

void MainWindow::onMaskReceived(quint32 mask1, quint32 mask2) {
    if (m_syncingFromMask) return;
    m_syncingFromMask = true;

    static quint32 lastMask1 = 0;
    static quint32 lastMask2 = 0;
    if (mask1 == lastMask1 && mask2 == lastMask2) {
        m_syncingFromMask = false;   // 关键：必须重置标志
        return;
    }
    lastMask1 = mask1;
    lastMask2 = mask2;

    // 遍历字段列表中的每个项
    for (int i = 0; i < m_fieldList->count(); ++i) {
        QListWidgetItem *item = m_fieldList->item(i);
        if (!item) continue;
        // 获取字段名对应的掩码位（需要字段定义信息）
        QString fieldName = item->text();
        bool shouldCheck = m_dataParser->isFieldEnabled(fieldName, mask1, mask2);
        if (shouldCheck != (item->checkState() == Qt::Checked)) {
            item->setCheckState(shouldCheck ? Qt::Checked : Qt::Unchecked);
        }
    }

    m_syncingFromMask = false;
}

// ==================== 串口处理 ====================
bool MainWindow::isReceiveTextByte(char byte) const {
    const uchar value = static_cast<uchar>(byte);
    return byte == '\r' || byte == '\n' || byte == '\t' || (value >= 0x20 && value <= 0x7E);
}

void MainWindow::processReceiveTextChunk(const QByteArray &chunk, QByteArray &lineBuffer) {
    for (char byte : chunk) {
        if (isReceiveTextByte(byte)) {
            lineBuffer.append(byte);
            if (byte == '\n') {
                flushReceiveTextLines(lineBuffer);
            }
            continue;
        }

        flushReceiveTextLines(lineBuffer);
        lineBuffer.clear();
    }

    if (lineBuffer.size() > 1024 && lineBuffer.indexOf('\n') == -1) {
        lineBuffer.clear();
    }
}

void MainWindow::flushReceiveTextLines(QByteArray &lineBuffer) {
    int newlinePos = -1;
    while ((newlinePos = lineBuffer.indexOf('\n')) != -1) {
        const QByteArray line = lineBuffer.left(newlinePos + 1);
        const QString text = QString::fromLatin1(line).trimmed();
        if (!text.isEmpty() && isLikelyReceiveTextLine(line)) {
            ui->plainTextEditReceive->appendPlainText(text);
            parseTuneResponse(text);
        }
        lineBuffer.remove(0, newlinePos + 1);
    }
}

bool MainWindow::isLikelyReceiveTextLine(const QByteArray &line) const {
    int letterCount = 0;
    int digitCount = 0;
    int whitespaceCount = 0;
    int punctuationCount = 0;

    for (char byte : line) {
        if (byte == '\r' || byte == '\n' || byte == '\t' || byte == ' ') {
            ++whitespaceCount;
        } else if ((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z')) {
            ++letterCount;
        } else if (byte >= '0' && byte <= '9') {
            ++digitCount;
        } else {
            ++punctuationCount;
        }
    }

    const int contentCount = letterCount + digitCount + punctuationCount;
    if (contentCount == 0 || letterCount == 0) {
        return false;
    }

    return punctuationCount <= letterCount + digitCount + whitespaceCount;
}

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
        ui->pushButtonStartToggle->setText("⏹");
        ui->comboPort->setEnabled(false);
        ui->comboBaud->setEnabled(false);
        ui->pushButtonRefresh->setEnabled(false);
    } else {
        ui->pushButtonStartToggle->setText("▶");
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

    if (ui->pushButtonStartToggle->text() == "⏹") {
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
    sendCurrentLineEditCommand();
}

void MainWindow::on_lineEditSend_returnPressed() {
    sendCurrentLineEditCommand();
}

void MainWindow::on_pushButtonFoc_clicked()     { sendCommand("start foc\r\n"); }
void MainWindow::on_pushButtonVvvf_clicked()    { sendCommand("start vvvf\r\n"); }
void MainWindow::on_pushButtonSixstep_clicked() { sendCommand("sixstep\r\n"); }
void MainWindow::on_pushButtonStop_clicked()    { sendCommand("stop\r\n"); }
void MainWindow::on_pushButtonAlign_clicked()   { sendCommand("align\r\n"); }
void MainWindow::on_pushButtonAudible_clicked() { sendCommand("audible\r\n"); }
void MainWindow::on_pushButtonReset_clicked()   { sendCommand("reset\r\n"); }
void MainWindow::on_pushButtonResetConnection_clicked() { sendCommand("sim reset\r\n"); }
void MainWindow::on_pushButtonPreset1_clicked() { sendCommand("log preset 1\r\n"); }
void MainWindow::on_pushButtonPreset2_clicked() { sendCommand("log preset 2\r\n"); }
void MainWindow::on_pushButtonPreset3_clicked() { sendCommand("log preset 3\r\n"); }
void MainWindow::on_pushButtonPreset4_clicked() { sendCommand("log preset 4\r\n"); }
void MainWindow::on_pushButtonRemoveAll_clicked() { sendCommand("log rm all\r\n"); }
void MainWindow::on_pushButtonBin_clicked()     { sendCommand("log bin\r\n"); }
void MainWindow::on_pushButtonUtf8_clicked()    { sendCommand("log utf8\r\n"); }
void MainWindow::on_pushButtonSimStart_clicked() { sendCommand("sim start\r\n"); }

void MainWindow::on_comboBoxTargetSelection_currentIndexChanged(int) {
    if (m_updatingTargetType) return;  // Reentry prevention
    m_updatingTargetType = true;

    QString newType = ui->comboBoxTargetSelection->currentText();
    QString oldType = (newType == "Speed") ? "Torque" : "Speed";

    // 1. Save the current value of the old type (exact value, could be manually entered or from slider)
    double oldValue;
    if (m_targetManuallyEdited) {
        bool ok;
        oldValue = ui->lineEditTarget->text().toDouble(&ok);
        if (!ok) oldValue = 0.0;
    } else {
        if (oldType == "Speed") {
            oldValue = ui->targetSlider->value();
        } else {
            oldValue = ui->targetSlider->value() / 1000.0;
        }
    }
    if (oldType == "Speed") {
        m_lastSpeedValue = oldValue;
    } else {
        m_lastTorqueValue = oldValue;
    }

    // 2. Update the slider range (signals are blocked to avoid interference)
    ui->targetSlider->blockSignals(true);
    updateTargetSliderLimits();   // Adjust range based on new type
    ui->targetSlider->blockSignals(false);

    // 3. Restore the last saved value of the newly selected type (and mark as manually edited, preserving precision)
    double newValue = (newType == "Speed") ? m_lastSpeedValue : m_lastTorqueValue;
    setTargetValue(newValue, true);   // The second parameter true indicates it should be treated as manually edited

    m_updatingTargetType = false;
}

void MainWindow::on_targetSlider_valueChanged(int value) {
    // Ignore changes triggered by programmatic updates when switching target type
    if (m_updatingTargetType) return;

    m_targetManuallyEdited = false;
    if (ui->comboBoxTargetSelection->currentText() == "Speed") {
        int snapped = (value / 100) * 100;
        if (snapped != value) {
            ui->targetSlider->blockSignals(true);
            ui->targetSlider->setValue(snapped);
            ui->targetSlider->blockSignals(false);
            value = snapped;
        }
        double realValue = value;
        ui->lineEditTarget->setText(QString::number(realValue, 'f', 0));
    } else {
        double realValue = value / 1000.0;
        ui->lineEditTarget->setText(QString::number(realValue, 'f', 3));
    }
}

void MainWindow::on_lineEditTarget_editingFinished() {
    bool ok;
    double val = ui->lineEditTarget->text().toDouble(&ok);
    if (!ok) return;

    // Only limit the range, do not round
    bool isSpeed = (ui->comboBoxTargetSelection->currentText() == "Speed");
    if (isSpeed) {
        val = qBound(-9000.0, val, 9000.0);
    } else {
        val = qBound(0.0, val, 0.1);
    }

    // Move the slider to the nearest step value (speed in hundreds, torque in 0.001 increments)
    int sliderValue;
    if (isSpeed) {
        sliderValue = static_cast<int>(qRound(val / 100.0) * 100); // Round to nearest 100
    } else {
        sliderValue = static_cast<int>(qRound(val * 1000.0));
    }
    ui->targetSlider->blockSignals(true);
    ui->targetSlider->setValue(sliderValue);
    ui->targetSlider->blockSignals(false);

    // Restore the original value entered by the user (which may have been modified due to range limits, but keep its precision)
    QString formatted = QString::number(val, 'f', 6);
    // Remove trailing zeros and possible decimal point if not needed
    formatted.remove(QRegularExpression("\\.?0+$"));
    if (formatted.isEmpty()) formatted = "0";
    ui->lineEditTarget->setText(formatted);
    m_targetManuallyEdited = true;
}

void MainWindow::on_timeSlider_valueChanged(int value) {
    m_timeManuallyEdited = false;
    ui->lineEditTime->setText(QString::number(value));
}

void MainWindow::on_lineEditTime_editingFinished() {
    bool ok;
    double sec = ui->lineEditTime->text().toDouble(&ok);
    if (!ok) return;
    sec = qBound(0.0, sec, 60.0);
    ui->timeSlider->setValue(static_cast<int>(qRound(sec)));
    QString formatted = QString::number(sec, 'f', 6);
    formatted.remove(QRegularExpression("\\.?0+$"));
    if (formatted.isEmpty()) formatted = "0";
    ui->lineEditTime->setText(formatted);
    m_timeManuallyEdited = true;
}

void MainWindow::on_pushButtonTargetSend_clicked() {
    QString mode = ui->comboBoxTargetSelection->currentText().toLower();
    double target, time;
    if (m_targetManuallyEdited) {
        // Use value manually entered by the user
        bool ok;
        target = ui->lineEditTarget->text().toDouble(&ok);
        if (!ok) return;
        // Clamp again to ensure the range
        if (mode == "speed") {
            target = qBound(-9000.0, target, 9000.0);
        } else {
            target = qBound(0.0, target, 0.1);
        }
    } else {
        // Use the value from the slider
        if (mode == "speed") {
            target = ui->targetSlider->value();
        } else {
            target = ui->targetSlider->value() / 1000.0;
        }
    }
    if (m_timeManuallyEdited) {
        bool ok;
        time = ui->lineEditTime->text().toDouble(&ok);
        if (!ok) return;
        time = qBound(0.0, time, 60.0);
    } else {
        time = ui->timeSlider->value();
    }
    QString cmd;
    if (qFuzzyIsNull(time) || ui->timeSlider->value() == 0) {
        cmd = QString("%1 %2\r\n").arg(mode).arg(target, 0, 'f', 6);  // Keep sufficient precision
    } else {
        cmd = QString("%1 %2 %3\r\n").arg(mode).arg(target, 0, 'f', 6).arg(time, 0, 'f', 6);
    }
    sendCommand(cmd);
}

void MainWindow::on_pushButtonTuneSend_clicked() {
    QString subsys = ui->comboBoxTuneSubsystem->currentText();
    QString param = ui->comboBoxTuneParameter->currentText();
    QString valueStr = ui->lineEditTune->text();
    bool ok;
    double value = valueStr.toDouble(&ok);
    if (!ok) {
        QMessageBox::warning(this, "Error", "Invalid numeric value");
        return;
    }
    // Set flag to record this change in history when the response comes back
    m_recordHistory = true;
    QString cmd = QString("tune %1 %2 %3\r\n").arg(subsys, param, valueStr);
    sendCommand(cmd);
}

void MainWindow::on_pushButtonTuneUndo_clicked() {
    if (m_currentParamHistory.undoStack.isEmpty()) {
        QMessageBox::information(this, "Undo", "No previous value to undo");
        return;
    }
    double oldVal = m_currentParamHistory.undoStack.pop();
    // Send tuning command to revert value, but disable history recording for this action to avoid loops
    m_recordHistory = false;
    QString subsys = ui->comboBoxTuneSubsystem->currentText();
    QString param = ui->comboBoxTuneParameter->currentText();
    QString cmd = QString("tune %1 %2 %3\r\n").arg(subsys, param, QString::number(oldVal, 'f', 4));
    sendCommand(cmd);
    // Note: The slave will return the new value (i.e., oldVal) and its previous value (i.e., currentValue),
    // The response will again trigger parseTuneResponse, automatically updating the history stack and display.
}

void MainWindow::on_pushButtonTuneEnquire_clicked() {
    QString subsys = ui->comboBoxTuneSubsystem->currentText();
    QString param = ui->comboBoxTuneParameter->currentText();
    QString cmd = QString("tune %1 %2 ?\r\n").arg(subsys, param);
    m_recordHistory = false;
    sendCommand(cmd);
}

void MainWindow::on_pushButtonIncrement_clicked() {
    double step = m_stepValues[ui->incrementSlider->value()];
    QString subsys = ui->comboBoxTuneSubsystem->currentText();
    QString param = ui->comboBoxTuneParameter->currentText();
    // Set flag to record this change in history when the response comes back
    m_recordHistory = true;
    QString cmd = QString("increment %1 %2 %3\r\n").arg(subsys, param, QString::number(step, 'f', 6));
    sendCommand(cmd);
}

void MainWindow::on_pushButtonDecrement_clicked() {
    double step = -m_stepValues[ui->incrementSlider->value()];
    QString subsys = ui->comboBoxTuneSubsystem->currentText();
    QString param = ui->comboBoxTuneParameter->currentText();
    // Set flag to record this change in history when the response comes back
    m_recordHistory = true;
    QString cmd = QString("increment %1 %2 %3\r\n").arg(subsys, param, QString::number(step, 'f', 6));
    sendCommand(cmd);
}

void MainWindow::on_incrementSlider_valueChanged(int value) {
    if (value >= 0 && value < m_stepValues.size()) {
        double step = m_stepValues[value];
        ui->incrementLabel->setText(formatStepValue(step));
    }
}

void MainWindow::on_comboBoxTuneSubsystem_currentIndexChanged(int index) {
    Q_UNUSED(index);
    // Clear history stack when switching subsystem
    m_currentParamHistory.undoStack.clear();
    // Update parameter combo box based on selected subsystem
    QString subsys = ui->comboBoxTuneSubsystem->currentText();
    ui->comboBoxTuneParameter->clear();
    // Define parameters for each subsystem
    if (subsys == "speed") {
        ui->comboBoxTuneParameter->addItems({"p", "i"});
    } else if (subsys == "id") {
        ui->comboBoxTuneParameter->addItems({"p", "i"});
    } else if (subsys == "iq") {
        ui->comboBoxTuneParameter->addItems({"p", "i"});
    } else if (subsys == "fw") {
        ui->comboBoxTuneParameter->addItems({"p", "i"});
    } else if (subsys == "gain") {
        ui->comboBoxTuneParameter->addItems({"ia", "ib", "ic", "va", "vb", "ibatt", "vbatt"});
    } else if (subsys == "offset") {
        ui->comboBoxTuneParameter->addItems({"ia", "ib", "ic", "va", "vb", "ibatt", "vbatt"});
    }
    // Display last known value for the first parameter of the new subsystem, if available
    QString key = getCurrentParamKey();
    if (m_paramLastValue.contains(key)) {
        double lastVal = m_paramLastValue[key];
        m_currentParamHistory.currentValue = lastVal;
        ui->lineEditTune->setText(QString::number(lastVal, 'f', 6));
    } else {
        m_currentParamHistory.currentValue = 0.0;
        ui->lineEditTune->clear();
    }
}

void MainWindow::on_comboBoxTuneParameter_currentIndexChanged(int index) {
    Q_UNUSED(index);
    // Clear history stack when switching parameter
    m_currentParamHistory.undoStack.clear();
    QString key = getCurrentParamKey();
    if (m_paramLastValue.contains(key)) {
        double lastVal = m_paramLastValue[key];
        m_currentParamHistory.currentValue = lastVal;
        ui->lineEditTune->setText(QString::number(lastVal, 'f', 6));
    } else {
        m_currentParamHistory.currentValue = 0.0;
        ui->lineEditTune->clear();
    }
}

void MainWindow::on_pushButtonPause_clicked() {
    if (!m_plotPaused) {
        m_faultAutoCapturePending = false;
        m_faultAutoCaptureSkipCurrentPacket = false;
        m_faultAutoCapturePacketsRemaining = 0;
    }

    setPlotPaused(!m_plotPaused);
}

void MainWindow::on_pushButtonSave_clicked() {
    if (m_isLogging) {
        stopTelemetryLogging();
        return;
    }

    if (!startTelemetryLogging()) {
        ui->pushButtonSave->setChecked(false);
    }
}

void MainWindow::on_pushButtonSelectConfig_clicked() {
    const QString startDir = m_dataParser && !m_dataParser->configurationPath().isEmpty()
                                 ? QFileInfo(m_dataParser->configurationPath()).absolutePath()
                                 : QDir::currentPath();
    const QString filePath = QFileDialog::getOpenFileName(this,
                                                          "Select Telemetry Configuration",
                                                          startDir,
                                                          "JSON configuration (*.json);;All files (*)");
    if (filePath.isEmpty()) {
        return;
    }

    const bool restartLogging = m_isLogging;
    if (restartLogging) {
        stopTelemetryLogging();
    }

    QString errorMessage;
    bool loaded = false;
    if (m_dataParser->thread() == QThread::currentThread()) {
        loaded = m_dataParser->loadConfiguration(filePath, &errorMessage);
    } else {
        QMetaObject::invokeMethod(m_dataParser, [&]() {
            loaded = m_dataParser->loadConfiguration(filePath, &errorMessage);
        }, Qt::BlockingQueuedConnection);
    }

    if (!loaded) {
        QMessageBox::critical(this, "Configuration Error", errorMessage);
        return;
    }

    m_timeStamps.clear();
    m_waveData.clear();
    m_pausedTimeStamps.clear();
    m_pausedWaveData.clear();
    m_latestTelemetryValues.clear();
    m_hasLastTimestamp = false;
    updateAllPlots();
    if (restartLogging && !startTelemetryLogging()) {
        ui->pushButtonSave->setChecked(false);
    }
    statusBar()->showMessage("Loaded telemetry configuration: " + filePath, 5000);
}

bool MainWindow::startTelemetryLogging() {
    if (m_isLogging) {
        return true;
    }

    const QString fileName = QDateTime::currentDateTime().toString("yyyy-MM-dd-hh.mm.ss'_log_data.csv'");
    m_logFile.setFileName(QDir::current().filePath(fileName));
    if (!m_logFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::critical(this, "Error", "Failed to create telemetry log file: " + m_logFile.errorString());
        return false;
    }

    m_logFields = m_dataParser->getFieldNames();
    m_logStream.setDevice(&m_logFile);
    m_logStream << "mcu_timestamp,error_code,error_flags,control_mode,control_mode_name";
    for (const QString &field : m_logFields) {
        m_logStream << "," << field;
    }
    m_logStream << "\n";

    m_isLogging = true;
    ui->pushButtonSave->setChecked(true);
    ui->pushButtonSave->setToolTip("Saving telemetry to " + m_logFile.fileName());
    return true;
}

void MainWindow::stopTelemetryLogging() {
    if (!m_isLogging && !m_logFile.isOpen()) {
        return;
    }

    m_logStream.flush();
    m_logStream.setDevice(nullptr);
    if (m_logFile.isOpen()) {
        m_logFile.close();
    }
    m_isLogging = false;
    if (ui && ui->pushButtonSave) {
        ui->pushButtonSave->setChecked(false);
        ui->pushButtonSave->setToolTip("Start or stop saving telemetry to CSV");
    }
}

void MainWindow::writeTelemetryLogRow(const QHash<QString, double> &values) {
    if (!m_isLogging || !m_logFile.isOpen()) {
        return;
    }

    const quint64 timestampTicks = static_cast<quint64>(values.value(DataParser::TIMESTAMP_FIELD, 0.0));

    m_logStream << QString::number(timestampTicks)
                << "," << m_telemetryStatus.errorCode
                << "," << m_telemetryStatus.errorNames.join("|")
                << "," << static_cast<int>(m_telemetryStatus.controlMode)
                << "," << m_telemetryStatus.controlModeName;
    for (const QString &field : m_logFields) {
        m_logStream << "," << QString::number(values.value(field, 0.0), 'g', 17);
    }
    m_logStream << "\n";
}

void MainWindow::sendCurrentLineEditCommand() {
    QString sendStr = ui->lineEditSend->text();
    if (sendStr.isEmpty())
        return;

    // 存入历史（避免与上一条重复）
    if (m_sendHistory.isEmpty() || m_sendHistory.last() != sendStr) {
        m_sendHistory.append(sendStr);
        if (m_sendHistory.size() > 64)
            m_sendHistory.removeFirst();
    }
    m_historyIndex = m_sendHistory.size(); // 指向末尾之后

    // 发送数据（自动添加换行）
    QString cmd = sendStr;
    if (!cmd.endsWith("\r\n"))
        cmd += "\r\n";
    QByteArray data = cmd.toUtf8();
    QMetaObject::invokeMethod(m_serialManager, "sendData",
                              Qt::QueuedConnection,
                              Q_ARG(QByteArray, data));

    // 显示回显
    ui->plainTextEditReceive->appendPlainText(">> " + sendStr.trimmed());

    // 清空输入框
    ui->lineEditSend->clear();
}

void MainWindow::onFieldCheckStateChanged(QListWidgetItem *item) {
    if (m_syncingFromMask) return;
    if (!item) return;
    QString fieldName = item->text();
    bool checked = (item->checkState() == Qt::Checked);
    
    // 构造命令字符串
    QString cmdName = m_dataParser->getCommandNameForField(fieldName);
    if (cmdName.isEmpty()) {
        return;
    }
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
            const QString cmdName = m_dataParser->getCommandNameForField(item->text());
            if (!cmdName.isEmpty()) {
                sendCommand(QString("log add %1\r\n").arg(cmdName));
            }
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

void MainWindow::updateTargetSliderLimits() {
    bool isSpeed = (ui->comboBoxTargetSelection->currentText() == "Speed");
    if (isSpeed) {
        ui->targetSlider->setRange(-9000, 9000);
        ui->targetSlider->setSingleStep(100);
        ui->targetLabelPrefix->setText("Speed");
    } else {
        ui->targetSlider->setRange(0, 100);   // 0 -> 0.0, 100 -> 0.100
        ui->targetSlider->setSingleStep(1);
        ui->targetLabelPrefix->setText("Torque");
    }
    // Adjust current value to avoid exceeding limits
    int current = ui->targetSlider->value();
    current = qBound(ui->targetSlider->minimum(), current, ui->targetSlider->maximum());
    ui->targetSlider->setValue(current);
}

double MainWindow::getCurrentTargetValue() const {
    if (m_targetManuallyEdited) {
        bool ok;
        double v = ui->lineEditTarget->text().toDouble(&ok);
        if (ok) return v;
    }
    bool isSpeed = (ui->comboBoxTargetSelection->currentText() == "Speed");
    if (isSpeed) {
        return ui->targetSlider->value();
    } else {
        return ui->targetSlider->value() / 1000.0;
    }
}

void MainWindow::setTargetValue(double val, bool markAsEdited) {
    ui->targetSlider->blockSignals(true);

    bool isSpeed = (ui->comboBoxTargetSelection->currentText() == "Speed");
    if (isSpeed) {
        val = qBound(-9000.0, val, 9000.0);
        int sliderVal = static_cast<int>(qRound(val / 100.0) * 100);
        ui->targetSlider->setValue(sliderVal);
    } else {
        val = qBound(0.0, val, 0.1);
        int sliderVal = static_cast<int>(qRound(val * 1000.0));
        ui->targetSlider->setValue(sliderVal);
    }
    // Format display: remove trailing zeros, keep sufficient precision (up to 6 decimal places)
    QString formatted = QString::number(val, 'f', 6);
    formatted.remove(QRegularExpression("\\.?0+$"));
    if (formatted.isEmpty()) formatted = "0";
    ui->lineEditTarget->setText(formatted);

    m_targetManuallyEdited = markAsEdited;
    ui->targetSlider->blockSignals(false);
}

void MainWindow::parseTuneResponse(const QString &line) {
    // Format: "speed p set to 100.000000 (was 50.000000)"
    QRegularExpression setRegex(R"((\w+)\s+(\w+)\s+set to\s+([0-9.-]+)\s+\(was\s+([0-9.-]+)\))");
    QRegularExpressionMatch setMatch = setRegex.match(line);
    if (setMatch.hasMatch()) {
        QString subsys = setMatch.captured(1);
        QString param = setMatch.captured(2);
        double newVal = setMatch.captured(3).toDouble();
        double oldVal = setMatch.captured(4).toDouble();

        QString key = subsys + ":" + param;
        // Update history stack: push old value onto stack (max 32)
        m_paramLastValue[key] = newVal;

        // If the current UI selection is this parameter, update lineEditTune to show the new value
        if (getCurrentParamKey() == key) {
            // Push to stack depending on the flag (undo command does not push)
            if (m_recordHistory) {
                if (m_currentParamHistory.undoStack.size() >= 32)
                    m_currentParamHistory.undoStack.pop_front();
                m_currentParamHistory.undoStack.push(oldVal);
            }
            m_currentParamHistory.currentValue = newVal;
            ui->lineEditTune->setText(QString::number(newVal, 'f', 6));
        }
        statusBar()->showMessage(QString("Value Captured (%1 %2 %3)").arg(subsys).arg(param).arg(newVal, 0, 'f', 6), 3000);
        return;
    }

    // Format for enquire response: "speed p is 100.000000"
    QRegularExpression queryRegex(R"((\w+)\s+(\w+)\s+is\s+([0-9.-]+))");
    QRegularExpressionMatch queryMatch = queryRegex.match(line);
    if (queryMatch.hasMatch()) {
        QString subsys = queryMatch.captured(1);
        QString param = queryMatch.captured(2);
        double value = queryMatch.captured(3).toDouble();

        QString key = subsys + ":" + param;
        m_paramLastValue[key] = value;

        if (getCurrentParamKey() == key) {
            // Enquire command does not affect history stack
            m_currentParamHistory.currentValue = value;
            ui->lineEditTune->setText(QString::number(value, 'f', 6));
        }
        statusBar()->showMessage(QString("Enquire Captured (%1 %2 %3)").arg(subsys).arg(param).arg(value, 0, 'f', 6), 3000);
        return;
    }
}

QString MainWindow::getCurrentParamKey() const {
    return ui->comboBoxTuneSubsystem->currentText() + ":" + ui->comboBoxTuneParameter->currentText();
}

QString MainWindow::formatStepValue(double step) const
{
    double absStep = std::fabs(step);
    if (absStep >= 1.0) {
        return QString::number(step, 'f', 0);        // 0 decimal places for integer steps
    } else if (absStep >= 0.1) {
        return QString::number(step, 'f', 1);        // 1 decimal place for steps between 0.1 and 1
    } else if (absStep >= 0.01) {
        return QString::number(step, 'f', 2);        // 2 decimal places for steps between 0.01 and 0.1
    } else if (absStep >= 0.001) {
        return QString::number(step, 'f', 3);        // 3 decimal places for steps between 0.001 and 0.01
    } else if (absStep >= 0.0001) {
        return QString::number(step, 'f', 4);        // 4 decimal places for steps between 0.0001 and 0.001
    } else {
        return QString::number(step, 'f', 5);        // 5 decimal places for steps between 0.00001 and 0.0001
    }
}
