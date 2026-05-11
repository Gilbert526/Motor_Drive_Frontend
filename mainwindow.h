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
#include "OscilloscopeWidget.h"
#include <QStack>
#include <QMap>

class AudioLevelMeter;
class SerialManager;
class DataParser;
struct IndicatorDef;
struct IndicatorStatusDef;
struct GaugeDef;

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
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
    void on_pushButtonAudible_clicked();
    void on_pushButtonReset_clicked();
    void on_pushButtonResetConnection_clicked();
    // Logging
    void on_pushButtonPreset1_clicked();
    void on_pushButtonPreset2_clicked();
    void on_pushButtonPreset3_clicked();
    void on_pushButtonPreset4_clicked();
    void on_pushButtonRemoveAll_clicked();
    void on_pushButtonBin_clicked();
    void on_pushButtonUtf8_clicked();
    void on_pushButtonSimStart_clicked();
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
    void on_pushButtonSelectConfig_clicked();

    void onFieldCheckStateChanged(QListWidgetItem *item);

    // 串口状态处理
    void handleSerialPortOpened(bool success, const QString &errorMsg);
    void handleSerialPortClosed();

    // 数据解析
    void handleNewData(const QHash<QString, double> &values);
    void handlePacketStatus(quint32 errorCode,
                            const QStringList &errorNames,
                            quint8 controlMode,
                            const QString &controlModeName,
                            bool controlModeKnown);

    // Update Field List on Mask Received
    void onMaskReceived(quint32 mask1, quint32 mask2);

    // 定时刷新波形
    void updatePlot();

    // 采样点数滑动条
    void on_sampleSlider_valueChanged(int value);

    // 字段列表双击：创建新示波器并添加该字段
    void on_fieldList_itemDoubleClicked(QListWidgetItem *item);

    // 配置某个示波器（弹出对话框选择字段）
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

    // Command line history
    QStringList m_sendHistory;
    int m_historyIndex;

    // 波形数据存储（所有字段的历史数据）
    QHash<QString, QVector<double>> m_waveData;
    int m_maxWavePoints;   // 内部存储最大点数（与滑动条值独立，用于限制存储）

    // 多示波器相关
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

    int m_currentMaxPoints;   // 当前显示的点数（滑动条值）

    // 定时器
    QTimer *m_plotTimer;
    QTimer *m_gaugeTimer;
    bool m_plotUpdatesSuspended;

    // Timer
    QVector<double> m_timeStamps;        // 每个采样点的时间（秒，相对）
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
        QStack<double> undoStack;  // 历史值栈（旧值）
        double currentValue;       // 当前值（用于显示）
    };
    TuneParamHistory m_currentParamHistory;   // 当前参数的历史（切换参数时清空）
    QMap<QString, double> m_paramLastValue;   // 所有参数的最后已知值（用于切换后显示）
    bool m_recordHistory;                     // 当前响应是否应记录历史（true: 记录, false: 不记录）

    // For incremental adjustments
    QVector<double> m_stepValues;

    // 辅助函数
    void sendCurrentLineEditCommand();

    void refreshSerialPorts();
    void updateUiForSerialState(bool isOpen);
    void sendCommand(const QString &cmd);
    void setupPlottingArea();           // 初始化动态示波器区域
    void setupGaugeArea();
    void addGauge(const GaugeDef &gauge);
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
    void loadAvailableFields();         // 从 DataParser 加载字段列表到左侧
    void updateAllPlots();              // 刷新所有示波器
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

    void addTimeStamp(double offsetSec); // 添加时间戳

    bool startTelemetryLogging();
    void stopTelemetryLogging();
    void writeTelemetryLogRow(const QHash<QString, double> &values);

    // Target setting helpers
    void updateTargetSliderLimits();   // 根据 Speed/Torque 更新滑块范围和步进
    double getCurrentTargetValue() const;
    void setTargetValue(double val, bool markAsEdited = true);

    // Tuning parameter handling
    void parseTuneResponse(const QString &line);   // 解析下位机返回
    QString getCurrentParamKey() const;            // 获取当前参数的唯一键

    QString formatStepValue(double step) const;    // Format step increment into a string
};

#endif // MAINWINDOW_H
