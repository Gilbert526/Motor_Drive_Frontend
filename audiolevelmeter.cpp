#include "audiolevelmeter.h"

#include <QFontMetrics>
#include <QPainter>
#include <QSizePolicy>
#include <QVector>
#include <QtMath>

#include <algorithm>
#include <functional>
#include <utility>

AudioLevelMeter::AudioLevelMeter(QWidget *parent)
    : QWidget(parent),
      m_minimum(0.0),
      m_maximum(100.0),
      m_value(0.0),
      m_displayValue(0.0),
      m_peakValue(0.0),
      m_warningThreshold(65.0),
      m_criticalThreshold(85.0),
      m_thresholdHysteresisPercent(0.0),
      m_divisionCount(5),
      m_colorState(ColorState::Normal),
      m_peakHoldMs(1000),
      m_peakHoldRemainingMs(0)
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

void AudioLevelMeter::setMajorTickCount(int tickCount)
{
    setDivisionCount(qMax(1, tickCount - 1));
}

void AudioLevelMeter::setPeakHoldMs(int holdMs)
{
    m_peakHoldMs = qMax(0, holdMs);
}

void AudioLevelMeter::setValue(double value)
{
    m_value = clampedValue(value);
    updateColorState(m_value);

    // Peak value is based on magnitude, allowing bipolar gauges to hold the
    // largest positive or negative excursion until decay begins.
    if (qAbs(m_value) > qAbs(m_peakValue)) {
        m_peakValue = m_value;
        m_peakHoldRemainingMs = m_peakHoldMs;
    }
    if (!m_peakDecayTimer.isActive()) {
        m_peakDecayTimer.start();
    }

    update();
}

QSize AudioLevelMeter::sizeHint() const
{
    return QSize(92, 240);
}

QSize AudioLevelMeter::minimumSizeHint() const
{
    return QSize(92, 160);
}

void AudioLevelMeter::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    // Overall content margins for the whole widget drawing area.
    // Adjust the first and third values to change the left/right margins
    // for the entire gauges area. The current margins are optimized for 3 gauges layout.
    const QRect area = rect().adjusted(2, 20, -2, -6);
    painter.fillRect(rect(), palette().window());
    const QColor primaryText = palette().color(QPalette::WindowText);
    const QColor secondaryText = primaryText.darker(150);
    const QColor gridColor = primaryText.darker(220);
    const QColor zeroLineColor = primaryText.lighter(135);
    const QColor peakLineColor = (primaryText.lightness() < 128)
        ? QColor(245, 245, 245)
        : primaryText.lighter(160);
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
    painter.setPen(QPen(peakLineColor, 2));
    painter.drawLine(meterRect.left() + 2, peakY, meterRect.right() - 2, peakY);

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

int AudioLevelMeter::yForValue(const QRect &meterRect, double value) const
{
    return meterRect.bottom() - qRound(meterRect.height() * normalizedValue(value));
}

QString AudioLevelMeter::formattedValue(double value) const
{
    const QString suffix = m_unit.isEmpty() ? QString() : QStringLiteral(" %1").arg(m_unit);
    return QStringLiteral("%1%2").arg(value, 0, 'f', 2).arg(suffix);
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

QColor AudioLevelMeter::fillColorForValue(double value) const
{
    Q_UNUSED(value);

    if (m_colorState == ColorState::Critical) {
        return QColor(218, 74, 64);
    }

    if (m_colorState == ColorState::Warning) {
        return QColor(228, 171, 48);
    }

    return QColor(57, 174, 101);
}
