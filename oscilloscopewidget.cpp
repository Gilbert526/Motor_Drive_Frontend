#include "OscilloscopeWidget.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QMimeData>

OscilloscopeWidget::OscilloscopeWidget(QWidget *parent):
    QWidget(parent),
    m_plot(new QCustomPlot(this)),
    m_yLocked(false) {
        setMinimumHeight(200);
        setupUi();
        m_colors = {Qt::red, Qt::green, Qt::blue, Qt::magenta, Qt::cyan, Qt::darkYellow, Qt::darkCyan};
}

void OscilloscopeWidget::setupUi() {
    // 标题栏
    m_titleLabel = new QLabel("Oscilloscope", this);
    m_titleLabel->setAlignment(Qt::AlignCenter);
    m_titleLabel->setStyleSheet("font-weight: bold; background-color: #f0f0f0;");
    
    // Config button
    m_configBtn = new QPushButton("⚙️", this);
    m_configBtn->setFixedSize(20, 20);
    m_configBtn->setToolTip("Configure fields to plot");
    connect(m_configBtn, &QPushButton::clicked, this, &OscilloscopeWidget::onConfigure);

    // Add Scope Below Button
    QPushButton *addBelowBtn = new QPushButton("+", this);
    addBelowBtn->setFixedSize(20, 20);
    addBelowBtn->setToolTip("Add scope below");
    connect(addBelowBtn, &QPushButton::clicked, this, &OscilloscopeWidget::addBelowRequested);

    // Remove Scope Button
    QPushButton *removeBtn = new QPushButton("-", this);
    removeBtn->setFixedSize(20, 20);
    removeBtn->setToolTip("Remove scope");
    connect(removeBtn, &QPushButton::clicked, this, &OscilloscopeWidget::removeRequested);

    // Y-axis Lock Button
    m_yLockBtn = new QPushButton("🔒", this);
    m_yLockBtn->setFixedSize(20, 20);
    m_yLockBtn->setCheckable(true);
    m_yLockBtn->setToolTip("Toggle disabling Y-axis auto-scaling");
    connect(m_yLockBtn, &QPushButton::clicked, this, &OscilloscopeWidget::onToggleYLock);
    
    QHBoxLayout *titleLayout = new QHBoxLayout;
    titleLayout->addWidget(m_titleLabel);
    titleLayout->addWidget(m_yLockBtn);
    titleLayout->addWidget(m_configBtn);
    titleLayout->addWidget(addBelowBtn);
    titleLayout->addWidget(removeBtn);
    
    // 绘图区域
    m_plot->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);
    m_plot->axisRect()->setRangeDrag(Qt::Vertical);
    m_plot->axisRect()->setRangeZoom(Qt::Vertical);
    m_plot->xAxis->setLabel("Time (s)");
    m_plot->xAxis->setNumberFormat("gbc"); // 自动选择格式
    m_plot->xAxis->setNumberFormat("f");
    m_plot->xAxis->setNumberPrecision(2);  // 3 位小数
    m_plot->yAxis->setLabel("Value");
    m_plot->legend->setVisible(true);
    m_plot->legend->setFont(QFont("Arial", 7));
    m_plot->setOpenGl(true);
    
    // 主布局
    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->addLayout(titleLayout);
    layout->addWidget(m_plot);
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

void OscilloscopeWidget::updatePlot(const QHash<QString, QVector<double>> &dataPool,
                                    const QVector<double> &timeStamps, int maxPoints) {
    if (m_fields.isEmpty()) return;
    if (timeStamps.isEmpty()) return;

    int totalPoints = timeStamps.size();
    int startIdx = qMax(0, totalPoints - maxPoints);
    int pointsToShow = totalPoints - startIdx;
    if (pointsToShow <= 0) return;

    QVector<double> x = timeStamps.mid(startIdx, pointsToShow);
    double xMin = x.first();
    double xMax = x.last();
    
    // Update each graph's data
    for (const QString &field : m_fields) {
        QCPGraph *graph = m_graphs.value(field);
        if (!graph) continue;
        const QVector<double> &data = dataPool.value(field);
        if (data.size() < totalPoints) continue;  // 数据长度不一致，跳过

        // 提取对应区间的 Y 值
        QVector<double> y = data.mid(startIdx, pointsToShow);
        graph->setData(x, y);
    }
    
    // Update x-axis based on timestamps
    m_plot->xAxis->setRange(xMin, xMax);
    // 只自动缩放 Y 轴，保留 X 轴范围
    updateYAxis();
    m_plot->replot();
}

void OscilloscopeWidget::clear() {
    m_plot->clearGraphs();
    m_graphs.clear();
    m_fields.clear();
    m_plot->legend->setVisible(false);
    m_plot->replot();
}

void OscilloscopeWidget::onConfigure() {
    // 通知主窗口弹出配置对话框
    emit fieldsChanged();
}

void OscilloscopeWidget::onToggleYLock() {
    m_yLocked = m_yLockBtn->isChecked();
    m_yLockBtn->setText(m_yLocked ? "🔓" : "🔒");
    if (!m_yLocked) {
        // 解锁时，立即自动调整一次Y轴（使用当前数据）
        updateYAxis();
        m_plot->replot();
    }
}

void OscilloscopeWidget::updateYAxis() {
    if (!m_yLocked) {
        // 自动缩放 Y 轴（只缩放，不改变 X 轴范围）
        m_plot->rescaleAxes(false);
        // 增加一点边距，避免曲线贴边
        m_plot->yAxis->scaleRange(1.1, m_plot->yAxis->range().center());
    }
    // 如果锁定，不做任何操作，保留用户当前设置的 Y 轴范围
}