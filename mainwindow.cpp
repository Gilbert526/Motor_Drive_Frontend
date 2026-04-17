#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "SerialManager.h"
#include "DataParser.h"
#include <QMessageBox>
#include <QSerialPortInfo>
#include <QDialog>
#include <QDialogButtonBox>
#include <QListWidgetItem>

MainWindow::MainWindow(QWidget *parent):
    QMainWindow(parent),
    ui(new Ui::MainWindow),
    m_serialManager(nullptr),
    m_dataParser(nullptr),
    m_serialThread(nullptr),
    m_historyIndex(-1),
    m_maxWavePoints(20000),     // 最多存储20000点
    m_currentMaxPoints(500),
    m_plotPaused(false),
    m_syncingFromMask(false),
    m_lastSpeedValue(0.0),
    m_lastTorqueValue(0.0),
    m_targetManuallyEdited(false),
    m_timeManuallyEdited(false),
    m_updatingTargetType(false),
    m_recordHistory(true),
    m_packetCounter(0),
    m_packetIntervalSec(1.0 / DEFAULT_PACKET_FREQ_HZ) {
        ui->setupUi(this);

        // 初始化串口管理线程
        m_serialManager = new SerialManager();
        m_serialThread = new QThread(this);
        m_serialManager->moveToThread(m_serialThread);
        connect(m_serialThread, &QThread::finished, m_serialManager, &QObject::deleteLater);

        // 创建数据解析器（主线程）
        m_dataParser = new DataParser(this);

        // 信号连接
        connect(m_serialManager, &SerialManager::portOpened, this, &MainWindow::handleSerialPortOpened);
        connect(m_serialManager, &SerialManager::portClosed, this, &MainWindow::handleSerialPortClosed);
        connect(m_serialManager, &SerialManager::rawDataReceived, m_dataParser, &DataParser::parseData);
        connect(m_dataParser, &DataParser::parsedData, this, &MainWindow::handleNewData);

        connect(m_dataParser, &DataParser::maskReceived, this, &MainWindow::onMaskReceived);

        // Display received message in text box
        connect(m_serialManager, &SerialManager::rawDataReceived, this, [this](const QByteArray &data) {
            static QByteArray buffer;           // Buffer for original data
            static QByteArray lineBuffer;       // Accumulated incomplete text lines
            buffer.append(data);

            int idx = 0;
            while (idx < buffer.size()) {
                // Look for the next frame header (0xAA 0x55)
                int headerPos = buffer.indexOf(QByteArray::fromHex("AA55"), idx);
                if (headerPos == -1) {
                    // Check for a split header: if the last byte is 0xAA, stop here
                    int endOfText = buffer.size();
                    if (buffer.endsWith(char(0xAA))) {
                        endOfText -= 1;
                    }
                    if (endOfText > idx) {
                        lineBuffer.append(buffer.mid(idx, endOfText - idx));
                        idx = endOfText; 
                    }
                    break;
                }

                // Text data before the frame header (possibly incomplete line)
                if (headerPos > idx) {
                    lineBuffer.append(buffer.mid(idx, headerPos - idx));
                }

                // Process complete lines in lineBuffer (separated by newline characters)
                int newlinePos;
                while ((newlinePos = lineBuffer.indexOf('\n')) != -1) {
                    QByteArray line = lineBuffer.left(newlinePos + 1); // Include newline character
                    QString text = QString::fromUtf8(line).trimmed();
                    if (!text.isEmpty()) {
                        ui->plainTextEditReceive->appendPlainText(text);
                        parseTuneResponse(text);   // Parse tuning response
                    }
                    lineBuffer.remove(0, newlinePos + 1);
                }

                // Try to get the binary frame length
                int frameLen = m_dataParser->getFrameLength(buffer, headerPos);
                if (frameLen == -1) {
                    // Insufficient data, retain the unprocessed portion from headerPos (incomplete frame header)
                    idx = headerPos;
                    break;
                }

                // Skip the entire binary frame
                idx = headerPos + frameLen;
            }

            // If the loop ends normally (idx reaches the end), clear the buffer
            if (idx >= buffer.size()) {
                buffer.clear();
            } else {
                // Retain the unprocessed portion (possibly an incomplete binary frame header)
                buffer = buffer.mid(idx);
            }
        });

        // 启动串口线程
        m_serialThread->start();

        // 初始化示波器区域
        setupPlottingArea();

        // 加载字段列表到左侧
        loadAvailableFields();

        // 定时器刷新波形
        m_plotTimer = new QTimer(this);
        connect(m_plotTimer, &QTimer::timeout, this, &MainWindow::updatePlot);
        m_plotTimer->start(50);

        // 串口UI初始化
        refreshSerialPorts();
        ui->comboBaud->addItems({"9600", "19200", "38400", "57600", "115200", "230400", "460800", "921600"});
        ui->comboBaud->setCurrentText("115200");

        ui->pushButtonRefresh->setText("Refresh");
        ui->pushButtonRefresh->setToolTip("Refresh serial ports");
        ui->pushButtonSend->setText("➢");
        ui->pushButtonSend->setToolTip("Send command");
        ui->pushButtonFoc->setText("FOC");
        ui->pushButtonFoc->setToolTip("Start motor with FOC");
        ui->pushButtonVvvf->setText("VVVF");
        ui->pushButtonVvvf->setToolTip("Start motor with VVVF ramp-up");
        ui->pushButtonSixstep->setText("Sixstep");
        ui->pushButtonSixstep->setToolTip("Start motor with six-step commutation");
        ui->pushButtonAlign->setText("Align");
        ui->pushButtonAlign->setToolTip("Align motor electrical and mechanical zero positions");
        ui->pushButtonStop->setText("Stop");
        ui->pushButtonStop->setToolTip("Stop motor");
        ui->pushButtonAudible->setText("Audible");
        ui->pushButtonAudible->setToolTip("Toggle audible PWM frequencies");
        ui->pushButtonReset->setText("Reset");
        ui->pushButtonReset->setToolTip("Reset motor state");
        ui->pushButtonPause->setText("⏸️");
        ui->plainTextEditReceive->setReadOnly(true);

        // Command line features
        ui->lineEditSend->installEventFilter(this);

        updateUiForSerialState(false);

        /*--- Target Setting ---*/
        // Initialize target selection
        ui->comboBoxTargetSelection->addItems({"Speed", "Torque"});
        m_currentTargetType = ui->comboBoxTargetSelection->currentText();

        ui->comboBoxTargetSelection->setToolTip("Select target type for motor control (Speed or Torque)");
        ui->targetSlider->setToolTip("Adjust target value using slider");
        ui->timeSlider->setToolTip("Adjust time duration for ramping to target value, set to 0 for immediate change");
        ui->lineEditTarget->setToolTip("Manually enter target value (overrides slider)");
        ui->lineEditTime->setToolTip("Manually enter time duration in seconds (overrides time slider)");
        ui->pushButtonTargetSend->setToolTip("Send target command");

        // Initialize target slider
        updateTargetSliderLimits();
        on_comboBoxTargetSelection_currentIndexChanged(0);
        setTargetValue(0.0, true);
        // Initialize time slider
        ui->timeSlider->setRange(0, 60);
        ui->timeSlider->setSingleStep(1);
        ui->timeSlider->setValue(0);
        on_timeSlider_valueChanged(0);

        /*--- Tuning ---*/
        // Populate subsystem combo box
        ui->comboBoxTuneSubsystem->addItems({"speed", "id", "iq", "fw", "gain", "offset"});

        // Connect tuning parameter signals
        connect(ui->comboBoxTuneSubsystem, QOverload<int>::of(&QComboBox::currentIndexChanged),
        this, &MainWindow::on_comboBoxTuneSubsystem_currentIndexChanged);

        // Initialize tuning parameters for the first subsystem
        on_comboBoxTuneSubsystem_currentIndexChanged(0);

        // Initialize increment slider with predefined step values
        const QVector<double> stepValues = {0.00001, 0.00005, 0.0001, 0.0005, 0.001, 0.005, 0.01, 0.05, 0.1, 0.5, 1.0, 5.0, 10.0, 50.0, 100.0, 500.0, 1000.0, 5000.0, 10000.0};
        ui->incrementSlider->setRange(0, stepValues.size() - 1);
        ui->incrementSlider->setValue(10); // 默认 1.0
        on_incrementSlider_valueChanged(10);

        ui->pushButtonTuneEnquire->setToolTip("Enquire current parameter value and display in tuning value box");
        ui->pushButtonIncrement->setToolTip("Increment parameter");
        ui->pushButtonDecrement->setToolTip("Decrement parameter");
        ui->pushButtonTuneSend->setToolTip("Send tuning command");
        ui->pushButtonTuneUndo->setToolTip("Undo last tuning change");
        ui->incrementLabel->setText("1");
        ui->incrementSlider->setToolTip("Adjust tuning increment step");

        m_stepValues = {0.00001, 0.00005, 0.0001, 0.0005, 0.001, 0.005, 0.01, 0.05, 0.1, 0.5, 1.0, 5.0, 10.0, 50.0, 100.0, 500.0, 1000.0, 5000.0, 10000.0};
    }

