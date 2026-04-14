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

signals:
    void parsedData(const QHash<QString, double> &values);

private:
    static const QByteArray SYNC_BYTES;   // 0xAA 0x55
    static const int MAX_FRAME_SIZE = 256;

    QList<FieldDef> m_fields;

    QByteArray m_buffer;

    QHash<QString, double> tryParsePacket(int startIdx, int &nextStartIdx);

    double unpackValue(const QByteArray &data, const FieldDef &field);
};

#endif // DATAPARSER_H
