#include "audiolevelmeter.h"

#include <QFontMetrics>
#include <QPainter>
#include <QPolygonF>
#include <QSizePolicy>
#include <QVector>
#include <QtMath>

#include <algorithm>
#include <functional>
#include <utility>

AudioLevelMeter::AudioLevelMeter(QWidget *parent)
    : QWidget(parent),
      m_displayMode(DisplayMode::VerticalBar),
      m_minimum(0.0),
      m_maximum(100.0),
      m_value(0.0),
      m_displayValue(0.0),
      m_peakValue(0.0),
      m_warningThreshold(65.0),
      m_criticalThreshold(85.0),
      m_thresholdHysteresisPercent(0.0),
      m_divisionCount(5),
      m_valueDecimals(2),
      m_colorState(ColorState::Normal),
      m_peakHoldMs(1000),
      m_peakHoldRemainingMs(0),
      m_peakTrackingEnabled(true),
      m_secondaryColorTracksPrimary(true)
{
    // Two lightweight timers decouple incoming telemetry from visual behavior:
    // one handles peak-hold decay and one throttles the displayed numeric value.
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMinimumSize(92, 180);

    m_peakDecayTimer.setInterval(40);
    connect(&m_peakDecayTimer, &QTimer::timeout, this, [this]() {
        if (m_peakHoldRemainingMs > 0) {
            m_peakHoldRemainingMs -= m_peakDecayTimer.interval();
            return;
        }

        const double decayStep = (m_maximum - m_minimum) * 0.015;
        if (m_minimum < 0.0 && m_maximum > 0.0) {
            const double currentLevel = levelForThreshold(m_value);
            const double peakLevel = levelForThreshold(m_peakValue);
            const double decayedLevel = qMax(currentLevel, peakLevel - decayStep);
            const double sign = (currentLevel >= decayedLevel)
                ? ((m_value < 0.0) ? -1.0 : 1.0)
                : ((m_peakValue < 0.0) ? -1.0 : 1.0);
            m_peakValue = sign * decayedLevel;
        } else {
            m_peakValue = qMax(m_value, m_peakValue - decayStep);
        }
        update();

        if (qFuzzyCompare(levelForThreshold(m_peakValue) + 1.0,
                          levelForThreshold(m_value) + 1.0)) {
            m_peakDecayTimer.stop();
        }
    });

    m_valueDisplayTimer.setInterval(500);
    connect(&m_valueDisplayTimer, &QTimer::timeout, this, [this]() {
        m_displayValue = m_value;
        update();
    });
    m_valueDisplayTimer.start();
}

void AudioLevelMeter::setDisplayMode(DisplayMode mode)
{
    if (m_displayMode == mode) {
        return;
    }

    m_displayMode = mode;
    if (m_displayMode == DisplayMode::Circular) {
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    } else {
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }
    updateGeometry();
    update();
}

void AudioLevelMeter::setTitle(const QString &title)
{
    if (m_title == title) {
        return;
    }

    m_title = title;
    update();
}

void AudioLevelMeter::setUnit(const QString &unit)
{
    if (m_unit == unit) {
        return;
    }

    m_unit = unit;
    update();
}

void AudioLevelMeter::setRange(double minimum, double maximum)
{
    if (maximum <= minimum) {
        return;
    }

    // Changing range also rebuilds default thresholds as percentages of the new
    // span, then clamps all retained values so painting remains well-defined.
    m_minimum = minimum;
    m_maximum = maximum;
    m_value = clampedValue(m_value);
    m_displayValue = clampedValue(m_displayValue);
    m_peakValue = (m_minimum < 0.0 && m_maximum > 0.0)
        ? qBound(m_minimum, m_peakValue, m_maximum)
        : clampedValue(m_peakValue);
    m_warningThreshold = m_minimum + (m_maximum - m_minimum) * 0.65;
    m_criticalThreshold = m_minimum + (m_maximum - m_minimum) * 0.85;
    updateColorState(m_value);
    update();
}

void AudioLevelMeter::setThresholds(double warningThreshold, double criticalThreshold)
{
    m_warningThreshold = warningThreshold;
    m_criticalThreshold = criticalThreshold;
    updateColorState(m_value);
    update();
}

