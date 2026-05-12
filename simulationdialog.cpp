#include "simulationdialog.h"
#include "simulationoptionsdialog.h"
#include "ui_simulationdialog.h"
#include "qcustomplot.h"

#include <QAbstractSocket>
#include <QCloseEvent>
#include <QDateTime>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QIntValidator>
#include <QJsonDocument>
#include <QJsonObject>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QNetworkInterface>
#include <QPalette>
#include <QShowEvent>
#include <QTimer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUdpSocket>
#include <QVBoxLayout>
#include <limits>

namespace {
QElapsedTimer &protocolClock()
{
    static QElapsedTimer timer;
    static const bool started = []() {
        timer.start();
        return true;
    }();
    Q_UNUSED(started);
    return timer;
}
}

simulationDialog::simulationDialog(QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::simulationDialog)
    , m_tcpServer(new QTcpServer(this))
    , m_discoverySocket(new QUdpSocket(this))
    , m_discoveryTimer(new QTimer(this))
    , m_syncPingTimer(new QTimer(this))
    , m_simulationTimer(new QTimer(this))
    , m_readyDeadlineTimer(new QTimer(this))
    , m_clientWatchdogTimer(new QTimer(this))
    , m_scopePlot(nullptr)
    , m_scopeStepLine(nullptr)
    , m_clientSocket(nullptr)
    , m_clientServerClockOffsetMs(0)
    , m_pendingFireDueServerMs(0)
    , m_nextProtocolId(1)
    , m_currentSimulationRow(0)
    , m_pendingSimulationId(-1)
    , m_simulationRunning(false)
    , m_simulationPaused(false)
    , m_simulationStartedWithClientConnection(false)
    , m_syncStopResetArmed(false)
    , m_clientSimulationActive(false)
    , m_scopeSliderTracksMax(true)
    , m_scopePlotDirtyWhileHidden(false)
    , m_simulationAborted(false)
    , m_clientExpectedLineCount(0)
{
    ui->setupUi(this);
    initializeConnectionUi();
    initializeSimulationUi();

    connect(m_tcpServer, &QTcpServer::newConnection,
            this, &simulationDialog::handleNewServerConnection);
    connect(m_discoverySocket, &QUdpSocket::readyRead,
            this, &simulationDialog::handleDiscoveryReadyRead);
    connect(m_discoveryTimer, &QTimer::timeout,
            this, &simulationDialog::sendDiscoveryProbe);
    connect(m_syncPingTimer, &QTimer::timeout,
            this, &simulationDialog::sendSyncPingToClients);
    connect(m_simulationTimer, &QTimer::timeout,
            this, &simulationDialog::runNextSimulationLine);
    m_readyDeadlineTimer->setSingleShot(true);
    connect(m_readyDeadlineTimer, &QTimer::timeout,
            this, &simulationDialog::handleReadyDeadlineExpired);
    m_clientWatchdogTimer->setSingleShot(true);
    connect(m_clientWatchdogTimer, &QTimer::timeout,
            this, &simulationDialog::handleClientWatchdogExpired);
    connect(ui->listWidgetClientFoundServer, &QListWidget::itemClicked,
            this, &simulationDialog::handleDiscoveredServerSelected);
    connect(ui->listWidgetClientFoundServer, &QListWidget::itemDoubleClicked,
            this, &simulationDialog::handleDiscoveredServerActivated);

    m_discoveryTimer->start(1000);
    m_syncPingTimer->start(1000);
    bindDiscoverySocketForClient();
}

simulationDialog::~simulationDialog()
{
    m_discoveryTimer->stop();
    m_syncPingTimer->stop();
    stopServer();
    disconnectClient();
    delete ui;
}

void simulationDialog::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange) {
        applyScopeTheme();
    }
    QDialog::changeEvent(event);
}

void simulationDialog::closeEvent(QCloseEvent *event)
{
    event->ignore();
    hide();
}

void simulationDialog::showEvent(QShowEvent *event)
{
    QDialog::showEvent(event);
    if (m_scopePlotDirtyWhileHidden) {
        m_scopePlotDirtyWhileHidden = false;
        updateScopePlot();
    }
}

void simulationDialog::initializeConnectionUi()
{
    m_lanAddress = firstLanIpv4Address();

    ui->radioButtonLocalhost->setText("Local only - 127.0.0.1");
    if (m_lanAddress.isEmpty()) {
        ui->radioButtonLan->setText("LAN - unavailable");
        ui->radioButtonLan->setEnabled(false);
    } else {
        ui->radioButtonLan->setText(QString("LAN - %1").arg(m_lanAddress));
    }
    ui->radioButtonAllconnection->setText("All interfaces - 0.0.0.0");

    if (ui->lineEditPort->text().isEmpty()) {
        ui->lineEditPort->setText("45454");
    }
    if (ui->lineEditClientPort->text().isEmpty()) {
        ui->lineEditClientPort->setText("45454");
    }
    if (ui->lineEditServerIp->text().isEmpty()) {
        ui->lineEditServerIp->setText("127.0.0.1");
    }

    auto *portValidator = new QIntValidator(1, 65535, this);
    ui->lineEditPort->setValidator(portValidator);
    ui->lineEditClientPort->setValidator(portValidator);
    ui->plainTextEditMessages->setReadOnly(true);
    ui->plainTextEditMessages->setMaximumBlockCount(1000);

    updateStatusIndicators();
    updateControlStates();
    emitMainStatus();
}

void simulationDialog::initializeSimulationUi()
{
    m_scopePlot = new QCustomPlot(ui->scopeWidget);
    auto *scopeLayout = new QVBoxLayout(ui->scopeWidget);
    scopeLayout->setContentsMargins(0, 0, 0, 0);
    scopeLayout->addWidget(m_scopePlot);

    m_scopePlot->addGraph();
    m_scopePlot->graph(0)->setName("Server");
    m_scopePlot->graph(0)->setPen(QPen(QColor("#1976d2"), 2));
    m_scopePlot->graph(0)->setValueAxis(m_scopePlot->yAxis);
    m_scopePlot->addGraph(m_scopePlot->xAxis, m_scopePlot->yAxis2);
    m_scopePlot->graph(1)->setName("Client");
    m_scopePlot->graph(1)->setPen(QPen(QColor("#d32f2f"), 2));
    m_scopePlot->graph(1)->setValueAxis(m_scopePlot->yAxis2);
    m_scopeStepLine = new QCPItemLine(m_scopePlot);
    m_scopeStepLine->setPen(QPen(QColor("#f57c00"), 2));
    m_scopeStepLine->setSelectable(false);
    m_scopeStepLine->setVisible(false);
    m_scopePlot->legend->setVisible(true);
    m_scopePlot->xAxis->setLabel("CSV row");
    m_scopePlot->yAxis->setLabel("Server");
    m_scopePlot->yAxis2->setLabel("Client");
    m_scopePlot->yAxis2->setVisible(true);
    m_scopePlot->legend->setFont(QFont("Arial", 7));
    applyScopeTheme();

    ui->progressBarSimulation->setMinimum(0);
    ui->progressBarSimulation->setValue(0);
    ui->horizontalSliderScope->setRange(1, 1000);
    ui->horizontalSliderScope->setValue(100);
    ui->horizontalScrollBarScope->setRange(0, 0);
    ui->horizontalScrollBarScope->setValue(0);

    m_simulationTimer->setInterval(100);
    updateSimulationControls();
    updateSimulationProgress();
    updateScopeControls();
}

void simulationDialog::applyScopeTheme()
{
    if (!m_scopePlot) {
        return;
    }

    const QPalette pal = palette();
    const QColor windowColor = pal.color(QPalette::Window);
    const QColor textColor = pal.color(QPalette::WindowText);
    const QColor midColor = pal.color(QPalette::Mid);
    const QColor gridColor = midColor;
    const QColor subGridColor = midColor.lighter(115);

    m_scopePlot->setBackground(windowColor);
    m_scopePlot->axisRect()->setBackground(windowColor);
    m_scopePlot->legend->setBrush(QBrush(windowColor));
    m_scopePlot->legend->setBorderPen(QPen(midColor));
    m_scopePlot->legend->setTextColor(textColor);

    for (QCPAxis *axis : {m_scopePlot->xAxis, m_scopePlot->yAxis, m_scopePlot->yAxis2}) {
        axis->setBasePen(QPen(textColor));
        axis->setTickPen(QPen(textColor));
        axis->setSubTickPen(QPen(textColor));
        axis->setTickLabelColor(textColor);
        axis->setLabelColor(textColor);
        axis->grid()->setPen(QPen(gridColor, 0, Qt::DotLine));
        axis->grid()->setSubGridPen(QPen(subGridColor, 0, Qt::DotLine));
        axis->grid()->setZeroLinePen(QPen(textColor, 0));
    }

    m_scopePlot->replot(QCustomPlot::rpQueuedReplot);
}

qint64 simulationDialog::monotonicNowMs() const
{
    return protocolClock().elapsed();
}

