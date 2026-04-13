#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QSerialPort>
#include <QSerialPortInfo>
#include <QPlainTextEdit>

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
    void on_pushButtonStart_clicked();
    void on_pushButtonSend_clicked();
    void readSerialData();          // 接收串口数据

private:
    Ui::MainWindow *ui;
    QSerialPort *serial;            // 串口对象
    void refreshSerialPorts();      // 刷新串口列表
};

#endif // MAINWINDOW_H