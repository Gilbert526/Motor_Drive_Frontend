#ifndef DATAPARSER_H
#define DATAPARSER_H

#include <QObject>
#include <QByteArray>
#include <QHash>
#include <QVector>

struct FieldDef {
    QString name;
    int size;
    char format;   // 'B', 'H', 'f'
    quint32 maskBit;
};

class DataParser : public QObject {
    Q_OBJECT

public:
    explicit DataParser(QObject *parent = nullptr);

    void parseData(const QByteArray &newData);
    
    QVector<double> getWaveform(const QString &fieldName) const;

    QStringList getFieldNames() const;

    QString getCommandNameForField(const QString &displayName) const;

    quint32 getMaskForField(const QString &fieldName) const;

    /**
     * @brief Parse the length of a complete binary frame from the given data
     * @param data Raw data containing the frame header (0xAA 0x55)
     * @param startIdx Start index of the frame header (default is 0)
     * @return Total number of bytes in the frame (including header and mask), or -1 if data is insufficient or format is incorrect
     */
    int getFrameLength(const QByteArray &data, int startIdx = 0) const;

signals:
    void parsedData(const QHash<QString, double> &values);

    void maskReceived(quint32 mask);

private:
    static const QByteArray SYNC_BYTES;   // 0xAA 0x55
    static const int MAX_FRAME_SIZE = 256;

    QList<FieldDef> m_fields;

    QByteArray m_buffer;

    QHash<QString, double> tryParsePacket(int startIdx, int &nextStartIdx);

    double unpackValue(const QByteArray &data, const FieldDef &field);

    QHash<QString, QString> m_displayToCmd;
    void initCommandMapping();
};

#endif // DATAPARSER_H