void AudioLevelMeter::setThresholdHysteresisPercent(double percent)
{
    m_thresholdHysteresisPercent = qMax(0.0, percent);
    updateColorState(m_value);
    update();
}

void AudioLevelMeter::setDivisionCount(int divisionCount)
{
    m_divisionCount = qMax(1, divisionCount);
    update();
}

void AudioLevelMeter::setValueDecimals(int decimals)
{
    m_valueDecimals = qBound(0, decimals, 6);
    update();
}

void AudioLevelMeter::setMajorTickCount(int tickCount)
{
    setDivisionCount(qMax(1, tickCount - 1));
}

void AudioLevelMeter::setPeakHoldMs(int holdMs)
{
    m_peakHoldMs = qMax(0, holdMs);
}

void AudioLevelMeter::setPeakTrackingEnabled(bool enabled)
{
    if (m_peakTrackingEnabled == enabled) {
        return;
    }

    m_peakTrackingEnabled = enabled;
    if (!m_peakTrackingEnabled) {
        m_peakDecayTimer.stop();
        m_peakHoldRemainingMs = 0;
        m_peakValue = clampedValue(m_value);
    }
    update();
}

void AudioLevelMeter::setSecondaryColorTracksPrimary(bool enabled)
{
    if (m_secondaryColorTracksPrimary == enabled) {
        return;
    }

    m_secondaryColorTracksPrimary = enabled;
    update();
}

void AudioLevelMeter::setPeakValue(double value)
{
    m_peakValue = clampedValue(value);
    if (!m_peakTrackingEnabled) {
        m_peakDecayTimer.stop();
    }
    update();
}

void AudioLevelMeter::setValue(double value)
{
    m_value = clampedValue(value);
    updateColorState(m_value);

    // Peak value is based on magnitude, allowing bipolar gauges to hold the
    // largest positive or negative excursion until decay begins.
    if (m_peakTrackingEnabled) {
        if (qAbs(m_value) > qAbs(m_peakValue)) {
            m_peakValue = m_value;
            m_peakHoldRemainingMs = m_peakHoldMs;
        }
        if (!m_peakDecayTimer.isActive()) {
            m_peakDecayTimer.start();
        }
    }

    update();
}

QSize AudioLevelMeter::sizeHint() const
{
    if (m_displayMode == DisplayMode::Circular) {
        return QSize(343, 343);
    }

    return QSize(92, 240);
}

QSize AudioLevelMeter::minimumSizeHint() const
{
    if (m_displayMode == DisplayMode::Circular) {
        return QSize(304, 304);
    }

    return QSize(92, 160);
}

void AudioLevelMeter::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillRect(rect(), palette().window());

    if (m_displayMode == DisplayMode::Circular) {
        const QRect area = rect().adjusted(2, 2, -2, -2);
        paintCircularGauge(painter, area);
    } else {
        const QRect area = rect().adjusted(2, 20, -2, -6);
        paintVerticalBar(painter, area);
    }
}

