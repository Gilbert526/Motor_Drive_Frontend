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
#include "OscilloscopeWidget.h"
#include <QStack>
#include <QMap>

#define DEFAULT_PACKET_FREQ_HZ 1000

class SerialManager;
class DataParser;

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

    const QVector<double>& getTimeStamps() const { return m_timeStamps; }

private slots:
    // Serial
    void on_pushButtonStartToggle_clicked();
    void on_pushButtonRefresh_clicked();
    // Basic Commands
    void on_pushButtonSend_clicked();
    void on_pushButtonStart_clicked();
    void on_pushButtonStop_clicked();
    void on_pushButtonAlign_clicked();
    void on_pushButtonAudible_clicked();
    void on_pushButtonReset_clicked();
    void on_pushButtonFocManual_clicked();
    // Logging
    void on_pushButtonPreset1_clicked();
    void on_pushButtonPreset2_clicked();
    void on_pushButtonPreset3_clicked();
    void on_pushButtonPreset4_clicked();
    void on_pushButtonRemoveAll_clicked();
    void on_pushButtonBin_clicked();
    void on_pushButtonUtf8_clicked();
    // Tuning
    void on_pushButtonTuneSend_clicked();
    void on_pushButtonTuneUndo_clicked();
    void on_pushButtonIncrement_clicked();
    void on_pushButtonDecrement_clicked();
    void on_incrementSlider_valueChanged(int value);
    void on_comboBoxTuneSubsystem_currentIndexChanged(int index);
    void on_comboBoxTuneParameter_currentIndexChanged(int index);
    // Scope control
    void on_pushButtonPause_clicked();

    void onFieldCheckStateChanged(QListWidgetItem *item);

    // 串口状态处理
    void handleSerialPortOpened(bool success, const QString &errorMsg);
    void handleSerialPortClosed();

    // 数据解析
    void handleNewData(const QHash<QString, double> &values);

    // Update Field List on Mask Received
    void onMaskReceived(quint32 mask);

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

private:
    Ui::MainWindow *ui;
    SerialManager   *m_serialManager;
    DataParser      *m_dataParser;
    QThread         *m_serialThread;

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

    int m_currentMaxPoints;   // 当前显示的点数（滑动条值）

    // 定时器
    QTimer *m_plotTimer;

    // Timer
    QVector<double> m_timeStamps;        // 每个采样点的时间（秒，相对）
    qint64 m_packetCounter;          // 累计接收的包数（从0开始）
    double m_packetIntervalSec;      // 包间隔秒数 = 1.0 / FREQ

    // Pause scope
    bool m_plotPaused;

    bool m_syncingFromMask;

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
    void refreshSerialPorts();
    void updateUiForSerialState(bool isOpen);
    void sendCommand(const QString &cmd);
    void setupPlottingArea();           // 初始化动态示波器区域
    void addOscilloscope(const QString &title = QString(), int index = -1);
    void removeOscilloscope(OscilloscopeWidget *osc);
    void updateAllMoveButtons();        // Update state of move up/down buttons for oscilloscopes
    void loadAvailableFields();         // 从 DataParser 加载字段列表到左侧
    void updateAllPlots();              // 刷新所有示波器
    void syncFieldCheckStates();

    void addTimeStamp(double offsetSec); // 添加时间戳

    // Tuning parameter handling
    void parseTuneResponse(const QString &line);   // 解析下位机返回
    QString getCurrentParamKey() const;            // 获取当前参数的唯一键

    QString formatStepValue(double step) const;    // Format step increment into a string
};

#endif // MAINWINDOW_H