QStringList simulationDialog::parseCsvLine(const QString &line) const
{
    if (!line.contains(',') && line.contains('\t')) {
        QStringList tabFields;
        for (const QString &field : line.split('\t')) {
            tabFields << field.trimmed();
        }
        return tabFields;
    }

    QStringList fields;
    QString current;
    bool inQuotes = false;

    for (int i = 0; i < line.size(); ++i) {
        const QChar ch = line.at(i);
        if (ch == '"') {
            if (inQuotes && i + 1 < line.size() && line.at(i + 1) == '"') {
                current.append('"');
                ++i;
            } else {
                inQuotes = !inQuotes;
            }
        } else if (ch == ',' && !inQuotes) {
            fields << current.trimmed();
            current.clear();
        } else {
            current.append(ch);
        }
    }

    fields << current.trimmed();
    return fields;
}

bool simulationDialog::loadCsvFile(const QString &filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QMessageBox::warning(this, "CSV Error", QString("Failed to open CSV file: %1").arg(file.errorString()));
        return false;
    }

    QVector<QStringList> rows;
    while (!file.atEnd()) {
        const QString line = QString::fromUtf8(file.readLine()).trimmed();
        if (!line.isEmpty()) {
            rows.append(parseCsvLine(line));
        }
    }

    if (rows.isEmpty()) {
        QMessageBox::warning(this, "CSV Error", "The selected CSV file is empty.");
        return false;
    }

    bool firstRowLooksNumeric = true;
    for (const QString &field : rows.first()) {
        bool ok = false;
        field.toDouble(&ok);
        if (!ok) {
            firstRowLooksNumeric = false;
            break;
        }
    }

    m_csvHeaders.clear();
    m_csvTextRows.clear();
    if (firstRowLooksNumeric) {
        for (int i = 0; i < rows.first().size(); ++i) {
            m_csvHeaders << QString("Column %1").arg(i + 1);
        }
        m_csvTextRows = rows;
    } else {
        m_csvHeaders = rows.takeFirst();
        m_csvTextRows = rows;
    }

    m_csvNumericRows.clear();
    for (const QStringList &row : std::as_const(m_csvTextRows)) {
        QVector<double> numericRow;
        numericRow.reserve(m_csvHeaders.size());
        for (int i = 0; i < m_csvHeaders.size(); ++i) {
            bool ok = false;
            const double value = i < row.size() ? row.at(i).toDouble(&ok) : 0.0;
            numericRow.append(ok ? value : 0.0);
        }
        m_csvNumericRows.append(numericRow);
    }

    m_csvPath = filePath;
    m_currentSimulationRow = 0;
    m_pendingSimulationId = -1;
    m_pendingReadyClients.clear();

    int timeColumn = -1;
    int speedColumn = -1;
    int torqueColumn = -1;
    for (int i = 0; i < m_csvHeaders.size(); ++i) {
        const QString header = m_csvHeaders.at(i).toLower();
        if (timeColumn < 0 && header.contains("time")) {
            timeColumn = i;
        }
        if (speedColumn < 0 && header.contains("speed")) {
            speedColumn = i;
        }
        if (torqueColumn < 0 && header.contains("torque")) {
            torqueColumn = i;
        }
    }

    if (speedColumn >= 0) {
        m_mapping.serverValueColumn = speedColumn;
        m_mapping.serverTargetType = "speed";
    } else if (m_mapping.serverValueColumn < 0 && !m_csvHeaders.isEmpty()) {
        m_mapping.serverValueColumn = 0;
    }

    if (torqueColumn >= 0) {
        m_mapping.clientValueColumn = torqueColumn;
        m_mapping.clientTargetType = "torque";
    } else if (m_mapping.clientValueColumn < 0 && m_csvHeaders.size() > 1) {
        m_mapping.clientValueColumn = 1;
    } else if (m_mapping.clientValueColumn < 0 && !m_csvHeaders.isEmpty()) {
        m_mapping.clientValueColumn = 0;
    }

    if (timeColumn >= 0) {
        m_mapping.timeColumn = timeColumn;
    } else if (m_mapping.timeColumn < 0 && !m_csvHeaders.isEmpty()) {
        m_mapping.timeColumn = 0;
    }
    m_mapping.startRow = qBound(0, m_mapping.startRow, qMax(0, m_csvNumericRows.size() - 1));
    m_currentSimulationRow = m_mapping.startRow;

    ui->progressBarSimulation->setMaximum(m_csvNumericRows.size());
    appendTcpMessage(QString("Loaded CSV: %1 (%2 rows)").arg(filePath).arg(m_csvNumericRows.size()));
    updateSimulationProgress();
    updateScopeControls();
    m_scopeSliderTracksMax = true;
    ui->horizontalSliderScope->setValue(ui->horizontalSliderScope->maximum());
    updateScopePlot();
    updateSimulationControls();
    return true;
}

QStringList simulationDialog::columnNames() const
{
    QStringList names;
    for (int i = 0; i < m_csvHeaders.size(); ++i) {
        names << QString("%1: %2").arg(i + 1).arg(m_csvHeaders.at(i));
    }
    return names;
}

double simulationDialog::numericValue(int row, int column) const
{
    if (row < 0 || row >= m_csvNumericRows.size() ||
        column < 0 || column >= m_csvNumericRows.at(row).size()) {
        return 0.0;
    }

    return m_csvNumericRows.at(row).at(column);
}

bool simulationDialog::mappingIsValid() const
{
    return !m_csvNumericRows.isEmpty() &&
           m_mapping.serverValueColumn >= 0 &&
           m_mapping.clientValueColumn >= 0 &&
           m_mapping.timeColumn >= 0 &&
           m_mapping.serverValueColumn < m_csvHeaders.size() &&
           m_mapping.clientValueColumn < m_csvHeaders.size() &&
           m_mapping.timeColumn < m_csvHeaders.size() &&
           m_mapping.startRow >= 0 &&
           m_mapping.startRow < m_csvNumericRows.size();
}

double simulationDialog::timeValueSeconds(int row) const
{
    double value = numericValue(row, m_mapping.timeColumn);
    if (m_mapping.timeUnit == "ms") {
        value /= 1000.0;
    } else if (m_mapping.timeUnit == "us") {
        value /= 1000000.0;
    }
    return value;
}

int simulationDialog::nextSimulationDelayMs(int completedRow) const
{
    const int nextRow = completedRow + 1;
    if (nextRow >= m_csvNumericRows.size() || !mappingIsValid()) {
        return 0;
    }

    const double currentTime = timeValueSeconds(completedRow);
    const double nextTime = timeValueSeconds(nextRow);
    return qMax(1, static_cast<int>(qRound((nextTime - currentTime) * 1000.0)));
}

bool simulationDialog::showSimulationOptionsDialog()
{
    if (m_csvHeaders.isEmpty()) {
        QMessageBox::information(this, "Simulation Options", "Load a CSV file before selecting simulation mappings.");
        return false;
    }

    SimulationOptionsDialog dialog(this);
    dialog.setCsvPreview(m_csvHeaders, m_csvTextRows);
    dialog.setMapping(m_mapping.serverTargetType,
                      m_mapping.serverValueColumn,
                      m_mapping.clientTargetType,
                      m_mapping.clientValueColumn,
                      m_mapping.timeColumn,
                      m_mapping.timeUnit,
                      m_mapping.startRow);

    if (dialog.exec() != QDialog::Accepted) {
        return false;
    }

    m_mapping.serverTargetType = dialog.serverTargetType();
    m_mapping.serverValueColumn = dialog.serverValueColumn();
    m_mapping.clientTargetType = dialog.clientTargetType();
    m_mapping.clientValueColumn = dialog.clientValueColumn();
    m_mapping.timeColumn = dialog.timeColumn();
    m_mapping.timeUnit = dialog.timeUnit();
    m_mapping.startRow = qBound(0, dialog.startRow(), qMax(0, m_csvNumericRows.size() - 1));
    if (!m_simulationRunning) {
        m_currentSimulationRow = m_mapping.startRow;
    }

    appendTcpMessage(QString("Mapping: server %1 <- %2, client %3 <- %4, time <- %5 %6, start row %7")
                         .arg(m_mapping.serverTargetType,
                              m_csvHeaders.value(m_mapping.serverValueColumn),
                              m_mapping.clientTargetType,
                              m_csvHeaders.value(m_mapping.clientValueColumn),
                              m_csvHeaders.value(m_mapping.timeColumn),
                              m_mapping.timeUnit)
                         .arg(m_mapping.startRow + 1));
    updateSimulationProgress();
    updateScopeControls();
    updateScopePlot();
    return true;
}

void simulationDialog::on_pushButtonOpenFile_clicked()
{
    const QString filePath = QFileDialog::getOpenFileName(this,
                                                          "Open Simulation CSV",
                                                          QString(),
                                                          "CSV files (*.csv);;All files (*.*)");
    if (!filePath.isEmpty()) {
        loadCsvFile(filePath);
    }
}

void simulationDialog::on_pushButtonSimOption_clicked()
{
    showSimulationOptionsDialog();
}