void AudioLevelMeter::paintVerticalBar(QPainter &painter, const QRect &area)
{
    // Overall content margins for the whole widget drawing area.
    // Adjust the first and third values to change the left/right margins
    // for the entire gauges area. The current margins are optimized for 3 gauges layout.
    const QColor primaryText = palette().color(QPalette::WindowText);
    const QColor secondaryText = primaryText.darker(150);
    const QColor gridColor = primaryText.darker(220);
    const QColor zeroLineColor = primaryText.lighter(135);
    const QColor frameColor = primaryText.darker(170);

    QFont titleFont = painter.font();
    titleFont.setBold(true);
    painter.setFont(titleFont);
    painter.setPen(primaryText);

    QFont valueFont = painter.font();
    valueFont.setBold(false);
    painter.setFont(valueFont);
    const int valueHeight = 24;
    painter.drawText(QRect(area.left(), area.top(), area.width(), valueHeight),
                     Qt::AlignCenter,
                     formattedValue(m_displayValue));

    painter.setFont(titleFont);
    const int titleHeight = 22;
    painter.drawText(QRect(area.left(), area.bottom() - titleHeight + 1, area.width(), titleHeight),
                     Qt::AlignCenter,
                     m_title);

    painter.setFont(valueFont);
    // Gauge/scale layout tuning:
    // - scaleWidth controls how much horizontal space is reserved for the labels/ticks.
    // - gap controls the spacing between the labels and the gauge body.
    // - configuredMeterWidth is the preferred gauge width from sizeHint().
    // - availableMeterWidth limits the gauge width to what actually fits in the widget.
    // - meterWidth is the final gauge width that gets drawn.
    // Change these values if you want to make the gauge body wider or narrower.
    const int scaleWidth = 48;
    const int gap = 6;
    const int configuredMeterWidth = qMax(12, sizeHint().width() - scaleWidth - gap);
    const int availableMeterWidth = qMax(12, area.width() - scaleWidth - gap);
    const int meterWidth = qMin(configuredMeterWidth, availableMeterWidth);
    const int meterAreaLeft = area.left() + scaleWidth + gap;
    const int meterAreaWidth = qMax(12, area.width() - scaleWidth - gap);
    const int horizontalOffset = meterWidth / 2;
    const int meterLeft = meterAreaLeft + qMax(0, (meterAreaWidth - meterWidth) / 2) - horizontalOffset;
    const int scaleLeft = area.left() - horizontalOffset;
    // meterRect defines the actual gauge body rectangle:
    // - the second argument controls the top position
    // - the third argument is the gauge width
    // - the fourth argument is the gauge height
    // Increase/decrease these offsets if you want to manually tune the gauge size.
    const QRect meterRect(meterLeft,
                          area.top() + valueHeight + 4,
                          meterWidth,
                          area.height() - titleHeight - valueHeight - 10);

    const QRectF trackRect = QRectF(meterRect).adjusted(0.5, 0.5, -0.5, -0.5);
    painter.setPen(QPen(frameColor, 1));
    painter.setBrush(QColor(28, 30, 33));
    painter.drawRoundedRect(trackRect, 4, 4);

    const int zeroY = yForValue(meterRect, 0.0);
    const int valueY = yForValue(meterRect, m_value);
    QRect fillRect(meterRect.left() + 3,
                   qMin(zeroY, valueY),
                   meterRect.width() - 5,
                   qAbs(zeroY - valueY));

    if (fillRect.height() > 0) {
        painter.fillRect(fillRect, fillColorForValue(m_value));
    }

    // Build evenly spaced major ticks and add a zero tick when the configured
    // range crosses zero but the regular divisions do not land exactly on it.
    QVector<double> tickValues;
    tickValues.reserve(m_divisionCount + 2);
    for (int i = 0; i <= m_divisionCount; ++i) {
        const double tickValue = m_maximum - i * (m_maximum - m_minimum) / double(m_divisionCount);
        tickValues.append(tickValue);
    }

    bool hasZeroTick = false;
    for (double tickValue : std::as_const(tickValues)) {
        if (qFuzzyIsNull(tickValue)) {
            hasZeroTick = true;
            break;
        }
    }
    if (!hasZeroTick && m_minimum <= 0.0 && m_maximum >= 0.0) {
        tickValues.append(0.0);
        std::sort(tickValues.begin(), tickValues.end(), std::greater<double>());
    }

    painter.setPen(QPen(gridColor, 1));
    for (double tickValue : std::as_const(tickValues)) {
        if (qFuzzyCompare(tickValue + 1.0, m_minimum + 1.0) ||
            qFuzzyCompare(tickValue + 1.0, m_maximum + 1.0)) {
            continue;
        }
        const int y = yForValue(meterRect, tickValue);
        painter.drawLine(meterRect.left() + 3, y, meterRect.right() - 3, y);
    }

    painter.setPen(QPen(zeroLineColor, 1));
    painter.drawLine(meterRect.left() + 2, zeroY, meterRect.right() - 2, zeroY);

    const int peakY = yForValue(meterRect, m_peakValue);
    const QColor markerColor = secondaryMarkerColor();
    const int markerHalfHeight = qMax(4, meterRect.width() / 6);
    const int markerDepth = qMax(5, meterRect.width() / 5);
    const int centerHalfHeight = qMax(1, markerHalfHeight / 3);
    QPolygonF marker;
    marker << QPointF(meterRect.left() + 1, peakY - markerHalfHeight)
           << QPointF(meterRect.left() + 1, peakY + markerHalfHeight)
           << QPointF(meterRect.left() + markerDepth, peakY + centerHalfHeight)
           << QPointF(meterRect.right() - markerDepth, peakY + centerHalfHeight)
           << QPointF(meterRect.right() - 1, peakY + markerHalfHeight)
           << QPointF(meterRect.right() - 1, peakY - markerHalfHeight)
           << QPointF(meterRect.right() - markerDepth, peakY - centerHalfHeight)
           << QPointF(meterRect.left() + markerDepth, peakY - centerHalfHeight);
    painter.setPen(QPen(palette().color(QPalette::Window), 1));
    painter.setBrush(markerColor);
    painter.drawPolygon(marker);

    painter.setPen(secondaryText);
    const QFontMetrics fm(painter.font());
    for (double tickValue : std::as_const(tickValues)) {
        const int y = yForValue(meterRect, tickValue);
        const QString label = QString::number(tickValue, 'f', (m_maximum - m_minimum <= 10.0) ? 1 : 0);

        painter.drawLine(meterRect.left() - 5, y, meterRect.left() - 1, y);
        painter.drawText(QRect(scaleLeft, y - fm.height() / 2, scaleWidth, fm.height()),
                         Qt::AlignRight | Qt::AlignVCenter,
                         label);
    }
}