MainWindow::~MainWindow() {
    if (m_serialThread->isRunning()) {
        m_serialThread->quit();
        m_serialThread->wait();
    }
    delete ui;
}

QList<QColor> MainWindow::getPresetColors()
{
    return {
        Qt::red, Qt::green, Qt::blue, Qt::magenta,
        Qt::cyan, Qt::darkYellow, Qt::darkCyan, Qt::darkMagenta,
        QColor(240, 131, 0),
        QColor(106, 45, 151),
        QColor(255, 143, 191)
    };
}

QStringList MainWindow::getColorNames()
{
    return {"Red", "Green", "Blue", "Magenta", "Cyan", "Dark Yellow", "Dark Cyan", "Dark Magenta",
            "Mikan", "Purple", "Pink"};
}

// ==================== 波形区域初始化 ====================
void MainWindow::setupPlottingArea() {
    // Obtain pointers to UI elements
    m_fieldList = ui->fieldListWidget;
    m_scrollArea = ui->scrollArea;
    m_oscContainer = ui->oscilloscopeContainer;
    m_sampleSlider = ui->sampleSlider;
    m_sampleLabel = ui->sampleLabel;

    // Set field list to single selection mode
    m_fieldList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_fieldList->setDragEnabled(true);
    m_fieldList->setDragDropMode(QAbstractItemView::DragOnly);
    m_fieldList->setDefaultDropAction(Qt::CopyAction);
    m_fieldList->clear();
    // Get available fields from DataParser and populate the list with checkable items
    QStringList allFields = m_dataParser->getFieldNames();
    for (const QString &field : allFields) {
        QListWidgetItem *item = new QListWidgetItem(field);
        item->setText(field);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        item->setCheckState(Qt::Unchecked);
        m_fieldList->addItem(item);
    }
    connect(m_fieldList, &QListWidget::itemChanged, this, &MainWindow::onFieldCheckStateChanged);

    // Set up the oscilloscope container with a vertical layout
    if (m_oscContainer->layout() == nullptr) {
        m_oscLayout = new QVBoxLayout(m_oscContainer);
        m_oscContainer->setLayout(m_oscLayout);
    } else {
        m_oscLayout = qobject_cast<QVBoxLayout*>(m_oscContainer->layout());
    }
    m_oscLayout->setAlignment(Qt::AlignTop);

    // Sample slider configuration
    m_sampleSlider->setRange(100, 10000);
    m_sampleSlider->setSingleStep(100);
    m_sampleSlider->setPageStep(100);
    m_sampleSlider->setTickInterval(100);
    m_sampleSlider->setValue(m_currentMaxPoints);
    m_sampleLabel->setText(QString::number(m_currentMaxPoints));
    connect(m_sampleSlider, &QSlider::valueChanged, this, &MainWindow::on_sampleSlider_valueChanged);

    // Double click field to add new oscilloscope
    connect(m_fieldList, &QListWidget::itemDoubleClicked, this, &MainWindow::on_fieldList_itemDoubleClicked);

    // Add a scope by default
    addOscilloscope("Scope 1");
    updateAllMoveButtons();
}

