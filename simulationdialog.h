#ifndef SIMULATIONDIALOG_H
#define SIMULATIONDIALOG_H

#include <QDialog>
#include <QHash>
#include <QHostAddress>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

namespace Ui {
class simulationDialog;
}

class QLabel;
class QListWidgetItem;
class QCustomPlot;
class QCPItemLine;
class QCloseEvent;
class QEvent;
class QShowEvent;
class QTimer;
class QTcpServer;
class QTcpSocket;
class QUdpSocket;

/**
 * @brief Dialog that coordinates synchronized simulation playback over TCP/UDP.
 *
 * The dialog can act as a server or a client. The server loads CSV targets,
 * schedules fire messages, and broadcasts synchronization data. Clients discover
 * servers over UDP, estimate clock offset through ping/pong messages, execute
 * staged commands at server-specified times, and report progress for plotting.
 */
class simulationDialog : public QDialog
{
    Q_OBJECT

public:
    explicit simulationDialog(QWidget *parent = nullptr);
    ~simulationDialog();

signals:
    /**
     * @brief Ask MainWindow to send a target command through the serial port.
     */
    void serialCommandRequested(const QString &command);

    /**
     * @brief Publish compact connection/simulation status for the main window.
     */
    void mainStatusChanged(const QString &connectionText,
                           const QString &connectionColor,
                           const QString &simulationText,
                           const QString &simulationColor);

protected:
    void changeEvent(QEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    void showEvent(QShowEvent *event) override;

private slots:
    void on_pushButtonServerStart_clicked();
    void on_pushButtonServerStop_clicked();
    void on_pushButtonClientConnect_clicked();
    void on_pushButtonClientDisconnect_clicked();
    void on_pushButtonOpenFile_clicked();
    void on_pushButtonSimOption_clicked();
    void on_pushButtonStartPause_clicked();
    void on_pushButtonSyncStop_clicked();
    void on_horizontalSliderScope_valueChanged(int value);
    void on_horizontalScrollBarScope_valueChanged(int value);

    void handleNewServerConnection();
    void handleServerClientReadyRead();
    void handleServerClientDisconnected();
    void handleClientConnected();
    void handleClientReadyRead();
    void handleClientDisconnected();
    void handleClientError();
    void sendDiscoveryProbe();
    void handleDiscoveryReadyRead();
    void handleDiscoveredServerSelected(QListWidgetItem *item);
    void handleDiscoveredServerActivated(QListWidgetItem *item);
    void sendSyncPingToClients();
    void runNextSimulationLine();

private:
    /**
     * @brief Command prepared from a CSV row and executed after a fire message.
     */
    struct StagedCommand {
        QString targetType;
        double targetValue = 0.0;
        int line = -1;
        bool valid = false;
    };

    /**
     * @brief User-selected CSV column mapping for local and peer targets.
     */
    struct SimulationMapping {
        QString serverTargetType = "speed";
        int serverValueColumn = -1;
        QString clientTargetType = "torque";
        int clientValueColumn = -1;
        int timeColumn = -1;
        QString timeUnit = "s";
        int startRow = 0;
    };

    Ui::simulationDialog *ui;

    QTcpServer *m_tcpServer;
    QUdpSocket *m_discoverySocket;
    QTimer *m_discoveryTimer;
    QTimer *m_syncPingTimer;
    QTimer *m_simulationTimer;
    QTimer *m_scopeMarkerUpdateTimer;
    QTimer *m_readyDeadlineTimer;
    QTimer *m_clientWatchdogTimer;
    QCustomPlot *m_scopePlot;
    QCPItemLine *m_scopeStepLine;
    QWidget *m_scopeStepOverlay;
    QList<QTcpSocket*> m_serverClients;
    QHash<QTcpSocket*, QByteArray> m_receiveBuffers;
    QHash<QTcpSocket*, QHash<int, qint64>> m_syncPingServerSendMs;
    QTcpSocket *m_clientSocket;
    QString m_lanAddress;
    QSet<QString> m_discoveredServerKeys;
    QHash<QString, qint64> m_discoveredServerLastSeen;
    QHash<int, StagedCommand> m_stagedCommands;
    QSet<QTcpSocket*> m_pendingReadyClients;
    SimulationMapping m_mapping;
    QString m_csvPath;
    QStringList m_csvHeaders;
    QVector<QStringList> m_csvTextRows;
    QVector<QVector<double>> m_csvNumericRows;
    QVector<double> m_clientReceivedX;
    QVector<double> m_clientReceivedY;
    qint64 m_clientServerClockOffsetMs;
    qint64 m_pendingFireDueServerMs;
    int m_nextProtocolId;
    int m_currentSimulationRow;
    int m_pendingSimulationId;
    bool m_simulationRunning;
    bool m_simulationPaused;
    bool m_simulationStartedWithClientConnection;
    bool m_syncStopResetArmed;
    bool m_clientSimulationActive;
    bool m_scopeSliderTracksMax;
    bool m_scopePlotDirtyWhileHidden;
    bool m_scopeMarkerUpdatePending;
    bool m_simulationAborted;
    int m_clientExpectedLineCount;
    QString m_clientReceivedTargetType;

    // UI setup and palette-sensitive refresh helpers.
    void initializeConnectionUi();
    void initializeSimulationUi();
    void applyScopeTheme();
    // CSV parsing, mapping, and row timing helpers.
    qint64 monotonicNowMs() const;
    QStringList parseCsvLine(const QString &line) const;
    bool loadCsvFile(const QString &filePath);
    bool showSimulationOptionsDialog();
    bool mappingIsValid() const;
    QStringList columnNames() const;
    double numericValue(int row, int column) const;
    double timeValueSeconds(int row) const;
    int nextSimulationDelayMs(int completedRow) const;
    // Simulation/scope state refresh helpers.
    void updateSimulationControls();
    void updateSimulationProgress();
    void updateScopeControls();
    void updateScopePlot();
    void updateScopeLegendAndAxes();
    void updateScopeStepMarker(bool activeClientMode);
    void updateScopeStepMarkerOnly();
    void sendAbortToPeers();
    void abortSimulation(const QString &reason, bool notifyPeers = true);
    void resetSimulationToStart();
    void sendFireForPendingSimulation();
    void scheduleLocalFire(int id, qint64 executeAtServerMs);
    // Simulation lifecycle and peer coordination helpers.
    QTcpSocket *firstConnectedServerClient() const;
    int simulatableLineCount() const;
    void handleReadyDeadlineExpired();
    void restartClientWatchdog();
    void handleClientWatchdogExpired();
    void sendFinishToPeers();
    // UDP discovery and address-selection helpers.
    void bindDiscoverySocketForClient();
    void bindDiscoverySocketForServer(quint16 port);
    QString firstLanIpv4Address() const;
    QList<QHostAddress> discoveryProbeAddresses() const;
    QHostAddress selectedListenAddress() const;
    quint16 readPortLineEdit(const QString &text, bool *ok) const;
    QString socketDescription(const QTcpSocket *socket) const;
    // Line-delimited JSON protocol helpers.
    void sendProtocolMessage(QTcpSocket *socket,
                             const QString &type,
                             const QJsonObject &payload = QJsonObject());
    void sendSyncPing(QTcpSocket *socket);
    void processReceivedData(QTcpSocket *socket, bool socketIsServerClient);
    void handleProtocolLine(QTcpSocket *socket,
                            const QByteArray &line,
                            bool socketIsServerClient);
    void handleSyncPing(QTcpSocket *socket, const QJsonObject &message);
    void handleSyncPong(QTcpSocket *socket, const QJsonObject &message);
    void handlePrepare(QTcpSocket *socket, const QJsonObject &message);
    void handleReady(QTcpSocket *socket, const QJsonObject &message);
    void handleFire(const QJsonObject &message);
    void handleStarter(const QJsonObject &message);
    void handleAbort();
    // Command execution, logging, teardown, and status helpers.
    QString buildTargetCommand(const StagedCommand &command) const;
    void executeStagedCommand(int id);
    void appendTcpMessage(const QString &message);
    void appendProtocolMessage(const QString &direction, const QString &type, const QJsonObject &message);
    void stopServer();
    void disconnectClient();
    void replyToDiscoveryRequest(const QHostAddress &senderAddress, quint16 senderPort);
    QString advertisedHostForPeer(const QHostAddress &peerAddress) const;
    void addDiscoveredServer(const QString &host, quint16 port, const QString &label);
    void refreshConnectedServerLabel();
    void pruneDiscoveredServers();
    void rebuildConnectedClientsList();
    void updateClientServerList();
    void updateStatusIndicators();
    void updateControlStates();
    void setStatusLabel(QLabel *label, const QString &text, const QString &colorName);
    void emitMainStatus();
};

#endif // SIMULATIONDIALOG_H