QVector<double> AudioLevelMeter::tickValues() const
{
    QVector<double> values;
    values.reserve(m_divisionCount + 2);
    for (int i = 0; i <= m_divisionCount; ++i) {
        values.append(m_minimum + i * (m_maximum - m_minimum) / double(m_divisionCount));
    }
    if (m_minimum < 0.0 && m_maximum > 0.0) {
        bool hasZero = false;
        for (double value : std::as_const(values)) {
            if (qFuzzyIsNull(value)) {
                hasZero = true;
                break;
            }
        }
        if (!hasZero) {
            values.append(0.0);
            std::sort(values.begin(), values.end());
        }
    }
    return values;
}

void AudioLevelMeter::paintCircularGauge(QPainter &painter, const QRect &area)
{
    const QColor windowColor = palette().color(QPalette::Window);
    const QColor primaryText = palette().color(QPalette::WindowText);
    const bool darkMode = windowColor.lightness() < 128;
    const QColor secondaryText = darkMode ? primaryText.darker(115) : primaryText.darker(150);
    const QColor gridColor = darkMode ? primaryText.darker(150) : primaryText.darker(220);
    const QColor frameColor = darkMode ? primaryText.darker(135) : primaryText.darker(170);
    const QColor trackColor = darkMode ? windowColor.lighter(135) : QColor(28, 30, 33);

    QFont titleFont = painter.font();
    titleFont.setBold(true);

    QFont valueFont = painter.font();
    valueFont.setBold(false);

    const int titleHeight = 18;
    const int valueHeight = 24;
    const int textGap = 2;
    constexpr double kArcRadiusFactor = 0.41;
    constexpr double kMarkerOuterFactor = kArcRadiusFactor + 2.5 / 17.0;
    const double sweepMargin = 5.0;
    const double maxSideForWidth = (area.width() - sweepMargin * 2.0) / (kMarkerOuterFactor * 2.0);
    const double maxSideForHeight = (area.height() - sweepMargin * 2.0) / (kMarkerOuterFactor * 2.0);
    const int side = qMax(90, qFloor(qMin(maxSideForWidth, maxSideForHeight)));
    const double hookWidth = qMax(8.0, side / 17.0);
    const double arcRadius = side * kArcRadiusFactor;
    const double sweepRadius = arcRadius + hookWidth * 2.5;
    const QPointF center(area.center().x(), area.top() + sweepMargin + sweepRadius);
    const QRectF dialRect(center.x() - side / 2.0,
                          center.y() - side / 2.0,
                          side,
                          side);
    const double radius = side * 0.44;
    const double tickOuterRadius = side * 0.48;
    const double tickInnerRadius = side * 0.41;
    const double labelRadius = side * 0.31;
    const QRectF arcRect(center.x() - arcRadius,
                         center.y() - arcRadius,
                         arcRadius * 2.0,
                         arcRadius * 2.0);

    painter.setPen(QPen(frameColor, hookWidth + 3, Qt::SolidLine, Qt::RoundCap));
    painter.drawArc(arcRect, qRound(225.0 * 16.0), qRound(-270.0 * 16.0));
    painter.setPen(QPen(trackColor, hookWidth, Qt::SolidLine, Qt::RoundCap));
    painter.drawArc(arcRect, qRound(225.0 * 16.0), qRound(-270.0 * 16.0));

    const double zeroValue = clampedValue(0.0);
    const double hookStartAngle = angleForValue(zeroValue);
    const double hookEndAngle = angleForValue(m_value);
    const double hookSweep = hookEndAngle - hookStartAngle;
    if (!qFuzzyIsNull(hookSweep)) {
        painter.setPen(QPen(fillColorForValue(m_value), hookWidth, Qt::SolidLine, Qt::RoundCap));
        painter.drawArc(arcRect, qRound(hookStartAngle * 16.0), qRound(hookSweep * 16.0));
    }

    QFont labelFont = valueFont;
    labelFont.setPointSize(qMax(7, qMin(valueFont.pointSize(), side / 18)));
    painter.setFont(labelFont);
    painter.setPen(QPen(gridColor, 1));
    const QFontMetrics fm(painter.font());
    const QVector<double> ticks = tickValues();
    const int targetLabelCount = qBound(9, side / 14, 17);
    const int labelStride = qMax(1, qCeil(double(qMax(1, ticks.size() - 1)) / double(targetLabelCount - 1)));
    auto circularLabel = [this](double value) {
        if (qAbs(value) >= 1000.0) {
            const double scaled = value / 1000.0;
            const int decimals = qFuzzyIsNull(scaled - qRound(scaled)) ? 0 : 1;
            return QStringLiteral("%1k").arg(scaled, 0, 'f', decimals);
        }
        return QString::number(value, 'f', (m_maximum - m_minimum <= 10.0) ? 1 : 0);
    };
    for (int i = 0; i < ticks.size(); ++i) {
        const double tickValue = ticks.at(i);
        const double angle = angleForValue(tickValue);
        const QPointF inner = pointOnCircle(center, tickInnerRadius, angle);
        const QPointF outer = pointOnCircle(center, tickOuterRadius, angle);
        painter.drawLine(inner, outer);

        const bool isZeroTick = qFuzzyIsNull(tickValue);
        if (!isZeroTick && i != 0 && i != ticks.size() - 1 && i % labelStride != 0) {
            continue;
        }

        const QString label = circularLabel(tickValue);
        const int labelWidth = qMax(34, fm.horizontalAdvance(label) + 8);
        const QPointF labelCenter = pointOnCircle(center, labelRadius, angle);
        const QRectF labelRect(labelCenter.x() - labelWidth / 2.0,
                               labelCenter.y() - fm.height() / 2.0,
                               labelWidth,
                               fm.height());
        painter.setPen(secondaryText);
        painter.drawText(labelRect, Qt::AlignCenter, label);
        painter.setPen(QPen(gridColor, 1));
    }

    const QColor needleColor = fillColorForValue(m_value);
    drawNeedle(painter, center, angleForValue(m_value), radius * 0.82, needleColor, qMax(3.0, side / 42.0));

    drawSecondaryArrow(painter,
                       center,
                       angleForValue(m_peakValue),
                       arcRadius + hookWidth * 0.9,
                       arcRadius + hookWidth * 2.5,
                       secondaryMarkerColor());

    painter.setPen(QPen(frameColor, 1));
    painter.setBrush(windowColor);
    painter.drawEllipse(center, qMax(4.0, side / 34.0), qMax(4.0, side / 34.0));

    const double lcdWidth = qBound(74.0, side * 0.38, 118.0);
    const double lowerTextOffset = valueHeight * 0.75;
    const QRectF lcdRect(center.x() - lcdWidth / 2.0,
                         center.y() + arcRadius * 0.44 + lowerTextOffset,
                         lcdWidth,
                         valueHeight);
    drawLcdValue(painter,
                 lcdRect,
                 formattedNumber(m_displayValue),
                 m_unit,
                 fillColorForValue(m_value));

    const int titleTop = qRound(lcdRect.bottom()) + textGap;
    painter.setFont(titleFont);
    painter.setPen(primaryText);
    painter.drawText(QRect(area.left(), titleTop, area.width(), titleHeight),
                     Qt::AlignCenter,
                     m_title);
}

