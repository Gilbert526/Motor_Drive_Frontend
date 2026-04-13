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
    void on_pushButtonStartToggle_clicked();
    void on_pushButtonRefresh_clicked();
    void on_pushButtonSend_clicked();
    void on_pushButtonStart_clicked();
    void on_pushButtonStop_clicked();
    void on_pushButtonAudible_clicked();
    void on_pushButtonReset_clicked();
    void readSerialData();
    void handleSerialError(QSerialPort::SerialPortError error);

private:
    Ui::MainWindow *ui;
    QSerialPort *serial;
    void refreshSerialPorts();
    void updateToggleButtonState(bool isOpen);
    void sendCommand(const QString &cmd);
};

#endif // MAINWINDOW_H