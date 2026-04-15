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

private slots:
    // 串口控制
    void on_pushButtonStartToggle_clicked();
    void on_pushButtonRefresh_clicked();
    void on_pushButtonSend_clicked();
    void on_pushButtonStart_clicked();
    void on_pushButtonStop_clicked();
    void on_pushButtonAlign_clicked();
    void on_pushButtonAudible_clicked();
    void on_pushButtonReset_clicked();
    void on_pushButtonPreset1_clicked();
    void on_pushButtonPreset2_clicked();
    void on_pushButtonPreset3_clicked();
    void on_pushButtonPreset4_clicked();

    // 串口状态处理
    void handleSerialPortOpened(bool success, const QString &errorMsg);
    void handleSerialPortClosed();

    // 数据解析
    void handleNewData(const QHash<QString, double> &values);

    // 定时刷新波形
    void updatePlot();

    // 采样点数滑动条
    void on_sampleSlider_valueChanged(int value);

    // 字段列表双击：创建新示波器并添加该字段
    void on_fieldList_itemDoubleClicked(QListWidgetItem *item);

    // 配置某个示波器（弹出对话框选择字段）
    void on_oscilloscopeConfigRequested(OscilloscopeWidget *osc);

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

    // 辅助函数
    void refreshSerialPorts();
    void updateUiForSerialState(bool isOpen);
    void sendCommand(const QString &cmd);
    void setupPlottingArea();           // 初始化动态示波器区域
    void addOscilloscope(const QString &title = QString(), int index = -1);
    void removeOscilloscope(OscilloscopeWidget *osc);
    void loadAvailableFields();         // 从 DataParser 加载字段列表到左侧
    void updateAllPlots();              // 刷新所有示波器
};

#endif // MAINWINDOW_H