#ifndef DATAPARSER_H
#define DATAPARSER_H

#include <QObject>
#include <QByteArray>
#include <QHash>
#include <QElapsedTimer>
#include <QFile>
#include <QStringList>
#include <QTextStream>
#include <QVector>
#include <optional>

struct FieldDef {
    QString name;
    int size;
    char format;   // 'B', 'H', 'f'
    quint32 maskBit;
};

struct ErrorDef {
    QString name;
    QString type;
    quint32 maskBit;
};

struct ModeDef {
    QString name;
    quint8 value;
};

struct CustomFieldDef {
    QString name;
    QString expression;
};

struct IndicatorStatusDef {
    bool hasValue = false;
    QString valueName;
    std::optional<double> numericValue;
    bool hasBit = false;
    int bit = 0;
    bool hasLowerBound = false;
    double lowerBound = 0.0;
    bool hasUpperBound = false;
    double upperBound = 0.0;
    QString displayText;
    QString color = "off";
    double timeSec = 0.5;
};

struct IndicatorDef {
    QString name;
    QString type;
    int indicator = 0;
    QString dataSource;
    QList<IndicatorStatusDef> statuses;
};

struct GaugeThresholdDef {
    bool hasLowerBound = false;
    double lowerBound = 0.0;
    bool hasUpperBound = false;
    double upperBound = 0.0;
    QString color;
};

struct GaugeDef {
    QString name;
    int gauge = 0;
    QString dataSource;
    QString topDisplayUnit;
    double minimum = 0.0;
    double maximum = 100.0;
    int divisions = 5;
    QList<GaugeThresholdDef> thresholds;
    double hysteresis = 0.0;
};

struct TuneParameterDef {
    QString name;
    QString command;
};

struct TuneSubsystemDef {
    QString name;
    QString command;
    QList<TuneParameterDef> parameters;
};

struct TelemetryFieldDef {
    QString name;
    int length = 0;
    bool variableLength = false;
    bool required = false;
    quint32 polynomial = 0x04C11DB7;
    QString format;
    QByteArray value;
};

struct TelemetryStructureDef {
    quint8 version = 0;
    QStringList fields;
};

struct PacketLayout {
    const TelemetryStructureDef *structure = nullptr;
    int startPos = 0;
    int versionPos = -1;
    int errorPos = -1;
    int modePos = -1;
    int timeHighPos = -1;
    int timeLowPos = -1;
    int timeUsPos = -1;
    int mask1Pos = -1;
    int mask2Pos = -1;
    int adcIdPos = -1;
    int sampleCountPos = -1;
    int resolutionBitPos = -1;
    int sequencePos = -1;
    int shuntPos = -1;
    int offsetPos = -1;
    int payloadPos = -1;
    int payload2Pos = -1;
    int crcPos = -1;
    int crcLength = 0;
    quint32 crcPolynomial = 0x04C11DB7;
    int fixedMetadataSize = 0;
    int payloadLength = 0;
    int payload2Length = 0;
    int totalLength = 0;
};

struct AdcSamplePacket {
    quint8 version = 0;
    quint8 adcId = 0;
    quint8 resolutionBit = 0;
    quint32 sequence = 0;
    bool hasTimestampTicks = false;
    bool hasTimestampUs = false;
    quint64 timestampTicks = 0;
    quint32 timestampUs = 0;
    double timestampSeconds = 0.0;
    float shunt = 0.0f;
    float offset = 0.0f;
    QVector<quint16> samples;
};

Q_DECLARE_METATYPE(AdcSamplePacket)

class DataParser : public QObject {
    Q_OBJECT

public:
    explicit DataParser(QObject *parent = nullptr);

    void parseData(const QByteArray &newData);

    bool loadConfiguration(const QString &filePath, QString *errorMessage = nullptr);
    QString configurationPath() const { return m_configurationPath; }
    
    QVector<double> getWaveform(const QString &fieldName) const;

    QStringList getFieldNames() const;