void simulationDialog::on_pushButtonStartPause_clicked()
{
    if (m_simulationRunning && !m_simulationPaused) {
        m_simulationPaused = true;
        m_simulationTimer->stop();
        appendTcpMessage("Simulation paused");
        updateSimulationControls();
        return;
    }

    if (!mappingIsValid()) {
        if (!showSimulationOptionsDialog()) {
            return;
        }
    }

    if (!m_tcpServer->isListening()) {
        QMessageBox::warning(this, "Simulation", "Start this instance as the TCP server before running simulation playback.");
        return;
    }

    m_simulationRunning = true;
    m_simulationPaused = false;
    m_simulationStartedWithClientConnection = firstConnectedServerClient() != nullptr;
    m_syncStopResetArmed = false;
    m_simulationAborted = false;
    appendTcpMessage("Simulation started");
    updateSimulationControls();
    runNextSimulationLine();
}

void simulationDialog::on_pushButtonSyncStop_clicked()
{
    if (m_syncStopResetArmed && !m_simulationRunning && !m_simulationPaused) {
        resetSimulationToStart();
        appendTcpMessage("Simulation reset to starting row");
        updateSimulationControls();
        return;
    }

    appendTcpMessage("Simulation stop: sending local stop and TCP abort");
    abortSimulation("simulation_stop", true);
    m_syncStopResetArmed = true;
    updateSimulationControls();
}

void simulationDialog::on_horizontalSliderScope_valueChanged(int)
{
    const int previousStart = ui->horizontalScrollBarScope->value();
    const int previousPageStep = qMax(1, ui->horizontalScrollBarScope->pageStep());
    const int previousCenter = previousStart + previousPageStep / 2;

    m_scopeSliderTracksMax = ui->horizontalSliderScope->value() == ui->horizontalSliderScope->maximum();
    updateScopeControls();

    const int newPageStep = qMax(1, ui->horizontalScrollBarScope->pageStep());
    const int newStart = qBound(ui->horizontalScrollBarScope->minimum(),
                                previousCenter - newPageStep / 2,
                                ui->horizontalScrollBarScope->maximum());
    ui->horizontalScrollBarScope->setValue(newStart);
    updateScopePlot();
}

void simulationDialog::on_horizontalScrollBarScope_valueChanged(int)
{
    updateScopePlot();
}

void simulationDialog::bindDiscoverySocketForClient()
{
    m_discoverySocket->close();
    m_discoverySocket->bind(QHostAddress::AnyIPv4,
                            0,
                            QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint);
}

void simulationDialog::bindDiscoverySocketForServer(quint16 port)
{
    m_discoverySocket->close();
    m_discoverySocket->bind(QHostAddress::AnyIPv4,
                            port,
                            QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint);
}

QString simulationDialog::firstLanIpv4Address() const
{
    const QList<QNetworkInterface> interfaces = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface &networkInterface : interfaces) {
        const bool usableInterface = networkInterface.flags().testFlag(QNetworkInterface::IsUp) &&
                                     networkInterface.flags().testFlag(QNetworkInterface::IsRunning) &&
                                     !networkInterface.flags().testFlag(QNetworkInterface::IsLoopBack);
        if (!usableInterface) {
            continue;
        }

        const QList<QNetworkAddressEntry> entries = networkInterface.addressEntries();
        for (const QNetworkAddressEntry &entry : entries) {
            const QHostAddress address = entry.ip();
            if (address.protocol() == QAbstractSocket::IPv4Protocol && !address.isLoopback()) {
                return address.toString();
            }
        }
    }

    return QString();
}

QList<QHostAddress> simulationDialog::discoveryProbeAddresses() const
{
    QList<QHostAddress> addresses;
    addresses << QHostAddress::LocalHost;
    addresses << QHostAddress::Broadcast;

    const QList<QNetworkInterface> interfaces = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface &networkInterface : interfaces) {
        const bool usableInterface = networkInterface.flags().testFlag(QNetworkInterface::IsUp) &&
                                     networkInterface.flags().testFlag(QNetworkInterface::IsRunning);
        if (!usableInterface) {
            continue;
        }

        const QList<QNetworkAddressEntry> entries = networkInterface.addressEntries();
        for (const QNetworkAddressEntry &entry : entries) {
            const QHostAddress broadcast = entry.broadcast();
            if (!broadcast.isNull() &&
                broadcast.protocol() == QAbstractSocket::IPv4Protocol &&
                !addresses.contains(broadcast)) {
                addresses << broadcast;
            }
        }
    }

    return addresses;
}

QHostAddress simulationDialog::selectedListenAddress() const
{
    if (ui->radioButtonLocalhost->isChecked()) {
        return QHostAddress::LocalHost;
    }

    if (ui->radioButtonLan->isChecked() && !m_lanAddress.isEmpty()) {
        return QHostAddress(m_lanAddress);
    }

    return QHostAddress::AnyIPv4;
}

quint16 simulationDialog::readPortLineEdit(const QString &text, bool *ok) const
{
    const int port = text.toInt(ok);
    if (!*ok || port < 1 || port > 65535) {
        *ok = false;
        return 0;
    }

    return static_cast<quint16>(port);
}

QString simulationDialog::socketDescription(const QTcpSocket *socket) const
{
    if (!socket) {
        return QString();
    }

    return QString("%1:%2").arg(socket->peerAddress().toString()).arg(socket->peerPort());
}

void simulationDialog::on_pushButtonServerStart_clicked()
{
    bool portOk = false;
    const quint16 port = readPortLineEdit(ui->lineEditPort->text(), &portOk);
    if (!portOk) {
        QMessageBox::warning(this, "Invalid Port", "Enter a TCP port between 1 and 65535.");
        return;
    }

    disconnectClient();
    stopServer();

    const QHostAddress listenAddress = selectedListenAddress();
    if (!m_tcpServer->listen(listenAddress, port)) {
        QMessageBox::critical(this,
                              "Server Error",
                              QString("Failed to start TCP server: %1").arg(m_tcpServer->errorString()));
        appendTcpMessage(QString("Server failed to start: %1").arg(m_tcpServer->errorString()));
    } else {
        bindDiscoverySocketForServer(port);
        appendTcpMessage(QString("Server listening on %1:%2")
                             .arg(listenAddress.toString())
                             .arg(port));
    }

    updateStatusIndicators();
    updateControlStates();
}

void simulationDialog::on_pushButtonServerStop_clicked()
{
    appendTcpMessage("Server stopped");
    stopServer();
    updateStatusIndicators();
    updateControlStates();
}

void simulationDialog::on_pushButtonClientConnect_clicked()
{
    bool portOk = false;
    const quint16 port = readPortLineEdit(ui->lineEditClientPort->text(), &portOk);
    if (!portOk) {
        QMessageBox::warning(this, "Invalid Port", "Enter a TCP port between 1 and 65535.");
        return;
    }

    const QString serverIp = ui->lineEditServerIp->text().trimmed();
    if (serverIp.isEmpty()) {
        QMessageBox::warning(this, "Missing Server IP", "Enter the TCP server IP address.");
        return;
    }

    stopServer();
    disconnectClient();

    m_clientSocket = new QTcpSocket(this);
    connect(m_clientSocket, &QTcpSocket::connected,
            this, &simulationDialog::handleClientConnected);
    connect(m_clientSocket, &QTcpSocket::readyRead,
            this, &simulationDialog::handleClientReadyRead);
    connect(m_clientSocket, &QTcpSocket::disconnected,
            this, &simulationDialog::handleClientDisconnected);
    connect(m_clientSocket, &QAbstractSocket::errorOccurred,
            this, &simulationDialog::handleClientError);

    appendTcpMessage(QString("Connecting to %1:%2").arg(serverIp).arg(port));
    m_clientSocket->connectToHost(serverIp, port);

    updateStatusIndicators();
    updateControlStates();
}

void simulationDialog::on_pushButtonClientDisconnect_clicked()
{
    appendTcpMessage("Client disconnected");
    disconnectClient();
    updateStatusIndicators();
    updateControlStates();
}

void simulationDialog::handleNewServerConnection()
{
    while (m_tcpServer->hasPendingConnections()) {
        QTcpSocket *socket = m_tcpServer->nextPendingConnection();
        if (!socket) {
            continue;
        }

        m_serverClients.append(socket);
        appendTcpMessage(QString("Client connected: %1").arg(socketDescription(socket)));
        connect(socket, &QTcpSocket::readyRead,
                this, &simulationDialog::handleServerClientReadyRead);
        connect(socket, &QTcpSocket::disconnected,
                this, &simulationDialog::handleServerClientDisconnected);
        connect(socket, &QAbstractSocket::errorOccurred, this, [this]() {
            rebuildConnectedClientsList();
            updateStatusIndicators();
            updateControlStates();
        });

        QJsonObject payload;
        payload["role"] = "server";
        payload["listenAddress"] = m_tcpServer->serverAddress().toString();
        payload["listenPort"] = static_cast<int>(m_tcpServer->serverPort());
        sendProtocolMessage(socket, "hello", payload);
        sendSyncPing(socket);
    }

    rebuildConnectedClientsList();
    updateStatusIndicators();
    updateControlStates();
}

