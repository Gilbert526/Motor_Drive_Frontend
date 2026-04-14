#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QThread>
#include <QHash>
#include <QVector>
#include <QTimer>
#include "qcustomplot.h"

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

    void handleSerialPortOpened(bool success, const QString &errorMsg);
    void handleSerialPortClosed();
    void handleNewData(const QHash<QString, double> &values);
    void updatePlot();   // 定时器刷新波形

private:
    Ui::MainWindow  *ui;
    SerialManager   *m_serialManager;
    DataParser      *m_dataParser;
    QThread         *m_serialThread;

    QHash<QString, QVector<double>> m_waveData;
    int m_maxWavePoints;          // 最大显示点数，例如 2000

    QCustomPlot *m_customPlot;
    QTimer       *m_plotTimer;
    QHash<QString, QCPGraph*> m_graphs;  // 字段名 -> 曲线对象
    QStringList  m_plotFields;           // 当前要显示的字段列表

    void setupPlot();
    void addGraphForField(const QString &fieldName, const QColor &color);
    void refreshPlotFields();    // 根据界面选择更新显示的曲线
    
    void refreshSerialPorts();
    void updateUiForSerialState(bool isOpen);
    void sendCommand(const QString &cmd);
};

#endif // MAINWINDOW_H