double AudioLevelMeter::clampedValue(double value) const
{
    return qBound(m_minimum, value, m_maximum);
}

double AudioLevelMeter::normalizedValue(double value) const
{
    if (m_maximum <= m_minimum) {
        return 0.0;
    }

    return (clampedValue(value) - m_minimum) / (m_maximum - m_minimum);
}

double AudioLevelMeter::angleForValue(double value) const
{
    return 225.0 - normalizedValue(value) * 270.0;
}

QPointF AudioLevelMeter::pointOnCircle(const QPointF &center, double radius, double angleDegrees) const
{
    const double radians = qDegreesToRadians(angleDegrees);
    return QPointF(center.x() + std::cos(radians) * radius,
                   center.y() - std::sin(radians) * radius);
}

int AudioLevelMeter::yForValue(const QRect &meterRect, double value) const
{
    return meterRect.bottom() - qRound(meterRect.height() * normalizedValue(value));
}

void AudioLevelMeter::drawNeedle(QPainter &painter,
                                 const QPointF &center,
                                 double angleDegrees,
                                 double length,
                                 const QColor &color,
                                 double width)
{
    const QPointF tip = pointOnCircle(center, length, angleDegrees);
    const QPointF tail = pointOnCircle(center, -length * 0.12, angleDegrees);
    painter.setPen(QPen(color, width, Qt::SolidLine, Qt::RoundCap));
    painter.drawLine(tail, tip);

    const QPointF left = pointOnCircle(tip, width * 2.2, angleDegrees + 150.0);
    const QPointF right = pointOnCircle(tip, width * 2.2, angleDegrees - 150.0);
    QPolygonF arrowHead;
    arrowHead << tip << left << right;
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawPolygon(arrowHead);
}