void MainWindow::addOscilloscope(const QString &title, int index) {
    OscilloscopeWidget *osc = new OscilloscopeWidget;
    osc->setColorList(getPresetColors());
    if (!title.isEmpty())
        osc->setTitle(title);
    else
        osc->setTitle(QString("Scope %1").arg(m_oscilloscopes.size() + 1));

    // 连接配置请求信号（点击齿轮按钮时）
    connect(osc, &OscilloscopeWidget::fieldsChanged, this, [this, osc]() {
        on_oscilloscopeConfigRequested(osc);
    });
    connect(osc, &OscilloscopeWidget::removeRequested, this, [this, osc]() {
        removeOscilloscope(osc);
    });
    connect(osc, &OscilloscopeWidget::addBelowRequested, this, [this, osc]() {
        int idx = m_oscLayout->indexOf(osc);
        if (idx >= 0) {
            addOscilloscope(QString("Scope %1").arg(m_oscilloscopes.size() + 1), idx + 1);
        }
    });

    connect(osc, &OscilloscopeWidget::moveUpRequested, this, &MainWindow::onMoveUpRequested);
    connect(osc, &OscilloscopeWidget::moveDownRequested, this, &MainWindow::onMoveDownRequested);

    connect(osc, &OscilloscopeWidget::refreshRequested, this, &MainWindow::updateAllPlots);

    if (index < 0 || index > m_oscLayout->count()) {
        m_oscLayout->addWidget(osc);
        m_oscilloscopes.append(osc);
    } else {
        m_oscLayout->insertWidget(index, osc);
        m_oscilloscopes.insert(index, osc);
    }

    updateAllMoveButtons();
}

void MainWindow::removeOscilloscope(OscilloscopeWidget *osc) {
    if (!osc) return;
    int idx = m_oscLayout->indexOf(osc);
    if (idx >= 0) {
        m_oscLayout->removeWidget(osc);
        m_oscilloscopes.removeAt(idx);
        osc->deleteLater();
    }

    updateAllMoveButtons();
}

void MainWindow::updateAllMoveButtons() {
    int count = m_oscilloscopes.size();
    for (int i = 0; i < count; ++i) {
        bool upEnabled = (i > 0);
        bool downEnabled = (i < count - 1);
        m_oscilloscopes[i]->setMoveButtonsEnabled(upEnabled, downEnabled);
    }
}

void MainWindow::loadAvailableFields() {
    if (!m_dataParser) return;
    QStringList allFields = m_dataParser->getFieldNames();
    m_fieldList->clear();
    for (const QString &field : allFields) {
        QListWidgetItem *item = new QListWidgetItem(field);
        item->setText(field);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        item->setCheckState(Qt::Unchecked);
        m_fieldList->addItem(item);
    }
}

void MainWindow::on_fieldList_itemDoubleClicked(QListWidgetItem *item) {
    // 双击字段：创建新示波器并添加该字段
    addOscilloscope();
    OscilloscopeWidget *newOsc = m_oscilloscopes.last();
    newOsc->setFields({item->text()});
}

void MainWindow::on_oscilloscopeConfigRequested(OscilloscopeWidget *osc) {
    QDialog dialog(this);
    dialog.setWindowTitle("Configure Fields to Plot");
    dialog.setMinimumWidth(350);

    QTableWidget *table = new QTableWidget(&dialog);
    table->setColumnCount(2);
    table->setHorizontalHeaderLabels({"Field", "Color"});
    table->setColumnWidth(0, 180); // 设置第一列宽度为 180 像素
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table->verticalHeader()->setVisible(false);

    QStringList allFields = m_dataParser->getFieldNames();
    QStringList currentFields = osc->getFields();

    // 预设颜色列表（与 OscilloscopeWidget 中一致）
    QList<QColor> presetColors = MainWindow::getPresetColors();
    QStringList colorNames = MainWindow::getColorNames();

    // 存储每个字段的颜色下拉框指针
    QHash<int, QComboBox*> colorCombos;

    table->setRowCount(allFields.size());
    for (int i = 0; i < allFields.size(); ++i) {
        QString field = allFields[i];
        bool checked = currentFields.contains(field);

        // 第一列：字段名 + 复选框（不可编辑）
        QTableWidgetItem *item = new QTableWidgetItem(field);
        item->setFlags(item->flags() & ~Qt::ItemIsEditable);   // 禁止编辑
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
        table->setItem(i, 0, item);

        // 第二列：如果已勾选，创建颜色下拉框；否则留空
        if (checked) {
            QComboBox *combo = new QComboBox();
            // 添加预设颜色（带图标）
            for (int j = 0; j < presetColors.size(); ++j) {
                QPixmap pixmap(16, 16);
                pixmap.fill(presetColors[j]);
                combo->addItem(QIcon(pixmap), colorNames[j], presetColors[j]);
            }
            combo->addItem("Custom...");   // 自定义选项

            // 尝试获取当前字段的现有颜色
            QColor currentColor = osc->getFieldColor(field);
            if (currentColor.isValid()) {
                int index = presetColors.indexOf(currentColor);
                if (index >= 0)
                    combo->setCurrentIndex(index);
                else
                    combo->setCurrentIndex(presetColors.size()); // Custom...
            } else {
                // 默认：根据字段在 currentFields 中的索引分配预设颜色
                int idx = currentFields.indexOf(field);
                if (idx >= 0 && idx < presetColors.size())
                    combo->setCurrentIndex(idx);
            }

            // 处理 Custom... 选项：弹出颜色对话框
            connect(combo, QOverload<int>::of(&QComboBox::activated), this, [combo, field, this](int index) {
                if (index == combo->count() - 1) { // 最后一项是 "Custom..."
                    QColor newColor = QColorDialog::getColor(combo->palette().button().color(), nullptr,
                                                             QString("Select Color for %1").arg(field));
                    if (newColor.isValid()) {
                        // 将自定义颜色存储为用户数据，并改变按钮图标显示
                        QPixmap pixmap(16, 16);
                        pixmap.fill(newColor);
                        combo->setItemIcon(index, QIcon(pixmap));
                        combo->setItemData(index, newColor, Qt::UserRole);
                        combo->setCurrentIndex(index);
                    } else {
                        // 取消选择，恢复之前的选择
                        int prev = combo->property("prevIndex").toInt();
                        combo->setCurrentIndex(prev);
                    }
                }
                combo->setProperty("prevIndex", combo->currentIndex());
            });

            // 存储当前索引以便取消时恢复
            combo->setProperty("prevIndex", combo->currentIndex());

            table->setCellWidget(i, 1, combo);
            colorCombos[i] = combo;
        } else {
            table->setCellWidget(i, 1, nullptr);
        }
    }

    // 双击字段行：切换勾选状态
    connect(table, &QTableWidget::cellDoubleClicked, this, [table](int row, int column) {
        Q_UNUSED(column);
        QTableWidgetItem *item = table->item(row, 0);
        if (item) {
            Qt::CheckState newState = (item->checkState() == Qt::Checked) ? Qt::Unchecked : Qt::Checked;
            item->setCheckState(newState);
        }
    });

    // 响应复选框状态变化：动态添加/删除颜色下拉框
    connect(table, &QTableWidget::itemChanged, this, [table, &colorCombos, presetColors, colorNames](QTableWidgetItem *item) {
        if (item->column() != 0) return;
        int row = item->row();
        bool checked = (item->checkState() == Qt::Checked);
        QString field = item->text();

        if (checked && !colorCombos.contains(row)) {
            // 创建颜色下拉框
            QComboBox *combo = new QComboBox();
            for (int j = 0; j < presetColors.size(); ++j) {
                QPixmap pixmap(16, 16);
                pixmap.fill(presetColors[j]);
                combo->addItem(QIcon(pixmap), colorNames[j], presetColors[j]);
            }
            combo->addItem("Custom...");
            combo->setCurrentIndex(0);
            combo->setProperty("prevIndex", 0);

            // 处理 Custom... 选项
            connect(combo, QOverload<int>::of(&QComboBox::activated), [combo, field](int index) {
                if (index == combo->count() - 1) {
                    QColor newColor = QColorDialog::getColor(Qt::white, nullptr,
                                                            QString("Select Color for %1").arg(field));
                    if (newColor.isValid()) {
                        QPixmap pixmap(16, 16);
                        pixmap.fill(newColor);
                        combo->setItemIcon(index, QIcon(pixmap));
                        combo->setItemData(index, newColor, Qt::UserRole);
                        combo->setCurrentIndex(index);
                    } else {
                        int prev = combo->property("prevIndex").toInt();
                        combo->setCurrentIndex(prev);
                    }
                }
                combo->setProperty("prevIndex", combo->currentIndex());
            });

            table->setCellWidget(row, 1, combo);
            colorCombos[row] = combo;
        } else if (!checked && colorCombos.contains(row)) {
            QWidget *oldWidget = table->cellWidget(row, 1);
            if (oldWidget) {
                table->removeCellWidget(row, 1);
                oldWidget->deleteLater();
            }
            colorCombos.remove(row);
        }
    });

    QDialogButtonBox *buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttonBox, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    QVBoxLayout *layout = new QVBoxLayout(&dialog);
    layout->addWidget(table);
    layout->addWidget(buttonBox);

    if (dialog.exec() == QDialog::Accepted) {
        QStringList selectedFields;
        QHash<QString, QColor> colorOverrides;

        for (int i = 0; i < allFields.size(); ++i) {
            QTableWidgetItem *item = table->item(i, 0);
            if (item->checkState() == Qt::Checked) {
                QString field = allFields[i];
                selectedFields << field;

                QComboBox *combo = qobject_cast<QComboBox*>(table->cellWidget(i, 1));
                if (combo) {
                    int idx = combo->currentIndex();
                    QColor color;
                    if (idx >= 0 && idx < presetColors.size()) {
                        color = presetColors[idx];
                    } else if (idx == presetColors.size()) {
                        // Custom... 选项：从 item data 中取自定义颜色
                        color = combo->itemData(idx, Qt::UserRole).value<QColor>();
                        if (!color.isValid())
                            color = Qt::black; // fallback
                    }
                    if (color.isValid())
                        colorOverrides[field] = color;
                }
            }
        }

        osc->setFields(selectedFields);
        for (auto it = colorOverrides.begin(); it != colorOverrides.end(); ++it) {
            osc->setFieldColor(it.key(), it.value());
        }
    }
}

