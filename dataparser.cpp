#include "dataparser.h"
#include <QDebug>
#include <qendian.h>

const QByteArray DataParser::SYNC_BYTES = QByteArray::fromHex("AA55");

DataParser::DataParser(QObject *parent): QObject{parent} {
    m_errors = {
        {"ERROR_PWM_CONFIG",     ERROR_PWM_CONFIG},
        {"ERROR_ADC_CONFIG",     ERROR_ADC_CONFIG},
        {"ERROR_DMA_CONFIG",     ERROR_DMA_CONFIG},
        {"ERROR_TIM_CONFIG",     ERROR_TIM_CONFIG},
        {"ERROR_ENCODER_CONFIG", ERROR_ENCODER_CONFIG},
        {"ERROR_FOC_CONFIG",     ERROR_FOC_CONFIG},
        {"ERROR_OVERCURRENT",    ERROR_OVERCURRENT},
        {"ERROR_UNDERVOLTAGE",   ERROR_UNDERVOLTAGE}
    };

    m_modes = {
        {"MOTOR_PROTECTION", static_cast<quint8>(MotorControlMode::MOTOR_PROTECTION)},
        {"MOTOR_STOP",       static_cast<quint8>(MotorControlMode::MOTOR_STOP)},
        {"MOTOR_MANUAL",     static_cast<quint8>(MotorControlMode::MOTOR_MANUAL)},
        {"MOTOR_ALIGN",      static_cast<quint8>(MotorControlMode::MOTOR_ALIGN)},
        {"MOTOR_STARTUP",    static_cast<quint8>(MotorControlMode::MOTOR_STARTUP)},
        {"MOTOR_VVVF",       static_cast<quint8>(MotorControlMode::MOTOR_VVVF)},
        {"MOTOR_SIX_STEP",   static_cast<quint8>(MotorControlMode::MOTOR_SIX_STEP)},
        {"MOTOR_FOC_MANUAL", static_cast<quint8>(MotorControlMode::MOTOR_FOC_MANUAL)},
        {"MOTOR_FOC_LINEAR", static_cast<quint8>(MotorControlMode::MOTOR_FOC_LINEAR)},
        {"MOTOR_FOC_DPWM",   static_cast<quint8>(MotorControlMode::MOTOR_FOC_DPWM)}
    };

    m_fields = {
        {"RPM",        4, 'f', 1 << 0},
        {"RPMSP",      4, 'f', 1 << 1},
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
    m_fields2 = {
        {"OM",         1, 'B', 1 << 0},
        {"M_INDEX",    4, 'f', 1 << 1},
        {"FW",         1, 'B', 1 << 2},
        {"UMAG",       4, 'f', 1 << 3},
        {"FFT",        4, 'f', 1 << 4}
    };
    initCommandMapping();
}

void DataParser::parseData(const QByteArray &newData) {
    m_buffer.append(newData);
    int idx = 0;
    while (idx < m_buffer.size()) {
        int nextIdx;
        QHash<QString, double> values = tryParsePacket(idx, nextIdx);
        if (nextIdx > idx) {
            if (nextIdx == idx + 1 && values.isEmpty()) {
                idx = nextIdx;
                continue;
            }
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
    if (m_buffer.size() < headerPos + minimumFrameSize())
        return result;

    // 读取 mask（小端32位）
    if (!hasValidFrameMetadata(m_buffer, headerPos)) {
        nextStartIdx = headerPos + 1;
        return result;
    }

    const int errorPos = headerPos + 2;
    const int modePos = errorPos + 4;
    const int mask1Pos = modePos + 1;
    const int mask2Pos = mask1Pos + 4;
    const quint32 errorCode = qFromLittleEndian<quint32>(m_buffer.constData() + errorPos);
    const quint8 controlMode = static_cast<quint8>(m_buffer.at(modePos));
    const quint32 mask1 = qFromLittleEndian<quint32>(m_buffer.constData() + mask1Pos);
    const quint32 mask2 = qFromLittleEndian<quint32>(m_buffer.constData() + mask2Pos);

    int payloadPos = mask2Pos + 4;
    int currentPos = payloadPos;

    // 按字段定义顺序解析
    for (const FieldDef &field : m_fields) {
        if (mask1 & field.maskBit) {
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
    for (const FieldDef &field : m_fields2) {
        if (mask2 & field.maskBit) {
            if (m_buffer.size() < currentPos + field.size)
                return result;
            QByteArray raw = m_buffer.mid(currentPos, field.size);
            double value = unpackValue(raw, field);
            result[field.name] = value;
            currentPos += field.size;
        }
    }

    nextStartIdx = currentPos;
    const QStringList errorNames = getErrorNames(errorCode);
    emit errorReceived(errorCode, errorNames);
    emit packetStatusReceived(errorCode,
                              errorNames,
                              controlMode,
                              getControlModeName(controlMode),
                              isControlModeKnown(controlMode));
    emit maskReceived(mask1, mask2);
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
    for (const auto &f : m_fields2)
        names << f.name;
    return names;
}

QString DataParser::getCommandNameForField(const QString &displayName) const {
    return m_displayToCmd.value(displayName, displayName);
}

quint32 DataParser::getMaskForField(const QString &fieldName) const {
    for (const FieldDef &field : m_fields) {
        if (field.name == fieldName) {
            return field.maskBit;
        }
    }
    return 0;   // 未找到
}

bool DataParser::isFieldEnabled(const QString &fieldName, quint32 mask1, quint32 mask2) const {
    for (const FieldDef &field : m_fields) {
        if (field.name == fieldName) {
            return (mask1 & field.maskBit) != 0;
        }
    }
    for (const FieldDef &field : m_fields2) {
        if (field.name == fieldName) {
            return (mask2 & field.maskBit) != 0;
        }
    }
    return false;
}

QStringList DataParser::getErrorNames(quint32 errorCode) const {
    QStringList names;
    for (const ErrorDef &error : m_errors) {
        if (errorCode & error.maskBit) {
            names << error.name;
        }
    }
    return names;
}

QString DataParser::getControlModeName(quint8 mode) const {
    for (const ModeDef &modeDef : m_modes) {
        if (modeDef.value == mode) {
            return modeDef.name;
        }
    }
    return "UNKNOWN_CONTROL_MODE";
}

bool DataParser::isControlModeKnown(quint8 mode) const {
    for (const ModeDef &modeDef : m_modes) {
        if (modeDef.value == mode) {
            return true;
        }
    }
    return false;
}

int DataParser::minimumFrameSize() const {
    return 2 + 4 + 1 + 4 + 4;
}

bool DataParser::hasValidFrameMetadata(const QByteArray &data, int startIdx) const {
    if (data.size() < startIdx + minimumFrameSize())
        return false;

    if (data.at(startIdx) != char(0xAA) || data.at(startIdx + 1) != char(0x55))
        return false;

    const int modePos = startIdx + 2 + 4;
    const int mask1Pos = modePos + 1;
    const int mask2Pos = mask1Pos + 4;
    const quint32 errorCode = qFromLittleEndian<quint32>(data.constData() + startIdx + 2);
    const quint8 controlMode = static_cast<quint8>(data.at(modePos));
    if (!isControlModeKnown(controlMode))
        return false;

    quint32 validErrorBits = 0;
    for (const ErrorDef &error : m_errors) {
        validErrorBits |= error.maskBit;
    }
    if ((errorCode & ~validErrorBits) != 0)
        return false;

    quint32 validMask1Bits = 0;
    for (const FieldDef &field : m_fields) {
        validMask1Bits |= field.maskBit;
    }

    quint32 validMask2Bits = 0;
    for (const FieldDef &field : m_fields2) {
        validMask2Bits |= field.maskBit;
    }

    const quint32 mask1 = qFromLittleEndian<quint32>(data.constData() + mask1Pos);
    const quint32 mask2 = qFromLittleEndian<quint32>(data.constData() + mask2Pos);
    return (mask1 & ~validMask1Bits) == 0 && (mask2 & ~validMask2Bits) == 0;
}

int DataParser::getFrameLength(const QByteArray &data, int startIdx) const
{
    // Check if the data is sufficient to contain the frame header (2 bytes) + mask (4 bytes)
    if (data.size() < startIdx + minimumFrameSize())
        return -1;

    if (!hasValidFrameMetadata(data, startIdx))
        return -1;

    // Read the mask (little-endian 32-bit) after the error code and mode byte.
    const int mask1Pos = startIdx + 2 + 4 + 1;
    const int mask2Pos = mask1Pos + 4;
    const quint32 mask1 = qFromLittleEndian<quint32>(data.constData() + mask1Pos);
    const quint32 mask2 = qFromLittleEndian<quint32>(data.constData() + mask2Pos);

    // Calculate the total length of the payload area
    int payloadLen = 0;
    for (const FieldDef &field : m_fields) {
        if (mask1 & field.maskBit) {
            payloadLen += field.size;
        }
    }
    for (const FieldDef &field : m_fields2) {
        if (mask2 & field.maskBit) {
            payloadLen += field.size;
        }
    }

    // Total length = frame header(2) + mask(4) + payload length
    int totalLen = 2 + 4 + 1 + 4 + 4 + payloadLen;
    if (data.size() < startIdx + totalLen)
        return -1;   // Insufficient data

    return totalLen;
}

void DataParser::addCommandMapping(const QString &displayName, const QString &commandName) {
    m_displayToCmd[displayName] = commandName;
}

void DataParser::initCommandMapping() {
    m_displayToCmd.clear();

    addCommandMapping("RPM", "rpm");
    addCommandMapping("RPMSP", "rpmsp");
    addCommandMapping("POS", "pos");
    addCommandMapping("ELPOS", "elpos");
    addCommandMapping("DUTY_A", "duty_a");
    addCommandMapping("DUTY_B", "duty_b");
    addCommandMapping("DUTY_C", "duty_c");
    addCommandMapping("IA", "ia");
    addCommandMapping("IB", "ib");
    addCommandMapping("IC", "ic");
    addCommandMapping("VA", "va");
    addCommandMapping("VB", "vb");
    addCommandMapping("VBATT", "vbatt");
    addCommandMapping("IBATT", "ibatt");
    addCommandMapping("IA_RAW", "ia_raw");
    addCommandMapping("IB_RAW", "ib_raw");
    addCommandMapping("IC_RAW", "ic_raw");
    addCommandMapping("VA_RAW", "va_raw");
    addCommandMapping("VB_RAW", "vb_raw");
    addCommandMapping("VBATT_RAW", "vbatt_raw");
    addCommandMapping("IBATT_RAW", "ibatt_raw");
    addCommandMapping("IA_MAX", "ia_max");
    addCommandMapping("IB_MAX", "ib_max");
    addCommandMapping("IC_MAX", "ic_max");
    addCommandMapping("IBATT_MAX", "ibatt_max");
    addCommandMapping("FOC_ID", "id");
    addCommandMapping("FOC_IQ", "iq");
    addCommandMapping("FOC_IDSP", "idsp");
    addCommandMapping("FOC_IQSP", "iqsp");
    addCommandMapping("FOC_VD", "vd");
    addCommandMapping("FOC_VQ", "vq");

    addCommandMapping("OM", "om");
    addCommandMapping("M_INDEX", "m_index");
    addCommandMapping("FW", "fw");
    addCommandMapping("UMAG", "umag");
    addCommandMapping("FFT", "fft");
}