void AudioLevelMeter::drawSecondaryArrow(QPainter &painter,
                                         const QPointF &center,
                                         double angleDegrees,
                                         double innerRadius,
                                         double outerRadius,
                                         const QColor &color)
{
    const QPointF tip = pointOnCircle(center, innerRadius, angleDegrees);
    const QPointF baseCenter = pointOnCircle(center, outerRadius, angleDegrees);
    const double markerWidth = qMax(10.0, (outerRadius - innerRadius) * 0.85);
    const QPointF left = pointOnCircle(baseCenter, markerWidth * 0.5, angleDegrees + 90.0);
    const QPointF right = pointOnCircle(baseCenter, markerWidth * 0.5, angleDegrees - 90.0);
    QPolygonF marker;
    marker << tip << left << right;

    const QColor windowColor = palette().color(QPalette::Window);
    const QColor textColor = palette().color(QPalette::WindowText);
    const QColor outlineColor = windowColor.lightness() < 128 ? textColor : windowColor;
    painter.setPen(QPen(outlineColor, 1.5));
    painter.setBrush(color);
    painter.drawPolygon(marker);
}

void AudioLevelMeter::drawLcdValue(QPainter &painter,
                                   const QRectF &rect,
                                   const QString &numberText,
                                   const QString &unitText,
                                   const QColor &textColor)
{
    painter.save();
    painter.setPen(QPen(QColor(8, 20, 12), 1));
    painter.setBrush(QColor(6, 18, 10));
    painter.drawRoundedRect(rect, 3, 3);

    const QRectF inner = rect.adjusted(3, 2, -3, -2);
    painter.setPen(QPen(QColor(42, 75, 48), 1));
    painter.drawLine(inner.topLeft(), inner.topRight());
    painter.drawLine(inner.topLeft(), inner.bottomLeft());

    QFont lcdFont(QStringLiteral("Consolas"));
    lcdFont.setStyleHint(QFont::Monospace);
    lcdFont.setBold(true);
    lcdFont.setPointSize(qMax(7, int(rect.height() * 0.46)));

    QFont unitFont = lcdFont;
    unitFont.setPointSize(qMax(7, int(lcdFont.pointSize() * 0.78)));
    const QFontMetrics unitFm(unitFont);
    const double unitWidth = unitText.isEmpty()
        ? 0.0
        : qMin<double>(inner.width() * 0.34, unitFm.horizontalAdvance(unitText) + 4.0);
    const QRectF unitRect(inner.right() - unitWidth,
                          inner.top(),
                          unitWidth,
                          inner.height());
    const QRectF numberRect(inner.left(),
                            inner.top(),
                            qMax(1.0, inner.width() - unitWidth - (unitText.isEmpty() ? 0.0 : 2.0)),
                            inner.height());

    QFont numberFont = lcdFont;
    QFontMetrics numberFm(numberFont);
    const int normalWidth = numberFm.horizontalAdvance(numberText);
    if (normalWidth > numberRect.width()) {
        const int stretch = qBound(50,
                                   int((numberRect.width() / double(qMax(1, normalWidth))) * 100.0),
                                   100);
        numberFont.setStretch(stretch);
    }

    painter.setFont(numberFont);
    painter.setPen(textColor);
    painter.drawText(numberRect, Qt::AlignCenter, numberText);
    if (!unitText.isEmpty()) {
        painter.setFont(unitFont);
        const QColor windowColor = palette().color(QPalette::Window);
        const QColor unitColor = windowColor.lightness() < 128
            ? palette().color(QPalette::WindowText)
            : QColor(245, 245, 245);
        painter.setPen(unitColor);
        painter.drawText(unitRect, Qt::AlignRight | Qt::AlignVCenter, unitText);
    }
    painter.restore();
}