void simulationDialog::handleServerClientReadyRead()
{
    auto *socket = qobject_cast<QTcpSocket*>(sender());
    processReceivedData(socket, true);
}

void simulationDialog::handleServerClientDisconnected()
{
    auto *socket = qobject_cast<QTcpSocket*>(sender());
    const bool lostSimulationClient = m_simulationRunning &&
                                      m_simulationStartedWithClientConnection &&
                                      socket &&
                                      m_serverClients.contains(socket);
    if (socket) {
        appendTcpMessage(QString("Client disconnected: %1").arg(socketDescription(socket)));
        m_serverClients.removeAll(socket);
        m_receiveBuffers.remove(socket);
        m_pendingReadyClients.remove(socket);
        socket->deleteLater();
    }

    if (lostSimulationClient) {
        abortSimulation("client_connection_lost", true);
    }

    rebuildConnectedClientsList();
    updateStatusIndicators();
    updateControlStates();
}

void simulationDialog::handleClientConnected()
{
    appendTcpMessage(QString("Connected to server: %1").arg(socketDescription(m_clientSocket)));
    QJsonObject payload;
    payload["role"] = "client";
    sendProtocolMessage(m_clientSocket, "hello", payload);

    updateClientServerList();
    updateStatusIndicators();
    updateControlStates();
}

void simulationDialog::handleClientReadyRead()
{
    processReceivedData(m_clientSocket, false);
}

void simulationDialog::handleClientDisconnected()
{
    auto *socket = qobject_cast<QTcpSocket*>(sender());
    if (!socket) {
        socket = m_clientSocket;
    }

    if (socket) {
        appendTcpMessage(QString("Disconnected from server: %1").arg(socketDescription(socket)));
        m_receiveBuffers.remove(socket);
        if (socket == m_clientSocket) {
            m_clientSocket = nullptr;
        }
        socket->deleteLater();
    } else {
        m_clientSocket = nullptr;
    }

    if (m_clientSimulationActive) {
        abortSimulation("server_connection_lost", false);
    }

    updateClientServerList();
    updateStatusIndicators();
    updateControlStates();
}

void simulationDialog::handleClientError()
{
    auto *socket = qobject_cast<QTcpSocket*>(sender());
    if (socket) {
        appendTcpMessage(QString("Client socket error: %1").arg(socket->errorString()));
    }
    if (socket && socket == m_clientSocket && socket->state() == QAbstractSocket::UnconnectedState) {
        m_receiveBuffers.remove(socket);
        disconnect(socket, nullptr, this, nullptr);
        m_clientSocket = nullptr;
        socket->deleteLater();
    }

    updateClientServerList();
    updateStatusIndicators();
    updateControlStates();
}

void simulationDialog::sendDiscoveryProbe()
{
    pruneDiscoveredServers();

    if (m_tcpServer->isListening()) {
        return;
    }

    bool portOk = false;
    const quint16 port = readPortLineEdit(ui->lineEditClientPort->text(), &portOk);
    if (!portOk || !m_discoverySocket->isValid()) {
        return;
    }

    QJsonObject message;
    message["protocol"] = "motor-drive-sync";
    message["version"] = 1;
    message["type"] = "discover";

    const QByteArray payload = QJsonDocument(message).toJson(QJsonDocument::Compact) + "\n";
    const QList<QHostAddress> addresses = discoveryProbeAddresses();
    for (const QHostAddress &address : addresses) {
        m_discoverySocket->writeDatagram(payload, address, port);
    }
}

void simulationDialog::handleDiscoveryReadyRead()
{
    while (m_discoverySocket->hasPendingDatagrams()) {
        QByteArray datagram;
        datagram.resize(static_cast<int>(m_discoverySocket->pendingDatagramSize()));

        QHostAddress senderAddress;
        quint16 senderPort = 0;
        m_discoverySocket->readDatagram(datagram.data(),
                                        datagram.size(),
                                        &senderAddress,
                                        &senderPort);

        const QJsonDocument document = QJsonDocument::fromJson(datagram.trimmed());
        if (!document.isObject()) {
            continue;
        }

        const QJsonObject message = document.object();
        if (message.value("protocol").toString() != "motor-drive-sync") {
            continue;
        }

        const QString type = message.value("type").toString();
        if (type == "discover" && m_tcpServer->isListening()) {
            replyToDiscoveryRequest(senderAddress, senderPort);
            continue;
        }

        if (type == "discover_response" && !m_tcpServer->isListening()) {
            const QString host = message.value("host").toString(senderAddress.toString());
            const int port = message.value("port").toInt(static_cast<int>(senderPort));
            const QString label = message.value("label").toString(QString("%1:%2").arg(host).arg(port));
            if (port >= 1 && port <= 65535) {
                addDiscoveredServer(host, static_cast<quint16>(port), label);
            }
        }
    }
}

void simulationDialog::handleDiscoveredServerSelected(QListWidgetItem *item)
{
    if (!item) {
        return;
    }

    const QString host = item->data(Qt::UserRole).toString();
    const int port = item->data(Qt::UserRole + 1).toInt();
    if (!host.isEmpty() && port >= 1 && port <= 65535) {
        ui->lineEditServerIp->setText(host);
        ui->lineEditClientPort->setText(QString::number(port));
    }
}

void simulationDialog::handleDiscoveredServerActivated(QListWidgetItem *item)
{
    handleDiscoveredServerSelected(item);
    if (item) {
        on_pushButtonClientConnect_clicked();
    }
}

void simulationDialog::sendProtocolMessage(QTcpSocket *socket,
                                           const QString &type,
                                           const QJsonObject &payload)
{
    if (!socket || socket->state() != QAbstractSocket::ConnectedState) {
        return;
    }

    QJsonObject message = payload;
    message["type"] = type;
    message["protocol"] = "motor-drive-sync";
    message["version"] = 1;
    message["timestampMs"] = QString::number(QDateTime::currentMSecsSinceEpoch());

    socket->write(QJsonDocument(message).toJson(QJsonDocument::Compact));
    socket->write("\n");

    appendProtocolMessage("TX", type, message);
}

void simulationDialog::sendSyncPing(QTcpSocket *socket)
{
    if (!socket || socket->state() != QAbstractSocket::ConnectedState) {
        return;
    }

    const int id = m_nextProtocolId++;
    const qint64 serverSendMs = monotonicNowMs();
    m_syncPingServerSendMs[socket][id] = serverSendMs;

    QJsonObject payload;
    payload["id"] = id;
    payload["serverSendMs"] = QString::number(serverSendMs);
    sendProtocolMessage(socket, "sync_ping", payload);
}

void simulationDialog::processReceivedData(QTcpSocket *socket, bool socketIsServerClient)
{
    if (!socket) {
        return;
    }

    QByteArray &buffer = m_receiveBuffers[socket];
    buffer.append(socket->readAll());

    int newlineIndex = buffer.indexOf('\n');
    while (newlineIndex >= 0) {
        const QByteArray line = buffer.left(newlineIndex).trimmed();
        buffer.remove(0, newlineIndex + 1);
        if (!line.isEmpty()) {
            handleProtocolLine(socket, line, socketIsServerClient);
        }
        newlineIndex = buffer.indexOf('\n');
    }
}

void simulationDialog::handleProtocolLine(QTcpSocket *socket,
                                          const QByteArray &line,
                                          bool socketIsServerClient)
{
    const QJsonDocument document = QJsonDocument::fromJson(line);
    if (!document.isObject()) {
        return;
    }

    const QJsonObject message = document.object();
    if (message.value("protocol").toString() != "motor-drive-sync") {
        return;
    }

    const QString type = message.value("type").toString();
    appendProtocolMessage("RX", type, message);

    if (type == "hello") {
        socket->setProperty("syncRole", message.value("role").toString());
        if (socketIsServerClient) {
            rebuildConnectedClientsList();
        } else {
            updateClientServerList();
        }
    } else if (type == "sync_ping") {
        handleSyncPing(socket, message);
    } else if (type == "sync_pong") {
        handleSyncPong(socket, message);
    } else if (type == "starter") {
        handleStarter(message);
    } else if (type == "prepare") {
        handlePrepare(socket, message);
    } else if (type == "ready") {
        handleReady(socket, message);
    } else if (type == "fire") {
        restartClientWatchdog();
        handleFire(message);
    } else if (type == "finish") {
        restartClientWatchdog();
        m_clientSimulationActive = false;
        m_clientWatchdogTimer->stop();
        sendProtocolMessage(socket, "finish_ack");
        appendTcpMessage("Finish received");
    } else if (type == "abort") {
        handleAbort();
    } else if (type == "finish_ack") {
        appendTcpMessage("Finish acknowledged by client");
    }
}