    QString getCommandNameForField(const QString &displayName) const;
    const QList<IndicatorDef>& getIndicators() const { return m_indicators; }
    const QList<GaugeDef>& getGauges() const { return m_gauges; }
    const QList<TuneSubsystemDef>& getTuningDefinitions() const { return m_tuning; }

    quint32 getMaskForField(const QString &fieldName) const;

    bool isFieldEnabled(const QString &fieldName, quint32 mask1, quint32 mask2) const;

    QStringList getErrorNames(quint32 errorCode) const;

    QString getControlModeName(quint8 mode) const;

    bool isControlModeKnown(quint8 mode) const;
    quint32 getErrorMaskForName(const QString &errorName) const;
    quint32 getErrorMaskForType(const QString &errorType) const;
    std::optional<quint8> getControlModeValueForName(const QString &modeName) const;
    int minimumFrameSize() const;
    bool hasValidFrameMetadata(const QByteArray &data, int startIdx = 0) const;

    /**
     * @brief Parse the length of a complete binary frame from the given data
     * @param data Raw data containing the frame header (0xAA 0x55)
     * @param startIdx Start index of the frame header (default is 0)
     * @return Total number of bytes in the frame (including header, version, error, mode, timestamp, mask1 and mask2), or -1 if data is insufficient or format is incorrect
     */
    int getFrameLength(const QByteArray &data, int startIdx = 0) const;
    int findNextFrameHeader(const QByteArray &data, int startIdx = 0) const;
    int findNextValidTelemetryFrameHeader(const QByteArray &data, int startIdx = 0) const;
    int partialFrameHeaderLength(const QByteArray &data) const;

    static constexpr const char *TIMESTAMP_FIELD = "__mcu_timestamp_ticks";
    static constexpr const char *TIMESTAMP_US_FIELD = "__mcu_timestamp_us";
    static constexpr const char *TIMESTAMP_SECONDS_FIELD = "__mcu_timestamp_seconds";

    bool startAdcCsvLogging(const QString &fileName, QString *errorMessage = nullptr);
    void stopAdcCsvLogging();
    void flushStaleAdcCsvSequences();
    bool startQuickCsvLogging(const QString &telemetryFileName,
                              const QString &adcFileName,
                              QString *errorMessage = nullptr);
    void stopQuickCsvLogging();

signals:
    void parsedData(const QHash<QString, double> &values);
    void adcSampleReceived(const AdcSamplePacket &packet);
    void adcSampleActivityReceived();
    void receivedTextLine(const QString &text);

    void maskReceived(quint32 mask1, quint32 mask2);

    void errorReceived(quint32 errorCode, const QStringList &errorNames);

    void packetStatusReceived(quint32 errorCode,
                              const QStringList &errorNames,
                              quint8 controlMode,
                              const QString &controlModeName,
                              bool controlModeKnown);
    void configurationChanged();

private:
    static const QByteArray SYNC_BYTES;   // 0xAA 0x55
    static const int MAX_FRAME_SIZE = 65536;

    QList<FieldDef> m_fields;
    QList<FieldDef> m_fields2;
    QList<ErrorDef> m_errors;
    QList<ModeDef> m_modes;
    QList<CustomFieldDef> m_customFields;
    QList<IndicatorDef> m_indicators;
    QList<GaugeDef> m_gauges;
    QList<TuneSubsystemDef> m_tuning;
    QHash<QString, TelemetryFieldDef> m_telemetryFields;
    QList<TelemetryStructureDef> m_telemetryStructures;
    QHash<QString, TelemetryFieldDef> m_adcSampleFields;
    QList<TelemetryStructureDef> m_adcSampleStructures;
    QByteArray m_telemetrySyncBytes;
    QByteArray m_adcSampleSyncBytes;