void MainWindow::onMoveUpRequested() {
    OscilloscopeWidget *osc = qobject_cast<OscilloscopeWidget*>(sender());
    if (!osc) return;
    int idx = m_oscilloscopes.indexOf(osc);
    if (idx <= 0) return;

    // 交换列表中的指针
    m_oscilloscopes.swapItemsAt(idx, idx - 1);  // 或 qSwap(m_oscilloscopes[idx], m_oscilloscopes[idx-1]);

    // 交换布局中的位置
    // 获取两个 widget 在布局中的索引（实际上与列表顺序一致，但布局可能因隐藏等原因不同，此处假设一致）
    // 更可靠：直接取布局中的两个 item，交换它们的位置
    QLayoutItem *itemUp = m_oscLayout->takeAt(idx - 1);
    QLayoutItem *itemDown = m_oscLayout->takeAt(idx - 1); // 注意：取走第一个后，原 idx 位置变为 idx-1
    if (itemUp && itemDown) {
        // 重新插入，顺序互换
        m_oscLayout->insertWidget(idx - 1, itemDown->widget());
        m_oscLayout->insertWidget(idx, itemUp->widget());
    }
    // 删除临时 item 对象（不删除 widget）
    delete itemUp;
    delete itemDown;

    updateAllMoveButtons();
}

void MainWindow::onMoveDownRequested() {
    OscilloscopeWidget *osc = qobject_cast<OscilloscopeWidget*>(sender());
    if (!osc) return;
    int idx = m_oscilloscopes.indexOf(osc);
    if (idx < 0 || idx >= m_oscilloscopes.size() - 1) return;

    m_oscilloscopes.swapItemsAt(idx, idx + 1);

    // 交换布局中的位置
    QLayoutItem *itemCurrent = m_oscLayout->takeAt(idx);
    QLayoutItem *itemNext = m_oscLayout->takeAt(idx); // 此时原 idx+1 移动到 idx
    if (itemCurrent && itemNext) {
        m_oscLayout->insertWidget(idx, itemNext->widget());
        m_oscLayout->insertWidget(idx + 1, itemCurrent->widget());
    }
    delete itemCurrent;
    delete itemNext;

    updateAllMoveButtons();
}

bool MainWindow::eventFilter(QObject *obj, QEvent *event) {
    if (obj == ui->lineEditSend && event->type() == QEvent::KeyPress) {
        QKeyEvent *keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->key() == Qt::Key_Up) {
            // 上键：浏览上一条历史
            if (!m_sendHistory.isEmpty() && m_historyIndex > 0) {
                m_historyIndex--;
                ui->lineEditSend->setText(m_sendHistory[m_historyIndex]);
            }
            return true;
        } else if (keyEvent->key() == Qt::Key_Down) {
            // 下键：浏览下一条历史
            if (!m_sendHistory.isEmpty() && m_historyIndex < m_sendHistory.size() - 1) {
                m_historyIndex++;
                ui->lineEditSend->setText(m_sendHistory[m_historyIndex]);
            } else if (m_historyIndex == m_sendHistory.size() - 1) {
                // 已经到最后一条，清空输入框
                ui->lineEditSend->clear();
                m_historyIndex = m_sendHistory.size(); // 指向末尾之后
            }
            return true;
        }
    }
    return QMainWindow::eventFilter(obj, event);
}

