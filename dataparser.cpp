#include "dataparser.h"
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <cmath>
#include <limits>
#include <qendian.h>

const QByteArray DataParser::SYNC_BYTES = QByteArray::fromHex("AA55");

DataParser::DataParser(QObject *parent): QObject{parent} {
    qRegisterMetaType<AdcSamplePacket>("AdcSamplePacket");
    QString errorMessage;
    if (!loadDefaultConfiguration(&errorMessage)) {
        qWarning() << "Failed to load telemetry parser configuration:" << errorMessage;
    }
}

namespace {
bool readObjectArray(const QJsonObject &root, const QString &key, QJsonArray *array, QString *errorMessage)
{
    const QJsonValue value = root.value(key);
    if (!value.isArray()) {
        if (errorMessage) {
            *errorMessage = QString("Missing or invalid '%1' array").arg(key);
        }
        return false;
    }
    *array = value.toArray();
    return true;
}

bool readOptionalObjectArray(const QJsonObject &root, const QString &key, QJsonArray *array, QString *errorMessage)
{
    if (!root.contains(key)) {
        *array = QJsonArray();
        return true;
    }
    return readObjectArray(root, key, array, errorMessage);
}

bool readRequiredString(const QJsonObject &object, const QString &key, QString *out, QString *errorMessage)
{
    const QJsonValue value = object.value(key);
    if (!value.isString() || value.toString().isEmpty()) {
        if (errorMessage) {
            *errorMessage = QString("Missing or invalid string '%1'").arg(key);
        }
        return false;
    }
    *out = value.toString();
    return true;
}

bool readIntRange(const QJsonObject &object,
                  const QString &key,
                  int minimum,
                  int maximum,
                  int *out,
                  QString *errorMessage)
{
    const QJsonValue value = object.value(key);
    if (!value.isDouble()) {
        if (errorMessage) {
            *errorMessage = QString("Missing or invalid integer '%1'").arg(key);
        }
        return false;
    }
    const int number = value.toInt();
    if (number < minimum || number > maximum) {
        if (errorMessage) {
            *errorMessage = QString("'%1' must be between %2 and %3").arg(key).arg(minimum).arg(maximum);
        }
        return false;
    }
    *out = number;
    return true;
}

bool readMaskBit(const QJsonObject &object, quint32 *maskBit, QString *errorMessage)
{
    int bit = 0;
    if (!readIntRange(object, "bit", 0, 31, &bit, errorMessage)) {
        return false;
    }
    *maskBit = 1u << bit;
    return true;
}

bool parseErrors(const QJsonArray &array, QList<ErrorDef> *errors, QString *errorMessage)
{
    QList<ErrorDef> parsed;
    for (int i = 0; i < array.size(); ++i) {
        if (!array[i].isObject()) {
            if (errorMessage) {
                *errorMessage = QString("errors[%1] must be an object").arg(i);
            }
            return false;
        }
        const QJsonObject object = array[i].toObject();
        QString name;
        QString type;
        quint32 maskBit = 0;
        if (!readRequiredString(object, "name", &name, errorMessage) ||
            !readRequiredString(object, "type", &type, errorMessage) ||
            !readMaskBit(object, &maskBit, errorMessage)) {
            if (errorMessage) {
                *errorMessage = QString("errors[%1]: %2").arg(i).arg(*errorMessage);
            }
            return false;
        }
        if (type != "config" && type != "undervoltage" && type != "overcurrent") {
            if (errorMessage) {
                *errorMessage = QString("errors[%1]: type must be config, undervoltage, or overcurrent").arg(i);
            }
            return false;
        }
        parsed.append({name, type, maskBit});
    }
    *errors = parsed;
    return true;
}

bool parseModes(const QJsonArray &array, QList<ModeDef> *modes, QString *errorMessage)
{
    QList<ModeDef> parsed;
    for (int i = 0; i < array.size(); ++i) {
        if (!array[i].isObject()) {
            if (errorMessage) {
                *errorMessage = QString("modes[%1] must be an object").arg(i);
            }
            return false;
        }
        const QJsonObject object = array[i].toObject();
        QString name;
        int value = 0;
        if (!readRequiredString(object, "name", &name, errorMessage) ||
            !readIntRange(object, "value", 0, 255, &value, errorMessage)) {
            if (errorMessage) {
                *errorMessage = QString("modes[%1]: %2").arg(i).arg(*errorMessage);
            }
            return false;
        }
        parsed.append({name, static_cast<quint8>(value)});
    }
    *modes = parsed;
    return true;
}

bool parseFields(const QJsonArray &array,
                 QList<FieldDef> *fields,
                 QHash<QString, QString> *commands,
                 QString *errorMessage)
{
    QList<FieldDef> parsed;
    for (int i = 0; i < array.size(); ++i) {
        if (!array[i].isObject()) {
            if (errorMessage) {
                *errorMessage = QString("fields[%1] must be an object").arg(i);
            }
            return false;
        }
        const QJsonObject object = array[i].toObject();
        QString name;
        QString format;
        int size = 0;
        quint32 maskBit = 0;
        if (!readRequiredString(object, "name", &name, errorMessage) ||
            !readIntRange(object, "size", 1, 8, &size, errorMessage) ||
            !readRequiredString(object, "format", &format, errorMessage) ||
            !readMaskBit(object, &maskBit, errorMessage)) {
            if (errorMessage) {
                *errorMessage = QString("fields[%1]: %2").arg(i).arg(*errorMessage);
            }
            return false;
        }
        if (format.size() != 1 || (format[0] != 'B' && format[0] != 'H' && format[0] != 'f')) {
            if (errorMessage) {
                *errorMessage = QString("fields[%1]: format must be one of B, H, f").arg(i);
            }
            return false;
        }
        parsed.append({name, size, format[0].toLatin1(), maskBit});

        const QString command = object.value("command").toString(name.toLower());
        commands->insert(name, command);
    }
    *fields = parsed;
    return true;
}

bool parseCustomFields(const QJsonArray &array, QList<CustomFieldDef> *customFields, QString *errorMessage)
{
    QList<CustomFieldDef> parsed;
    for (int i = 0; i < array.size(); ++i) {
        if (!array[i].isObject()) {
            if (errorMessage) {
                *errorMessage = QString("custom_fields[%1] must be an object").arg(i);
            }
            return false;
        }

        const QJsonObject object = array[i].toObject();
        QString name;
        QString expression;
        if (!readRequiredString(object, "name", &name, errorMessage) ||
            !readRequiredString(object, "expression", &expression, errorMessage)) {
            if (errorMessage) {
                *errorMessage = QString("custom_fields[%1]: %2").arg(i).arg(*errorMessage);
            }
            return false;
        }
        parsed.append({name, expression});
    }

    *customFields = parsed;
    return true;
}

bool parseIndicatorStatus(const QJsonObject &object, IndicatorStatusDef *status, QString *errorMessage)
{
    status->displayText = object.value("displayText").toString();
    status->color = object.value("color").toString("off");
    status->timeSec = qMax(0.05, object.value("time").toDouble(0.5));

    const QJsonValue value = object.value("value");
    if (!value.isUndefined()) {
        status->hasValue = true;
        if (value.isString()) {
            status->valueName = value.toString();
        } else if (value.isDouble()) {
            status->numericValue = value.toDouble();
        } else {
            if (errorMessage) {
                *errorMessage = "value must be a string or number";
            }
            return false;
        }
    }

    if (object.contains("bit")) {
        if (!readIntRange(object, "bit", 0, 31, &status->bit, errorMessage)) {
            return false;
        }
        status->hasBit = true;
    }

    if (object.contains("lowerBound")) {
        const QJsonValue lower = object.value("lowerBound");
        if (!lower.isDouble()) {
            if (errorMessage) {
                *errorMessage = "lowerBound must be numeric";
            }
            return false;
        }
        status->hasLowerBound = true;
        status->lowerBound = lower.toDouble();
    }

    if (object.contains("upperBound")) {
        const QJsonValue upper = object.value("upperBound");
        if (!upper.isDouble()) {
            if (errorMessage) {
                *errorMessage = "upperBound must be numeric";
            }
            return false;
        }
        status->hasUpperBound = true;
        status->upperBound = upper.toDouble();
    }

    return true;
}

bool parseIndicators(const QJsonArray &array, QList<IndicatorDef> *indicators, QString *errorMessage)
{
    QList<IndicatorDef> parsed;
    for (int i = 0; i < array.size(); ++i) {
        if (!array[i].isObject()) {
            if (errorMessage) {
                *errorMessage = QString("indicators[%1] must be an object").arg(i);
            }
            return false;
        }

        const QJsonObject object = array[i].toObject();
        IndicatorDef indicator;
        if (!readRequiredString(object, "name", &indicator.name, errorMessage) ||
            !readRequiredString(object, "type", &indicator.type, errorMessage) ||
            !readIntRange(object, "indicator", 0, 8, &indicator.indicator, errorMessage) ||
            !readRequiredString(object, "dataSource", &indicator.dataSource, errorMessage)) {
            if (errorMessage) {
                *errorMessage = QString("indicators[%1]: %2").arg(i).arg(*errorMessage);
            }
            return false;
        }
        if (indicator.type != "mode" && indicator.type != "condition" && indicator.type != "bitwise") {
            if (errorMessage) {
                *errorMessage = QString("indicators[%1]: type must be mode, condition, or bitwise").arg(i);
            }
            return false;
        }

        QJsonArray statusesJson;
        if (!readObjectArray(object, "status", &statusesJson, errorMessage)) {
            if (errorMessage) {
                *errorMessage = QString("indicators[%1]: %2").arg(i).arg(*errorMessage);
            }
            return false;
        }
        for (int j = 0; j < statusesJson.size(); ++j) {
            if (!statusesJson[j].isObject()) {
                if (errorMessage) {
                    *errorMessage = QString("indicators[%1].status[%2] must be an object").arg(i).arg(j);
                }
                return false;
            }
            IndicatorStatusDef status;
            if (!parseIndicatorStatus(statusesJson[j].toObject(), &status, errorMessage)) {
                if (errorMessage) {
                    *errorMessage = QString("indicators[%1].status[%2]: %3").arg(i).arg(j).arg(*errorMessage);
                }
                return false;
            }
            if (status.displayText.isEmpty()) {
                status.displayText = indicator.name;
            }
            indicator.statuses.append(status);
        }
        parsed.append(indicator);
    }

    *indicators = parsed;
    return true;
}

bool parseGaugeThreshold(const QJsonObject &object, GaugeThresholdDef *threshold, QString *errorMessage)
{
    if (object.contains("lowerBound")) {
        const QJsonValue lower = object.value("lowerBound");
        if (!lower.isDouble()) {
            if (errorMessage) {
                *errorMessage = "lowerBound must be numeric";
            }
            return false;
        }
        threshold->hasLowerBound = true;
        threshold->lowerBound = lower.toDouble();
    }

    if (object.contains("upperBound")) {
        const QJsonValue upper = object.value("upperBound");
        if (!upper.isDouble()) {
            if (errorMessage) {
                *errorMessage = "upperBound must be numeric";
            }
            return false;
        }
        threshold->hasUpperBound = true;
        threshold->upperBound = upper.toDouble();
    }

    if (!readRequiredString(object, "color", &threshold->color, errorMessage)) {
        return false;
    }

    return true;
}

bool parseGauges(const QJsonArray &array, QList<GaugeDef> *gauges, QString *errorMessage)
{
    QList<GaugeDef> parsed;
    for (int i = 0; i < array.size(); ++i) {
        if (!array[i].isObject()) {
            if (errorMessage) {
                *errorMessage = QString("gauges[%1] must be an object").arg(i);
            }
            return false;
        }

        const QJsonObject object = array[i].toObject();
        GaugeDef gauge;
        if (!readRequiredString(object, "name", &gauge.name, errorMessage) ||
            !readIntRange(object, "gauge", 0, 16, &gauge.gauge, errorMessage) ||
            !readRequiredString(object, "dataSource", &gauge.dataSource, errorMessage)) {
            if (errorMessage) {
                *errorMessage = QString("gauges[%1]: %2").arg(i).arg(*errorMessage);
            }
            return false;
        }

        gauge.topDisplayUnit = object.value("topDisplayUnit").toString();
        const QJsonValue minValue = object.value("min");
        const QJsonValue maxValue = object.value("max");
        if (!minValue.isDouble() || !maxValue.isDouble()) {
            if (errorMessage) {
                *errorMessage = QString("gauges[%1]: min and max must be numeric").arg(i);
            }
            return false;
        }
        gauge.minimum = minValue.toDouble();
        gauge.maximum = maxValue.toDouble();
        if (gauge.maximum <= gauge.minimum) {
            if (errorMessage) {
                *errorMessage = QString("gauges[%1]: max must be greater than min").arg(i);
            }
            return false;
        }
        if (!readIntRange(object, "divisions", 1, 100, &gauge.divisions, errorMessage)) {
            if (errorMessage) {
                *errorMessage = QString("gauges[%1]: %2").arg(i).arg(*errorMessage);
            }
            return false;
        }
        gauge.hysteresis = qMax(0.0, object.value("hysteresis").toDouble(0.0));

        QJsonArray thresholdsJson;
        if (!readObjectArray(object, "thresholds", &thresholdsJson, errorMessage)) {
            if (errorMessage) {
                *errorMessage = QString("gauges[%1]: %2").arg(i).arg(*errorMessage);
            }
            return false;
        }
        for (int j = 0; j < thresholdsJson.size(); ++j) {
            if (!thresholdsJson[j].isObject()) {
                if (errorMessage) {
                    *errorMessage = QString("gauges[%1].thresholds[%2] must be an object").arg(i).arg(j);
                }
                return false;
            }
            GaugeThresholdDef threshold;
            if (!parseGaugeThreshold(thresholdsJson[j].toObject(), &threshold, errorMessage)) {
                if (errorMessage) {
                    *errorMessage = QString("gauges[%1].thresholds[%2]: %3").arg(i).arg(j).arg(*errorMessage);
                }
                return false;
            }
            gauge.thresholds.append(threshold);
        }

        parsed.append(gauge);
    }

    std::sort(parsed.begin(), parsed.end(), [](const GaugeDef &a, const GaugeDef &b) {
        return a.gauge < b.gauge;
    });
    *gauges = parsed;
    return true;
}

bool parseUnsignedValue(const QJsonValue &value, quint32 *out)
{
    if (value.isDouble()) {
        *out = static_cast<quint32>(value.toDouble());
        return true;
    }
    if (value.isString()) {
        bool ok = false;
        *out = value.toString().toUInt(&ok, 0);
        return ok;
    }
    return false;
}

QByteArray parseHexByteValue(const QString &text, int expectedLength)
{
    QString hex = text.trimmed();
    if (hex.startsWith("0x", Qt::CaseInsensitive)) {
        hex = hex.mid(2);
    }
    if (hex.size() % 2 != 0) {
        hex.prepend('0');
    }
    const QByteArray bytes = QByteArray::fromHex(hex.toLatin1());
    return bytes.size() == expectedLength ? bytes : QByteArray();
}

bool parsePacketFields(const QJsonObject &root,
                       const QString &key,
                       QHash<QString, TelemetryFieldDef> *packetFields,
                       QString *errorMessage)
{
    const QJsonValue fieldsValue = root.value(key);
    if (!fieldsValue.isObject()) {
        if (errorMessage) {
            *errorMessage = QString("Missing or invalid '%1' object").arg(key);
        }
        return false;
    }

    QHash<QString, TelemetryFieldDef> parsed;
    const QJsonObject fieldsObject = fieldsValue.toObject();
    for (auto it = fieldsObject.constBegin(); it != fieldsObject.constEnd(); ++it) {
        if (!it.value().isObject()) {
            if (errorMessage) {
                *errorMessage = QString("telemetry_fields.%1 must be an object").arg(it.key());
            }
            return false;
        }

        const QJsonObject object = it.value().toObject();
        TelemetryFieldDef field;
        field.name = it.key();
        const QJsonValue lengthValue = object.value("length");
        if (lengthValue.isString() && lengthValue.toString() == "variable") {
            field.variableLength = true;
        } else if (lengthValue.isDouble()) {
            field.length = lengthValue.toInt();
            if (field.length <= 0) {
                if (errorMessage) {
                    *errorMessage = QString("%1.%2 length must be positive").arg(key, it.key());
                }
                return false;
            }
        } else {
            if (errorMessage) {
                *errorMessage = QString("%1.%2 length must be numeric or 'variable'").arg(key, it.key());
            }
            return false;
        }
        field.required = object.value("required").toBool(false);
        if (object.contains("polynomial") && !parseUnsignedValue(object.value("polynomial"), &field.polynomial)) {
            if (errorMessage) {
                *errorMessage = QString("%1.%2 polynomial must be numeric or hex string").arg(key, it.key());
            }
            return false;
        }
        field.format = object.value("format").toString();
        if (object.contains("value")) {
            if (!object.value("value").isString() || field.variableLength) {
                if (errorMessage) {
                    *errorMessage = QString("%1.%2 value must be a hex string on a fixed-length field").arg(key, it.key());
                }
                return false;
            }
            field.value = parseHexByteValue(object.value("value").toString(), field.length);
            if (field.value.isEmpty()) {
                if (errorMessage) {
                    *errorMessage = QString("%1.%2 value length does not match field length").arg(key, it.key());
                }
                return false;
            }
        }
        parsed.insert(field.name, field);
    }

    *packetFields = parsed;
    return true;
}

bool parsePacketStructures(const QJsonObject &root,
                           const QString &key,
                           const QHash<QString, TelemetryFieldDef> &packetFields,
                           QList<TelemetryStructureDef> *packetStructures,
                           QString *errorMessage)
{
    QJsonArray structuresJson;
    if (!readObjectArray(root, key, &structuresJson, errorMessage)) {
        return false;
    }

    QList<TelemetryStructureDef> parsed;
    for (int i = 0; i < structuresJson.size(); ++i) {
        if (!structuresJson[i].isObject()) {
            if (errorMessage) {
                *errorMessage = QString("%1[%2] must be an object").arg(key).arg(i);
            }
            return false;
        }

        const QJsonObject object = structuresJson[i].toObject();
        int version = 0;
        if (!readIntRange(object, "version", 0, 255, &version, errorMessage)) {
            if (errorMessage) {
                *errorMessage = QString("%1[%2]: %3").arg(key).arg(i).arg(*errorMessage);
            }
            return false;
        }
        QJsonArray fieldsJson;
        if (!readObjectArray(object, "fields", &fieldsJson, errorMessage)) {
            if (errorMessage) {
                *errorMessage = QString("%1[%2]: %3").arg(key).arg(i).arg(*errorMessage);
            }
            return false;
        }

        TelemetryStructureDef structure;
        structure.version = static_cast<quint8>(version);
        for (int j = 0; j < fieldsJson.size(); ++j) {
            if (!fieldsJson[j].isString()) {
                if (errorMessage) {
                    *errorMessage = QString("%1[%2].fields[%3] must be a string").arg(key).arg(i).arg(j);
                }
                return false;
            }
            const QString fieldName = fieldsJson[j].toString();
            if (!packetFields.contains(fieldName)) {
                if (errorMessage) {
                    *errorMessage = QString("%1[%2].fields[%3] references unknown field '%4'")
                                        .arg(key).arg(i).arg(j).arg(fieldName);
                }
                return false;
            }
            structure.fields.append(fieldName);
        }
        if (!structure.fields.contains("header") || !structure.fields.contains("version")) {
            if (errorMessage) {
                *errorMessage = QString("%1[%2] must include header and version").arg(key).arg(i);
            }
            return false;
        }
        parsed.append(structure);
    }

    *packetStructures = parsed;
    return true;
}

class ExpressionParser {
public:
    ExpressionParser(const QString &expression, const QHash<QString, double> &values)
        : m_expression(expression), m_values(values) {}