void simulationDialog::handleSyncPing(QTcpSocket *socket, const QJsonObject &message)
{
    if (!socket) {
        return;
    }

    const qint64 clientReceiveMs = monotonicNowMs();

    QJsonObject payload;
    payload["id"] = message.value("id").toInt();
    payload["serverSendMs"] = message.value("serverSendMs").toString();
    payload["clientReceiveMs"] = QString::number(clientReceiveMs);
    payload["clientSendMs"] = QString::number(monotonicNowMs());
    sendProtocolMessage(socket, "sync_pong", payload);

    bool ok = false;
    const qint64 serverSendMs = payload.value("serverSendMs").toString().toLongLong(&ok);
    if (ok) {
        // Client-side fallback estimate; server-calculated offset is more accurate when supplied in fire.
        m_clientServerClockOffsetMs = clientReceiveMs - serverSendMs;
    }
}

void simulationDialog::handleSyncPong(QTcpSocket *socket, const QJsonObject &message)
{
    if (!socket) {
        return;
    }

    const int id = message.value("id").toInt(-1);
    if (id < 0 || !m_syncPingServerSendMs.contains(socket) ||
        !m_syncPingServerSendMs[socket].contains(id)) {
        return;
    }

    const qint64 serverReceiveMs = monotonicNowMs();
    const qint64 serverSendMs = m_syncPingServerSendMs[socket].take(id);

    bool receiveOk = false;
    bool sendOk = false;
    const qint64 clientReceiveMs = message.value("clientReceiveMs").toString().toLongLong(&receiveOk);
    const qint64 clientSendMs = message.value("clientSendMs").toString().toLongLong(&sendOk);
    if (!receiveOk || !sendOk) {
        return;
    }

    const qint64 roundTripMs = (serverReceiveMs - serverSendMs) - (clientSendMs - clientReceiveMs);
    const qint64 clockOffsetMs = ((clientReceiveMs - serverSendMs) + (clientSendMs - serverReceiveMs)) / 2;
    socket->setProperty("lastSyncRttMs", QString::number(roundTripMs));
    socket->setProperty("clientClockOffsetMs", QString::number(clockOffsetMs));
}

void simulationDialog::handlePrepare(QTcpSocket *socket, const QJsonObject &message)
{
    if (!socket) {
        return;
    }

    const int id = message.value("id").toInt(-1);
    const QString targetType = message.value("targetType").toString().trimmed().toLower();
    const QJsonValue targetValueJson = message.value("targetValue");
    if (id < 0 || targetType.isEmpty() || !targetValueJson.isDouble()) {
        return;
    }

    StagedCommand staged;
    staged.targetType = targetType;
    staged.targetValue = targetValueJson.toDouble();
    staged.line = message.value("line").toInt(-1);
    staged.valid = true;
    m_stagedCommands[id] = staged;
    m_clientSimulationActive = true;
    m_syncStopResetArmed = false;
    m_simulationAborted = false;
    m_clientReceivedX.append(staged.line >= 0 ? staged.line : m_clientReceivedX.size() + 1);
    m_clientReceivedY.append(staged.targetValue);
    ui->progressBarSimulation->setMaximum(qMax(1, qMax(m_clientExpectedLineCount, m_clientReceivedY.size())));
    ui->progressBarSimulation->setValue(m_clientReceivedY.size());
    if (m_clientExpectedLineCount > 0) {
        ui->progressBarSimulation->setFormat(QString("%1/%2 received")
                                                 .arg(m_clientReceivedY.size())
                                                 .arg(m_clientExpectedLineCount));
    } else {
        ui->progressBarSimulation->setFormat(QString("%1 received").arg(m_clientReceivedY.size()));
    }
    restartClientWatchdog();
    updateSimulationControls();
    updateScopeControls();
    updateScopePlot();

    QJsonObject payload;
    payload["id"] = id;
    if (staged.line >= 0) {
        payload["line"] = staged.line;
    }
    sendProtocolMessage(socket, "ready", payload);
}

void simulationDialog::handleStarter(const QJsonObject &message)
{
    m_clientExpectedLineCount = message.value("lineCount").toInt(0);
    m_clientReceivedTargetType = message.value("clientTargetType").toString(m_clientReceivedTargetType);
    m_clientReceivedX.clear();
    m_clientReceivedY.clear();
    m_stagedCommands.clear();
    m_clientSimulationActive = true;
    m_syncStopResetArmed = false;
    m_simulationAborted = false;
    ui->horizontalScrollBarScope->setValue(0);

    const bool shouldTrackMax = m_scopeSliderTracksMax ||
                                ui->horizontalSliderScope->value() == ui->horizontalSliderScope->maximum();
    m_scopeSliderTracksMax = shouldTrackMax;
    updateScopeControls();
    if (shouldTrackMax) {
        ui->horizontalSliderScope->setValue(ui->horizontalSliderScope->maximum());
    }

    ui->progressBarSimulation->setMaximum(qMax(1, m_clientExpectedLineCount));
    ui->progressBarSimulation->setValue(0);
    ui->progressBarSimulation->setFormat(QString("0/%1 received").arg(m_clientExpectedLineCount));
    restartClientWatchdog();
    appendTcpMessage(QString("Starter received: %1 lines").arg(m_clientExpectedLineCount));
    updateSimulationControls();
    updateScopePlot();
}

void simulationDialog::handleReady(QTcpSocket *socket, const QJsonObject &message)
{
    if (!socket) {
        return;
    }

    const int readyId = message.value("id").toInt(-1);
    socket->setProperty("lastReadyId", readyId);
    socket->setProperty("lastReadyTimestampMs", message.value("timestampMs").toString());
    appendTcpMessage(QString("Ready received for id %1").arg(readyId));

    if (readyId == m_pendingSimulationId) {
        m_pendingReadyClients.remove(socket);
        if (m_pendingReadyClients.isEmpty()) {
            sendFireForPendingSimulation();
        }
    }
}

void simulationDialog::handleFire(const QJsonObject &message)
{
    const int id = message.value("id").toInt(-1);
    if (id < 0 || !m_stagedCommands.contains(id) || !m_stagedCommands[id].valid) {
        return;
    }

    bool executeAtOk = false;
    qint64 executeAtClientMs = message.value("executeAtClientMs").toString().toLongLong(&executeAtOk);
    if (!executeAtOk) {
        bool executeAtServerOk = false;
        const qint64 executeAtServerMs = message.value("executeAtServerMs").toString().toLongLong(&executeAtServerOk);
        if (executeAtServerOk) {
            executeAtClientMs = executeAtServerMs + m_clientServerClockOffsetMs;
            executeAtOk = true;
        }
    }

    const qint64 delayMs = executeAtOk ? qMax<qint64>(0, executeAtClientMs - monotonicNowMs()) : 0;
    QTimer::singleShot(static_cast<int>(qMin<qint64>(delayMs, std::numeric_limits<int>::max())),
                       this,
                       [this, id]() {
                           executeStagedCommand(id);
                       });
}

QString simulationDialog::buildTargetCommand(const StagedCommand &command) const
{
    return QString("%1 %2\r\n").arg(command.targetType,
                                    QString::number(command.targetValue, 'f', 6));
}

void simulationDialog::executeStagedCommand(int id)
{
    if (!m_stagedCommands.contains(id) || !m_stagedCommands[id].valid) {
        return;
    }

    const StagedCommand command = m_stagedCommands.take(id);
    appendTcpMessage(QString("Executing id %1: %2").arg(id).arg(buildTargetCommand(command).trimmed()));
    emit serialCommandRequested(buildTargetCommand(command));
}

void simulationDialog::handleAbort()
{
    m_stagedCommands.clear();
    appendTcpMessage("Abort received: sending stop");
    abortSimulation("remote_abort", false);
}

void simulationDialog::appendTcpMessage(const QString &message)
{
    ui->plainTextEditMessages->appendPlainText(
        QString("[%1] %2")
            .arg(QDateTime::currentDateTime().toString("HH:mm:ss.zzz"),
                 message));
}

void simulationDialog::appendProtocolMessage(const QString &direction,
                                             const QString &type,
                                             const QJsonObject &message)
{
    if (type == "sync_ping" || type == "sync_pong" || type == "discover" || type == "discover_response") {
        return;
    }

    QStringList parts;
    parts << QString("%1 %2").arg(direction, type);

    if (message.contains("id")) {
        parts << QString("id=%1").arg(message.value("id").toInt());
    }
    if (message.contains("line")) {
        parts << QString("line=%1").arg(message.value("line").toInt());
    }
    if (message.contains("targetType")) {
        parts << QString("target=%1").arg(message.value("targetType").toString());
    }
    if (message.contains("targetValue")) {
        parts << QString("value=%1").arg(message.value("targetValue").toDouble(), 0, 'f', 6);
    }
    if (message.contains("executeAtServerMs")) {
        parts << QString("executeAtServerMs=%1").arg(message.value("executeAtServerMs").toString());
    }
    if (message.contains("reason")) {
        parts << QString("reason=%1").arg(message.value("reason").toString());
    }

    appendTcpMessage(parts.join(" | "));
}

