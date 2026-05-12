#include "OscilloscopeWidget.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QMimeData>
#include <QPalette>
#include <cmath>
#include <limits>

OscilloscopeWidget::OscilloscopeWidget(QWidget *parent):
    QWidget(parent),
    m_plot(new QCustomPlot(this)),
    m_yLocked(false) {
        setMinimumHeight(200);
        setAcceptDrops(true);
        m_plot->setAcceptDrops(false);
        setupUi();
}

void OscilloscopeWidget::setupUi() {
    // 标题栏
    m_titleLabel = new QLabel("Oscilloscope", this);
    m_titleLabel->setAlignment(Qt::AlignCenter);
    
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

    // Move Up Button (↑)
    m_moveUpBtn = new QPushButton("↑", this);
    m_moveUpBtn->setFixedSize(20, 20);
    m_moveUpBtn->setToolTip("Move scope up");
    connect(m_moveUpBtn, &QPushButton::clicked, this, &OscilloscopeWidget::moveUpRequested);

    // Move Down Button (↓)
    m_moveDownBtn = new QPushButton("↓", this);
    m_moveDownBtn->setFixedSize(20, 20);
    m_moveDownBtn->setToolTip("Move scope down");
    connect(m_moveDownBtn, &QPushButton::clicked, this, &OscilloscopeWidget::moveDownRequested);
    
    QHBoxLayout *titleLayout = new QHBoxLayout;
    titleLayout->addWidget(m_titleLabel);
    titleLayout->addWidget(m_yLockBtn);
    titleLayout->addWidget(addBelowBtn);
    titleLayout->addWidget(removeBtn);
    titleLayout->addWidget(m_moveUpBtn);
    titleLayout->addWidget(m_moveDownBtn);
    titleLayout->addWidget(m_configBtn);
    
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
    m_plot->setOpenGl(false);
    m_plot->setNoAntialiasingOnDrag(true);
    m_plot->setPlottingHint(QCP::phFastPolylines, true);
    applyTheme();
    
    // 主布局
    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->addLayout(titleLayout);
    layout->addWidget(m_plot);
}

void OscilloscopeWidget::setMoveButtonsEnabled(bool upEnabled, bool downEnabled) {
    m_moveUpBtn->setEnabled(upEnabled);
    m_moveDownBtn->setEnabled(downEnabled);
}

void OscilloscopeWidget::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange) {
        applyTheme();
    }
    QWidget::changeEvent(event);
}

void OscilloscopeWidget::setTitle(const QString &title) {
    m_titleLabel->setText(title);
}

void OscilloscopeWidget::setFields(const QStringList &fields) {
    m_fields = fields;
    m_plot->clearGraphs();
    m_graphs.clear();
    m_renderBuffers.clear();
    
    for (int i = 0; i < m_fields.size(); ++i) {
        QCPGraph *graph = m_plot->addGraph();
        graph->setName(m_fields[i]);
        graph->setPen(QPen(m_colors[i % m_colors.size()], 1.5));
        graph->setAdaptiveSampling(true);
        m_graphs[m_fields[i]] = graph;
    }
    m_plot->legend->setVisible(!m_fields.isEmpty());
    // 注意：不发射 fieldsChanged，避免循环
    emit refreshRequested();
}

