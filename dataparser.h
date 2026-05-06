#ifndef DATAPARSER_H
#define DATAPARSER_H

#include <QObject>
#include <QByteArray>
#include <QHash>
#include <QStringList>
#include <QVector>

struct FieldDef {
    QString name;
    int size;
    char format;   // 'B', 'H', 'f'
    quint32 maskBit;
};

struct ErrorDef {
    QString name;
    quint32 maskBit;
};

struct ModeDef {
    QString name;
    quint8 value;
};

class DataParser : public QObject {
    Q_OBJECT

public:
    enum ErrorFlag : quint32 {
        ERROR_PWM_CONFIG     = 1 << 0,
        ERROR_ADC_CONFIG     = 1 << 1,
        ERROR_DMA_CONFIG     = 1 << 2,
        ERROR_TIM_CONFIG     = 1 << 3,
        ERROR_ENCODER_CONFIG = 1 << 4,
        ERROR_FOC_CONFIG     = 1 << 5,
        ERROR_OVERCURRENT    = 1 << 6,
        ERROR_UNDERVOLTAGE   = 1 << 7
    };

    enum class MotorControlMode : quint8 {
        MOTOR_PROTECTION,
        MOTOR_STOP,
        MOTOR_MANUAL,
        MOTOR_ALIGN,
        MOTOR_STARTUP,
        MOTOR_VVVF,
        MOTOR_SIX_STEP,
        MOTOR_FOC_MANUAL,
        MOTOR_FOC_LINEAR,
        MOTOR_FOC_DPWM
    };

    explicit DataParser(QObject *parent = nullptr);

    void parseData(const QByteArray &newData);
    
    QVector<double> getWaveform(const QString &fieldName) const;

    QStringList getFieldNames() const;

    QString getCommandNameForField(const QString &displayName) const;

    quint32 getMaskForField(const QString &fieldName) const;

    bool isFieldEnabled(const QString &fieldName, quint32 mask1, quint32 mask2) const;

    QStringList getErrorNames(quint32 errorCode) const;

    QString getControlModeName(quint8 mode) const;

    bool isControlModeKnown(quint8 mode) const;
    int minimumFrameSize() const;
    bool hasValidFrameMetadata(const QByteArray &data, int startIdx = 0) const;

    /**
     * @brief Parse the length of a complete binary frame from the given data
     * @param data Raw data containing the frame header (0xAA 0x55)
     * @param startIdx Start index of the frame header (default is 0)
     * @return Total number of bytes in the frame (including header, error, mode, mask1 and mask2), or -1 if data is insufficient or format is incorrect
     */
    int getFrameLength(const QByteArray &data, int startIdx = 0) const;

signals:
    void parsedData(const QHash<QString, double> &values);

    void maskReceived(quint32 mask1, quint32 mask2);

    void errorReceived(quint32 errorCode, const QStringList &errorNames);

    void packetStatusReceived(quint32 errorCode,
                              const QStringList &errorNames,
                              quint8 controlMode,
                              const QString &controlModeName,
                              bool controlModeKnown);

private:
    static const QByteArray SYNC_BYTES;   // 0xAA 0x55
    static const int MAX_FRAME_SIZE = 256;

    QList<FieldDef> m_fields;
    QList<FieldDef> m_fields2;
    QList<ErrorDef> m_errors;
    QList<ModeDef> m_modes;

    QByteArray m_buffer;

    QHash<QString, double> tryParsePacket(int startIdx, int &nextStartIdx);

    double unpackValue(const QByteArray &data, const FieldDef &field);
    void addCommandMapping(const QString &displayName, const QString &commandName);

    QHash<QString, QString> m_displayToCmd;
    void initCommandMapping();
};

#endif // DATAPARSER_H
