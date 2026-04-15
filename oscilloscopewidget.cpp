#include "OscilloscopeWidget.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QDragEnterEvent>
#include <QMimeData>

OscilloscopeWidget::OscilloscopeWidget(QWidget *parent):
    QWidget(parent),
    m_plot(new QCustomPlot(this)) {
        setupUi();
        m_colors = {Qt::red, Qt::green, Qt::blue, Qt::magenta, Qt::cyan, Qt::darkYellow, Qt::darkCyan};
}

void OscilloscopeWidget::setupUi() {
    // 标题栏
    m_titleLabel = new QLabel("Oscilloscope", this);
    m_titleLabel->setAlignment(Qt::AlignCenter);
    m_titleLabel->setStyleSheet("font-weight: bold; background-color: #f0f0f0;");
    
    // 配置按钮
    m_configBtn = new QPushButton("⚙️", this);
    m_configBtn->setFixedSize(20, 20);
    connect(m_configBtn, &QPushButton::clicked, this, &OscilloscopeWidget::onConfigure);
    
    QHBoxLayout *titleLayout = new QHBoxLayout;
    titleLayout->addWidget(m_titleLabel);
    titleLayout->addWidget(m_configBtn);
    
    // 绘图区域
    m_plot->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);
    m_plot->xAxis->setLabel("Sample");
    m_plot->yAxis->setLabel("Value");
    m_plot->legend->setVisible(true);
    m_plot->legend->setFont(QFont("Arial", 7));
    
    // 主布局
    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->addLayout(titleLayout);
    layout->addWidget(m_plot);
    
    setAcceptDrops(true);
}

void OscilloscopeWidget::setTitle(const QString &title) {
    m_titleLabel->setText(title);
}

void OscilloscopeWidget::setFields(const QStringList &fields) {
    m_fields = fields;
    m_plot->clearGraphs();
    m_graphs.clear();
    
    for (int i = 0; i < m_fields.size(); ++i) {
        QCPGraph *graph = m_plot->addGraph();
        graph->setName(m_fields[i]);
        graph->setPen(QPen(m_colors[i % m_colors.size()], 1.5));
        m_graphs[m_fields[i]] = graph;
    }
    m_plot->legend->setVisible(!m_fields.isEmpty());
    // 注意：不发射 fieldsChanged，避免循环
}

void OscilloscopeWidget::updatePlot(const QHash<QString, QVector<double>> &dataPool, int maxPoints) {
    if (m_fields.isEmpty()) return;
    
    // 找出所有曲线数据的最大长度
    int maxDataSize = 0;
    for (const QString &field : m_fields) {
        if (dataPool.contains(field)) {
            maxDataSize = qMax(maxDataSize, dataPool[field].size());
        }
    }
    if (maxDataSize == 0) return;
    
    // 更新每条曲线的数据
    for (const QString &field : m_fields) {
        QCPGraph *graph = m_graphs.value(field);
        if (!graph) continue;
        const QVector<double> &data = dataPool.value(field);
        if (data.isEmpty()) continue;
        
        QVector<double> x(data.size());
        for (int i = 0; i < data.size(); ++i) x[i] = i;
        graph->setData(x, data);
    }
    
    // 设置 X 轴范围（显示最近 maxPoints 点）
    int xStart = qMax(0, maxDataSize - maxPoints);
    m_plot->xAxis->setRange(xStart, maxDataSize);
    // 只自动缩放 Y 轴，保留 X 轴范围
    m_plot->rescaleAxes(false);
    m_plot->replot();
}

void OscilloscopeWidget::clear() {
    m_plot->clearGraphs();
    m_graphs.clear();
    m_fields.clear();
    m_plot->legend->setVisible(false);
    m_plot->replot();
}

void OscilloscopeWidget::dragEnterEvent(QDragEnterEvent *event) {
    if (event->mimeData()->hasText())
        event->acceptProposedAction();
}

void OscilloscopeWidget::dropEvent(QDropEvent *event) {
    QString fieldName = event->mimeData()->text();
    if (!fieldName.isEmpty() && !m_fields.contains(fieldName)) {
        QStringList newFields = m_fields;
        newFields.append(fieldName);
        setFields(newFields);
    }
    event->acceptProposedAction();
}

void OscilloscopeWidget::onConfigure() {
    // 通知主窗口弹出配置对话框
    emit fieldsChanged();
}