QString AudioLevelMeter::formattedNumber(double value) const
{
    return QString::number(value, 'f', m_valueDecimals);
}

QString AudioLevelMeter::formattedValue(double value) const
{
    const QString suffix = m_unit.isEmpty() ? QString() : QStringLiteral(" %1").arg(m_unit);
    return QStringLiteral("%1%2").arg(formattedNumber(value), suffix);
}

double AudioLevelMeter::levelForThreshold(double value) const
{
    if (m_minimum < 0.0 && m_maximum > 0.0) {
        return qAbs(value);
    }

    return value;
}

double AudioLevelMeter::hysteresisAmount() const
{
    const double maxMagnitude = qMax(qAbs(m_minimum), qAbs(m_maximum));
    return maxMagnitude * (m_thresholdHysteresisPercent / 100.0);
}

void AudioLevelMeter::updateColorState(double value)
{
    // Hysteresis applies only when leaving Warning/Critical states. Entering a
    // higher state remains immediate so dangerous values are highlighted quickly.
    const double level = levelForThreshold(value);
    const double warning = qMin(m_warningThreshold, m_criticalThreshold);
    const double critical = qMax(m_warningThreshold, m_criticalThreshold);
    const double hysteresis = hysteresisAmount();

    switch (m_colorState) {
    case ColorState::Normal:
        if (level >= critical) {
            m_colorState = ColorState::Critical;
        } else if (level >= warning) {
            m_colorState = ColorState::Warning;
        }
        break;
    case ColorState::Warning:
        if (level >= critical) {
            m_colorState = ColorState::Critical;
        } else if (level < warning - hysteresis) {
            m_colorState = ColorState::Normal;
        }
        break;
    case ColorState::Critical:
        if (level < critical - hysteresis) {
            m_colorState = (level >= warning) ? ColorState::Warning : ColorState::Normal;
        }
        break;
    }
}

QColor AudioLevelMeter::colorForState(ColorState state) const
{
    if (state == ColorState::Critical) {
        return QColor(218, 74, 64);
    }

    if (state == ColorState::Warning) {
        return QColor(228, 171, 48);
    }

    return QColor(57, 174, 101);
}

QColor AudioLevelMeter::colorForDisplayValue(double value) const
{
    const double level = levelForThreshold(value);
    const double warning = qMin(m_warningThreshold, m_criticalThreshold);
    const double critical = qMax(m_warningThreshold, m_criticalThreshold);

    if (level >= critical) {
        return colorForState(ColorState::Critical);
    }
    if (level >= warning) {
        return colorForState(ColorState::Warning);
    }
    return colorForState(ColorState::Normal);
}

QColor AudioLevelMeter::fillColorForValue(double value) const
{
    Q_UNUSED(value);

    return colorForState(m_colorState);
}

QColor AudioLevelMeter::secondaryMarkerColor() const
{
    return m_secondaryColorTracksPrimary
        ? fillColorForValue(m_value)
        : colorForDisplayValue(m_peakValue);
}