void MainWindow::on_sampleSlider_valueChanged(int value) {
    // 1. Calculate the "snapped" value
    int snappedValue = (value / 100) * 100;

    // 2. Block signals temporarily to prevent infinite loops when we reset the value
    m_sampleSlider->blockSignals(true);
    m_sampleSlider->setValue(snappedValue);
    m_sampleSlider->blockSignals(false);

    // 3. Update your label and internal variables
    m_currentMaxPoints = snappedValue;
    m_sampleLabel->setText(QString::number(snappedValue));
    updateAllPlots();
}

void MainWindow::updateAllPlots() {
    for (OscilloscopeWidget *osc : m_oscilloscopes) {
        osc->updatePlot(m_waveData, m_timeStamps, m_currentMaxPoints);
    }
}

void MainWindow::updatePlot() {
    if (!m_plotPaused) {
        updateAllPlots();
    }
}

// ==================== 数据处理 ====================
void MainWindow::handleNewData(const QHash<QString, double> &values) {
    // Calculate current time
    double currentTime = m_packetCounter * m_packetIntervalSec;
    m_packetCounter++;
    addTimeStamp(currentTime);
    // 追加到波形缓冲区
    for (auto it = values.begin(); it != values.end(); ++it) {
        const QString &field = it.key();
        double val = it.value();
        QVector<double> &vec = m_waveData[field];
        vec.append(val);
        if (vec.size() > m_maxWavePoints) {
            vec.remove(0, vec.size() - m_maxWavePoints);
        }
    }
    // 可选：在接收区显示关键数值（调试用，可注释）
    // if (values.contains("RPM")) {
    //     ui->plainTextEditReceive->appendPlainText(QString("RPM: %1").arg(values["RPM"], 0, 'f', 1));
    // }
}

void MainWindow::onMaskReceived(quint32 mask) {
    if (m_syncingFromMask) return;
    m_syncingFromMask = true;

    static quint32 lastMask = 0;
    if (mask == lastMask) {
        m_syncingFromMask = false;   // 关键：必须重置标志
        return;
    }
    lastMask = mask;

    // 遍历字段列表中的每个项
    for (int i = 0; i < m_fieldList->count(); ++i) {
        QListWidgetItem *item = m_fieldList->item(i);
        if (!item) continue;
        // 获取字段名对应的掩码位（需要字段定义信息）
        QString fieldName = item->text();
        quint32 bitMask = m_dataParser->getMaskForField(fieldName);
        bool shouldCheck = (mask & bitMask) != 0;
        if (shouldCheck != (item->checkState() == Qt::Checked)) {
            item->setCheckState(shouldCheck ? Qt::Checked : Qt::Unchecked);
        }
    }

    m_syncingFromMask = false;
}

// ==================== 串口处理 ====================
void MainWindow::refreshSerialPorts() {
    ui->comboPort->clear();
    foreach (const QSerialPortInfo &info, QSerialPortInfo::availablePorts()) {
        ui->comboPort->addItem(info.portName());
    }
    if (ui->comboPort->count() == 0)
        ui->comboPort->addItem("No available ports");
}

void MainWindow::updateUiForSerialState(bool isOpen) {
    if (isOpen) {
        ui->pushButtonStartToggle->setText("Stop");
        ui->comboPort->setEnabled(false);
        ui->comboBaud->setEnabled(false);
        ui->pushButtonRefresh->setEnabled(false);
    } else {
        ui->pushButtonStartToggle->setText("Start");
        ui->comboPort->setEnabled(true);
        ui->comboBaud->setEnabled(true);
        ui->pushButtonRefresh->setEnabled(true);
    }
}

void MainWindow::sendCommand(const QString &cmd) {
    if (!m_serialManager) return;
    QByteArray data = cmd.toUtf8();
    QMetaObject::invokeMethod(m_serialManager, "sendData",
                              Qt::QueuedConnection,
                              Q_ARG(QByteArray, data));
    ui->plainTextEditReceive->appendPlainText(">> " + cmd);
}

void MainWindow::on_pushButtonStartToggle_clicked() {
    if (m_serialManager->thread() == nullptr) return;

    if (ui->pushButtonStartToggle->text() == "Stop") {
        QMetaObject::invokeMethod(m_serialManager, "closeSerialPort", Qt::QueuedConnection);
    } else {
        QString portName = ui->comboPort->currentText();
        qint32 baudRate = ui->comboBaud->currentText().toInt();
        if (portName == "No available ports") {
            QMessageBox::warning(this, "Warning", "No available ports, please refresh the list");
            return;
        }
        QMetaObject::invokeMethod(m_serialManager, "openSerialPort",
                                  Qt::QueuedConnection,
                                  Q_ARG(QString, portName),
                                  Q_ARG(qint32, baudRate));
        ui->pushButtonStartToggle->setEnabled(false);
    }
}

void MainWindow::on_pushButtonRefresh_clicked() {
    refreshSerialPorts();
}

void MainWindow::on_pushButtonSend_clicked() {
    sendCurrentLineEditCommand();
}

void MainWindow::on_lineEditSend_returnPressed() {
    sendCurrentLineEditCommand();
}

void MainWindow::on_pushButtonFoc_clicked()     { sendCommand("start foc\r\n"); }
void MainWindow::on_pushButtonVvvf_clicked()    { sendCommand("start vvvf\r\n"); }
void MainWindow::on_pushButtonSixstep_clicked() { sendCommand("sixstep\r\n"); }
void MainWindow::on_pushButtonStop_clicked()    { sendCommand("stop\r\n"); }
void MainWindow::on_pushButtonAlign_clicked()   { sendCommand("align\r\n"); }
void MainWindow::on_pushButtonAudible_clicked() { sendCommand("audible\r\n"); }
void MainWindow::on_pushButtonReset_clicked()   { sendCommand("reset\r\n"); }
void MainWindow::on_pushButtonFocManual_clicked() { sendCommand("foc manual\r\n"); }
void MainWindow::on_pushButtonPreset1_clicked() { sendCommand("log preset 1\r\n"); }
void MainWindow::on_pushButtonPreset2_clicked() { sendCommand("log preset 2\r\n"); }
void MainWindow::on_pushButtonPreset3_clicked() { sendCommand("log preset 3\r\n"); }
void MainWindow::on_pushButtonPreset4_clicked() { sendCommand("log preset 4\r\n"); }
void MainWindow::on_pushButtonRemoveAll_clicked() { sendCommand("log rm all\r\n"); }
void MainWindow::on_pushButtonBin_clicked()     { sendCommand("log bin\r\n"); }
void MainWindow::on_pushButtonUtf8_clicked()    { sendCommand("log utf8\r\n"); }

