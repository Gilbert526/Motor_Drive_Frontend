#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "audiolevelmeter.h"
#include "SerialManager.h"
#include "DataParser.h"
#include "simulationdialog.h"
#include <QMessageBox>
#include <QMetaObject>
#include <QSerialPortInfo>
#include <QDialog>
#include <QDialogButtonBox>
#include <QBoxLayout>
#include <QButtonGroup>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QListWidgetItem>
#include <QSignalBlocker>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QSet>
#include <QStyle>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace {
constexpr const char *kSaveButtonStyle =
    "QPushButton#pushButtonSave:checked,"
    "QPushButton#pushButtonSaveAdc:checked,"
    "QPushButton#pushButtonQuickSave:checked {"
    " background-color: #31e63a;"
    " border: 1px solid #0a8a1f;"
    "}"
    "QPushButton#pushButtonSaveAdc[adcRecent=\"true\"] {"
    " background-color: #00d5ff;"
    " border: 1px solid #008ba6;"
    "}"
    "QPushButton#pushButtonSaveAdc[adcRecent=\"true\"]:checked {"
    " background-color: #31e63a;"
    " border: 1px solid #0a8a1f;"
    "}";

// Apply the current Qt style recursively after palette/theme-sensitive widgets
// are changed. This is used instead of rebuilding widgets so existing state,
// signal connections, and layout ownership remain untouched.
void refreshStyle(QWidget *widget)
{
    if (!widget) {
        return;
    }
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    widget->update();
}

QString uniqueFilePath(const QString &filePath)
{
    // Preserve the requested path when available; otherwise append a numeric
    // suffix before the extension so quick-save/logging never overwrites data.
    if (!QFileInfo::exists(filePath)) {
        return filePath;
    }

    const QFileInfo info(filePath);
    const QDir dir = info.dir();
    const QString baseName = info.completeBaseName();
    const QString suffix = info.suffix();
    for (int i = 1; ; ++i) {
        const QString candidate = dir.filePath(QString("%1_%2.%3")
                                                   .arg(baseName)
                                                   .arg(i)
                                                   .arg(suffix));
        if (!QFileInfo::exists(candidate)) {
            return candidate;
        }
    }
}
}