void simulationDialog::stopServer()
{
    const QList<QTcpSocket*> clients = m_serverClients;
    m_serverClients.clear();

    for (QTcpSocket *socket : clients) {
        if (!socket) {
            continue;
        }
        disconnect(socket, nullptr, this, nullptr);
        m_receiveBuffers.remove(socket);
        m_syncPingServerSendMs.remove(socket);
        socket->disconnectFromHost();
        socket->deleteLater();
    }
    m_receiveBuffers.clear();
    m_syncPingServerSendMs.clear();
    ui->listWidgetConnectedClients->clear();

    if (m_tcpServer->isListening()) {
        m_tcpServer->close();
    }

    bindDiscoverySocketForClient();
}

void simulationDialog::disconnectClient()
{
    if (!m_clientSocket) {
        return;
    }

    QTcpSocket *socket = m_clientSocket;
    m_clientSocket = nullptr;
    m_receiveBuffers.remove(socket);
    m_syncPingServerSendMs.remove(socket);
    disconnect(socket, nullptr, this, nullptr);
    socket->disconnectFromHost();
    socket->deleteLater();
}

void simulationDialog::replyToDiscoveryRequest(const QHostAddress &senderAddress, quint16 senderPort)
{
    if (ui->radioButtonLocalhost->isChecked() && !senderAddress.isLoopback()) {
        return;
    }

    QJsonObject message;
    message["protocol"] = "motor-drive-sync";
    message["version"] = 1;
    message["type"] = "discover_response";
    message["host"] = advertisedHostForPeer(senderAddress);
    message["port"] = static_cast<int>(m_tcpServer->serverPort());
    message["role"] = "server";
    message["label"] = QString("%1:%2")
                           .arg(message["host"].toString())
                           .arg(static_cast<int>(m_tcpServer->serverPort()));

    const QByteArray payload = QJsonDocument(message).toJson(QJsonDocument::Compact) + "\n";
    m_discoverySocket->writeDatagram(payload, senderAddress, senderPort);
}

QString simulationDialog::advertisedHostForPeer(const QHostAddress &peerAddress) const
{
    const QHostAddress serverAddress = m_tcpServer->serverAddress();
    if (serverAddress == QHostAddress::Any || serverAddress == QHostAddress::AnyIPv4) {
        if (peerAddress.isLoopback()) {
            return "127.0.0.1";
        }
        if (!m_lanAddress.isEmpty()) {
            return m_lanAddress;
        }
        return peerAddress.toString();
    }

    return serverAddress.toString();
}

void simulationDialog::addDiscoveredServer(const QString &host, quint16 port, const QString &label)
{
    const QString key = QString("%1:%2").arg(host).arg(port);
    m_discoveredServerLastSeen[key] = QDateTime::currentMSecsSinceEpoch();

    if (m_discoveredServerKeys.contains(key)) {
        for (int i = 0; i < ui->listWidgetClientFoundServer->count(); ++i) {
            QListWidgetItem *item = ui->listWidgetClientFoundServer->item(i);
            if (item && item->data(Qt::UserRole + 2).toString() == key) {
                const bool isConnectedServer = m_clientSocket &&
                                               m_clientSocket->state() == QAbstractSocket::ConnectedState &&
                                               m_clientSocket->peerAddress().toString() == host &&
                                               m_clientSocket->peerPort() == port;
                item->setText(isConnectedServer ? QString("%1 (server)").arg(label) : label);
                item->setData(Qt::UserRole, host);
                item->setData(Qt::UserRole + 1, static_cast<int>(port));
                return;
            }
        }

        m_discoveredServerKeys.remove(key);
    }

    m_discoveredServerKeys.insert(key);
    auto *item = new QListWidgetItem(label);
    item->setData(Qt::UserRole, host);
    item->setData(Qt::UserRole + 1, static_cast<int>(port));
    item->setData(Qt::UserRole + 2, key);
    ui->listWidgetClientFoundServer->addItem(item);
}

void simulationDialog::refreshConnectedServerLabel()
{
    for (int i = 0; i < ui->listWidgetClientFoundServer->count(); ++i) {
        QListWidgetItem *item = ui->listWidgetClientFoundServer->item(i);
        if (!item) {
            continue;
        }

        const QString host = item->data(Qt::UserRole).toString();
        const int port = item->data(Qt::UserRole + 1).toInt();
        QString label = QString("%1:%2").arg(host).arg(port);
        if (m_clientSocket &&
            m_clientSocket->state() == QAbstractSocket::ConnectedState &&
            m_clientSocket->peerAddress().toString() == host &&
            m_clientSocket->peerPort() == port) {
            label += " (server)";
        }
        item->setText(label);
    }
}

void simulationDialog::pruneDiscoveredServers()
{
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    constexpr qint64 staleAfterMs = 3500;

    for (int i = ui->listWidgetClientFoundServer->count() - 1; i >= 0; --i) {
        QListWidgetItem *item = ui->listWidgetClientFoundServer->item(i);
        if (!item) {
            continue;
        }

        const QString key = item->data(Qt::UserRole + 2).toString();
        const qint64 lastSeenMs = m_discoveredServerLastSeen.value(key, 0);
        if (key.isEmpty() || nowMs - lastSeenMs <= staleAfterMs) {
            continue;
        }

        m_discoveredServerKeys.remove(key);
        m_discoveredServerLastSeen.remove(key);
        delete ui->listWidgetClientFoundServer->takeItem(i);
    }
}

void simulationDialog::rebuildConnectedClientsList()
{
    ui->listWidgetConnectedClients->clear();
    for (QTcpSocket *socket : std::as_const(m_serverClients)) {
        if (!socket || socket->state() != QAbstractSocket::ConnectedState) {
            continue;
        }

        QString entry = socketDescription(socket);
        const QString role = socket->property("syncRole").toString();
        if (!role.isEmpty()) {
            entry += QString(" (%1)").arg(role);
        }
        ui->listWidgetConnectedClients->addItem(entry);
    }
}

void simulationDialog::sendSyncPingToClients()
{
    if (!m_tcpServer->isListening()) {
        return;
    }

    for (QTcpSocket *socket : std::as_const(m_serverClients)) {
        sendSyncPing(socket);
    }
}

QTcpSocket *simulationDialog::firstConnectedServerClient() const
{
    for (QTcpSocket *socket : m_serverClients) {
        if (socket && socket->state() == QAbstractSocket::ConnectedState) {
            return socket;
        }
    }

    return nullptr;
}

void simulationDialog::runNextSimulationLine()
{
    if (!m_simulationRunning || m_simulationPaused || m_pendingSimulationId >= 0) {
        return;
    }

    if (m_currentSimulationRow >= m_csvNumericRows.size()) {
        m_simulationRunning = false;
        m_simulationTimer->stop();
        m_simulationAborted = false;
        appendTcpMessage("Simulation complete");
        updateSimulationControls();
        updateSimulationProgress();
        return;
    }

    const int rowBeingSent = m_currentSimulationRow;
    const int id = m_nextProtocolId++;
    m_pendingSimulationId = id;
    m_pendingFireDueServerMs = monotonicNowMs() + 120;

    StagedCommand localCommand;
    localCommand.targetType = m_mapping.serverTargetType;
    localCommand.targetValue = numericValue(rowBeingSent, m_mapping.serverValueColumn);
    localCommand.line = rowBeingSent + 1;
    localCommand.valid = true;
    m_stagedCommands[id] = localCommand;

    m_pendingReadyClients.clear();
    for (QTcpSocket *socket : std::as_const(m_serverClients)) {
        if (!socket || socket->state() != QAbstractSocket::ConnectedState) {
            continue;
        }

        if (rowBeingSent == m_mapping.startRow) {
            QJsonObject starterPayload;
            starterPayload["lineCount"] = simulatableLineCount();
            starterPayload["clientTargetType"] = m_mapping.clientTargetType;
            starterPayload["serverTargetType"] = m_mapping.serverTargetType;
            sendProtocolMessage(socket, "starter", starterPayload);
        }

        QJsonObject payload;
        payload["id"] = id;
        payload["line"] = localCommand.line;
        payload["targetType"] = m_mapping.clientTargetType;
        payload["targetValue"] = numericValue(rowBeingSent, m_mapping.clientValueColumn);
        sendProtocolMessage(socket, "prepare", payload);
        m_pendingReadyClients.insert(socket);
    }

    appendTcpMessage(QString("Prepared row %1 as id %2").arg(localCommand.line).arg(id));
    if (m_pendingReadyClients.isEmpty()) {
        sendFireForPendingSimulation();
    } else {
        const int deadlineMs = static_cast<int>(qMax<qint64>(1, m_pendingFireDueServerMs - monotonicNowMs()));
        m_readyDeadlineTimer->start(deadlineMs);
    }
}

