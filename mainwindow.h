#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QThread>
#include <QHash>
#include <QVector>
#include <QTimer>
#include <QListWidget>
#include <QScrollArea>
#include <QSplitter>
#include <QSlider>
#include <QLabel>
#include <QVBoxLayout>
#include <QFile>
#include <QTextStream>
#include <QPointer>
#include "OscilloscopeWidget.h"
#include <QStack>
#include <QMap>

class AudioLevelMeter;
class SerialManager;
class DataParser;
class simulationDialog;
struct IndicatorDef;
struct IndicatorStatusDef;
struct GaugeDef;
struct AdcSamplePacket;

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

/**
 * @brief Main application window and coordinator for serial telemetry UI.
 *
 * MainWindow owns the visible controls, the serial worker thread, parser, live
 * waveform buffers, dashboard gauges/indicators, CSV logging, tuning controls,
 * and simulation dialog integration. Protocol parsing and serial I/O are
 * delegated to DataParser and SerialManager respectively.
 */
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    /**
     * @brief Latest decoded telemetry status shared by indicators and logging.
     */
    struct TelemetryStatus {
        quint32 errorCode = 0;
        QStringList errorNames;
        quint8 controlMode = 0;
        QString controlModeName = "UNKNOWN_CONTROL_MODE";
        bool controlModeKnown = false;
    };

    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

    const QVector<double>& getTimeStamps() const { return m_timeStamps; }
    const TelemetryStatus& telemetryStatus() const { return m_telemetryStatus; }

    static QList<QColor> getPresetColors();
    static QStringList getColorNames();

