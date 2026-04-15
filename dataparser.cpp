#include "dataparser.h"
#include <QDebug>
#include <qendian.h>

const QByteArray DataParser::SYNC_BYTES = QByteArray::fromHex("AA55");

DataParser::DataParser(QObject *parent): QObject{parent} {
    m_fields = {
        {"HALL",       1, 'B', 1 << 0},
        {"RPM",        4, 'f', 1 << 1},
        {"POS",        2, 'H', 1 << 2},
        {"ELPOS",      2, 'H', 1 << 3},
        {"DUTY_A",     4, 'f', 1 << 4},
        {"DUTY_B",     4, 'f', 1 << 5},
        {"DUTY_C",     4, 'f', 1 << 6},
        {"IA",         4, 'f', 1 << 7},
        {"IB",         4, 'f', 1 << 8},
        {"IC",         4, 'f', 1 << 9},
        {"VA",         4, 'f', 1 << 10},
        {"VB",         4, 'f', 1 << 11},
        {"VBATT",      4, 'f', 1 << 12},
        {"IBATT",      4, 'f', 1 << 13},
        {"IA_RAW",     2, 'H', 1 << 14},
        {"IB_RAW",     2, 'H', 1 << 15},
        {"IC_RAW",     2, 'H', 1 << 16},
        {"VA_RAW",     2, 'H', 1 << 17},
        {"VB_RAW",     2, 'H', 1 << 18},
        {"VBATT_RAW",  2, 'H', 1 << 19},
        {"IBATT_RAW",  2, 'H', 1 << 20},
        {"IA_MAX",     4, 'f', 1 << 21},
        {"IB_MAX",     4, 'f', 1 << 22},
        {"IC_MAX",     4, 'f', 1 << 23},
        {"IBATT_MAX",  4, 'f', 1 << 24},
        {"FOC_ID",     4, 'f', 1 << 25},
        {"FOC_IQ",     4, 'f', 1 << 26},
        {"FOC_IDSP",   4, 'f', 1 << 27},
        {"FOC_IQSP",   4, 'f', 1 << 28},
        {"FOC_VD",     4, 'f', 1 << 29},
        {"FOC_VQ",     4, 'f', 1 << 30}
    };
    initCommandMapping();
}

void DataParser::parseData(const QByteArray &newData) {
    m_buffer.append(newData);
    int idx = 0;
    while (idx < m_buffer.size()) {
        int nextIdx;
        QHash<QString, double> values = tryParsePacket(idx, nextIdx);
        if (!values.isEmpty()) {
            emit parsedData(values);   // Inform MainWindow of new parsed data
            idx = nextIdx;
        } else {
            break;  // 数据不足，等待更多数据
        }
    }
    // 保留未处理的部分（最多保留一帧的最大长度，避免无限增长）
    if (idx > 0) {
        m_buffer = m_buffer.mid(idx);
        if (m_buffer.size() > MAX_FRAME_SIZE)
            m_buffer.clear();
    }
}

QHash<QString, double> DataParser::tryParsePacket(int startIdx, int &nextStartIdx) {
    QHash<QString, double> result;
    nextStartIdx = startIdx;

    // 查找帧头
    int headerPos = m_buffer.indexOf(SYNC_BYTES, startIdx);
    if (headerPos == -1) {
        // 没有找到帧头，跳过所有已扫描的数据（但保留最后几个字节防止跨边界）
        nextStartIdx = m_buffer.size() - SYNC_BYTES.size() + 1;
        if (nextStartIdx < startIdx) nextStartIdx = m_buffer.size();
        return result;
    }

    // 帧头位置确定了，检查是否有足够空间读取 mask (4字节)
    if (m_buffer.size() < headerPos + 2 + 4)
        return result;

    // 读取 mask（小端32位）
    quint32 mask = 0;
    const uchar* p = reinterpret_cast<const uchar*>(m_buffer.data() + headerPos + 2);
    mask = p[0] | (p[1]<<8) | (p[2]<<16) | (p[3]<<24);

    int payloadPos = headerPos + 2 + 4;
    int currentPos = payloadPos;

    // 按字段定义顺序解析
    for (const FieldDef &field : m_fields) {
        if (mask & field.maskBit) {
            // 该字段存在，检查缓冲区长度是否足够
            if (m_buffer.size() < currentPos + field.size)
                return result;  // 数据不足
            QByteArray raw = m_buffer.mid(currentPos, field.size);
            double value = unpackValue(raw, field);
            result[field.name] = value;
            currentPos += field.size;
        }
        // 如果字段不存在，不移动指针，继续下一个字段
    }

    // 成功解析一个完整包
    nextStartIdx = currentPos;
    return result;
}

double DataParser::unpackValue(const QByteArray &data, const FieldDef &field) {
    if (data.size() < field.size) return 0.0;
    switch (field.format) {
    case 'B': // unsigned char
        return static_cast<double>(static_cast<quint8>(data[0]));
    case 'H': // unsigned short (小端)
        return static_cast<double>(qFromLittleEndian<quint16>(data.data()));
    case 'f': // float (小端)
        return static_cast<double>(qFromLittleEndian<float>(data.data()));
    default:
        return 0.0;
    }
}

QVector<double> DataParser::getWaveform(const QString &fieldName) const {
    // 此函数暂不实现，由 MainWindow 自己维护波形队列
    return QVector<double>();
}

QStringList DataParser::getFieldNames() const {
    QStringList names;
    for (const auto &f : m_fields)
        names << f.name;
    return names;
}

QString DataParser::getCommandNameForField(const QString &displayName) const {
    return m_displayToCmd.value(displayName, displayName);
}

void DataParser::initCommandMapping() {
    m_displayToCmd["HALL"]      = "hall";
    m_displayToCmd["RPM"]       = "rpm";
    m_displayToCmd["POS"]       = "pos";
    m_displayToCmd["ELPOS"]     = "elpos";
    m_displayToCmd["DUTY_A"]    = "duty_a";
    m_displayToCmd["DUTY_B"]    = "duty_b";
    m_displayToCmd["DUTY_C"]    = "duty_c";
    m_displayToCmd["IA"]        = "ia";
    m_displayToCmd["IB"]        = "ib";
    m_displayToCmd["IC"]        = "ic";
    m_displayToCmd["VA"]        = "va";
    m_displayToCmd["VB"]        = "vb";
    m_displayToCmd["VBATT"]     = "vbatt";
    m_displayToCmd["IBATT"]     = "ibatt";
    m_displayToCmd["IA_RAW"]    = "ia_raw";
    m_displayToCmd["IB_RAW"]    = "ib_raw";
    m_displayToCmd["IC_RAW"]    = "ic_raw";
    m_displayToCmd["VA_RAW"]    = "va_raw";
    m_displayToCmd["VB_RAW"]    = "vb_raw";
    m_displayToCmd["VBATT_RAW"] = "vbatt_raw";
    m_displayToCmd["IBATT_RAW"] = "ibatt_raw";
    m_displayToCmd["IA_MAX"]    = "ia_max";
    m_displayToCmd["IB_MAX"]    = "ib_max";
    m_displayToCmd["IC_MAX"]    = "ic_max";
    m_displayToCmd["IBATT_MAX"] = "ibatt_max";
    m_displayToCmd["FOC_ID"]    = "id";
    m_displayToCmd["FOC_IQ"]    = "iq";
    m_displayToCmd["FOC_IDSP"]  = "idsp";
    m_displayToCmd["FOC_IQSP"]  = "iqsp";
    m_displayToCmd["FOC_VD"]    = "vd";
    m_displayToCmd["FOC_VQ"]    = "vq";
}