    bool evaluate(double *out)
    {
        m_pos = 0;
        m_ok = true;
        const double value = parseExpression();
        skipSpaces();
        if (!m_ok || m_pos != m_expression.size() || !std::isfinite(value)) {
            return false;
        }
        *out = value;
        return true;
    }

private:
    double parseExpression()
    {
        double value = parseTerm();
        while (m_ok) {
            skipSpaces();
            if (match('+')) {
                value += parseTerm();
            } else if (match('-')) {
                value -= parseTerm();
            } else {
                break;
            }
        }
        return value;
    }

    double parseTerm()
    {
        double value = parseUnary();
        while (m_ok) {
            skipSpaces();
            if (match('*')) {
                value *= parseUnary();
            } else if (match('/')) {
                const double divisor = parseUnary();
                if (qFuzzyIsNull(divisor)) {
                    m_ok = false;
                    return 0.0;
                }
                value /= divisor;
            } else {
                break;
            }
        }
        return value;
    }

    double parseUnary()
    {
        skipSpaces();
        if (match('+')) {
            return parseUnary();
        }
        if (match('-')) {
            return -parseUnary();
        }
        if (peekCastType()) {
            const QString castType = parseCastType();
            return applyCast(parseUnary(), castType);
        }
        return parsePrimary();
    }

