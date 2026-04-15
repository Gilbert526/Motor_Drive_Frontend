#ifndef OSCILLOSCOPEWIDGET_H
#define OSCILLOSCOPEWIDGET_H

#include <QWidget>
#include "qcustomplot.h"
#include <QHash>
#include <QVector>
#include <QDragEnterEvent>
#include <QDropEvent>

class OscilloscopeWidget : public QWidget {
    Q_OBJECT

public:
    explicit OscilloscopeWidget(QWidget *parent = nullptr);

    void setFields(const QStringList &fields);
    QStringList getFields() const { return m_fields; }
    void updatePlot(const QHash<QString, QVector<double>> &dataPool, int maxPoints);
    void clear();
    void setTitle(const QString &title);

signals:
    void fieldsChanged();   // 请求配置（点击齿轮时发出）
    void removeRequested();
    void addBelowRequested();

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private slots:
    void onConfigure();

private:
    QCustomPlot *m_plot;
    QStringList m_fields;
    QHash<QString, QCPGraph*> m_graphs;
    QList<QColor> m_colors;
    QLabel *m_titleLabel;
    QPushButton *m_configBtn;
    
    void setupUi();
};

#endif // OSCILLOSCOPEWIDGET_H