void OscilloscopeWidget::updatePlot(const QHash<QString, QVector<double>> &dataPool,
                                    const QVector<double> &timeStamps, int maxPoints) {
    if (m_fields.isEmpty()) return;
    if (timeStamps.isEmpty()) return;

    int totalPoints = timeStamps.size();
    int startIdx = qMax(0, totalPoints - maxPoints);
    int pointsToShow = totalPoints - startIdx;
    if (pointsToShow <= 0) return;

    const double xMin = timeStamps[startIdx];
    const double xMax = timeStamps[totalPoints - 1];
    const int targetSamples = qMax(200, m_plot->viewport().width() * 2);
    const bool shouldDownsample = pointsToShow > targetSamples;
    const int stride = shouldDownsample ? qMax(1, pointsToShow / targetSamples) : 1;
    double yMin = std::numeric_limits<double>::max();
    double yMax = std::numeric_limits<double>::lowest();
    bool hasVisibleData = false;
    
    // Update each graph's data
    for (const QString &field : m_fields) {
        QCPGraph *graph = m_graphs.value(field);
        if (!graph) continue;
        const QVector<double> &data = dataPool.value(field);
        const int dataStartIdx = qMax(0, totalPoints - data.size());
        const int visibleStartIdx = qMax(startIdx, dataStartIdx);
        const int visiblePoints = totalPoints - visibleStartIdx;
        RenderBuffers &buffers = m_renderBuffers[field];
        buffers.x.clear();
        buffers.y.clear();
        buffers.x.reserve(shouldDownsample ? (qMax(0, visiblePoints) / stride) * 2 + 2 : qMax(0, visiblePoints));
        buffers.y.reserve(buffers.x.capacity());

        if (visiblePoints <= 0) {
            graph->setData(buffers.x, buffers.y, true);
            continue;
        }

        if (shouldDownsample) {
            for (int bucketStart = visibleStartIdx; bucketStart < totalPoints; bucketStart += stride) {
                const int bucketEnd = qMin(totalPoints, bucketStart + stride);
                int minIndex = -1;
                int maxIndex = -1;
                double minValue = std::numeric_limits<double>::max();
                double maxValue = std::numeric_limits<double>::lowest();

                for (int i = bucketStart; i < bucketEnd; ++i) {
                    const int dataIndex = i - dataStartIdx;
                    if (dataIndex < 0 || dataIndex >= data.size()) {
                        continue;
                    }
                    const double value = data[dataIndex];
                    if (!std::isfinite(value)) {
                        continue;
                    }
                    if (value < minValue) {
                        minValue = value;
                        minIndex = i;
                    }
                    if (value > maxValue) {
                        maxValue = value;
                        maxIndex = i;
                    }
                }

                if (minIndex < 0 || maxIndex < 0) {
                    continue;
                }

                if (minIndex <= maxIndex) {
                    buffers.x.append(timeStamps[minIndex]);
                    buffers.y.append(minValue);
                    if (minIndex != maxIndex) {
                        buffers.x.append(timeStamps[maxIndex]);
                        buffers.y.append(maxValue);
                    }
                } else {
                    buffers.x.append(timeStamps[maxIndex]);
                    buffers.y.append(maxValue);
                    buffers.x.append(timeStamps[minIndex]);
                    buffers.y.append(minValue);
                }

                yMin = qMin(yMin, minValue);
                yMax = qMax(yMax, maxValue);
                hasVisibleData = true;
            }
        } else {
            for (int i = visibleStartIdx; i < totalPoints; ++i) {
                const int dataIndex = i - dataStartIdx;
                if (dataIndex < 0 || dataIndex >= data.size()) {
                    continue;
                }
                const double value = data[dataIndex];
                if (!std::isfinite(value)) {
                    continue;
                }
                buffers.x.append(timeStamps[i]);
                buffers.y.append(value);
                yMin = qMin(yMin, value);
                yMax = qMax(yMax, value);
                hasVisibleData = true;
            }
        }

        graph->setData(buffers.x, buffers.y, true);
    }
    
    // Update x-axis based on timestamps
    m_plot->xAxis->setRange(xMin, xMax);
    updateYAxis(yMin, yMax, hasVisibleData);
    m_plot->replot(QCustomPlot::rpQueuedReplot);
}

void OscilloscopeWidget::clear() {
    m_plot->clearGraphs();
    m_graphs.clear();
    m_renderBuffers.clear();
    m_fields.clear();
    m_plot->legend->setVisible(false);
    m_plot->replot(QCustomPlot::rpQueuedReplot);
}

void OscilloscopeWidget::addField(const QString &fieldName) {
    if (m_fields.contains(fieldName)) return;
    QStringList newFields = m_fields;
    newFields.append(fieldName);
    setFields(newFields);
}

void OscilloscopeWidget::setFieldColor(const QString &fieldName, const QColor &color) {
    if (!m_graphs.contains(fieldName))
        return;
    m_graphs[fieldName]->setPen(QPen(color, 1.5));
    m_plot->replot();
}

QColor OscilloscopeWidget::getFieldColor(const QString &fieldName) const {
    if (m_graphs.contains(fieldName))
        return m_graphs[fieldName]->pen().color();
    return QColor();
}

void OscilloscopeWidget::setColorList(const QList<QColor> &colors)
{
    m_colors = colors;
    // If a plot is already created, refresh the colors
    if (!m_fields.isEmpty()) {
        for (int i = 0; i < m_fields.size(); ++i) {
            const QString &field = m_fields[i];
            if (m_graphs.contains(field)) {
                m_graphs[field]->setPen(QPen(m_colors[i % m_colors.size()], 1.5));
            }
        }
        m_plot->replot();
    }
}

void OscilloscopeWidget::dragEnterEvent(QDragEnterEvent *event) {
    if (event->mimeData()->hasText() || event->mimeData()->hasFormat("application/x-qabstractitemmodeldatalist"))
        event->acceptProposedAction();
}