    double parsePrimary()
    {
        skipSpaces();
        if (match('(')) {
            const double value = parseExpression();
            if (!match(')')) {
                m_ok = false;
                return 0.0;
            }
            return value;
        }

        if (m_pos < m_expression.size() && (m_expression[m_pos].isDigit() || m_expression[m_pos] == '.')) {
            return parseNumber();
        }

        if (m_pos < m_expression.size() && (m_expression[m_pos].isLetter() || m_expression[m_pos] == '_')) {
            const QString identifier = parseIdentifier();
            if (identifier == "static_cast") {
                return parseStaticCast();
            }

            skipSpaces();
            if (match('(')) {
                QVector<double> args;
                skipSpaces();
                if (!match(')')) {
                    while (m_ok) {
                        args.append(parseExpression());
                        skipSpaces();
                        if (match(')')) {
                            break;
                        }
                        if (!match(',')) {
                            m_ok = false;
                            return 0.0;
                        }
                    }
                }
                return callFunction(identifier, args);
            }

            if (identifier == "M_PI" || identifier == "PI") return std::acos(-1.0);
            if (identifier == "SQRT3") return std::sqrt(3.0);
            if (identifier == "SQRT2") return std::sqrt(2.0);
            if (identifier == "M_E" || identifier == "E") return std::exp(1.0);
            if (m_values.contains(identifier)) return m_values.value(identifier);
            m_ok = false;
            return 0.0;
        }

        m_ok = false;
        return 0.0;
    }