void MainWindow::on_comboBoxTargetSelection_currentIndexChanged(int) {
    if (m_updatingTargetType) return;  // Reentry prevention
    m_updatingTargetType = true;

    QString newType = ui->comboBoxTargetSelection->currentText();
    QString oldType = (newType == "Speed") ? "Torque" : "Speed";

    // 1. Save the current value of the old type (exact value, could be manually entered or from slider)
    double oldValue;
    if (m_targetManuallyEdited) {
        bool ok;
        oldValue = ui->lineEditTarget->text().toDouble(&ok);
        if (!ok) oldValue = 0.0;
    } else {
        if (oldType == "Speed") {
            oldValue = ui->targetSlider->value();
        } else {
            oldValue = ui->targetSlider->value() / 10.0;
        }
    }
    if (oldType == "Speed") {
        m_lastSpeedValue = oldValue;
    } else {
        m_lastTorqueValue = oldValue;
    }

    // 2. Update the slider range (signals are blocked to avoid interference)
    ui->targetSlider->blockSignals(true);
    updateTargetSliderLimits();   // Adjust range based on new type
    ui->targetSlider->blockSignals(false);

    // 3. Restore the last saved value of the newly selected type (and mark as manually edited, preserving precision)
    double newValue = (newType == "Speed") ? m_lastSpeedValue : m_lastTorqueValue;
    setTargetValue(newValue, true);   // The second parameter true indicates it should be treated as manually edited

    m_updatingTargetType = false;
}

void MainWindow::on_targetSlider_valueChanged(int value) {
    // Ignore changes triggered by programmatic updates when switching target type
    if (m_updatingTargetType) return;

    m_targetManuallyEdited = false;
    if (ui->comboBoxTargetSelection->currentText() == "Speed") {
        int snapped = (value / 10) * 10;
        if (snapped != value) {
            ui->targetSlider->blockSignals(true);
            ui->targetSlider->setValue(snapped);
            ui->targetSlider->blockSignals(false);
            value = snapped;
        }
        double realValue = value;
        ui->lineEditTarget->setText(QString::number(realValue, 'f', 0));
    } else {
        double realValue = value / 10.0;
        ui->lineEditTarget->setText(QString::number(realValue, 'f', 1));
    }
}

void MainWindow::on_lineEditTarget_editingFinished() {
    bool ok;
    double val = ui->lineEditTarget->text().toDouble(&ok);
    if (!ok) return;

    // Only limit the range, do not round
    bool isSpeed = (ui->comboBoxTargetSelection->currentText() == "Speed");
    if (isSpeed) {
        val = qBound(-5000.0, val, 5000.0);
    } else {
        val = qBound(-10.0, val, 10.0);
    }

    // Move the slider to the nearest step value (speed in tens, torque in 0.1 increments)
    int sliderValue;
    if (isSpeed) {
        sliderValue = static_cast<int>(qRound(val / 10.0) * 10);
    } else {
        sliderValue = static_cast<int>(qRound(val * 10.0));
    }
    ui->targetSlider->blockSignals(true);
    ui->targetSlider->setValue(sliderValue);
    ui->targetSlider->blockSignals(false);

    // Restore the original value entered by the user (which may have been modified due to range limits, but keep its precision)
    QString formatted = QString::number(val, 'f', 6);
    // Remove trailing zeros and possible decimal point if not needed
    formatted.remove(QRegularExpression("\\.?0+$"));
    if (formatted.isEmpty()) formatted = "0";
    ui->lineEditTarget->setText(formatted);
    m_targetManuallyEdited = true;
}

void MainWindow::on_timeSlider_valueChanged(int value) {
    m_timeManuallyEdited = false;
    ui->lineEditTime->setText(QString::number(value));
}

void MainWindow::on_lineEditTime_editingFinished() {
    bool ok;
    double sec = ui->lineEditTime->text().toDouble(&ok);
    if (!ok) return;
    sec = qBound(0.0, sec, 60.0);
    ui->timeSlider->setValue(static_cast<int>(qRound(sec)));
    QString formatted = QString::number(sec, 'f', 6);
    formatted.remove(QRegularExpression("\\.?0+$"));
    if (formatted.isEmpty()) formatted = "0";
    ui->lineEditTime->setText(formatted);
    m_timeManuallyEdited = true;
}

void MainWindow::on_pushButtonTargetSend_clicked() {
    QString mode = ui->comboBoxTargetSelection->currentText().toLower();
    double target, time;
    if (m_targetManuallyEdited) {
        // Use value manually entered by the user
        bool ok;
        target = ui->lineEditTarget->text().toDouble(&ok);
        if (!ok) return;
        // Clamp again to ensure the range
        if (mode == "speed") {
            target = qBound(-5000.0, target, 5000.0);
        } else {
            target = qBound(-10.0, target, 10.0);
        }
    } else {
        // Use the value from the slider
        if (mode == "speed") {
            target = ui->targetSlider->value();
        } else {
            target = ui->targetSlider->value() / 10.0;
        }
    }
    if (m_timeManuallyEdited) {
        bool ok;
        time = ui->lineEditTime->text().toDouble(&ok);
        if (!ok) return;
        time = qBound(0.0, time, 60.0);
    } else {
        time = ui->timeSlider->value();
    }
    QString cmd;
    if (qFuzzyIsNull(time) || ui->timeSlider->value() == 0) {
        cmd = QString("%1 %2\r\n").arg(mode).arg(target, 0, 'f', 6);  // Keep sufficient precision
    } else {
        cmd = QString("%1 %2 %3\r\n").arg(mode).arg(target, 0, 'f', 6).arg(time, 0, 'f', 6);
    }
    sendCommand(cmd);
}

void MainWindow::on_pushButtonTuneSend_clicked() {
    QString subsys = ui->comboBoxTuneSubsystem->currentText();
    QString param = ui->comboBoxTuneParameter->currentText();
    QString valueStr = ui->lineEditTune->text();
    bool ok;
    double value = valueStr.toDouble(&ok);
    if (!ok) {
        QMessageBox::warning(this, "Error", "Invalid numeric value");
        return;
    }
    // Set flag to record this change in history when the response comes back
    m_recordHistory = true;
    QString cmd = QString("tune %1 %2 %3\r\n").arg(subsys, param, valueStr);
    sendCommand(cmd);
}