MainWindow::MainWindow(QWidget *parent):
    QMainWindow(parent),
    ui(new Ui::MainWindow),
    m_serialManager(nullptr),
    m_dataParser(nullptr),
    m_serialThread(nullptr),
    m_historyIndex(-1),
    m_maxWavePoints(20000),     // Store up to 20,000 samples per field.
    m_faultCaptureDrainArmed(false),
    m_faultCaptureDrainQueued(false),
    m_gaugeCircularMode(false),
    m_gaugeTestTimer(nullptr),
    m_gaugeTestResetTimer(nullptr),
    m_indicatorTestTimer(nullptr),
    m_gaugeTestPhase(0),
    m_gaugeTestTick(0),
    m_indicatorTestStep(0),
    m_indicatorTestMaxSteps(0),
    m_currentMaxPoints(500),
    m_plotPaused(false),
    m_plotDirty(false),
    m_latestTelemetryChanged(false),
    m_faultAutoCaptureTriggerMask(0),
    m_faultAutoCaptureDisplayPoints(5000),
    m_faultAutoCapturePacketsAfterTrigger(50),
    m_faultAutoCapturePending(false),
    m_faultAutoCaptureSkipCurrentPacket(false),
    m_faultAutoCapturePacketsRemaining(0),
    m_isLogging(false),
    m_isAdcLogging(false),
    m_isQuickSaving(false),
    m_adcPacketActive(false),
    m_lastAdcPacketMs(0),
    m_adcActivityTimer(nullptr),
    m_quickSaveTimer(nullptr),
    m_syncingFromMask(false),
    m_lastSpeedValue(0.0),
    m_lastTorqueValue(0.0),
    m_targetManuallyEdited(false),
    m_timeManuallyEdited(false),
    m_updatingTargetType(false),
    m_recordHistory(true),
    m_gaugeTimer(nullptr),
    m_plotUpdatesSuspended(false),
    m_syncingPlotAreaVisibility(false),
    m_plotAreaDefaultWidth(0),
    m_scopeAreaStoredWidth(0),
    m_spaceVectorAreaStoredWidth(0),
    m_spaceVectorSourceGroup(nullptr),
    m_selectedSpaceVectorPlotIndex(-1),
    m_spaceVectorTestLastSampleSec(0.0),
    m_spaceVectorTestActive(false),
    m_lastTimestampTicks(0),
    m_hasLastTimestamp(false) {
        ui->setupUi(this);
        m_plotAreaDefaultWidth = width();
        setWindowTitle("Tuning Master");

        // Initialize the serial manager thread.
        m_serialManager = new SerialManager();
        m_serialThread = new QThread(this);
        m_serialManager->moveToThread(m_serialThread);
        connect(m_serialThread, &QThread::finished, m_serialManager, &QObject::deleteLater);

        // Create the data parser in the main thread.
        m_dataParser = new DataParser();
        m_dataParser->moveToThread(m_serialThread);
        connect(m_serialThread, &QThread::finished, m_dataParser, &QObject::deleteLater);
        updateFaultAutoCaptureMask();

        // Signal connections.
        connect(m_serialManager, &SerialManager::portOpened, this, &MainWindow::handleSerialPortOpened);
        connect(m_serialManager, &SerialManager::portClosed, this, &MainWindow::handleSerialPortClosed);
        connect(m_serialManager, &SerialManager::rawDataReceived, m_dataParser, &DataParser::parseData);
        connect(m_dataParser,
                &DataParser::parsedData,
                this,
                [this](const QHash<QString, double> &values) {
                    enqueueTelemetryValues(values);
                },
                Qt::DirectConnection);
        connect(m_dataParser, &DataParser::adcSampleReceived, this, &MainWindow::handleAdcSample);
        connect(m_dataParser, &DataParser::adcSampleActivityReceived, this, [this]() {
            m_lastAdcPacketMs = QDateTime::currentMSecsSinceEpoch();
            m_adcPacketActive = true;
            if (m_adcActivityTimer) {
                m_adcActivityTimer->start(100);
            }
            updateAdcSaveButtonState();
        });
        connect(m_dataParser, &DataParser::packetStatusReceived, this, &MainWindow::handlePacketStatus);
        connect(m_dataParser, &DataParser::receivedTextLine, this, [this](const QString &text) {
            ui->plainTextEditReceive->appendPlainText(text);
            handleAdcStatusText(text);
            parseTuneResponse(text);
        });
        connect(m_dataParser, &DataParser::configurationChanged, this, [this]() {
            updateFaultAutoCaptureMask();
            loadAvailableFields();
            setupGaugeArea();
            setupTuningArea();
            setupSpaceVectorArea();
            syncFieldCheckStates();
            updateStatusIndicators();
        });

        connect(m_dataParser, &DataParser::maskReceived, this, &MainWindow::onMaskReceived);

        // Start the serial worker thread.
        m_serialThread->start(QThread::HighPriority);

        // Initialize the oscilloscope area.
        setupPlottingArea();
        setupGaugeArea();
        updateStatusIndicators();
        applyIndicatorStatus(ui->labelServerStatus, "Disconnected", "off");
        applyIndicatorStatus(ui->labelSimStatus, "N/A", "off");

        // Load the field list into the left panel.
        loadAvailableFields();

        // Timer-driven waveform refresh.
        m_plotTimer = new QTimer(this);
        m_plotTimer->setTimerType(Qt::PreciseTimer);
        connect(m_plotTimer, &QTimer::timeout, this, &MainWindow::updatePlot);
        m_plotTimer->start(16);

        m_adcActivityTimer = new QTimer(this);
        m_adcActivityTimer->setSingleShot(true);
        connect(m_adcActivityTimer, &QTimer::timeout, this, &MainWindow::updateAdcSaveButtonState);

        // Initialize the serial UI.
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
        ui->pushButtonGaugeToggle->setText("◷");
        ui->pushButtonGaugeToggle->setToolTip("Show circular gauges");
        ui->pushButtonGaugeToggle->setCheckable(false);
        ui->pushButtonGaugeTest->setText("↻");
        ui->pushButtonGaugeTest->setToolTip("Sweep gauges for visual testing");
        ui->pushButtonGaugeTest->setCheckable(true);
        ui->pushButtonPause->setText("⏸️");
        ui->pushButtonScopeToggle->setText("〰");
        ui->pushButtonScopeToggle->setToolTip("Show scope area");
        ui->pushButtonScopeToggle->setCheckable(true);
        ui->pushButtonScopeToggle->setChecked(true);
        ui->pushButtonSpaceVectorToggle->setText("⬡");
        ui->pushButtonSpaceVectorToggle->setToolTip("Show space vector area");
        ui->pushButtonSpaceVectorToggle->setCheckable(true);
        ui->pushButtonSpaceVectorToggle->setChecked(false);
        connect(ui->pushButtonScopeToggle, &QPushButton::toggled, this, [this]() {
            updatePlotAreaVisibility(true);
        });
        connect(ui->pushButtonSpaceVectorToggle, &QPushButton::toggled, this, [this]() {
            updatePlotAreaVisibility(true);
        });
        connect(ui->splitterPlotArea, &QSplitter::splitterMoved,
                this, &MainWindow::syncPlotAreaButtonsFromSplitter);
        updatePlotAreaVisibility(false);
        ui->pushButtonSVTest->setText("↻");
        ui->pushButtonSVTest->setToolTip("Run space vector test sweep");
        ui->pushButtonSVTest->setCheckable(true);
        ui->pushButtonSVArrow->setText("↗");
        ui->pushButtonSVArrow->setToolTip("Show latest vector arrow");
        ui->pushButtonSVArrow->setCheckable(true);
        ui->pushButtonSVArrow->setChecked(true);
        ui->spaceVectorContainer->setArrowVisible(ui->pushButtonSVArrow->isChecked());
        connect(ui->pushButtonSVArrow, &QPushButton::toggled,
                ui->spaceVectorContainer, &SpaceVectorWidget::setArrowVisible);
        connect(ui->pushButtonSVTest, &QPushButton::toggled, this, [this](bool checked) {
            if (checked) {
                startSpaceVectorTest();
            } else {
                stopSpaceVectorTest();
            }
        });
        setupSpaceVectorArea();
        updateSpaceVectorControls();
        ui->pushButtonSave->setCheckable(true);
        ui->pushButtonSave->setToolTip("Start or stop saving telemetry to CSV");
        ui->pushButtonSave->setStyleSheet(kSaveButtonStyle);
        ui->pushButtonSaveAdc->setCheckable(true);
        ui->pushButtonSaveAdc->setToolTip("Start or stop saving ADC samples to CSV");
        ui->pushButtonSaveAdc->setStyleSheet(kSaveButtonStyle);
        ui->pushButtonSaveAdc->setProperty("adcRecent", false);
        ui->pushButtonQuickSave->setCheckable(true);
        ui->pushButtonQuickSave->setToolTip("Start quick logging for the selected data");
        ui->pushButtonQuickSave->setStyleSheet(kSaveButtonStyle);
        ui->lineEditSaveTime->setToolTip("Quick log duration in seconds");
        updateAdcSaveButtonState();
        ui->plainTextEditReceive->setReadOnly(true);

        m_quickSaveTimer = new QTimer(this);
        m_quickSaveTimer->setSingleShot(true);
        connect(m_quickSaveTimer, &QTimer::timeout, this, &MainWindow::stopQuickSave);

        m_gaugeTestTimer = new QTimer(this);
        m_gaugeTestTimer->setInterval(16);
        connect(m_gaugeTestTimer, &QTimer::timeout, this, &MainWindow::advanceGaugeTest);

        m_gaugeTestResetTimer = new QTimer(this);
        m_gaugeTestResetTimer->setSingleShot(true);
        m_gaugeTestResetTimer->setInterval(3000);
        connect(m_gaugeTestResetTimer, &QTimer::timeout, this, &MainWindow::resetGaugesAfterTest);

        m_indicatorTestTimer = new QTimer(this);
        m_indicatorTestTimer->setInterval(500);
        connect(m_indicatorTestTimer, &QTimer::timeout, this, &MainWindow::advanceIndicatorTest);
        updateGaugeModeControls();

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
        // Connect tuning parameter signals
        connect(ui->comboBoxTuneSubsystem, QOverload<int>::of(&QComboBox::currentIndexChanged),
        this, &MainWindow::on_comboBoxTuneSubsystem_currentIndexChanged);

        setupTuningArea();

        // Initialize increment slider with predefined step values
        const QVector<double> stepValues = {0.00001, 0.00005, 0.0001, 0.0005, 0.001, 0.005, 0.01, 0.05, 0.1, 0.5, 1.0, 5.0, 10.0, 50.0, 100.0, 500.0, 1000.0, 5000.0, 10000.0};
        ui->incrementSlider->setRange(0, stepValues.size() - 1);
        ui->incrementSlider->setValue(10); // Default 1.0.
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
    stopQuickSave();
    stopTelemetryLogging();
    stopAdcLogging();
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

// ==================== Waveform Area Initialization ====================
void MainWindow::setupPlottingArea() {
    // This area is dynamic: the left field list supplies selectable telemetry
    // names, while the right scroll area owns a variable number of scopes.
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

void MainWindow::updatePlotAreaVisibility(bool allowWidthRetraction)
{
    if (m_syncingPlotAreaVisibility) {
        return;
    }

    const QList<int> currentSizes = ui->splitterPlotArea->sizes();
    const int currentScopeWidth = currentSizes.size() > 0 ? currentSizes[0] : 0;
    const int currentSpaceVectorWidth = currentSizes.size() > 1 ? currentSizes[1] : 0;
    if (currentScopeWidth > 0) {
        m_scopeAreaStoredWidth = currentScopeWidth;
    }
    if (currentSpaceVectorWidth > 0) {
        m_spaceVectorAreaStoredWidth = currentSpaceVectorWidth;
    }

    const bool wasScopeExpanded = currentScopeWidth > 0;
    const bool wasSpaceVectorExpanded = currentSpaceVectorWidth > 0;
    const bool wasBothExpanded = wasScopeExpanded && wasSpaceVectorExpanded;
    const bool showScope = ui->pushButtonScopeToggle->isChecked();
    const bool showSpaceVector = ui->pushButtonSpaceVectorToggle->isChecked();
    const bool hideFromBoth = wasBothExpanded && (!showScope || !showSpaceVector);
    const bool canRetractWidth = allowWidthRetraction &&
                                 hideFromBoth &&
                                 qAbs(width() - m_plotAreaDefaultWidth) <= 2;
    int widthDelta = 0;

    if (canRetractWidth) {
        if (wasScopeExpanded && !showScope) {
            widthDelta += qMax(0, currentScopeWidth);
        }
        if (wasSpaceVectorExpanded && !showSpaceVector) {
            widthDelta += qMax(0, currentSpaceVectorWidth);
        }
    }

    ui->scopeAreaWidget->setVisible(true);
    ui->spaceVectorAreaWidget->setVisible(true);
    ui->splitterPlotArea->setVisible(true);

    const int scopeWidth = showScope
                               ? qMax(m_scopeAreaStoredWidth, ui->scopeAreaWidget->minimumWidth())
                               : 0;
    const int spaceVectorWidth = showSpaceVector
                                     ? qMax(m_spaceVectorAreaStoredWidth, ui->spaceVectorAreaWidget->minimumWidth())
                                     : 0;
    ui->splitterPlotArea->setSizes({scopeWidth, spaceVectorWidth});

    if (canRetractWidth && widthDelta > 0) {
        const int nextWidth = qMax(minimumSizeHint().width(), width() - widthDelta);
        resize(nextWidth, height());
    }
}

void MainWindow::syncPlotAreaButtonsFromSplitter()
{
    if (m_syncingPlotAreaVisibility) {
        return;
    }

    const QList<int> sizes = ui->splitterPlotArea->sizes();
    if (sizes.size() < 2) {
        return;
    }

    const bool showScope = sizes[0] > 0;
    const bool showSpaceVector = sizes[1] > 0;
    if (showScope) {
        m_scopeAreaStoredWidth = sizes[0];
    }
    if (showSpaceVector) {
        m_spaceVectorAreaStoredWidth = sizes[1];
    }

    m_syncingPlotAreaVisibility = true;
    ui->pushButtonScopeToggle->setChecked(showScope);
    ui->pushButtonSpaceVectorToggle->setChecked(showSpaceVector);
    m_syncingPlotAreaVisibility = false;
}

void MainWindow::setupGaugeArea()
{
    // Gauge definitions come from the parser configuration. Rebuilding the area
    // on configuration changes keeps the UI in sync with the loaded protocol.
    QLayout *existingLayout = ui->gaugeArea->layout();
    QBoxLayout *gaugeLayout = qobject_cast<QBoxLayout*>(existingLayout);
    if (!gaugeLayout) {
        delete existingLayout;
        gaugeLayout = new QBoxLayout(m_gaugeCircularMode
                                         ? QBoxLayout::TopToBottom
                                         : QBoxLayout::LeftToRight,
                                     ui->gaugeArea);
        gaugeLayout->setContentsMargins(6, 0, 6, 0);
        ui->gaugeArea->setLayout(gaugeLayout);
    }
    gaugeLayout->setDirection(m_gaugeCircularMode
                                  ? QBoxLayout::TopToBottom
                                  : QBoxLayout::LeftToRight);
    gaugeLayout->setSpacing(m_gaugeCircularMode ? 2 : 18);

    gaugeLayout->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    m_gaugeBindings.clear();
    while (QLayoutItem *item = gaugeLayout->takeAt(0)) {
        if (QWidget *widget = item->widget()) {
            widget->deleteLater();
        }
        delete item;
    }

    gaugeLayout->setAlignment(m_gaugeCircularMode
                                  ? (Qt::AlignHCenter | Qt::AlignTop)
                                  : Qt::AlignCenter);
    if (!m_gaugeCircularMode) {
        gaugeLayout->addStretch(1);
    }
    for (const GaugeDef &gauge : m_dataParser->getGauges()) {
        addGauge(gauge);
    }
    if (!m_gaugeCircularMode) {
        gaugeLayout->addStretch(1);
    }

    if (m_gaugeTimer) {
        m_gaugeTimer->stop();
        m_gaugeTimer->deleteLater();
        m_gaugeTimer = nullptr;
    }
}

void MainWindow::addGauge(const GaugeDef &gauge)
{
    QBoxLayout *gaugeLayout = qobject_cast<QBoxLayout*>(ui->gaugeArea->layout());
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
    meter->setValueDecimals(gauge.valueDecimals);
    meter->setPeakHoldMs(1000);
    meter->setDisplayMode(m_gaugeCircularMode
                              ? AudioLevelMeter::DisplayMode::Circular
                              : AudioLevelMeter::DisplayMode::VerticalBar);

    const QString secondarySource = gauge.secondaryDataSource.trimmed();
    const bool secondaryUsesPeakHold = secondarySource.isEmpty() ||
        secondarySource.compare("MAXVAL", Qt::CaseInsensitive) == 0;
    meter->setPeakTrackingEnabled(secondaryUsesPeakHold);
    meter->setSecondaryColorTracksPrimary(secondaryUsesPeakHold);

    gaugeLayout->addWidget(meter, 0);
    m_gaugeBindings.append({gauge.dataSource,
                            secondaryUsesPeakHold ? QString() : secondarySource,
                            secondaryUsesPeakHold,
                            meter,
                            gauge.minimum,
                            gauge.maximum});
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
        if (!values.contains(binding.fieldName)) {
            continue;
        }
        const double primaryValue = values.value(binding.fieldName);
        std::optional<double> secondaryValue;
        if (!binding.secondaryUsesPeakHold && values.contains(binding.secondaryFieldName)) {
            secondaryValue = values.value(binding.secondaryFieldName);
        }
        applyGaugeValues(binding, primaryValue, secondaryValue);
    }
}

void MainWindow::flushGaugeUpdates()
{
    updateStatusIndicators();
}

void MainWindow::applyGaugeValues(GaugeBinding &binding,
                                  double primaryValue,
                                  std::optional<double> secondaryValue)
{
    binding.currentValue = primaryValue;
    binding.hasCurrentValue = true;
    if (secondaryValue.has_value()) {
        binding.currentSecondaryValue = secondaryValue.value();
        binding.hasCurrentSecondaryValue = true;
    }

    if (!binding.meter) {
        return;
    }

    binding.meter->setValue(primaryValue);
    if (secondaryValue.has_value()) {
        binding.meter->setPeakValue(secondaryValue.value());
    } else if (!binding.secondaryUsesPeakHold) {
        binding.meter->setPeakValue(primaryValue);
    }
}

void MainWindow::updateGaugeModeControls()
{
    if (!ui) {
        return;
    }

    ui->pushButtonGaugeToggle->setText(m_gaugeCircularMode ? "▥" : "◷");
    ui->pushButtonGaugeToggle->setToolTip(m_gaugeCircularMode
                                              ? "Show vertical bar gauges"
                                              : "Show circular gauges");
    const bool testActive = (m_gaugeTestTimer && m_gaugeTestTimer->isActive()) ||
        (m_indicatorTestTimer && m_indicatorTestTimer->isActive());
    ui->pushButtonGaugeTest->setText(testActive ? "■" : "↻");
    ui->pushButtonGaugeTest->setToolTip(testActive
                                            ? "Stop gauge test sweep"
                                            : "Sweep gauges for visual testing");
}

void MainWindow::setupSpaceVectorArea()
{
    if (m_spaceVectorSourceGroup) {
        delete m_spaceVectorSourceGroup;
    }
    m_spaceVectorSourceGroup = new QButtonGroup(this);
    m_spaceVectorSourceGroup->setExclusive(true);

    for (QPushButton *button : m_spaceVectorSourceButtons) {
        ui->gridLayout_5->removeWidget(button);
        button->deleteLater();
    }
    m_spaceVectorSourceButtons.clear();

    ui->gridLayout_5->removeWidget(ui->vectorControlFiller);
    const QList<SpaceVectorPlotDef> plots = m_dataParser->getSpaceVectorPlots();
    for (int i = 0; i < plots.size(); ++i) {
        const SpaceVectorPlotDef &plot = plots[i];
        QPushButton *button = new QPushButton(plot.type == "abc" ? "a" : "α", ui->widgetWarpperSpaceVectorControl);
        button->setCheckable(true);
        button->setFixedSize(27, 24);
        button->setToolTip(plot.name);
        m_spaceVectorSourceGroup->addButton(button, i);
        m_spaceVectorSourceButtons.append(button);
        ui->gridLayout_5->addWidget(button, 0, 2 + i);
    }
    ui->gridLayout_5->addWidget(ui->vectorControlFiller, 0, 2 + plots.size());

    if (m_selectedSpaceVectorPlotIndex < 0 || m_selectedSpaceVectorPlotIndex >= plots.size()) {
        m_selectedSpaceVectorPlotIndex = plots.isEmpty() ? -1 : 0;
    }
    if (m_selectedSpaceVectorPlotIndex >= 0) {
        m_spaceVectorSourceButtons[m_selectedSpaceVectorPlotIndex]->setChecked(true);
    }

    connect(m_spaceVectorSourceGroup, &QButtonGroup::idClicked, this, [this](int id) {
        if (m_selectedSpaceVectorPlotIndex == id) {
            return;
        }
        m_selectedSpaceVectorPlotIndex = id;
        ui->spaceVectorContainer->clearTrace();
    });
}

void MainWindow::updateSpaceVectorControls()
{
    ui->pushButtonSVTest->setText(m_spaceVectorTestActive ? "■" : "↻");
    ui->pushButtonSVTest->setToolTip(m_spaceVectorTestActive
                                         ? "Stop space vector test sweep"
                                         : "Run space vector test sweep");
    for (QPushButton *button : m_spaceVectorSourceButtons) {
        button->setEnabled(!m_spaceVectorTestActive);
    }
}

void MainWindow::appendSpaceVectorSample(const QHash<QString, double> &values,
                                         bool allowDuringTest,
                                         bool capToBoundary)
{
    if ((m_spaceVectorTestActive && !allowDuringTest) || !m_dataParser) {
        return;
    }

    const QList<SpaceVectorPlotDef> plots = m_dataParser->getSpaceVectorPlots();
    if (m_selectedSpaceVectorPlotIndex < 0 || m_selectedSpaceVectorPlotIndex >= plots.size()) {
        return;
    }

    const SpaceVectorPlotDef &plot = plots[m_selectedSpaceVectorPlotIndex];
    const QHash<QString, double> &sourceValues = allowDuringTest ? values : m_latestTelemetryValues;
    if (!sourceValues.contains(plot.vdcDataSource)) {
        return;
    }

    const double vdc = sourceValues.value(plot.vdcDataSource);
    double alpha = 0.0;
    double beta = 0.0;

    if (plot.type == "alpha-beta") {
        if (!values.contains(plot.alphaDataSource) && !values.contains(plot.betaDataSource)) {
            return;
        }
        if (!sourceValues.contains(plot.alphaDataSource) ||
            !sourceValues.contains(plot.betaDataSource)) {
            return;
        }
        alpha = sourceValues.value(plot.alphaDataSource);
        beta = sourceValues.value(plot.betaDataSource);
    } else if (plot.type == "abc") {
        if (!values.contains(plot.aDataSource) &&
            !values.contains(plot.bDataSource) &&
            !values.contains(plot.cDataSource)) {
            return;
        }
        if (!sourceValues.contains(plot.aDataSource) ||
            !sourceValues.contains(plot.bDataSource) ||
            !sourceValues.contains(plot.cDataSource)) {
            return;
        }
        const double a = sourceValues.value(plot.aDataSource);
        const double b = sourceValues.value(plot.bDataSource);
        const double c = sourceValues.value(plot.cDataSource);
        alpha = (2.0 * a - b - c) / 3.0;
        beta = (b - c) / std::sqrt(3.0);
    } else {
        return;
    }

    ui->spaceVectorContainer->appendSample(alpha, beta, vdc, capToBoundary);
}

void MainWindow::updateSpaceVectorPlot()
{
    if (m_spaceVectorTestActive) {
        const double elapsedSec = m_spaceVectorTestElapsed.elapsed() / 1000.0;
        constexpr double kTestRevolutionSec = 2.0;
        constexpr double kTestDurationSec = 10.0;
        constexpr double kTestSamplesPerSecond = 5000.0;
        constexpr double kPi = 3.14159265358979323846;
        constexpr double kTestVdc = 24.0;
        const double endSec = qMin(elapsedSec, kTestDurationSec);

        if (endSec > m_spaceVectorTestLastSampleSec) {
            const int firstSample = static_cast<int>(std::floor(m_spaceVectorTestLastSampleSec * kTestSamplesPerSecond));
            const int lastSample = static_cast<int>(std::floor(endSec * kTestSamplesPerSecond));
            const QList<SpaceVectorPlotDef> plots = m_dataParser->getSpaceVectorPlots();
            if (m_selectedSpaceVectorPlotIndex >= 0 &&
                m_selectedSpaceVectorPlotIndex < plots.size()) {
                const SpaceVectorPlotDef &plot = plots[m_selectedSpaceVectorPlotIndex];

                for (int sample = firstSample; sample < lastSample; ++sample) {
                    const double sampleSec = (sample + 1) / kTestSamplesPerSecond;
                    double magnitude = kTestVdc / std::sqrt(3.0) / 2.0;
                    double phase = std::fmod(sampleSec, kTestRevolutionSec) / kTestRevolutionSec * 2.0 * kPi;
                    bool capToBoundary = false;

                    if (sampleSec < 2.0) {
                        magnitude = kTestVdc / std::sqrt(3.0) / 2.0;
                    } else if (sampleSec < 4.0) {
                        magnitude = kTestVdc / std::sqrt(3.0);
                    } else if (sampleSec < 6.0) {
                        magnitude = 2.0 * kTestVdc / kPi;
                        capToBoundary = true;
                    } else {
                        const double rampSec = sampleSec - 6.0;
                        phase = rampSec / kTestRevolutionSec * 2.0 * kPi;
                        magnitude = (2.0 * kTestVdc / kPi) * qBound(0.0, rampSec / 4.0, 1.0);
                        capToBoundary = true;
                    }

                    const double alpha = magnitude * std::cos(phase);
                    const double beta = magnitude * std::sin(phase);
                    QHash<QString, double> sampleValues;
                    sampleValues.insert(plot.vdcDataSource, kTestVdc);
                    if (plot.type == "abc") {
                        sampleValues.insert(plot.aDataSource, alpha);
                        sampleValues.insert(plot.bDataSource, -0.5 * alpha + std::sqrt(3.0) * 0.5 * beta);
                        sampleValues.insert(plot.cDataSource, -0.5 * alpha - std::sqrt(3.0) * 0.5 * beta);
                    } else {
                        sampleValues.insert(plot.alphaDataSource, alpha);
                        sampleValues.insert(plot.betaDataSource, beta);
                    }
                    appendSpaceVectorSample(sampleValues, true, capToBoundary);
                }
            }
            m_spaceVectorTestLastSampleSec = endSec;
        }

        if (elapsedSec >= kTestDurationSec) {
            stopSpaceVectorTest();
        }
    }

    ui->spaceVectorContainer->refreshPlot();
}

void MainWindow::startSpaceVectorTest()
{
    m_spaceVectorTestActive = true;
    m_spaceVectorTestElapsed.restart();
    m_spaceVectorTestLastSampleSec = 0.0;
    ui->spaceVectorContainer->clearTrace();
    ui->pushButtonSVTest->setChecked(true);
    updateSpaceVectorControls();
}

void MainWindow::stopSpaceVectorTest()
{
    m_spaceVectorTestActive = false;
    {
        const QSignalBlocker blocker(ui->pushButtonSVTest);
        ui->pushButtonSVTest->setChecked(false);
    }
    updateSpaceVectorControls();
}

void MainWindow::startGaugeTest()
{
    const bool testGauges = !m_gaugeBindings.isEmpty();
    const bool testIndicators = hasTestableIndicators();
    if (!testGauges && !testIndicators) {
        ui->pushButtonGaugeTest->setChecked(false);
        return;
    }

    if (testGauges) {
        m_gaugeTestPhase = 0;
        m_gaugeTestTick = 0;
    }
    if (m_gaugeTestResetTimer) {
        m_gaugeTestResetTimer->stop();
    }
    if (testGauges) {
        for (GaugeBinding &binding : m_gaugeBindings) {
            if (binding.meter) {
                binding.meter->setPeakTrackingEnabled(false);
            }
            applyGaugeValues(binding, binding.minimum, binding.minimum);
        }

        if (m_gaugeTestTimer) {
            m_gaugeTestTimer->start();
        }
    }
    if (testIndicators) {
        startIndicatorTest();
    }
    updateGaugeModeControls();
}

void MainWindow::startIndicatorTest()
{
    if (!hasTestableIndicators()) {
        return;
    }

    m_indicatorTestStep = 0;
    m_indicatorTestMaxSteps = 0;
    for (const IndicatorDef &indicator : m_dataParser->getIndicators()) {
        m_indicatorTestMaxSteps = qMax(m_indicatorTestMaxSteps, indicator.statuses.size());
    }

    advanceIndicatorTest();
    if (m_indicatorTestTimer) {
        m_indicatorTestTimer->start();
    }
}

void MainWindow::stopIndicatorTest(bool restoreLiveState)
{
    if (m_indicatorTestTimer) {
        m_indicatorTestTimer->stop();
    }
    m_indicatorTestStep = 0;
    m_indicatorTestMaxSteps = 0;
    if (restoreLiveState) {
        updateStatusIndicators();
    }
    if (!m_gaugeTestTimer || !m_gaugeTestTimer->isActive()) {
        ui->pushButtonGaugeTest->setChecked(false);
    }
}

void MainWindow::advanceIndicatorTest()
{
    if (!m_dataParser || m_indicatorTestMaxSteps <= 0) {
        stopIndicatorTest();
        return;
    }
    if (m_indicatorTestStep >= m_indicatorTestMaxSteps) {
        stopIndicatorTest();
        updateGaugeModeControls();
        return;
    }

    for (const IndicatorDef &indicator : m_dataParser->getIndicators()) {
        if (m_indicatorTestStep < indicator.statuses.size()) {
            const IndicatorStatusDef &status = indicator.statuses.at(m_indicatorTestStep);
            applyIndicatorState(indicator, &status);
        }
    }

    ++m_indicatorTestStep;
}

bool MainWindow::hasTestableIndicators() const
{
    if (!m_dataParser) {
        return false;
    }

    for (const IndicatorDef &indicator : m_dataParser->getIndicators()) {
        if (!indicator.statuses.isEmpty()) {
            return true;
        }
    }
    return false;
}

void MainWindow::stopGaugeTest(bool scheduleReset)
{
    if (m_gaugeTestTimer) {
        m_gaugeTestTimer->stop();
    }
    stopIndicatorTest(true);

    ui->pushButtonGaugeTest->setChecked(false);
    if (scheduleReset && m_gaugeTestResetTimer) {
        m_gaugeTestResetTimer->start();
    } else {
        resetGaugesAfterTest();
    }
    updateGaugeModeControls();
}

void MainWindow::advanceGaugeTest()
{
    constexpr int kTicksPerPhase = 100;
    const double progress = qMin(1.0, m_gaugeTestTick / double(kTicksPerPhase - 1));

    for (GaugeBinding &binding : m_gaugeBindings) {
        if (!binding.meter) {
            continue;
        }

        const double sweptValue = binding.minimum + (binding.maximum - binding.minimum) * progress;
        if (m_gaugeTestPhase == 0) {
            applyGaugeValues(binding, sweptValue, binding.minimum);
        } else {
            applyGaugeValues(binding, binding.maximum, sweptValue);
        }
    }

    ++m_gaugeTestTick;
    if (m_gaugeTestTick >= kTicksPerPhase) {
        m_gaugeTestTick = 0;
        ++m_gaugeTestPhase;
        if (m_gaugeTestPhase > 1) {
            if (m_gaugeTestTimer) {
                m_gaugeTestTimer->stop();
            }
            if (m_gaugeTestResetTimer) {
                m_gaugeTestResetTimer->start();
            }
            if (!m_indicatorTestTimer || !m_indicatorTestTimer->isActive()) {
                ui->pushButtonGaugeTest->setChecked(false);
            }
            updateGaugeModeControls();
        }
    }
}

void MainWindow::resetGaugesAfterTest()
{
    for (GaugeBinding &binding : m_gaugeBindings) {
        if (!binding.meter) {
            continue;
        }

        const double restoredValue = qBound(binding.minimum, 0.0, binding.maximum);
        const double restoredSecondaryValue = restoredValue;
        if (binding.meter) {
            binding.meter->setPeakTrackingEnabled(false);
        }
        applyGaugeValues(binding, restoredValue, restoredSecondaryValue);
        if (binding.meter) {
            binding.meter->setPeakTrackingEnabled(binding.secondaryUsesPeakHold);
        }
    }
    updateGaugeModeControls();
}

void MainWindow::updateStatusIndicators()
{
    // Indicators are declarative: each one resolves a data source and then picks
    // the first configured status rule that matches the current telemetry state.
    if (!m_dataParser) {
        return;
    }
    if (m_indicatorTestTimer && m_indicatorTestTimer->isActive()) {
        return;
    }

    QSet<int> configuredIndicators;
    for (const IndicatorDef &indicator : m_dataParser->getIndicators()) {
        configuredIndicators.insert(indicator.indicator);

        const IndicatorStatusDef *status = resolveIndicatorStatus(indicator);
        if (!status) {
            status = defaultIndicatorStatus(indicator);
        }

        applyIndicatorState(indicator, status);
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

void MainWindow::applyIndicatorState(const IndicatorDef &indicator, const IndicatorStatusDef *status)
{
    if (!status) {
        return;
    }

    QLabel *label = findChild<QLabel*>(QString("statusLed%1").arg(indicator.indicator));
    if (!label) {
        return;
    }

    applyIndicatorStatus(label, status->displayText, status->color);
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
    // Scope ownership is transferred to the layout/container. The parallel
    // m_oscilloscopes list records visual order for move up/down operations.
    OscilloscopeWidget *osc = new OscilloscopeWidget;
    osc->setColorList(getPresetColors());
    if (!title.isEmpty())
        osc->setTitle(title);
    else
        osc->setTitle(QString("Scope %1").arg(m_oscilloscopes.size() + 1));

    // Connect configuration requests from the scope gear button.
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
    // Double-clicking a field creates a new oscilloscope and adds that field.
    addOscilloscope();
    OscilloscopeWidget *newOsc = m_oscilloscopes.last();
    newOsc->setFields({item->text()});
}

void MainWindow::on_oscilloscopeConfigRequested(OscilloscopeWidget *osc) {
    // The configuration dialog mirrors the current scope field list. Applying
    // changes replaces that field list and then reapplies chosen graph colors.
    QDialog dialog(this);
    dialog.setWindowTitle("Configure Fields to Plot");
    dialog.setMinimumWidth(350);

    QTableWidget *table = new QTableWidget(&dialog);
    table->setColumnCount(2);
    table->setHorizontalHeaderLabels({"Field", "Color"});
    table->setColumnWidth(0, 180); // Set the first column width to 180 pixels.
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table->verticalHeader()->setVisible(false);

    QStringList allFields = m_dataParser->getFieldNames();
    QStringList currentFields = osc->getFields();

    // Preset color list, kept consistent with OscilloscopeWidget.
    QList<QColor> presetColors = MainWindow::getPresetColors();
    QStringList colorNames = MainWindow::getColorNames();

    // Store a color combo-box pointer for each field.
    QHash<int, QComboBox*> colorCombos;

    table->setRowCount(allFields.size());
    for (int i = 0; i < allFields.size(); ++i) {
        QString field = allFields[i];
        bool checked = currentFields.contains(field);

        // First column: field name plus checkbox, not editable.
        QTableWidgetItem *item = new QTableWidgetItem(field);
        item->setFlags(item->flags() & ~Qt::ItemIsEditable);   // Disable editing.
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
        table->setItem(i, 0, item);

        // Second column: create a color combo box only for checked fields.
        if (checked) {
            QComboBox *combo = new QComboBox();
            // Add preset colors with swatch icons.
            for (int j = 0; j < presetColors.size(); ++j) {
                QPixmap pixmap(16, 16);
                pixmap.fill(presetColors[j]);
                combo->addItem(QIcon(pixmap), colorNames[j], presetColors[j]);
            }
            combo->addItem("Custom...");   // Custom color option.

            // Try to preserve the current color for this field.
            QColor currentColor = osc->getFieldColor(field);
            if (currentColor.isValid()) {
                int index = presetColors.indexOf(currentColor);
                if (index >= 0)
                    combo->setCurrentIndex(index);
                else
                    combo->setCurrentIndex(presetColors.size()); // Custom...
            } else {
                // Default: assign a preset color by current field index.
                int idx = currentFields.indexOf(field);
                if (idx >= 0 && idx < presetColors.size())
                    combo->setCurrentIndex(idx);
            }

            // Handle Custom... by opening the color dialog.
            connect(combo, QOverload<int>::of(&QComboBox::activated), this, [combo, field, this](int index) {
                if (index == combo->count() - 1) { // The last item is "Custom..."
                    QColor newColor = QColorDialog::getColor(combo->palette().button().color(), nullptr,
                                                             QString("Select Color for %1").arg(field));
                    if (newColor.isValid()) {
                        // Store the custom color as user data and update its icon.
                        QPixmap pixmap(16, 16);
                        pixmap.fill(newColor);
                        combo->setItemIcon(index, QIcon(pixmap));
                        combo->setItemData(index, newColor, Qt::UserRole);
                        combo->setCurrentIndex(index);
                    } else {
                        // Selection was cancelled; restore the previous choice.
                        int prev = combo->property("prevIndex").toInt();
                        combo->setCurrentIndex(prev);
                    }
                }
                combo->setProperty("prevIndex", combo->currentIndex());
            });

            // Store the current index so cancellation can restore it.
            combo->setProperty("prevIndex", combo->currentIndex());

            table->setCellWidget(i, 1, combo);
            colorCombos[i] = combo;
        } else {
            table->setCellWidget(i, 1, nullptr);
        }
    }

    // Double-clicking a field row toggles its checked state.
    connect(table, &QTableWidget::cellDoubleClicked, this, [table](int row, int column) {
        Q_UNUSED(column);
        QTableWidgetItem *item = table->item(row, 0);
        if (item) {
            Qt::CheckState newState = (item->checkState() == Qt::Checked) ? Qt::Unchecked : Qt::Checked;
            item->setCheckState(newState);
        }
    });

    // React to checkbox changes by adding/removing color combo boxes dynamically.
    connect(table, &QTableWidget::itemChanged, this, [table, &colorCombos, presetColors, colorNames](QTableWidgetItem *item) {
        if (item->column() != 0) return;
        int row = item->row();
        bool checked = (item->checkState() == Qt::Checked);
        QString field = item->text();

        if (checked && !colorCombos.contains(row)) {
            // Create a color combo box.
            QComboBox *combo = new QComboBox();
            for (int j = 0; j < presetColors.size(); ++j) {
                QPixmap pixmap(16, 16);
                pixmap.fill(presetColors[j]);
                combo->addItem(QIcon(pixmap), colorNames[j], presetColors[j]);
            }
            combo->addItem("Custom...");
            combo->setCurrentIndex(0);
            combo->setProperty("prevIndex", 0);

            // Handle the Custom... option.
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
                        // Custom... option: read the stored custom color from item data.
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

    // Swap pointers in the list.
    m_oscilloscopes.swapItemsAt(idx, idx - 1);  // Equivalent to swapping adjacent oscilloscope pointers.

    // Swap positions in the layout by taking the two adjacent layout items and
    // reinserting their widgets in the opposite order.
    QLayoutItem *itemUp = m_oscLayout->takeAt(idx - 1);
    QLayoutItem *itemDown = m_oscLayout->takeAt(idx - 1); // After taking the first item, the original idx shifts to idx - 1.
    if (itemUp && itemDown) {
        // Reinsert in the opposite order.
        m_oscLayout->insertWidget(idx - 1, itemDown->widget());
        m_oscLayout->insertWidget(idx, itemUp->widget());
    }
    // Delete temporary layout items without deleting their widgets.
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

    // Swap positions in the layout.
    QLayoutItem *itemCurrent = m_oscLayout->takeAt(idx);
    QLayoutItem *itemNext = m_oscLayout->takeAt(idx); // The original idx + 1 item has shifted to idx.
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
            // Up arrow: browse the previous command-history entry.
            if (!m_sendHistory.isEmpty() && m_historyIndex > 0) {
                m_historyIndex--;
                ui->lineEditSend->setText(m_sendHistory[m_historyIndex]);
            }
            return true;
        } else if (keyEvent->key() == Qt::Key_Down) {
            // Down arrow: browse the next command-history entry.
            if (!m_sendHistory.isEmpty() && m_historyIndex < m_sendHistory.size() - 1) {
                m_historyIndex++;
                ui->lineEditSend->setText(m_sendHistory[m_historyIndex]);
            } else if (m_historyIndex == m_sendHistory.size() - 1) {
                // Already at the newest entry; clear the input box.
                ui->lineEditSend->clear();
                m_historyIndex = m_sendHistory.size(); // Point just after the end.
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
            m_plotTimer->start(16);
        }
        drainPendingTelemetry(true);
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
    if (m_plotUpdatesSuspended) {
        return;
    }

    drainPendingTelemetry(false);
    updateStatusIndicators();
    updateSpaceVectorPlot();
}

// ==================== Data Handling ====================
void MainWindow::enqueueTelemetryValues(const QHash<QString, double> &values)
{
    {
        QMutexLocker locker(&m_pendingTelemetryMutex);
        m_pendingTelemetryPackets.append(values);
        if (m_pendingTelemetryPackets.size() > m_maxWavePoints) {
            m_pendingTelemetryPackets.remove(0, m_pendingTelemetryPackets.size() - m_maxWavePoints);
        }
    }

    if (!m_faultCaptureDrainArmed.load(std::memory_order_acquire)) {
        return;
    }

    bool expected = false;
    if (!m_faultCaptureDrainQueued.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }

    QMetaObject::invokeMethod(this, [this]() {
        m_faultCaptureDrainQueued.store(false, std::memory_order_release);
        drainPendingTelemetry(true);
    }, Qt::QueuedConnection);
}

QVector<QHash<QString, double>> MainWindow::takePendingTelemetryPackets()
{
    QMutexLocker locker(&m_pendingTelemetryMutex);
    QVector<QHash<QString, double>> packets;
    packets.swap(m_pendingTelemetryPackets);
    return packets;
}

void MainWindow::clearPendingTelemetryPackets()
{
    QMutexLocker locker(&m_pendingTelemetryMutex);
    m_pendingTelemetryPackets.clear();
    m_faultCaptureDrainQueued.store(false, std::memory_order_release);
}

bool MainWindow::drainPendingTelemetry(bool forcePlotUpdate)
{
    const QVector<QHash<QString, double>> packets = takePendingTelemetryPackets();
    if (packets.isEmpty()) {
        if (forcePlotUpdate && !m_plotPaused && m_plotDirty) {
            updateAllPlots();
        }
        return false;
    }

    for (const QHash<QString, double> &values : packets) {
        ingestTelemetryPacket(values);
    }

    if (m_latestTelemetryChanged) {
        updateGauges(m_latestTelemetryValues);
        m_latestTelemetryChanged = false;
    }

    if ((forcePlotUpdate || m_plotDirty) && !m_plotPaused) {
        updateAllPlots();
    }

    return true;
}

void MainWindow::ingestTelemetryPacket(const QHash<QString, double> &values)
{
    // This is the central telemetry fan-out for buffered packets: maintain plot
    // buffers and latest-value state, while rendering happens on the UI cadence.
    const double currentTime = values.value(DataParser::TIMESTAMP_SECONDS_FIELD,
                                            static_cast<double>(static_cast<quint64>(values.value(DataParser::TIMESTAMP_FIELD, 0.0))) / 275000000.0);
    const quint64 timestampTicks = values.contains(DataParser::TIMESTAMP_US_FIELD)
                                       ? static_cast<quint64>(values.value(DataParser::TIMESTAMP_US_FIELD, 0.0))
                                       : static_cast<quint64>(values.value(DataParser::TIMESTAMP_FIELD, 0.0));

    if (m_hasLastTimestamp && timestampTicks < m_lastTimestampTicks) {
        m_timeStamps.clear();
        m_waveData.clear();
        m_pausedTimeStamps.clear();
        m_pausedWaveData.clear();
        m_latestTelemetryValues.clear();
        m_latestTelemetryChanged = true;
    }

    m_lastTimestampTicks = timestampTicks;
    m_hasLastTimestamp = true;

    addTimeStamp(currentTime);
    const int targetSize = m_timeStamps.size();
    const double missingValue = std::numeric_limits<double>::quiet_NaN();

    for (auto it = m_waveData.begin(); it != m_waveData.end(); ++it) {
        QVector<double> &vec = it.value();
        while (vec.size() < targetSize - 1) {
            vec.prepend(missingValue);
        }
        if (vec.size() > targetSize - 1) {
            vec.remove(0, vec.size() - (targetSize - 1));
        }
        vec.append(missingValue);
    }

    for (auto it = values.cbegin(); it != values.cend(); ++it) {
        if (it.key() != DataParser::TIMESTAMP_FIELD &&
            it.key() != DataParser::TIMESTAMP_US_FIELD &&
            it.key() != DataParser::TIMESTAMP_SECONDS_FIELD) {
            m_latestTelemetryValues[it.key()] = it.value();
            m_latestTelemetryChanged = true;
        }
    }
    appendSpaceVectorSample(values);
    // Append values to waveform buffers.
    for (auto it = values.begin(); it != values.end(); ++it) {
        const QString &field = it.key();
        if (field == DataParser::TIMESTAMP_FIELD ||
            field == DataParser::TIMESTAMP_US_FIELD ||
            field == DataParser::TIMESTAMP_SECONDS_FIELD) {
            continue;
        }
        QVector<double> &vec = m_waveData[field];
        while (vec.size() < targetSize) {
            vec.prepend(missingValue);
        }
        if (vec.size() > targetSize) {
            vec.remove(0, vec.size() - targetSize);
        }
        vec[targetSize - 1] = it.value();
    }
    m_plotDirty = true;
    advanceFaultAutoCapture();
}

void MainWindow::handleAdcSample(const AdcSamplePacket &packet) {
    writeAdcLogRows(packet);
}

void MainWindow::capturePausedPlotSnapshot()
{
    // Pause mode freezes a copy of the currently visible buffers while the live
    // buffers continue receiving data in the background.
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
    // Fault auto-capture freezes the display shortly after a configured fault so
    // pre-trigger and post-trigger samples are both visible for diagnosis.
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
    m_faultCaptureDrainArmed.store(true, std::memory_order_release);

    if (m_currentMaxPoints != requestedPoints) {
        m_sampleSlider->setValue(requestedPoints);
    } else {
        m_plotDirty = true;
        updateAllPlots();
    }

    drainPendingTelemetry(true);
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
        m_faultCaptureDrainArmed.store(false, std::memory_order_release);
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
        m_syncingFromMask = false;   // Important: always reset the guard flag.
        return;
    }
    lastMask1 = mask1;
    lastMask2 = mask2;

    // Walk every item in the field list.
    for (int i = 0; i < m_fieldList->count(); ++i) {
        QListWidgetItem *item = m_fieldList->item(i);
        if (!item) continue;
        // Resolve the mask bit for this field name from DataParser definitions.
        QString fieldName = item->text();
        bool shouldCheck = m_dataParser->isFieldEnabled(fieldName, mask1, mask2);
        if (shouldCheck != (item->checkState() == Qt::Checked)) {
            item->setCheckState(shouldCheck ? Qt::Checked : Qt::Unchecked);
        }
    }

    m_syncingFromMask = false;
}

// ==================== Serial Handling ====================
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
        ui->pushButtonStartToggle->setText("■");
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
    // All serial command paths funnel through this helper so threading,
    // connection checks, and byte conversion stay consistent.
    if (!m_serialManager) return;
    QByteArray data = cmd.toUtf8();
    QMetaObject::invokeMethod(m_serialManager, "sendData",
                              Qt::QueuedConnection,
                              Q_ARG(QByteArray, data));
    ui->plainTextEditReceive->appendPlainText(">> " + cmd);
}

void MainWindow::setupTuningArea()
{
    const QList<TuneSubsystemDef> &tuning = m_dataParser->getTuningDefinitions();
    const QString previousSubsystemCommand = currentTuneSubsystemCommand();
    const QString previousParameterCommand = currentTuneParameterCommand();

    ui->comboBoxTuneSubsystem->blockSignals(true);
    ui->comboBoxTuneParameter->blockSignals(true);
    ui->comboBoxTuneSubsystem->clear();
    ui->comboBoxTuneParameter->clear();

    for (const TuneSubsystemDef &subsystem : tuning) {
        ui->comboBoxTuneSubsystem->addItem(subsystem.name, subsystem.command);
    }

    int subsystemIndex = ui->comboBoxTuneSubsystem->findData(previousSubsystemCommand);
    if (subsystemIndex < 0 && ui->comboBoxTuneSubsystem->count() > 0) {
        subsystemIndex = 0;
    }
    if (subsystemIndex >= 0) {
        ui->comboBoxTuneSubsystem->setCurrentIndex(subsystemIndex);
    }

    ui->comboBoxTuneSubsystem->blockSignals(false);
    ui->comboBoxTuneParameter->blockSignals(false);
    on_comboBoxTuneSubsystem_currentIndexChanged(subsystemIndex);

    const int parameterIndex = ui->comboBoxTuneParameter->findData(previousParameterCommand);
    if (parameterIndex >= 0) {
        ui->comboBoxTuneParameter->setCurrentIndex(parameterIndex);
    }
}

QString MainWindow::currentTuneSubsystemCommand() const
{
    const QVariant command = ui->comboBoxTuneSubsystem->currentData();
    return command.toString().isEmpty()
               ? ui->comboBoxTuneSubsystem->currentText()
               : command.toString();
}

QString MainWindow::currentTuneParameterCommand() const
{
    const QVariant command = ui->comboBoxTuneParameter->currentData();
    return command.toString().isEmpty()
               ? ui->comboBoxTuneParameter->currentText()
               : command.toString();
}

void MainWindow::on_pushButtonStartToggle_clicked() {
    if (m_serialManager->thread() == nullptr) return;

    if (ui->pushButtonStartToggle->text() == "■") {
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
void MainWindow::on_pushButtonResetAlign_clicked() { sendCommand("align reset\r\n"); }

void MainWindow::on_pushButtonSyncSim_clicked()
{
    if (!m_simulationDialog) {
        m_simulationDialog = new simulationDialog(this);
        connect(m_simulationDialog, &simulationDialog::serialCommandRequested,
                this, [this](const QString &command) {
                    sendCommand(command);
                });
        connect(m_simulationDialog, &simulationDialog::mainStatusChanged,
                this, [this](const QString &connectionText,
                              const QString &connectionColor,
                              const QString &simulationText,
                              const QString &simulationColor) {
                    applyIndicatorStatus(ui->labelServerStatus, connectionText, connectionColor);
                    applyIndicatorStatus(ui->labelSimStatus, simulationText, simulationColor);
                });
    }

    m_simulationDialog->show();
    m_simulationDialog->raise();
    m_simulationDialog->activateWindow();
}

void MainWindow::on_pushButtonPreset1_clicked() { sendCommand("log preset 1\r\n"); }
void MainWindow::on_pushButtonPreset2_clicked() { sendCommand("log preset 2\r\n"); }
void MainWindow::on_pushButtonPreset3_clicked() { sendCommand("log preset 3\r\n"); }
void MainWindow::on_pushButtonPreset4_clicked() { sendCommand("log preset 4\r\n"); }
void MainWindow::on_pushButtonRemoveAll_clicked() { sendCommand("log rm all\r\n"); }
void MainWindow::on_pushButtonBin_clicked()     { sendCommand("log bin\r\n"); }
void MainWindow::on_pushButtonUtf8_clicked()    { sendCommand("log utf8\r\n"); }

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
        val = qBound(0.0, val, 0.15);
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
            target = qBound(0.0, target, 0.15);
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
    QString subsys = currentTuneSubsystemCommand();
    QString param = currentTuneParameterCommand();
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
    QString subsys = currentTuneSubsystemCommand();
    QString param = currentTuneParameterCommand();
    QString cmd = QString("tune %1 %2 %3\r\n").arg(subsys, param, QString::number(oldVal, 'f', 4));
    sendCommand(cmd);
    // Note: The slave will return the new value (i.e., oldVal) and its previous value (i.e., currentValue),
    // The response will again trigger parseTuneResponse, automatically updating the history stack and display.
}

void MainWindow::on_pushButtonTuneEnquire_clicked() {
    QString subsys = currentTuneSubsystemCommand();
    QString param = currentTuneParameterCommand();
    QString cmd = QString("tune %1 %2 ?\r\n").arg(subsys, param);
    m_recordHistory = false;
    sendCommand(cmd);
}

void MainWindow::on_pushButtonIncrement_clicked() {
    double step = m_stepValues[ui->incrementSlider->value()];
    QString subsys = currentTuneSubsystemCommand();
    QString param = currentTuneParameterCommand();
    // Set flag to record this change in history when the response comes back
    m_recordHistory = true;
    QString cmd = QString("increment %1 %2 %3\r\n").arg(subsys, param, QString::number(step, 'f', 6));
    sendCommand(cmd);
}

void MainWindow::on_pushButtonDecrement_clicked() {
    double step = -m_stepValues[ui->incrementSlider->value()];
    QString subsys = currentTuneSubsystemCommand();
    QString param = currentTuneParameterCommand();
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

    const QString previousParameterCommand = currentTuneParameterCommand();
    const QString subsys = currentTuneSubsystemCommand();

    ui->comboBoxTuneParameter->clear();
    for (const TuneSubsystemDef &definition : m_dataParser->getTuningDefinitions()) {
        if (definition.command != subsys) {
            continue;
        }
        for (const TuneParameterDef &parameter : definition.parameters) {
            ui->comboBoxTuneParameter->addItem(parameter.name, parameter.command);
        }
        break;
    }

    const int previousParameterIndex = ui->comboBoxTuneParameter->findData(previousParameterCommand);
    if (previousParameterIndex >= 0) {
        ui->comboBoxTuneParameter->setCurrentIndex(previousParameterIndex);
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

void MainWindow::on_pushButtonGaugeToggle_clicked()
{
    const bool restartGaugeTest = m_gaugeTestTimer && m_gaugeTestTimer->isActive();
    if (restartGaugeTest) {
        stopGaugeTest(false);
    }

    m_gaugeCircularMode = !m_gaugeCircularMode;
    setupGaugeArea();
    if (restartGaugeTest) {
        ui->pushButtonGaugeTest->setChecked(true);
        startGaugeTest();
    }
    updateGaugeModeControls();
}

void MainWindow::on_pushButtonGaugeTest_clicked()
{
    if (ui->pushButtonGaugeTest->isChecked()) {
        startGaugeTest();
    } else {
        stopGaugeTest();
    }
}

void MainWindow::on_pushButtonPause_clicked() {
    if (!m_plotPaused) {
        m_faultAutoCapturePending = false;
        m_faultAutoCaptureSkipCurrentPacket = false;
        m_faultAutoCapturePacketsRemaining = 0;
        m_faultCaptureDrainArmed.store(false, std::memory_order_release);
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

void MainWindow::on_pushButtonSaveAdc_clicked() {
    if (m_isAdcLogging) {
        stopAdcLogging();
        updateAdcSaveButtonState();
        return;
    }

    if (!startAdcLogging()) {
        ui->pushButtonSaveAdc->setChecked(false);
    }
    updateAdcSaveButtonState();
}

void MainWindow::on_pushButtonQuickSave_clicked() {
    if (m_isQuickSaving) {
        stopQuickSave();
        return;
    }

    if (!startQuickSave()) {
        ui->pushButtonQuickSave->setChecked(false);
    }
}

void MainWindow::on_pushButtonLogAdc_clicked() {
    updateAdcSaveButtonState();
    sendCommand(m_adcPacketActive ? "log rm adc\r\n" : "log add adc\r\n");
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
    const bool restartAdcLogging = m_isAdcLogging;
    if (restartLogging) {
        stopTelemetryLogging();
    }
    if (restartAdcLogging) {
        stopAdcLogging();
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
    clearPendingTelemetryPackets();
    m_latestTelemetryChanged = false;
    m_faultCaptureDrainArmed.store(false, std::memory_order_release);
    m_hasLastTimestamp = false;
    updateAllPlots();
    if (restartLogging && !startTelemetryLogging()) {
        ui->pushButtonSave->setChecked(false);
    }
    if (restartAdcLogging && !startAdcLogging()) {
        ui->pushButtonSaveAdc->setChecked(false);
    }
    updateAdcSaveButtonState();
    statusBar()->showMessage("Loaded telemetry configuration: " + filePath, 5000);
}

bool MainWindow::startTelemetryLogging() {
    // Telemetry CSV logging runs in the parser worker so file I/O follows the
    // packet stream without blocking the UI refresh loop.
    if (m_isLogging) {
        return true;
    }

    const QString fileName = QDateTime::currentDateTime().toString("yyyy-MM-dd-hh.mm.ss'_log_data.csv'");
    const QString filePath = QDir::current().filePath(fileName);
    bool started = false;
    QString errorMessage;
    QMetaObject::invokeMethod(m_dataParser, [this, filePath, &started, &errorMessage]() {
        started = m_dataParser->startTelemetryCsvLogging(filePath, &errorMessage);
    }, Qt::BlockingQueuedConnection);
    if (!started) {
        QMessageBox::critical(this, "Error", "Failed to create telemetry log file: " + errorMessage);
        return false;
    }

    m_isLogging = true;
    ui->pushButtonSave->setChecked(true);
    ui->pushButtonSave->setToolTip("Saving telemetry to " + filePath);
    return true;
}

void MainWindow::stopTelemetryLogging() {
    if (!m_isLogging) {
        return;
    }

    QMetaObject::invokeMethod(m_dataParser, [this]() {
        m_dataParser->stopTelemetryCsvLogging();
    }, Qt::BlockingQueuedConnection);
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

    const QString timestamp = values.contains(DataParser::TIMESTAMP_US_FIELD)
                                  ? QString::number(static_cast<quint64>(values.value(DataParser::TIMESTAMP_US_FIELD, 0.0)))
                                  : QString::number(static_cast<quint64>(values.value(DataParser::TIMESTAMP_FIELD, 0.0)));

    m_logStream << timestamp
                << "," << m_telemetryStatus.errorCode
                << "," << m_telemetryStatus.errorNames.join("|")
                << "," << static_cast<int>(m_telemetryStatus.controlMode)
                << "," << m_telemetryStatus.controlModeName;
    for (const QString &field : m_logFields) {
        m_logStream << "," << QString::number(values.value(field, 0.0), 'g', 17);
    }
    m_logStream << "\n";
}

bool MainWindow::startAdcLogging() {
    if (m_isAdcLogging) {
        return true;
    }

    const QString fileName = QDateTime::currentDateTime().toString("yyyy-MM-dd-hh.mm.ss'_adc_sample.csv'");
    const QString filePath = QDir::current().filePath(fileName);
    bool started = false;
    QString errorMessage;
    QMetaObject::invokeMethod(m_dataParser, [this, filePath, &started, &errorMessage]() {
        started = m_dataParser->startAdcCsvLogging(filePath, &errorMessage);
    }, Qt::BlockingQueuedConnection);
    if (!started) {
        QMessageBox::critical(this, "Error", "Failed to create ADC sample log file: " + errorMessage);
        return false;
    }

    m_pendingAdcSequences.clear();
    m_isAdcLogging = true;
    ui->pushButtonSaveAdc->setChecked(true);
    ui->pushButtonSaveAdc->setToolTip("Saving ADC samples to " + filePath);
    return true;
}

void MainWindow::stopAdcLogging() {
    if (!m_isAdcLogging) {
        return;
    }

    QMetaObject::invokeMethod(m_dataParser, [this]() {
        m_dataParser->stopAdcCsvLogging();
    }, Qt::BlockingQueuedConnection);
    m_isAdcLogging = false;
    m_pendingAdcSequences.clear();
    if (ui && ui->pushButtonSaveAdc) {
        ui->pushButtonSaveAdc->setChecked(false);
        ui->pushButtonSaveAdc->setToolTip("Start or stop saving ADC samples to CSV");
    }
}

void MainWindow::writeAdcLogRows(const AdcSamplePacket &packet) {
    if (!m_isAdcLogging || !m_adcLogFile.isOpen() || packet.adcId < 1 || packet.adcId > 3) {
        return;
    }

    const QString timestamp = packet.hasTimestampUs
                                  ? QString::number(packet.timestampUs)
                                  : QString::number(packet.timestampTicks);
    const double resolution = std::pow(2.0, static_cast<int>(packet.resolutionBit)) - 1.0;
    if (resolution <= 0.0 || qFuzzyIsNull(static_cast<double>(packet.shunt))) {
        return;
    }

    PendingAdcSequence &pending = m_pendingAdcSequences[packet.sequence];
    if (pending.time.isEmpty()) {
        pending.time = timestamp;
    }
    pending.lastUpdateMs = QDateTime::currentMSecsSinceEpoch();
    pending.receivedMask |= 1 << (packet.adcId - 1);
    if (pending.rows.size() < packet.samples.size()) {
        pending.rows.resize(packet.samples.size());
    }

    const int adcIndex = static_cast<int>(packet.adcId) - 1;
    for (int i = 0; i < packet.samples.size(); ++i) {
        const quint16 raw = packet.samples.at(i);
        const double voltage = ((static_cast<double>(raw) / resolution) * 3.3 - (1.65 + packet.offset)) / 50.0;
        const double current = voltage / packet.shunt;
        pending.rows[i].hasAdc[adcIndex] = true;
        pending.rows[i].raw[adcIndex] = raw;
        pending.rows[i].current[adcIndex] = current;
    }

    if ((pending.receivedMask & 0b111) == 0b111) {
        flushAdcSequence(packet.sequence);
    }
    flushStaleAdcSequences();
}

void MainWindow::flushAdcSequence(quint32 sequence) {
    if (!m_adcLogFile.isOpen() || !m_pendingAdcSequences.contains(sequence)) {
        return;
    }

    const PendingAdcSequence pending = m_pendingAdcSequences.take(sequence);
    for (const PendingAdcSampleRow &row : pending.rows) {
        QStringList columns;
        columns << pending.time << "" << "" << "" << "" << "" << "";
        for (int adc = 0; adc < 3; ++adc) {
            if (!row.hasAdc[adc]) {
                continue;
            }
            columns[1 + adc] = QString::number(row.raw[adc]);
            columns[4 + adc] = QString::number(row.current[adc], 'g', 17);
        }
        m_adcLogStream << columns.join(',') << "\n";
    }
}

void MainWindow::flushStaleAdcSequences() {
    if (m_pendingAdcSequences.isEmpty()) {
        return;
    }

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    QList<quint32> toFlush;
    quint32 newestSequence = 0;
    bool hasNewest = false;
    for (auto it = m_pendingAdcSequences.cbegin(); it != m_pendingAdcSequences.cend(); ++it) {
        if (!hasNewest || it.key() > newestSequence) {
            newestSequence = it.key();
            hasNewest = true;
        }
        if (nowMs - it.value().lastUpdateMs > 1000) {
            toFlush.append(it.key());
        }
    }

    if (m_pendingAdcSequences.size() > 8 && hasNewest) {
        for (auto it = m_pendingAdcSequences.cbegin(); it != m_pendingAdcSequences.cend(); ++it) {
            if (it.key() + 2 < newestSequence) {
                toFlush.append(it.key());
            }
        }
    }

    std::sort(toFlush.begin(), toFlush.end());
    toFlush.erase(std::unique(toFlush.begin(), toFlush.end()), toFlush.end());
    for (quint32 sequence : toFlush) {
        flushAdcSequence(sequence);
    }
}

bool MainWindow::startQuickSave() {
    // Quick save starts telemetry and/or ADC logging and then relies on a
    // timeout to stop collection automatically after a short capture window.
    if (m_isQuickSaving) {
        return true;
    }

    const bool saveTelemetry = ui->checkBoxTelemetry && ui->checkBoxTelemetry->isChecked();
    const bool saveAdc = ui->checkBoxAdc && ui->checkBoxAdc->isChecked();
    if (!saveTelemetry && !saveAdc) {
        QMessageBox::warning(this, "Quick Save", "Select Telemetry, ADC, or both before starting quick save.");
        return false;
    }

    bool ok = false;
    const double seconds = ui->lineEditSaveTime->text().trimmed().toDouble(&ok);
    if (!ok || seconds <= 0.0 ||
        seconds > static_cast<double>(std::numeric_limits<int>::max()) / 1000.0) {
        QMessageBox::warning(this, "Quick Save", "Enter a positive quick save duration in seconds.");
        return false;
    }

    const int durationMs = static_cast<int>(std::llround(seconds * 1000.0));
    const QString timestamp = QDateTime::currentDateTime().toString("yyyy-MM-dd-hh.mm.ss");
    const QString telemetryPath = saveTelemetry ? quickSaveFilePath(timestamp, "_log_data") : QString();
    const QString adcPath = saveAdc ? quickSaveFilePath(timestamp, "_adc_sample") : QString();

    bool started = false;
    QString errorMessage;
    QMetaObject::invokeMethod(m_dataParser, [this, telemetryPath, adcPath, &started, &errorMessage]() {
        started = m_dataParser->startQuickCsvLogging(telemetryPath, adcPath, &errorMessage);
    }, Qt::BlockingQueuedConnection);
    if (!started) {
        QMessageBox::critical(this, "Error", "Failed to create quick save file: " + errorMessage);
        return false;
    }

    m_isQuickSaving = true;
    ui->pushButtonQuickSave->setChecked(true);
    QStringList files;
    if (!telemetryPath.isEmpty()) {
        files << QFileInfo(telemetryPath).fileName();
    }
    if (!adcPath.isEmpty()) {
        files << QFileInfo(adcPath).fileName();
    }
    ui->pushButtonQuickSave->setToolTip("Quick saving to " + files.join(", "));
    if (m_quickSaveTimer) {
        m_quickSaveTimer->start(durationMs);
    }
    return true;
}

void MainWindow::stopQuickSave() {
    const bool wasQuickSaving = m_isQuickSaving;
    if (m_quickSaveTimer) {
        m_quickSaveTimer->stop();
    }

    if (wasQuickSaving && m_dataParser) {
        QMetaObject::invokeMethod(m_dataParser, [this]() {
            m_dataParser->stopQuickCsvLogging();
        }, Qt::BlockingQueuedConnection);
    }
    m_isQuickSaving = false;

    if (ui && ui->pushButtonQuickSave) {
        ui->pushButtonQuickSave->setChecked(false);
        ui->pushButtonQuickSave->setToolTip("Start quick logging for the selected data");
    }
}

QString MainWindow::quickSaveFilePath(const QString &timestamp, const QString &fileStem) const {
    const QString fileName = timestamp + fileStem + ui->lineEditSaveSuffix->text() + ".csv";
    return uniqueFilePath(QDir::current().filePath(fileName));
}

void MainWindow::updateAdcSaveButtonState() {
    if (!ui || !ui->pushButtonSaveAdc) {
        return;
    }
    if (m_isAdcLogging) {
        QMetaObject::invokeMethod(m_dataParser, [this]() {
            m_dataParser->flushStaleAdcCsvSequences();
        }, Qt::QueuedConnection);
        flushStaleAdcSequences();
        if (m_adcActivityTimer && !m_adcActivityTimer->isActive()) {
            m_adcActivityTimer->start(600);
        }
    }

    m_adcPacketActive = m_lastAdcPacketMs > 0 &&
                        QDateTime::currentMSecsSinceEpoch() - m_lastAdcPacketMs <= 100;
    const bool adcRecentProperty = !m_isAdcLogging && m_adcPacketActive;
    if (ui->pushButtonSaveAdc->property("adcRecent").toBool() != adcRecentProperty) {
        ui->pushButtonSaveAdc->setProperty("adcRecent", adcRecentProperty);
        refreshStyle(ui->pushButtonSaveAdc);
    }
}

void MainWindow::handleAdcStatusText(const QString &text)
{
    const QString normalized = text.trimmed().toLower();
    if (normalized == "adc print enabled") {
        m_adcPacketActive = true;
        m_lastAdcPacketMs = QDateTime::currentMSecsSinceEpoch();
        if (m_adcActivityTimer) {
            m_adcActivityTimer->start(100);
        }
        updateAdcSaveButtonState();
        return;
    }

    if (normalized == "adc print disabled") {
        updateAdcSaveButtonState();
    }
}

void MainWindow::sendCurrentLineEditCommand() {
    QString sendStr = ui->lineEditSend->text();
    if (sendStr.isEmpty())
        return;

    // Store command history, avoiding duplicates of the immediately previous entry.
    if (m_sendHistory.isEmpty() || m_sendHistory.last() != sendStr) {
        m_sendHistory.append(sendStr);
        if (m_sendHistory.size() > 64)
            m_sendHistory.removeFirst();
    }
    m_historyIndex = m_sendHistory.size(); // Point just after the end.

    // Send data, automatically adding a newline.
    QString cmd = sendStr;
    if (!cmd.endsWith("\r\n"))
        cmd += "\r\n";
    QByteArray data = cmd.toUtf8();
    QMetaObject::invokeMethod(m_serialManager, "sendData",
                              Qt::QueuedConnection,
                              Q_ARG(QByteArray, data));

    // Display local echo.
    ui->plainTextEditReceive->appendPlainText(">> " + sendStr.trimmed());

    // Clear the input box.
    ui->lineEditSend->clear();
}

void MainWindow::onFieldCheckStateChanged(QListWidgetItem *item) {
    if (m_syncingFromMask) return;
    if (!item) return;
    QString fieldName = item->text();
    bool checked = (item->checkState() == Qt::Checked);
    
    // Build the command string.
    QString cmdName = m_dataParser->getCommandNameForField(fieldName);
    if (cmdName.isEmpty()) {
        return;
    }
    QString cmd = checked ? QString("log add %1\r\n").arg(cmdName)
                          : QString("log rm %1\r\n").arg(cmdName);
    sendCommand(cmd);   // Reuse the existing sendCommand path.
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
        ui->targetSlider->setRange(0, 150);   // 0 -> 0.0, 150 -> 0.150
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
        val = qBound(0.0, val, 0.15);
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
    // Firmware replies are plain text. Set/enquire responses update the current
    // parameter display, remember last known values, and maintain undo history.
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
    return currentTuneSubsystemCommand() + ":" + currentTuneParameterCommand();
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