    double parseStaticCast()
    {
        skipSpaces();
        if (!match('<')) {
            m_ok = false;
            return 0.0;
        }
        const QString castType = parseTypeName();
        if (castType.isEmpty() || !match('>')) {
            m_ok = false;
            return 0.0;
        }
        skipSpaces();
        if (!match('(')) {
            m_ok = false;
            return 0.0;
        }
        const double value = parseExpression();
        if (!match(')')) {
            m_ok = false;
            return 0.0;
        }
        return applyCast(value, castType);
    }

    double parseNumber()
    {
        int start = m_pos;
        while (m_pos < m_expression.size()) {
            const QChar ch = m_expression[m_pos];
            if (ch.isDigit() || ch == '.' || ch == 'e' || ch == 'E' ||
                ((ch == '+' || ch == '-') && m_pos > start &&
                 (m_expression[m_pos - 1] == 'e' || m_expression[m_pos - 1] == 'E'))) {
                ++m_pos;
            } else {
                break;
            }
        }
        const int end = m_pos;
        if (m_pos < m_expression.size() && (m_expression[m_pos] == 'f' || m_expression[m_pos] == 'F')) {
            ++m_pos;
        }
        bool ok = false;
        const double value = m_expression.mid(start, end - start).toDouble(&ok);
        if (!ok) {
            m_ok = false;
        }
        return value;
    }

    QString parseIdentifier()
    {
        int start = m_pos;
        while (m_pos < m_expression.size() &&
               (m_expression[m_pos].isLetterOrNumber() || m_expression[m_pos] == '_')) {
            ++m_pos;
        }
        return m_expression.mid(start, m_pos - start);
    }

    QString parseTypeName()
    {
        skipSpaces();
        const QString typeName = parseIdentifier();
        skipSpaces();
        return typeName;
    }

    bool peekCastType()
    {
        const int originalPos = m_pos;
        if (!match('(')) {
            m_pos = originalPos;
            return false;
        }
        const QString typeName = parseTypeName();
        const bool isCast = isCastType(typeName) && match(')');
        m_pos = originalPos;
        return isCast;
    }

    QString parseCastType()
    {
        match('(');
        const QString typeName = parseTypeName();
        match(')');
        return typeName;
    }

    double callFunction(const QString &name, const QVector<double> &args)
    {
        if (args.size() == 1) {
            if (name == "sqrt") return args[0] >= 0.0 ? std::sqrt(args[0]) : invalid();
            if (name == "sin") return std::sin(args[0]);
            if (name == "cos") return std::cos(args[0]);
            if (name == "tan") return std::tan(args[0]);
            if (name == "abs" || name == "fabs") return std::fabs(args[0]);
        }
        if (args.size() == 2) {
            if (name == "pow") return std::pow(args[0], args[1]);
            if (name == "min") return qMin(args[0], args[1]);
            if (name == "max") return qMax(args[0], args[1]);
        }
        return invalid();
    }

    double applyCast(double value, const QString &typeName)
    {
        if (!isCastType(typeName) || !std::isfinite(value)) {
            m_ok = false;
            return 0.0;
        }
        if (typeName == "float" || typeName == "double") {
            return value;
        }
        if (typeName == "int" || typeName == "int8_t" || typeName == "int16_t" ||
            typeName == "int32_t" || typeName == "int64_t" || typeName == "uint8_t" ||
            typeName == "uint16_t" || typeName == "uint32_t" || typeName == "uint64_t") {
            return std::trunc(value);
        }
        m_ok = false;
        return 0.0;
    }