void MainWindow::on_pushButtonTuneUndo_clicked() {
    if (m_currentParamHistory.undoStack.isEmpty()) {
        QMessageBox::information(this, "Undo", "No previous value to undo");
        return;
    }
    double oldVal = m_currentParamHistory.undoStack.pop();
    // Send tuning command to revert value, but disable history recording for this action to avoid loops
    m_recordHistory = false;
    QString subsys = ui->comboBoxTuneSubsystem->currentText();
    QString param = ui->comboBoxTuneParameter->currentText();
    QString cmd = QString("tune %1 %2 %3\r\n").arg(subsys, param, QString::number(oldVal, 'f', 4));
    sendCommand(cmd);
    // Note: The slave will return the new value (i.e., oldVal) and its previous value (i.e., currentValue),
    // The response will again trigger parseTuneResponse, automatically updating the history stack and display.
}

void MainWindow::on_pushButtonTuneEnquire_clicked() {
    QString subsys = ui->comboBoxTuneSubsystem->currentText();
    QString param = ui->comboBoxTuneParameter->currentText();
    QString cmd = QString("tune %1 %2 ?\r\n").arg(subsys, param);
    m_recordHistory = false;
    sendCommand(cmd);
}

void MainWindow::on_pushButtonIncrement_clicked() {
    double step = m_stepValues[ui->incrementSlider->value()];
    QString subsys = ui->comboBoxTuneSubsystem->currentText();
    QString param = ui->comboBoxTuneParameter->currentText();
    // Set flag to record this change in history when the response comes back
    m_recordHistory = true;
    QString cmd = QString("increment %1 %2 %3\r\n").arg(subsys, param, QString::number(step, 'f', 6));
    sendCommand(cmd);
}

void MainWindow::on_pushButtonDecrement_clicked() {
    double step = -m_stepValues[ui->incrementSlider->value()];
    QString subsys = ui->comboBoxTuneSubsystem->currentText();
    QString param = ui->comboBoxTuneParameter->currentText();
    // Set flag to record this change in history when the response comes back
    m_recordHistory = true;
    QString cmd = QString("increment %1 %2 %3\r\n").arg(subsys, param, QString::number(step, 'f', 6));
    sendCommand(cmd);
}

void MainWindow::on_incrementSlider_valueChanged(int value) {
    if (value >= 0 && value < m_stepValues.size()) {
        double step = m_stepValues[value];
        ui->incrementLabel->setText(formatStepValue(step));
    }
}

void MainWindow::on_comboBoxTuneSubsystem_currentIndexChanged(int index) {
    Q_UNUSED(index);
    // Clear history stack when switching subsystem
    m_currentParamHistory.undoStack.clear();
    // Update parameter combo box based on selected subsystem
    QString subsys = ui->comboBoxTuneSubsystem->currentText();
    ui->comboBoxTuneParameter->clear();
    // Define parameters for each subsystem
    if (subsys == "speed") {
        ui->comboBoxTuneParameter->addItems({"p", "i"});
    } else if (subsys == "id") {
        ui->comboBoxTuneParameter->addItems({"p", "i"});
    } else if (subsys == "iq") {
        ui->comboBoxTuneParameter->addItems({"p", "i"});
    } else if (subsys == "fw") {
        ui->comboBoxTuneParameter->addItems({"p", "i"});
    } else if (subsys == "gain") {
        ui->comboBoxTuneParameter->addItems({"ia", "ib", "ic", "va", "vb", "ibatt", "vbatt"});
    } else if (subsys == "offset") {
        ui->comboBoxTuneParameter->addItems({"ia", "ib", "ic", "va", "vb", "ibatt", "vbatt"});
    }
    // Display last known value for the first parameter of the new subsystem, if available
    QString key = getCurrentParamKey();
    if (m_paramLastValue.contains(key)) {
        double lastVal = m_paramLastValue[key];
        m_currentParamHistory.currentValue = lastVal;
        ui->lineEditTune->setText(QString::number(lastVal, 'f', 6));
    } else {
        m_currentParamHistory.currentValue = 0.0;
        ui->lineEditTune->clear();
    }
}

void MainWindow::on_comboBoxTuneParameter_currentIndexChanged(int index) {
    Q_UNUSED(index);
    // Clear history stack when switching parameter
    m_currentParamHistory.undoStack.clear();
    QString key = getCurrentParamKey();
    if (m_paramLastValue.contains(key)) {
        double lastVal = m_paramLastValue[key];
        m_currentParamHistory.currentValue = lastVal;
        ui->lineEditTune->setText(QString::number(lastVal, 'f', 6));
    } else {
        m_currentParamHistory.currentValue = 0.0;
        ui->lineEditTune->clear();
    }
}

void MainWindow::on_pushButtonPause_clicked() {
    m_plotPaused = !m_plotPaused;
    ui->pushButtonPause->setText(m_plotPaused ? "▶️" : "⏸️");
    if (!m_plotPaused) {
        // 如果从暂停恢复，立即刷新一次
        updateAllPlots();
    }
}

void MainWindow::sendCurrentLineEditCommand() {
    QString sendStr = ui->lineEditSend->text();
    if (sendStr.isEmpty())
        return;

    // 存入历史（避免与上一条重复）
    if (m_sendHistory.isEmpty() || m_sendHistory.last() != sendStr) {
        m_sendHistory.append(sendStr);
        if (m_sendHistory.size() > 64)
            m_sendHistory.removeFirst();
    }
    m_historyIndex = m_sendHistory.size(); // 指向末尾之后

    // 发送数据（自动添加换行）
    QString cmd = sendStr;
    if (!cmd.endsWith("\r\n"))
        cmd += "\r\n";
    QByteArray data = cmd.toUtf8();
    QMetaObject::invokeMethod(m_serialManager, "sendData",
                              Qt::QueuedConnection,
                              Q_ARG(QByteArray, data));

    // 显示回显
    ui->plainTextEditReceive->appendPlainText(">> " + sendStr.trimmed());

    // 清空输入框
    ui->lineEditSend->clear();
}

void MainWindow::onFieldCheckStateChanged(QListWidgetItem *item) {
    if (m_syncingFromMask) return;
    if (!item) return;
    QString fieldName = item->text();
    bool checked = (item->checkState() == Qt::Checked);
    
    // 构造命令字符串
    QString cmdName = m_dataParser->getCommandNameForField(fieldName);
    QString cmd = checked ? QString("log add %1\r\n").arg(cmdName)
                          : QString("log rm %1\r\n").arg(cmdName);
    sendCommand(cmd);   // 复用已有的 sendCommand
}