void simulationDialog::sendFireForPendingSimulation()
{
    if (m_pendingSimulationId < 0) {
        return;
    }

    if (!m_pendingReadyClients.isEmpty()) {
        return;
    }

    m_readyDeadlineTimer->stop();
    const int id = m_pendingSimulationId;
    const qint64 executeAtServerMs = qMax(m_pendingFireDueServerMs, monotonicNowMs() + 20);

    for (QTcpSocket *socket : std::as_const(m_serverClients)) {
        if (!socket || socket->state() != QAbstractSocket::ConnectedState) {
            continue;
        }

        bool offsetOk = false;
        const qint64 offsetMs = socket->property("clientClockOffsetMs").toString().toLongLong(&offsetOk);

        QJsonObject payload;
        payload["id"] = id;
        payload["line"] = m_currentSimulationRow + 1;
        payload["executeAtServerMs"] = QString::number(executeAtServerMs);
        if (offsetOk) {
            payload["executeAtClientMs"] = QString::number(executeAtServerMs + offsetMs);
        }
        sendProtocolMessage(socket, "fire", payload);
    }

    scheduleLocalFire(id, executeAtServerMs);
    appendTcpMessage(QString("Fire id %1 at %2").arg(id).arg(executeAtServerMs));

    const int completedRow = m_currentSimulationRow;
    ++m_currentSimulationRow;
    m_pendingSimulationId = -1;
    m_pendingFireDueServerMs = 0;
    updateSimulationProgress();
    updateScopeStepMarkerOnly();

    if (m_simulationRunning && !m_simulationPaused && m_currentSimulationRow < m_csvNumericRows.size()) {
        m_simulationTimer->start(nextSimulationDelayMs(completedRow));
    } else if (m_simulationRunning && m_currentSimulationRow >= m_csvNumericRows.size()) {
        m_simulationRunning = false;
        m_simulationTimer->stop();
        m_simulationAborted = false;
        sendFinishToPeers();
        appendTcpMessage("Simulation complete; finish sent");
        updateSimulationControls();
    }
}

void simulationDialog::handleReadyDeadlineExpired()
{
    if (m_pendingSimulationId < 0 || m_pendingReadyClients.isEmpty()) {
        return;
    }

    appendTcpMessage(QString("Ready deadline expired for id %1").arg(m_pendingSimulationId));
    abortSimulation("ready_timeout", true);
}

void simulationDialog::scheduleLocalFire(int id, qint64 executeAtServerMs)
{
    const qint64 delayMs = qMax<qint64>(0, executeAtServerMs - monotonicNowMs());
    QTimer::singleShot(static_cast<int>(qMin<qint64>(delayMs, std::numeric_limits<int>::max())),
                       this,
                       [this, id]() {
                           executeStagedCommand(id);
                       });
}

void simulationDialog::sendAbortToPeers()
{
    QJsonObject payload;
    payload["reason"] = "simulation_stop";

    for (QTcpSocket *socket : std::as_const(m_serverClients)) {
        sendProtocolMessage(socket, "abort", payload);
    }

    if (m_clientSocket && m_clientSocket->state() == QAbstractSocket::ConnectedState) {
        sendProtocolMessage(m_clientSocket, "abort", payload);
    }
}

void simulationDialog::restartClientWatchdog()
{
    if (m_clientSimulationActive) {
        m_clientWatchdogTimer->start(1000);
    }
}

void simulationDialog::handleClientWatchdogExpired()
{
    if (!m_clientSimulationActive) {
        return;
    }

    appendTcpMessage("Client watchdog expired: aborting simulation");
    abortSimulation("client_message_timeout", true);
}

void simulationDialog::sendFinishToPeers()
{
    for (QTcpSocket *socket : std::as_const(m_serverClients)) {
        sendProtocolMessage(socket, "finish");
    }
}

void simulationDialog::abortSimulation(const QString &reason, bool notifyPeers)
{
    m_simulationRunning = false;
    m_simulationPaused = false;
    m_simulationTimer->stop();
    m_readyDeadlineTimer->stop();
    m_clientWatchdogTimer->stop();
    m_pendingSimulationId = -1;
    m_pendingFireDueServerMs = 0;
    m_pendingReadyClients.clear();
    m_stagedCommands.clear();
    m_clientSimulationActive = false;
    m_simulationAborted = true;

    appendTcpMessage(QString("Abort: %1").arg(reason));
    emit serialCommandRequested("stop\r\n");

    if (notifyPeers) {
        QJsonObject payload;
        payload["reason"] = reason;
        for (QTcpSocket *socket : std::as_const(m_serverClients)) {
            sendProtocolMessage(socket, "abort", payload);
        }
        if (m_clientSocket && m_clientSocket->state() == QAbstractSocket::ConnectedState) {
            sendProtocolMessage(m_clientSocket, "abort", payload);
        }
    }

    updateSimulationControls();
    updateSimulationProgress();
    updateScopePlot();
}

void simulationDialog::resetSimulationToStart()
{
    m_currentSimulationRow = qBound(0, m_mapping.startRow, qMax(0, m_csvNumericRows.size() - 1));
    m_syncStopResetArmed = false;
    updateSimulationProgress();
    updateScopeControls();
    ui->horizontalScrollBarScope->setValue(m_currentSimulationRow);
    updateScopePlot();
}

void simulationDialog::updateSimulationControls()
{
    ui->pushButtonStartPause->setText(m_simulationRunning && !m_simulationPaused ? "⏸️" : "▶️");
    ui->pushButtonStartPause->setEnabled(!m_csvNumericRows.isEmpty());
    ui->pushButtonSimOption->setEnabled(!m_csvNumericRows.isEmpty());
    ui->pushButtonSyncStop->setEnabled(m_simulationRunning || m_clientSocket || m_tcpServer->isListening());
    emitMainStatus();
}

void simulationDialog::updateSimulationProgress()
{
    const int maxLine = m_csvNumericRows.size();
    ui->progressBarSimulation->setMaximum(qMax(1, maxLine));
    ui->progressBarSimulation->setValue(qBound(0, m_currentSimulationRow, maxLine));
    ui->progressBarSimulation->setFormat(QString("%1/%2").arg(m_currentSimulationRow).arg(maxLine));
}

void simulationDialog::updateScopeControls()
{
    const bool showClientBuffer = !m_tcpServer->isListening() &&
                                  (m_clientSimulationActive || !m_clientReceivedY.isEmpty());
    const int rowCount = showClientBuffer ? m_clientReceivedY.size() : simulatableLineCount();
    if (rowCount <= 0) {
        ui->horizontalScrollBarScope->setRange(0, 0);
        ui->sliderLabelScope->setText("0");
        return;
    }

    const int maxPoints = qMin(65536, rowCount);
    const bool shouldTrackMax = m_scopeSliderTracksMax ||
                                ui->horizontalSliderScope->value() == ui->horizontalSliderScope->maximum();
    ui->horizontalSliderScope->setMaximum(qMax(1, maxPoints));
    if (shouldTrackMax) {
        m_scopeSliderTracksMax = true;
        ui->horizontalSliderScope->setValue(ui->horizontalSliderScope->maximum());
    }
    const int requestedPoints = qBound(1, ui->horizontalSliderScope->value(), qMax(1, maxPoints));
    ui->sliderLabelScope->setText(QString::number(requestedPoints));
    ui->horizontalScrollBarScope->setPageStep(requestedPoints);
    if (showClientBuffer) {
        ui->horizontalScrollBarScope->setRange(0, qMax(0, rowCount - requestedPoints));
    } else {
        const int startRow = qBound(0, m_mapping.startRow, qMax(0, m_csvNumericRows.size() - 1));
        ui->horizontalScrollBarScope->setRange(startRow, qMax(startRow, m_csvNumericRows.size() - requestedPoints));
    }
}