    bool isCastType(const QString &typeName) const
    {
        return typeName == "float" || typeName == "double" || typeName == "int" ||
               typeName == "int8_t" || typeName == "int16_t" ||
               typeName == "int32_t" || typeName == "int64_t" ||
               typeName == "uint8_t" || typeName == "uint16_t" ||
               typeName == "uint32_t" || typeName == "uint64_t";
    }

    double invalid()
    {
        m_ok = false;
        return 0.0;
    }

    void skipSpaces()
    {
        while (m_pos < m_expression.size() && m_expression[m_pos].isSpace()) {
            ++m_pos;
        }
    }

    bool match(QChar expected)
    {
        skipSpaces();
        if (m_pos < m_expression.size() && m_expression[m_pos] == expected) {
            ++m_pos;
            return true;
        }
        return false;
    }

    const QString &m_expression;
    const QHash<QString, double> &m_values;
    int m_pos = 0;
    bool m_ok = true;
};
}

QStringList DataParser::configurationSearchPaths()
{
    const QString fileName = "telemetry_config.json";
    QStringList paths;
    paths << QDir(QCoreApplication::applicationDirPath()).filePath(fileName);
    paths << QDir::current().filePath(fileName);
    paths << QDir(QCoreApplication::applicationDirPath()).filePath("config/" + fileName);
    paths << QDir::current().filePath("config/" + fileName);
    paths.removeDuplicates();
    return paths;
}

bool DataParser::loadDefaultConfiguration(QString *errorMessage)
{
    QString lastError;
    for (const QString &path : configurationSearchPaths()) {
        if (!QFileInfo::exists(path)) {
            continue;
        }
        if (loadConfiguration(path, &lastError)) {
            return true;
        }
    }

    if (errorMessage) {
        *errorMessage = lastError.isEmpty()
                            ? "Could not find telemetry_config.json"
                            : lastError;
    }
    return false;
}

bool DataParser::loadConfiguration(const QString &filePath, QString *errorMessage)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (errorMessage) {
            *errorMessage = "Failed to open configuration file: " + file.errorString();
        }
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        if (errorMessage) {
            *errorMessage = QString("Invalid JSON: %1").arg(parseError.errorString());
        }
        return false;
    }
    if (!document.isObject()) {
        if (errorMessage) {
            *errorMessage = "Configuration root must be a JSON object";
        }
        return false;
    }

    const QJsonObject root = document.object();
    QHash<QString, TelemetryFieldDef> telemetryFields;
    QList<TelemetryStructureDef> telemetryStructures;
    QHash<QString, TelemetryFieldDef> adcSampleFields;
    QList<TelemetryStructureDef> adcSampleStructures;
    if (!parsePacketFields(root, "telemetry_fields", &telemetryFields, errorMessage) ||
        !parsePacketStructures(root, "telemetry_structure", telemetryFields, &telemetryStructures, errorMessage) ||
        !parsePacketFields(root, "adc_sample_fields", &adcSampleFields, errorMessage) ||
        !parsePacketStructures(root, "adc_sample_structure", adcSampleFields, &adcSampleStructures, errorMessage)) {
        return false;
    }

    QJsonArray errorsJson;
    QJsonArray modesJson;
    QJsonArray fieldsJson;
    QJsonArray fields2Json;
    QJsonArray customFieldsJson;
    QJsonArray indicatorsJson;
    QJsonArray gaugesJson;
    if (!readObjectArray(root, "errors", &errorsJson, errorMessage) ||
        !readObjectArray(root, "modes", &modesJson, errorMessage) ||
        !readObjectArray(root, "fields", &fieldsJson, errorMessage) ||
        !readObjectArray(root, "fields2", &fields2Json, errorMessage) ||
        !readOptionalObjectArray(root, "custom_fields", &customFieldsJson, errorMessage) ||
        !readOptionalObjectArray(root, "indicators", &indicatorsJson, errorMessage) ||
        !readOptionalObjectArray(root, "gauges", &gaugesJson, errorMessage)) {
        return false;
    }

    QList<ErrorDef> errors;
    QList<ModeDef> modes;
    QList<FieldDef> fields;
    QList<FieldDef> fields2;
    QList<CustomFieldDef> customFields;
    QList<IndicatorDef> indicators;
    QList<GaugeDef> gauges;
    QHash<QString, QString> commandMap;
    if (!parseErrors(errorsJson, &errors, errorMessage) ||
        !parseModes(modesJson, &modes, errorMessage) ||
        !parseFields(fieldsJson, &fields, &commandMap, errorMessage) ||
        !parseFields(fields2Json, &fields2, &commandMap, errorMessage) ||
        !parseCustomFields(customFieldsJson, &customFields, errorMessage) ||
        !parseIndicators(indicatorsJson, &indicators, errorMessage) ||
        !parseGauges(gaugesJson, &gauges, errorMessage)) {
        return false;
    }

    m_errors = errors;
    m_modes = modes;
    m_fields = fields;
    m_fields2 = fields2;
    m_customFields = customFields;
    m_indicators = indicators;
    m_gauges = gauges;
    m_telemetryFields = telemetryFields;
    m_telemetryStructures = telemetryStructures;
    m_adcSampleFields = adcSampleFields;
    m_adcSampleStructures = adcSampleStructures;
    m_telemetrySyncBytes = m_telemetryFields.value("header").value.isEmpty()
                               ? SYNC_BYTES
                               : m_telemetryFields.value("header").value;
    m_adcSampleSyncBytes = m_adcSampleFields.value("header").value;
    m_displayToCmd = commandMap;
    m_configurationPath = QFileInfo(filePath).absoluteFilePath();
    m_buffer.clear();
    emit configurationChanged();
    return true;
}