void MainWindow::handleSerialPortOpened(bool success, const QString &errorMsg) {
    ui->pushButtonStartToggle->setEnabled(true);
    if (success) {
        updateUiForSerialState(true);
        syncFieldCheckStates();
        statusBar()->showMessage("Serial port opened", 3000);
    } else {
        updateUiForSerialState(false);
        QMessageBox::critical(this, "Error", "Failed to open serial port: " + errorMsg);
    }
}

void MainWindow::handleSerialPortClosed() {
    updateUiForSerialState(false);
    statusBar()->showMessage("Serial port closed", 3000);
}

void MainWindow::syncFieldCheckStates()
{
    for (int i = 0; i < m_fieldList->count(); ++i) {
        QListWidgetItem *item = m_fieldList->item(i);
        if (item->checkState() == Qt::Checked) {
            sendCommand(QString("log add %1\r\n").arg(m_dataParser->getCommandNameForField(item->text())));
        }
    }
}

void MainWindow::addTimeStamp(double offsetSec)
{
    m_timeStamps.append(offsetSec);
    if (m_timeStamps.size() > m_maxWavePoints) {
        m_timeStamps.remove(0, m_timeStamps.size() - m_maxWavePoints);
    }
}

void MainWindow::updateTargetSliderLimits() {
    bool isSpeed = (ui->comboBoxTargetSelection->currentText() == "Speed");
    if (isSpeed) {
        ui->targetSlider->setRange(-5000, 5000);
        ui->targetSlider->setSingleStep(10);
        ui->targetLabelPrefix->setText("Speed");
    } else {
        ui->targetSlider->setRange(-100, 100);   // -100 → -10.0, 100 → 10.0
        ui->targetSlider->setSingleStep(1);
        ui->targetLabelPrefix->setText("Torque");
    }
    // Adjust current value to avoid exceeding limits
    int current = ui->targetSlider->value();
    current = qBound(ui->targetSlider->minimum(), current, ui->targetSlider->maximum());
    ui->targetSlider->setValue(current);
}

double MainWindow::getCurrentTargetValue() const {
    if (m_targetManuallyEdited) {
        bool ok;
        double v = ui->lineEditTarget->text().toDouble(&ok);
        if (ok) return v;
    }
    bool isSpeed = (ui->comboBoxTargetSelection->currentText() == "Speed");
    if (isSpeed) {
        return ui->targetSlider->value();
    } else {
        return ui->targetSlider->value() / 10.0;
    }
}

void MainWindow::setTargetValue(double val, bool markAsEdited) {
    ui->targetSlider->blockSignals(true);

    bool isSpeed = (ui->comboBoxTargetSelection->currentText() == "Speed");
    if (isSpeed) {
        val = qBound(-5000.0, val, 5000.0);
        int sliderVal = static_cast<int>(qRound(val / 10.0) * 10);
        ui->targetSlider->setValue(sliderVal);
    } else {
        val = qBound(-10.0, val, 10.0);
        int sliderVal = static_cast<int>(qRound(val * 10.0));
        ui->targetSlider->setValue(sliderVal);
    }
    // Format display: remove trailing zeros, keep sufficient precision (up to 6 decimal places)
    QString formatted = QString::number(val, 'f', 6);
    formatted.remove(QRegularExpression("\\.?0+$"));
    if (formatted.isEmpty()) formatted = "0";
    ui->lineEditTarget->setText(formatted);

    m_targetManuallyEdited = markAsEdited;
    ui->targetSlider->blockSignals(false);
}

void MainWindow::parseTuneResponse(const QString &line) {
    // Format: "speed p set to 100.000000 (was 50.000000)"
    QRegularExpression setRegex(R"((\w+)\s+(\w+)\s+set to\s+([0-9.-]+)\s+\(was\s+([0-9.-]+)\))");
    QRegularExpressionMatch setMatch = setRegex.match(line);
    if (setMatch.hasMatch()) {
        QString subsys = setMatch.captured(1);
        QString param = setMatch.captured(2);
        double newVal = setMatch.captured(3).toDouble();
        double oldVal = setMatch.captured(4).toDouble();

        QString key = subsys + ":" + param;
        // Update history stack: push old value onto stack (max 32)
        m_paramLastValue[key] = newVal;

        // If the current UI selection is this parameter, update lineEditTune to show the new value
        if (getCurrentParamKey() == key) {
            // Push to stack depending on the flag (undo command does not push)
            if (m_recordHistory) {
                if (m_currentParamHistory.undoStack.size() >= 32)
                    m_currentParamHistory.undoStack.pop_front();
                m_currentParamHistory.undoStack.push(oldVal);
            }
            m_currentParamHistory.currentValue = newVal;
            ui->lineEditTune->setText(QString::number(newVal, 'f', 6));
        }
        return;
    }

    // Format for enquire response: "speed p is 100.000000"
    QRegularExpression queryRegex(R"((\w+)\s+(\w+)\s+is\s+([0-9.-]+))");
    QRegularExpressionMatch queryMatch = queryRegex.match(line);
    if (queryMatch.hasMatch()) {
        QString subsys = queryMatch.captured(1);
        QString param = queryMatch.captured(2);
        double value = queryMatch.captured(3).toDouble();

        QString key = subsys + ":" + param;
        m_paramLastValue[key] = value;

        if (getCurrentParamKey() == key) {
            // Enquire command does not affect history stack
            m_currentParamHistory.currentValue = value;
            ui->lineEditTune->setText(QString::number(value, 'f', 6));
        }
        return;
    }
}

QString MainWindow::getCurrentParamKey() const {
    return ui->comboBoxTuneSubsystem->currentText() + ":" + ui->comboBoxTuneParameter->currentText();
}

QString MainWindow::formatStepValue(double step) const
{
    double absStep = std::fabs(step);
    if (absStep >= 1.0) {
        return QString::number(step, 'f', 0);        // 0 decimal places for integer steps
    } else if (absStep >= 0.1) {
        return QString::number(step, 'f', 1);        // 1 decimal place for steps between 0.1 and 1
    } else if (absStep >= 0.01) {
        return QString::number(step, 'f', 2);        // 2 decimal places for steps between 0.01 and 0.1
    } else if (absStep >= 0.001) {
        return QString::number(step, 'f', 3);        // 3 decimal places for steps between 0.001 and 0.01
    } else if (absStep >= 0.0001) {
        return QString::number(step, 'f', 4);        // 4 decimal places for steps between 0.0001 and 0.001
    } else {
        return QString::number(step, 'f', 5);        // 5 decimal places for steps between 0.00001 and 0.0001
    }
}