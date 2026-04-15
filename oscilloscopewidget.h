#ifndef OSCILLOSCOPEWIDGET_H
#define OSCILLOSCOPEWIDGET_H

#include <QWidget>
#include "qcustomplot.h"
#include <QHash>
#include <QVector>

class OscilloscopeWidget : public QWidget {
    Q_OBJECT

public:
    explicit OscilloscopeWidget(QWidget *parent = nullptr);

    void setFields(const QStringList &fields);
    QStringList getFields() const { return m_fields; }
    void updatePlot(const QHash<QString, QVector<double>> &dataPool,
                    const QVector<double> &timeStamps, int maxPoints);
    void clear();
    void setTitle(const QString &title);

signals:
    void fieldsChanged();   // 请求配置（点击齿轮时发出）
    void removeRequested();
    void addBelowRequested();

private slots:
    void onConfigure();
    void onToggleYLock();

private:
    QCustomPlot *m_plot;
    QStringList m_fields;
    QHash<QString, QCPGraph*> m_graphs;
    QList<QColor> m_colors;
    QLabel *m_titleLabel;
    QPushButton *m_configBtn;

    bool m_yLocked;           // Y轴是否锁定
    QPushButton *m_yLockBtn;  // 按钮指针
    void updateYAxis();       // 根据锁定状态决定是否自动缩放
    
    void setupUi();
};

#endif // OSCILLOSCOPEWIDGET_H