void DataParser::parseData(const QByteArray &newData) {
    m_buffer.append(newData);
    int idx = 0;
    while (idx < m_buffer.size()) {
        int nextIdx;
        QHash<QString, double> values = tryParsePacket(idx, nextIdx);
        if (nextIdx > idx) {
            if (values.isEmpty()) {
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

const TelemetryStructureDef* DataParser::structureForVersion(quint8 version,
                                                             const QList<TelemetryStructureDef> &structures) const
{
    for (const TelemetryStructureDef &structure : structures) {
        if (structure.version == version) {
            return &structure;
        }
    }
    return nullptr;
}

int DataParser::minimumStructureSize(const TelemetryStructureDef &structure,
                                     const QHash<QString, TelemetryFieldDef> &fields) const
{
    int size = 0;
    for (const QString &fieldName : structure.fields) {
        const TelemetryFieldDef field = fields.value(fieldName);
        if (field.variableLength) {
            break;
        }
        size += field.length;
    }
    return size;
}

int DataParser::payloadLengthForMask(quint32 mask, const QList<FieldDef> &fields) const
{
    int length = 0;
    for (const FieldDef &field : fields) {
        if (mask & field.maskBit) {
            length += field.size;
        }
    }
    return length;
}

std::optional<PacketLayout> DataParser::buildPacketLayout(const QByteArray &data,
                                                          int startIdx,
                                                          bool requireComplete) const
{
    const int headerLength = m_telemetryFields.value("header").length;
    const int versionLength = m_telemetryFields.value("version").length;
    if (headerLength <= 0 || versionLength != 1 ||
        data.size() < startIdx + headerLength + versionLength) {
        return std::nullopt;
    }

    const quint8 version = static_cast<quint8>(data.at(startIdx + headerLength));
    const TelemetryStructureDef *structure = structureForVersion(version, m_telemetryStructures);
    if (!structure) {
        return std::nullopt;
    }

    PacketLayout layout;
    layout.structure = structure;
    layout.startPos = startIdx;
    int pos = startIdx;
    quint32 mask1 = 0;
    quint32 mask2 = 0;
    bool hasMask1 = false;
    bool hasMask2 = false;
    bool passedVariableField = false;

    for (const QString &fieldName : structure->fields) {
        const TelemetryFieldDef field = m_telemetryFields.value(fieldName);
        if (fieldName == "payload") {
            if (!hasMask1) {
                return std::nullopt;
            }
            passedVariableField = true;
            layout.payloadPos = pos;
            layout.payloadLength = payloadLengthForMask(mask1, m_fields);
            pos += layout.payloadLength;
            continue;
        }
        if (fieldName == "payload2") {
            if (!hasMask2) {
                return std::nullopt;
            }
            passedVariableField = true;
            layout.payload2Pos = pos;
            layout.payload2Length = payloadLengthForMask(mask2, m_fields2);
            pos += layout.payload2Length;
            continue;
        }
        if (field.variableLength || field.length <= 0) {
            return std::nullopt;
        }

        if (requireComplete && data.size() < pos + field.length) {
            return std::nullopt;
        }
        if (!requireComplete && !passedVariableField && data.size() < pos + field.length) {
            return std::nullopt;
        }

        if (fieldName == "header") {
            if (field.length != m_telemetrySyncBytes.size() ||
                data.mid(pos, field.length) != m_telemetrySyncBytes) {
                return std::nullopt;
            }
        } else if (fieldName == "version") {
            layout.versionPos = pos;
        } else if (fieldName == "errors") {
            layout.errorPos = pos;
        } else if (fieldName == "modes") {
            layout.modePos = pos;
        } else if (fieldName == "timestamp_high") {
            layout.timeHighPos = pos;
        } else if (fieldName == "timestamp_low") {
            layout.timeLowPos = pos;
        } else if (fieldName == "timestamp_us") {
            layout.timeUsPos = pos;
        } else if (fieldName == "mask") {
            layout.mask1Pos = pos;
            mask1 = qFromLittleEndian<quint32>(data.constData() + pos);
            hasMask1 = true;
        } else if (fieldName == "mask2") {
            layout.mask2Pos = pos;
            mask2 = qFromLittleEndian<quint32>(data.constData() + pos);
            hasMask2 = true;
        } else if (fieldName == "crc") {
            layout.crcPos = pos;
            layout.crcLength = field.length;
            layout.crcPolynomial = field.polynomial;
        }
        pos += field.length;
        if (layout.payloadPos < 0) {
            layout.fixedMetadataSize = pos - startIdx;
        }
    }

    layout.totalLength = pos - startIdx;
    if (requireComplete && data.size() < startIdx + layout.totalLength) {
        return std::nullopt;
    }

    return layout;
}

bool DataParser::validatePacketCrc(const QByteArray &data, const PacketLayout &layout) const
{
    if (layout.crcPos < 0) {
        return true;
    }
    if (layout.crcLength != 4 || data.size() < layout.crcPos + layout.crcLength) {
        return false;
    }

    const quint32 expected = qFromLittleEndian<quint32>(data.constData() + layout.crcPos);
    const quint32 calculated = calculateCrc32(data.constData() + layout.startPos,
                                             layout.crcPos - layout.startPos,
                                             layout.crcPolynomial);
    return expected == calculated;
}

quint32 DataParser::calculateCrc32(const char *data, int length, quint32 polynomial) const
{
    quint32 crc = 0xFFFFFFFFu;
    for (int i = 0; i < length; ++i) {
        crc ^= static_cast<quint32>(static_cast<quint8>(data[i])) << 24;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80000000u) ? ((crc << 1) ^ polynomial) : (crc << 1);
        }
    }
    return crc;
}

int DataParser::formatElementSize(const QString &format)
{
    if (format == "B") return 1;
    if (format == "H") return 2;
    if (format == "f") return 4;
    return 1;
}

double DataParser::timestampSeconds(bool hasTimestampUs,
                                    quint32 timestampUs,
                                    bool hasTimestampTicks,
                                    quint64 timestampTicks)
{
    if (hasTimestampUs) {
        return static_cast<double>(timestampUs) / 1000000.0;
    }
    if (hasTimestampTicks) {
        return static_cast<double>(timestampTicks) / 275000000.0;
    }
    return 0.0;
}

std::optional<PacketLayout> DataParser::buildAdcPacketLayout(const QByteArray &data,
                                                             int startIdx,
                                                             bool requireComplete) const
{
    const int headerLength = m_adcSampleFields.value("header").length;
    const int versionLength = m_adcSampleFields.value("version").length;
    if (headerLength <= 0 || versionLength != 1 ||
        data.size() < startIdx + headerLength + versionLength) {
        return std::nullopt;
    }

    const quint8 version = static_cast<quint8>(data.at(startIdx + headerLength));
    const TelemetryStructureDef *structure = structureForVersion(version, m_adcSampleStructures);
    if (!structure) {
        return std::nullopt;
    }

    PacketLayout layout;
    layout.structure = structure;
    layout.startPos = startIdx;
    int pos = startIdx;
    quint16 sampleCount = 0;
    bool hasSampleCount = false;

    for (const QString &fieldName : structure->fields) {
        const TelemetryFieldDef field = m_adcSampleFields.value(fieldName);
        if (fieldName == "payload") {
            if (!hasSampleCount) {
                return std::nullopt;
            }
            layout.payloadPos = pos;
            layout.payloadLength = static_cast<int>(sampleCount) * formatElementSize(field.format);
            pos += layout.payloadLength;
            continue;
        }
        if (field.variableLength || field.length <= 0) {
            return std::nullopt;
        }

        if (requireComplete && data.size() < pos + field.length) {
            return std::nullopt;
        }
        if (!requireComplete && data.size() < pos + field.length) {
            return std::nullopt;
        }

        if (fieldName == "header") {
            if (field.length != m_adcSampleSyncBytes.size() ||
                data.mid(pos, field.length) != m_adcSampleSyncBytes) {
                return std::nullopt;
            }
        } else if (fieldName == "version") {
            layout.versionPos = pos;
        } else if (fieldName == "adc_id") {
            layout.adcIdPos = pos;
        } else if (fieldName == "sample_count") {
            layout.sampleCountPos = pos;
            sampleCount = qFromLittleEndian<quint16>(data.constData() + pos);
            hasSampleCount = true;
        } else if (fieldName == "resolution_bit") {
            layout.resolutionBitPos = pos;
        } else if (fieldName == "sequence") {
            layout.sequencePos = pos;
        } else if (fieldName == "timestamp_high") {
            layout.timeHighPos = pos;
        } else if (fieldName == "timestamp_low") {
            layout.timeLowPos = pos;
        } else if (fieldName == "timestamp_us") {
            layout.timeUsPos = pos;
        } else if (fieldName == "shunt") {
            layout.shuntPos = pos;
        } else if (fieldName == "offset") {
            layout.offsetPos = pos;
        } else if (fieldName == "crc") {
            layout.crcPos = pos;
            layout.crcLength = field.length;
            layout.crcPolynomial = field.polynomial;
        }
        pos += field.length;
        if (layout.payloadPos < 0) {
            layout.fixedMetadataSize = pos - startIdx;
        }
    }

    layout.totalLength = pos - startIdx;
    if (requireComplete && data.size() < startIdx + layout.totalLength) {
        return std::nullopt;
    }

    return layout;
}

QHash<QString, double> DataParser::tryParsePacket(int startIdx, int &nextStartIdx) {
    QHash<QString, double> result;
    nextStartIdx = startIdx;

    // 查找帧头
    int headerPos = findNextFrameHeader(m_buffer, startIdx);
    if (headerPos == -1) {
        // 没有找到帧头，跳过所有已扫描的数据（但保留最后几个字节防止跨边界）
        nextStartIdx = m_buffer.size() - qMax(m_telemetrySyncBytes.size(), m_adcSampleSyncBytes.size()) + 1;
        if (nextStartIdx < startIdx) nextStartIdx = m_buffer.size();
        return result;
    }

    if (!m_adcSampleSyncBytes.isEmpty() &&
        m_buffer.mid(headerPos, m_adcSampleSyncBytes.size()) == m_adcSampleSyncBytes) {
        tryParseAdcPacket(headerPos, nextStartIdx);
        return result;
    }

    // 帧头位置确定了，检查是否有足够空间读取固定帧元数据
    if (m_buffer.size() < headerPos + minimumFrameSize())
        return result;

    if (!hasValidFrameMetadata(m_buffer, headerPos)) {
        nextStartIdx = headerPos + 1;
        return result;
    }

    const std::optional<PacketLayout> maybeLayout = buildPacketLayout(m_buffer, headerPos, true);
    if (!maybeLayout.has_value()) {
        return result;
    }
    const PacketLayout layout = maybeLayout.value();
    if (!validatePacketCrc(m_buffer, layout)) {
        nextStartIdx = headerPos + 1;
        return result;
    }

    const quint32 errorCode = qFromLittleEndian<quint32>(m_buffer.constData() + layout.errorPos);
    const quint8 controlMode = static_cast<quint8>(m_buffer.at(layout.modePos));
    const bool hasTimestampTicks = layout.timeHighPos >= 0 && layout.timeLowPos >= 0;
    const bool hasTimestampUs = layout.timeUsPos >= 0;
    const quint16 timeHigh = hasTimestampTicks ? qFromLittleEndian<quint16>(m_buffer.constData() + layout.timeHighPos) : 0;
    const quint32 timeLow = hasTimestampTicks ? qFromLittleEndian<quint32>(m_buffer.constData() + layout.timeLowPos) : 0;
    const quint64 timestampTicks = (static_cast<quint64>(timeHigh) << 32) | timeLow;
    const quint32 timestampUs = hasTimestampUs ? qFromLittleEndian<quint32>(m_buffer.constData() + layout.timeUsPos) : 0;
    const quint32 mask1 = qFromLittleEndian<quint32>(m_buffer.constData() + layout.mask1Pos);
    const quint32 mask2 = qFromLittleEndian<quint32>(m_buffer.constData() + layout.mask2Pos);

    int currentPos = layout.payloadPos;
    if (hasTimestampTicks) {
        result[TIMESTAMP_FIELD] = static_cast<double>(timestampTicks);
    }
    if (hasTimestampUs) {
        result[TIMESTAMP_US_FIELD] = static_cast<double>(timestampUs);
    }
    result[TIMESTAMP_SECONDS_FIELD] = timestampSeconds(hasTimestampUs, timestampUs, hasTimestampTicks, timestampTicks);

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

    for (const CustomFieldDef &field : m_customFields) {
        double value = 0.0;
        ExpressionParser parser(field.expression, result);
        if (parser.evaluate(&value)) {
            result[field.name] = value;
        }
    }

    nextStartIdx = headerPos + layout.totalLength;
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

bool DataParser::tryParseAdcPacket(int headerPos, int &nextStartIdx)
{
    nextStartIdx = headerPos;
    const std::optional<PacketLayout> maybeLayout = buildAdcPacketLayout(m_buffer, headerPos, true);
    if (!maybeLayout.has_value()) {
        return false;
    }
    const PacketLayout layout = maybeLayout.value();
    if (!validatePacketCrc(m_buffer, layout)) {
        nextStartIdx = headerPos + 1;
        return false;
    }
    if (layout.adcIdPos < 0 || layout.sampleCountPos < 0 || layout.resolutionBitPos < 0 ||
        layout.sequencePos < 0 || layout.shuntPos < 0 || layout.offsetPos < 0 ||
        layout.payloadPos < 0) {
        nextStartIdx = headerPos + 1;
        return false;
    }

    AdcSamplePacket packet;
    packet.version = layout.versionPos >= 0 ? static_cast<quint8>(m_buffer.at(layout.versionPos)) : 0;
    packet.adcId = static_cast<quint8>(m_buffer.at(layout.adcIdPos));
    packet.resolutionBit = static_cast<quint8>(m_buffer.at(layout.resolutionBitPos));
    packet.sequence = qFromLittleEndian<quint32>(m_buffer.constData() + layout.sequencePos);
    packet.shunt = qFromLittleEndian<float>(m_buffer.constData() + layout.shuntPos);
    packet.offset = qFromLittleEndian<float>(m_buffer.constData() + layout.offsetPos);

    if (layout.timeHighPos >= 0 && layout.timeLowPos >= 0) {
        const quint16 timeHigh = qFromLittleEndian<quint16>(m_buffer.constData() + layout.timeHighPos);
        const quint32 timeLow = qFromLittleEndian<quint32>(m_buffer.constData() + layout.timeLowPos);
        packet.timestampTicks = (static_cast<quint64>(timeHigh) << 32) | timeLow;
        packet.hasTimestampTicks = true;
    }
    if (layout.timeUsPos >= 0) {
        packet.timestampUs = qFromLittleEndian<quint32>(m_buffer.constData() + layout.timeUsPos);
        packet.hasTimestampUs = true;
    }
    packet.timestampSeconds = timestampSeconds(packet.hasTimestampUs,
                                               packet.timestampUs,
                                               packet.hasTimestampTicks,
                                               packet.timestampTicks);

    const quint16 sampleCount = qFromLittleEndian<quint16>(m_buffer.constData() + layout.sampleCountPos);
    packet.samples.reserve(sampleCount);
    for (int i = 0; i < sampleCount; ++i) {
        const int pos = layout.payloadPos + i * static_cast<int>(sizeof(quint16));
        if (pos + static_cast<int>(sizeof(quint16)) > headerPos + layout.totalLength) {
            break;
        }
        packet.samples.append(qFromLittleEndian<quint16>(m_buffer.constData() + pos));
    }

    nextStartIdx = headerPos + layout.totalLength;
    emit adcSampleReceived(packet);
    return true;
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
    for (const auto &f : m_customFields)
        names << f.name;
    return names;
}

QString DataParser::getCommandNameForField(const QString &displayName) const {
    return m_displayToCmd.value(displayName);
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
    for (const CustomFieldDef &field : m_customFields) {
        if (field.name == fieldName) {
            return true;
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

quint32 DataParser::getErrorMaskForName(const QString &errorName) const {
    for (const ErrorDef &error : m_errors) {
        if (error.name == errorName) {
            return error.maskBit;
        }
    }
    return 0;
}

quint32 DataParser::getErrorMaskForType(const QString &errorType) const {
    quint32 mask = 0;
    for (const ErrorDef &error : m_errors) {
        if (error.type == errorType) {
            mask |= error.maskBit;
        }
    }
    return mask;
}

std::optional<quint8> DataParser::getControlModeValueForName(const QString &modeName) const {
    for (const ModeDef &modeDef : m_modes) {
        if (modeDef.name == modeName) {
            return modeDef.value;
        }
    }
    return std::nullopt;
}

int DataParser::minimumFrameSize() const {
    int minimumSize = std::numeric_limits<int>::max();
    for (const TelemetryStructureDef &structure : m_telemetryStructures) {
        const int size = minimumStructureSize(structure, m_telemetryFields);
        if (size > 0) {
            minimumSize = qMin(minimumSize, size);
        }
    }
    return minimumSize == std::numeric_limits<int>::max() ? 2 + 1 : minimumSize;
}

bool DataParser::hasValidFrameMetadata(const QByteArray &data, int startIdx) const {
    const std::optional<PacketLayout> maybeLayout = buildPacketLayout(data, startIdx, false);
    if (!maybeLayout.has_value())
        return false;

    const PacketLayout layout = maybeLayout.value();
    if (layout.errorPos < 0 || layout.modePos < 0 || layout.mask1Pos < 0 || layout.mask2Pos < 0)
        return false;

    const quint32 errorCode = qFromLittleEndian<quint32>(data.constData() + layout.errorPos);
    const quint8 controlMode = static_cast<quint8>(data.at(layout.modePos));
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

    const quint32 mask1 = qFromLittleEndian<quint32>(data.constData() + layout.mask1Pos);
    const quint32 mask2 = qFromLittleEndian<quint32>(data.constData() + layout.mask2Pos);
    return (mask1 & ~validMask1Bits) == 0 && (mask2 & ~validMask2Bits) == 0;
}

int DataParser::getFrameLength(const QByteArray &data, int startIdx) const
{
    if (!m_adcSampleSyncBytes.isEmpty() &&
        data.size() >= startIdx + m_adcSampleSyncBytes.size() &&
        data.mid(startIdx, m_adcSampleSyncBytes.size()) == m_adcSampleSyncBytes) {
        const std::optional<PacketLayout> maybeLayout = buildAdcPacketLayout(data, startIdx, true);
        return maybeLayout.has_value() ? maybeLayout->totalLength : -1;
    }

    // Check if the data is sufficient to contain the fixed frame metadata.
    if (data.size() < startIdx + minimumFrameSize())
        return -1;

    if (!hasValidFrameMetadata(data, startIdx))
        return -1;

    const std::optional<PacketLayout> maybeLayout = buildPacketLayout(data, startIdx, true);
    if (!maybeLayout.has_value())
        return -1;   // Insufficient data

    return maybeLayout->totalLength;
}

int DataParser::findNextFrameHeader(const QByteArray &data, int startIdx) const
{
    int best = -1;
    const QList<QByteArray> headers{m_telemetrySyncBytes, m_adcSampleSyncBytes};
    for (const QByteArray &header : headers) {
        if (header.isEmpty()) {
            continue;
        }
        const int pos = data.indexOf(header, startIdx);
        if (pos >= 0 && (best < 0 || pos < best)) {
            best = pos;
        }
    }
    return best;
}

int DataParser::partialFrameHeaderLength(const QByteArray &data) const
{
    int best = 0;
    const QList<QByteArray> headers{m_telemetrySyncBytes, m_adcSampleSyncBytes};
    for (const QByteArray &header : headers) {
        for (int len = 1; len < header.size(); ++len) {
            if (data.size() >= len && data.right(len) == header.left(len)) {
                best = qMax(best, len);
            }
        }
    }
    return best;
}

void DataParser::addCommandMapping(const QString &displayName, const QString &commandName) {
    m_displayToCmd[displayName] = commandName;
}
