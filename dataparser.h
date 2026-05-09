#ifndef DATAPARSER_H
#define DATAPARSER_H

#include <QObject>
#include <QByteArray>
#include <QHash>
#include <QStringList>
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
     * @return Total number of bytes in the frame (including header, error, mode, timestamp, mask1 and mask2), or -1 if data is insufficient or format is incorrect
     */
    int getFrameLength(const QByteArray &data, int startIdx = 0) const;

    static constexpr const char *TIMESTAMP_FIELD = "__mcu_timestamp_ticks";

signals:
    void parsedData(const QHash<QString, double> &values);

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
    static const int MAX_FRAME_SIZE = 256;

    QList<FieldDef> m_fields;
    QList<FieldDef> m_fields2;
    QList<ErrorDef> m_errors;
    QList<ModeDef> m_modes;
    QList<CustomFieldDef> m_customFields;

    QByteArray m_buffer;
    QString m_configurationPath;

    QHash<QString, double> tryParsePacket(int startIdx, int &nextStartIdx);

    double unpackValue(const QByteArray &data, const FieldDef &field);
    void addCommandMapping(const QString &displayName, const QString &commandName);
    bool loadDefaultConfiguration(QString *errorMessage = nullptr);
    static QStringList configurationSearchPaths();

    QHash<QString, QString> m_displayToCmd;
};

#endif // DATAPARSER_H