    QByteArray m_buffer;
    QByteArray m_textLineBuffer;
    QString m_configurationPath;
    QElapsedTimer m_telemetryEmitTimer;
    QElapsedTimer m_adcActivityEmitTimer;
    bool m_hasLastMask = false;
    quint32 m_lastMask1 = 0;
    quint32 m_lastMask2 = 0;
    bool m_hasLastPacketStatus = false;
    quint32 m_lastStatusErrorCode = 0;
    quint8 m_lastStatusControlMode = 0;
    bool m_lastStatusControlModeKnown = false;
    bool m_isAdcLogging = false;
    bool m_isQuickTelemetryLogging = false;
    bool m_isQuickAdcLogging = false;
    QFile m_adcLogFile;
    QTextStream m_adcLogStream;
    QFile m_quickTelemetryLogFile;
    QTextStream m_quickTelemetryLogStream;
    QStringList m_quickTelemetryLogFields;
    QFile m_quickAdcLogFile;
    QTextStream m_quickAdcLogStream;
    struct PendingAdcSampleRow {
        bool hasAdc[3] = {false, false, false};
        quint16 raw[3] = {0, 0, 0};
        double current[3] = {0.0, 0.0, 0.0};
    };
    struct PendingAdcSequence {
        QString time;
        QVector<PendingAdcSampleRow> rows;
        int receivedMask = 0;
        qint64 lastUpdateMs = 0;
    };
    QHash<quint32, PendingAdcSequence> m_pendingAdcSequences;
    QHash<quint32, PendingAdcSequence> m_quickPendingAdcSequences;

    QHash<QString, double> tryParsePacket(int startIdx, int &nextStartIdx);

    double unpackValue(const QByteArray &data, const FieldDef &field);
    void maybeEmitParsedData(const QHash<QString, double> &values);
    void maybeEmitPacketMetadata(quint32 errorCode, quint8 controlMode, quint32 mask1, quint32 mask2);
    void processReceiveTextChunk(const QByteArray &chunk);
    void flushReceiveTextLines();
    static bool isReceiveTextByte(char byte);
    static bool isLikelyReceiveTextLine(const QByteArray &line);
    bool startTelemetryCsvLogging(QFile *file,
                                  QTextStream *stream,
                                  QStringList *fields,
                                  const QString &fileName,
                                  QString *errorMessage);
    void stopTelemetryCsvLogging(QFile *file,
                                 QTextStream *stream,
                                 QStringList *fields);
    void writeTelemetryLogRow(QTextStream *stream,
                              const QStringList &fields,
                              const QHash<QString, double> &values);
    void writeAdcLogRows(const AdcSamplePacket &packet);
    void writeQuickAdcLogRows(const AdcSamplePacket &packet);
    void flushAdcSequence(quint32 sequence, const QString &reason = QString());
    void flushQuickAdcSequence(quint32 sequence, const QString &reason = QString());
    void flushStaleAdcSequences();
    void flushStaleQuickAdcSequences();
    void addCommandMapping(const QString &displayName, const QString &commandName);
    bool loadDefaultConfiguration(QString *errorMessage = nullptr);
    static QStringList configurationSearchPaths();
    const TelemetryStructureDef* structureForVersion(quint8 version, const QList<TelemetryStructureDef> &structures) const;
    int minimumStructureSize(const TelemetryStructureDef &structure, const QHash<QString, TelemetryFieldDef> &fields) const;
    int minimumAdcFrameSize() const;
    std::optional<PacketLayout> buildPacketLayout(const QByteArray &data, int startIdx, bool requireComplete) const;
    std::optional<PacketLayout> buildAdcPacketLayout(const QByteArray &data, int startIdx, bool requireComplete) const;
    int payloadLengthForMask(quint32 mask, const QList<FieldDef> &fields) const;
    bool validatePacketCrc(const QByteArray &data, const PacketLayout &layout) const;
    quint32 calculateCrc32(const char *data, int length, quint32 polynomial) const;
    bool tryParseAdcPacket(int headerPos, int &nextStartIdx);
    static int formatElementSize(const QString &format);
    static double timestampSeconds(bool hasTimestampUs, quint32 timestampUs, bool hasTimestampTicks, quint64 timestampTicks);

    QHash<QString, QString> m_displayToCmd;
};

#endif // DATAPARSER_H