void OscilloscopeWidget::dragMoveEvent(QDragMoveEvent *event) {
    if (event->mimeData()->hasText() || event->mimeData()->hasFormat("application/x-qabstractitemmodeldatalist")) {
        event->acceptProposedAction();
    }
}

void OscilloscopeWidget::dropEvent(QDropEvent *event) {
    QString fieldName;

    if (event->mimeData()->hasText()) {
        fieldName = event->mimeData()->text();
    } 
    // If dragging from a standard QListWidget, the text is buried in a stream
    else if (event->mimeData()->hasFormat("application/x-qabstractitemmodeldatalist")) {
        QTreeWidget *tree = qobject_cast<QTreeWidget*>(event->source()); // or QListWidget
        if (event->source()) {
             // If coming from your QListWidget, it's easier to just get the current item
             QListWidget *list = qobject_cast<QListWidget*>(event->source());
             if (list && list->currentItem()) {
                 fieldName = list->currentItem()->text();
             }
        }
    }
    
    if (!fieldName.isEmpty() && !m_fields.contains(fieldName)) {
        addField(fieldName);
        event->acceptProposedAction();
    } else {
        event->ignore();
    }
}

void OscilloscopeWidget::onConfigure() {
    // 通知主窗口弹出配置对话框
    emit fieldsChanged();
}

void OscilloscopeWidget::applyTheme()
{
    const QPalette pal = palette();
    const QColor windowColor = pal.color(QPalette::Window);
    const QColor baseColor = pal.color(QPalette::Base);
    const QColor textColor = pal.color(QPalette::WindowText);
    const QColor buttonColor = pal.color(QPalette::Button);
    const QColor buttonTextColor = pal.color(QPalette::ButtonText);
    const QColor midColor = pal.color(QPalette::Mid);
    const QColor gridColor = midColor;
    const QColor subGridColor = midColor.lighter(115);

    m_titleLabel->setStyleSheet(QString(
        "font-weight: bold; background-color: %1; color: %2;"
    ).arg(buttonColor.name(), buttonTextColor.name()));

    const QList<QPushButton*> buttons = findChildren<QPushButton*>();
    for (QPushButton *button : buttons) {
        button->setStyleSheet(QString(
            "QPushButton { background-color: %1; color: %2; border: 1px solid %3; }"
            "QPushButton:checked { background-color: %4; }"
        ).arg(buttonColor.name(),
              buttonTextColor.name(),
              midColor.name(),
              buttonColor.darker(115).name()));
    }

    m_plot->setBackground(baseColor);
    m_plot->axisRect()->setBackground(windowColor);
    m_plot->legend->setBrush(QBrush(windowColor));
    m_plot->legend->setBorderPen(QPen(midColor));
    m_plot->legend->setTextColor(textColor);

    for (QCPAxis *axis : {m_plot->xAxis, m_plot->yAxis}) {
        axis->setBasePen(QPen(textColor));
        axis->setTickPen(QPen(textColor));
        axis->setSubTickPen(QPen(textColor));
        axis->setTickLabelColor(textColor);
        axis->setLabelColor(textColor);
        axis->grid()->setPen(QPen(gridColor, 0, Qt::DotLine));
        axis->grid()->setSubGridPen(QPen(subGridColor, 0, Qt::DotLine));
        axis->grid()->setZeroLinePen(QPen(textColor, 0));
    }

    m_plot->replot(QCustomPlot::rpQueuedReplot);
}

void OscilloscopeWidget::onToggleYLock() {
    m_yLocked = m_yLockBtn->isChecked();
    m_yLockBtn->setText(m_yLocked ? "🔓" : "🔒");
    if (!m_yLocked) {
        bool hasData = false;
        double yMin = std::numeric_limits<double>::max();
        double yMax = std::numeric_limits<double>::lowest();

        for (auto it = m_renderBuffers.cbegin(); it != m_renderBuffers.cend(); ++it) {
            const QVector<double> &values = it.value().y;
            for (double value : values) {
                yMin = qMin(yMin, value);
                yMax = qMax(yMax, value);
                hasData = true;
            }
        }

        updateYAxis(yMin, yMax, hasData);
        m_plot->replot(QCustomPlot::rpQueuedReplot);
    }
}

void OscilloscopeWidget::updateYAxis(double yMin, double yMax, bool hasData) {
    if (m_yLocked || !hasData) {
        return;
    }

    if (qFuzzyCompare(yMin, yMax)) {
        const double padding = qAbs(yMin) * 0.05 + 1.0;
        m_plot->yAxis->setRange(yMin - padding, yMax + padding);
        return;
    }

    const double padding = (yMax - yMin) * 0.05;
    m_plot->yAxis->setRange(yMin - padding, yMax + padding);
}