private slots:
    // Serial
    void on_pushButtonStartToggle_clicked();
    void on_pushButtonRefresh_clicked();
    // Basic Commands
    void on_pushButtonSend_clicked();
    void on_lineEditSend_returnPressed();
    void on_pushButtonVvvf_clicked();
    void on_pushButtonSixstep_clicked();
    void on_pushButtonFoc_clicked();
    void on_pushButtonStop_clicked();
    void on_pushButtonAlign_clicked();
    void on_pushButtonResetAlign_clicked();
    void on_pushButtonAudible_clicked();
    void on_pushButtonReset_clicked();
    void on_pushButtonSyncSim_clicked();
    // Logging
    void on_pushButtonPreset1_clicked();
    void on_pushButtonPreset2_clicked();
    void on_pushButtonPreset3_clicked();
    void on_pushButtonPreset4_clicked();
    void on_pushButtonRemoveAll_clicked();
    void on_pushButtonBin_clicked();
    void on_pushButtonUtf8_clicked();
    void on_pushButtonLogAdc_clicked();
    // Setting Targets
    void on_comboBoxTargetSelection_currentIndexChanged(int index);
    void on_targetSlider_valueChanged(int value);
    void on_lineEditTarget_editingFinished();
    void on_timeSlider_valueChanged(int value);
    void on_lineEditTime_editingFinished();
    void on_pushButtonTargetSend_clicked();
    // Tuning
    void on_pushButtonTuneSend_clicked();
    void on_pushButtonTuneUndo_clicked();
    void on_pushButtonTuneEnquire_clicked();
    void on_pushButtonIncrement_clicked();
    void on_pushButtonDecrement_clicked();
    void on_incrementSlider_valueChanged(int value);
    void on_comboBoxTuneSubsystem_currentIndexChanged(int index);
    void on_comboBoxTuneParameter_currentIndexChanged(int index);
    // Scope control
    void on_pushButtonPause_clicked();
    void on_pushButtonSave_clicked();
    void on_pushButtonSaveAdc_clicked();
    void on_pushButtonQuickSave_clicked();
    void on_pushButtonSelectConfig_clicked();

    void onFieldCheckStateChanged(QListWidgetItem *item);

    // Serial port state handlers.
    void handleSerialPortOpened(bool success, const QString &errorMsg);
    void handleSerialPortClosed();

    // Parsed telemetry and packet-status handlers.
    void handleNewData(const QHash<QString, double> &values);
    void handleAdcSample(const AdcSamplePacket &packet);
    void handlePacketStatus(quint32 errorCode,
                            const QStringList &errorNames,
                            quint8 controlMode,
                            const QString &controlModeName,
                            bool controlModeKnown);

    // Update Field List on Mask Received
    void onMaskReceived(quint32 mask1, quint32 mask2);

    // Timer-driven plot refresh.
    void updatePlot();

    // Visible sample count slider.
    void on_sampleSlider_valueChanged(int value);

    // Field-list double click: create a new scope containing that field.
    void on_fieldList_itemDoubleClicked(QListWidgetItem *item);

    // Scope configuration dialog for choosing fields and colors.
    void on_oscilloscopeConfigRequested(OscilloscopeWidget *osc);

    // Scope move up/down
    void onMoveUpRequested();
    void onMoveDownRequested();

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    Ui::MainWindow *ui;
    SerialManager   *m_serialManager;
    DataParser      *m_dataParser;
    QThread         *m_serialThread;
    QPointer<simulationDialog> m_simulationDialog;

    // Command line history
    QStringList m_sendHistory;
    int m_historyIndex;

    // Waveform data storage for all telemetry fields.
    QHash<QString, QVector<double>> m_waveData;
    int m_maxWavePoints;   // Internal storage limit, independent of the display slider.

    // Multi-oscilloscope UI and model state.
    QListWidget *m_fieldList;
    QScrollArea *m_scrollArea;
    QWidget     *m_oscContainer;
    QVBoxLayout *m_oscLayout;
    QSlider     *m_sampleSlider;
    QLabel      *m_sampleLabel;
    QList<OscilloscopeWidget*> m_oscilloscopes;

    struct GaugeBinding {
        QString fieldName;
        AudioLevelMeter *meter = nullptr;
        double pendingValue = 0.0;
        bool hasPendingValue = false;
    };
    QList<GaugeBinding> m_gaugeBindings;
    QHash<QString, double> m_latestTelemetryValues;

    int m_currentMaxPoints;   // Current visible point count from the slider.

    // Refresh/activity timers.
    QTimer *m_plotTimer;
    QTimer *m_gaugeTimer;
    bool m_plotUpdatesSuspended;

    // Timer
    QVector<double> m_timeStamps;        // Sample timestamps in relative seconds.
    quint64 m_lastTimestampTicks;
    bool m_hasLastTimestamp;

    // Pause scope
    bool m_plotPaused;
    bool m_plotDirty;
    QVector<double> m_pausedTimeStamps;
    QHash<QString, QVector<double>> m_pausedWaveData;

    // Fault-triggered auto capture tuning.
    // Adjust these values to change which faults trigger a capture,
    // how wide the display window becomes, and how many packets to keep after the trigger.
    quint32 m_faultAutoCaptureTriggerMask;
    int m_faultAutoCaptureDisplayPoints;
    int m_faultAutoCapturePacketsAfterTrigger;
    bool m_faultAutoCapturePending;
    bool m_faultAutoCaptureSkipCurrentPacket;
    int m_faultAutoCapturePacketsRemaining;

    // CSV telemetry logging
    bool m_isLogging;
    QFile m_logFile;
    QTextStream m_logStream;
    QStringList m_logFields;
    TelemetryStatus m_telemetryStatus;

    // CSV ADC sample logging
    bool m_isAdcLogging;
    bool m_isQuickSaving;
    bool m_adcPacketActive;
    QFile m_adcLogFile;
    QTextStream m_adcLogStream;
    qint64 m_lastAdcPacketMs;
    QTimer *m_adcActivityTimer;
    QTimer *m_quickSaveTimer;
    struct PendingAdcSampleRow {
        bool hasAdc[3] = {false, false, false};
        quint16 raw[3] = {0, 0, 0};
        double current[3] = {0.0, 0.0, 0.0};
    };
    struct PendingAdcSequence {
        QString time;
        QVector<PendingAdcSampleRow> rows;
        int receivedMask = 0;
        qint64 lastUpdateMs = 0;
    };
    QHash<quint32, PendingAdcSequence> m_pendingAdcSequences;

    bool m_syncingFromMask;

    // Target type selection
    QString m_currentTargetType;

    // Target setting last values
    double m_lastSpeedValue;
    double m_lastTorqueValue;

    // Manual Target Setting
    bool m_targetManuallyEdited;
    bool m_timeManuallyEdited;

    // Changing target type flag
    bool m_updatingTargetType;

    // Tuning parameter history for undo functionality
    struct TuneParamHistory {
        QStack<double> undoStack;  // Previous values available for undo.
        double currentValue;       // Current displayed value.
    };
    TuneParamHistory m_currentParamHistory;   // History for the selected parameter; reset when selection changes.
    QMap<QString, double> m_paramLastValue;   // Last known value for each parameter, used when switching selections.
    bool m_recordHistory;                     // Whether the next firmware response should be pushed to undo history.

    // For incremental adjustments
    QVector<double> m_stepValues;

    // Helper functions.
    void sendCurrentLineEditCommand();

    void refreshSerialPorts();
    void updateUiForSerialState(bool isOpen);
    void sendCommand(const QString &cmd);
    void setupPlottingArea();           // Initialize the dynamic oscilloscope area.
    void setupGaugeArea();
    void addGauge(const GaugeDef &gauge);
    void setupTuningArea();
    QString currentTuneSubsystemCommand() const;
    QString currentTuneParameterCommand() const;
    void deriveGaugeThresholds(const GaugeDef &gauge,
                               double *warningThreshold,
                               double *criticalThreshold) const;
    void updateGauges(const QHash<QString, double> &values);
    void flushGaugeUpdates();
    void updateStatusIndicators();
    void updateFaultAutoCaptureMask();
    void applyIndicatorStatus(QLabel *label, const QString &text, const QString &colorName);
    const IndicatorStatusDef* resolveIndicatorStatus(const IndicatorDef &indicator) const;
    const IndicatorStatusDef* resolveModeIndicatorStatus(const IndicatorDef &indicator) const;
    const IndicatorStatusDef* resolveConditionIndicatorStatus(const IndicatorDef &indicator) const;
    const IndicatorStatusDef* resolveBitwiseIndicatorStatus(const IndicatorDef &indicator) const;
    const IndicatorStatusDef* defaultIndicatorStatus(const IndicatorDef &indicator) const;
    bool resolveIndicatorDataSourceValue(const QString &dataSource, double *value) const;
    void addOscilloscope(const QString &title = QString(), int index = -1);
    void removeOscilloscope(OscilloscopeWidget *osc);
    void updateAllMoveButtons();        // Update state of move up/down buttons for oscilloscopes
    void loadAvailableFields();         // Load DataParser fields into the left-hand field list.
    void updateAllPlots();              // Refresh every oscilloscope.
    void syncFieldCheckStates();
    void capturePausedPlotSnapshot();
    void setPlotPaused(bool paused);
    void startFaultAutoCapture(quint32 triggeredMask);
    void advanceFaultAutoCapture();
    bool isReceiveTextByte(char byte) const;
    void processReceiveTextChunk(const QByteArray &chunk, QByteArray &lineBuffer);
    void flushReceiveTextLines(QByteArray &lineBuffer);
    bool isLikelyReceiveTextLine(const QByteArray &line) const;
    void updatePlotRefreshState();

    void addTimeStamp(double offsetSec); // Append a relative timestamp.

    bool startTelemetryLogging();
    void stopTelemetryLogging();
    void writeTelemetryLogRow(const QHash<QString, double> &values);
    bool startAdcLogging();
    void stopAdcLogging();
    void writeAdcLogRows(const AdcSamplePacket &packet);
    void flushAdcSequence(quint32 sequence);
    void flushStaleAdcSequences();
    bool startQuickSave();
    void stopQuickSave();
    QString quickSaveFilePath(const QString &timestamp, const QString &fileStem) const;
    void updateAdcSaveButtonState();
    void handleAdcStatusText(const QString &text);

    // Target setting helpers
    void updateTargetSliderLimits();   // Update slider range and step mapping for Speed/Torque.
    double getCurrentTargetValue() const;
    void setTargetValue(double val, bool markAsEdited = true);

    // Tuning parameter handling
    void parseTuneResponse(const QString &line);   // Parse a firmware tuning response line.
    QString getCurrentParamKey() const;            // Return the unique key for the selected parameter.

    QString formatStepValue(double step) const;    // Format step increment into a string
};

#endif // MAINWINDOW_H