void simulationDialog::updateScopePlot()
{
    if (!m_scopePlot) {
        return;
    }
    if (!isVisible()) {
        m_scopePlotDirtyWhileHidden = true;
        return;
    }

    QVector<double> x;
    QVector<double> serverY;
    QVector<double> clientY;

    const bool activeClientMode = !m_tcpServer->isListening() && !m_clientReceivedY.isEmpty();
    updateScopeLegendAndAxes();

    if (activeClientMode) {
        const int rowCount = m_clientReceivedY.size();
        const int points = rowCount > 0 ? qBound(1, ui->horizontalSliderScope->value(), qMax(1, rowCount)) : 0;
        const int minStart = qBound(0, m_mapping.startRow, qMax(0, rowCount - 1));
        const int start = qBound(minStart, ui->horizontalScrollBarScope->value(), qMax(minStart, rowCount - points));
        const int end = qMin(rowCount, start + points);
        for (int row = start; row < end; ++row) {
            x.append(row < m_clientReceivedX.size() ? m_clientReceivedX.at(row) : row + 1);
            serverY.append(m_clientReceivedY.at(row));
        }
    } else if (mappingIsValid()) {
        const int rowCount = m_csvNumericRows.size();
        const int points = qBound(1, ui->horizontalSliderScope->value(), qMax(1, rowCount));
        const int start = qBound(0, ui->horizontalScrollBarScope->value(), qMax(0, rowCount - points));
        const int end = qMin(rowCount, start + points);

        for (int row = start; row < end; ++row) {
            x.append(m_mapping.timeColumn >= 0 ? timeValueSeconds(row) : static_cast<double>(row + 1));
            serverY.append(numericValue(row, m_mapping.serverValueColumn));
            clientY.append(numericValue(row, m_mapping.clientValueColumn));
        }
    }

    m_scopePlot->graph(0)->setData(x, serverY);
    m_scopePlot->graph(1)->setData(x, clientY);

    if (!x.isEmpty()) {
        if (qFuzzyCompare(x.first(), x.last())) {
            m_scopePlot->xAxis->setRange(x.first() - 1.0, x.last() + 1.0);
        } else {
            m_scopePlot->xAxis->setRange(x.first(), x.last());
        }
        double serverMinY = std::numeric_limits<double>::max();
        double serverMaxY = std::numeric_limits<double>::lowest();
        for (double value : serverY) {
            serverMinY = qMin(serverMinY, value);
            serverMaxY = qMax(serverMaxY, value);
        }
        if (qFuzzyCompare(serverMinY, serverMaxY)) {
            serverMinY -= 1.0;
            serverMaxY += 1.0;
        }
        const double serverPadding = (serverMaxY - serverMinY) * 0.05;
        m_scopePlot->yAxis->setRange(serverMinY - serverPadding, serverMaxY + serverPadding);

        if (!activeClientMode && !clientY.isEmpty()) {
            double clientMinY = std::numeric_limits<double>::max();
            double clientMaxY = std::numeric_limits<double>::lowest();
            for (double value : clientY) {
                clientMinY = qMin(clientMinY, value);
                clientMaxY = qMax(clientMaxY, value);
            }
            if (qFuzzyCompare(clientMinY, clientMaxY)) {
                clientMinY -= 1.0;
                clientMaxY += 1.0;
            }
            const double clientPadding = (clientMaxY - clientMinY) * 0.05;
            m_scopePlot->yAxis2->setRange(clientMinY - clientPadding, clientMaxY + clientPadding);
        }
    } else {
        m_scopePlot->xAxis->setRange(0, 1);
        m_scopePlot->yAxis->setRange(0, 1);
        m_scopePlot->yAxis2->setRange(0, 1);
    }

    updateScopeStepMarker(activeClientMode);
    m_scopePlot->replot(QCustomPlot::rpQueuedReplot);
}

void simulationDialog::updateScopeLegendAndAxes()
{
    const bool activeClientMode = !m_tcpServer->isListening() && !m_clientReceivedY.isEmpty();
    const QString serverName = m_mapping.serverTargetType.isEmpty() ? "Server" : m_mapping.serverTargetType;
    const QString clientName = activeClientMode
                                   ? (m_clientReceivedTargetType.isEmpty() ? "Client" : m_clientReceivedTargetType)
                                   : (m_mapping.clientTargetType.isEmpty() ? "Client" : m_mapping.clientTargetType);

    m_scopePlot->graph(0)->setName(activeClientMode ? clientName : serverName);
    m_scopePlot->graph(1)->setName(clientName);
    m_scopePlot->graph(1)->setVisible(!activeClientMode);
    m_scopePlot->yAxis->setLabel(activeClientMode ? clientName : serverName);
    m_scopePlot->yAxis2->setLabel(clientName);
    m_scopePlot->yAxis2->setVisible(!activeClientMode);
    m_scopePlot->legend->setVisible(activeClientMode ? !m_clientReceivedY.isEmpty() : mappingIsValid());
}

void simulationDialog::updateScopeStepMarker(bool activeClientMode)
{
    if (!m_scopeStepLine) {
        return;
    }

    if (activeClientMode || !mappingIsValid() || m_csvNumericRows.isEmpty()) {
        m_scopeStepLine->setVisible(false);
        return;
    }

    const int currentRow = qBound(0, m_currentSimulationRow, m_csvNumericRows.size() - 1);
    const double stepX = m_mapping.timeColumn >= 0
                             ? timeValueSeconds(currentRow)
                             : static_cast<double>(currentRow + 1);
    const QCPRange xRange = m_scopePlot->xAxis->range();
    const QCPRange yRange = m_scopePlot->yAxis->range();
    const double markerX = qBound(xRange.lower, stepX, xRange.upper);

    m_scopeStepLine->start->setCoords(markerX, yRange.lower);
    m_scopeStepLine->end->setCoords(markerX, yRange.upper);
    m_scopeStepLine->setVisible(true);
}

void simulationDialog::updateScopeStepMarkerOnly()
{
    if (!m_scopePlot) {
        return;
    }
    if (!isVisible()) {
        m_scopePlotDirtyWhileHidden = true;
        return;
    }

    const bool activeClientMode = !m_tcpServer->isListening() && !m_clientReceivedY.isEmpty();
    updateScopeStepMarker(activeClientMode);
    m_scopePlot->replot(QCustomPlot::rpQueuedReplot);
}

int simulationDialog::simulatableLineCount() const
{
    if (m_csvNumericRows.isEmpty()) {
        return 0;
    }
    return qMax(0, m_csvNumericRows.size() - qBound(0, m_mapping.startRow, m_csvNumericRows.size()));
}

void simulationDialog::updateClientServerList()
{
    if (!m_clientSocket || m_clientSocket->state() == QAbstractSocket::UnconnectedState) {
        return;
    }

    const QString host = m_clientSocket->peerAddress().toString();
    const quint16 port = m_clientSocket->peerPort();
    addDiscoveredServer(host,
                        port,
                        QString("%1:%2").arg(host).arg(port));
    refreshConnectedServerLabel();
}

void simulationDialog::updateStatusIndicators()
{
    const bool serverRunning = m_tcpServer->isListening();
    const bool serverHasClient = std::any_of(m_serverClients.cbegin(),
                                             m_serverClients.cend(),
                                             [](const QTcpSocket *socket) {
                                                 return socket && socket->state() == QAbstractSocket::ConnectedState;
                                             });
    const bool clientRunning = m_clientSocket &&
                               m_clientSocket->state() == QAbstractSocket::ConnectedState;

    setStatusLabel(ui->labelServerStatus,
                   serverRunning ? "Server Running" : "Server Stopped",
                   serverRunning ? "green" : "grey");

    if (serverHasClient) {
        setStatusLabel(ui->labelClientStatus, "Client Connected", "green");
    } else if (clientRunning) {
        setStatusLabel(ui->labelClientStatus, "Client Running", "green");
    } else {
        setStatusLabel(ui->labelClientStatus, "Client Stopped", "grey");
    }

    if (serverHasClient || clientRunning) {
        setStatusLabel(ui->labelDisconnected, "Connected", "grey");
    } else {
        setStatusLabel(ui->labelDisconnected, "Disconnected", "red");
    }

    emitMainStatus();
}

void simulationDialog::updateControlStates()
{
    const bool serverRunning = m_tcpServer->isListening();
    const bool clientActive = m_clientSocket &&
                              m_clientSocket->state() != QAbstractSocket::UnconnectedState;

    ui->pushButtonServerStart->setEnabled(!serverRunning);
    ui->pushButtonServerStop->setEnabled(serverRunning);
    ui->pushButtonClientConnect->setEnabled(!clientActive);
    ui->pushButtonClientDisconnect->setEnabled(clientActive);
}

void simulationDialog::setStatusLabel(QLabel *label, const QString &text, const QString &colorName)
{
    label->setText(text);

    if (colorName == "green") {
        label->setStyleSheet("QLabel { background-color: #2e7d32; color: white; border-radius: 4px; }");
    } else if (colorName == "red") {
        label->setStyleSheet("QLabel { background-color: #c62828; color: white; border-radius: 4px; }");
    } else {
        label->setStyleSheet("QLabel { background-color: #6c757d; color: white; border-radius: 4px; }");
    }
}

void simulationDialog::emitMainStatus()
{
    const bool serverConnected = std::any_of(m_serverClients.cbegin(),
                                             m_serverClients.cend(),
                                             [](const QTcpSocket *socket) {
                                                 return socket && socket->state() == QAbstractSocket::ConnectedState;
                                             });
    const bool clientConnected = m_clientSocket &&
                                 m_clientSocket->state() == QAbstractSocket::ConnectedState;
    const bool connected = serverConnected || clientConnected;

    QString connectionText = "Disconnected";
    QString connectionColor = "off";
    if (serverConnected) {
        connectionText = "Server";
        connectionColor = "green";
    } else if (clientConnected) {
        connectionText = "Client";
        connectionColor = "green";
    }

    QString simulationText = "N/A";
    QString simulationColor = "off";
    if (m_simulationRunning || m_clientSimulationActive) {
        simulationText = "Running";
        simulationColor = "green";
    } else if (m_simulationAborted) {
        simulationText = "Aborted";
        simulationColor = "red";
    } else if (connected) {
        if (serverConnected && !m_csvNumericRows.isEmpty()) {
            simulationText = "Loaded";
            simulationColor = "cyan";
        } else {
            simulationText = "Connected";
            simulationColor = "lightblue";
        }
    }

    emit mainStatusChanged(connectionText, connectionColor, simulationText, simulationColor);
}
