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
      m_divisionCount(5),
      m_peakHoldMs(1000),
      m_peakHoldRemainingMs(0)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMinimumSize(80, 180);

    m_peakDecayTimer.setInterval(40);
    connect(&m_peakDecayTimer, &QTimer::timeout, this, [this]() {
        if (m_peakHoldRemainingMs > 0) {
            m_peakHoldRemainingMs -= m_peakDecayTimer.interval();
            return;
        }

        const double decayStep = (m_maximum - m_minimum) * 0.015;
        if (m_minimum < 0.0 && m_maximum > 0.0) {
            const double peakSign = (m_peakValue < 0.0) ? -1.0 : 1.0;
            const double decayedLevel = qMax(levelForThreshold(m_value),
                                             levelForThreshold(m_peakValue) - decayStep);
            m_peakValue = peakSign * decayedLevel;
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

    m_minimum = minimum;
    m_maximum = maximum;
    m_value = clampedValue(m_value);
    m_displayValue = clampedValue(m_displayValue);
    m_peakValue = clampedValue(m_peakValue);
    m_warningThreshold = m_minimum + (m_maximum - m_minimum) * 0.65;
    m_criticalThreshold = m_minimum + (m_maximum - m_minimum) * 0.85;
    update();
}

void AudioLevelMeter::setThresholds(double warningThreshold, double criticalThreshold)
{
    m_warningThreshold = warningThreshold;
    m_criticalThreshold = criticalThreshold;
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

    if (levelForThreshold(m_value) >= levelForThreshold(m_peakValue)) {
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
    return QSize(80, 240);
}

QSize AudioLevelMeter::minimumSizeHint() const
{
    return QSize(80, 160);
}

void AudioLevelMeter::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QRect area = rect().adjusted(6, 6, -6, -6);
    painter.fillRect(rect(), palette().window());

    QFont titleFont = painter.font();
    titleFont.setBold(true);
    painter.setFont(titleFont);
    painter.setPen(QColor(35, 35, 35));

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
    const int scaleWidth = 34;
    const int gap = 6;
    const int configuredMeterWidth = qMax(12, sizeHint().width() - scaleWidth - gap);
    const int availableMeterWidth = qMax(12, area.width() - scaleWidth - gap);
    const int meterWidth = qMin(configuredMeterWidth, availableMeterWidth);
    const int meterAreaLeft = area.left() + scaleWidth + gap;
    const int meterAreaWidth = qMax(12, area.width() - scaleWidth - gap);
    const int horizontalOffset = meterWidth / 2;
    const int meterLeft = meterAreaLeft + qMax(0, (meterAreaWidth - meterWidth) / 2) - horizontalOffset;
    const int scaleLeft = area.left() - horizontalOffset;
    const QRect meterRect(meterLeft,
                          area.top() + valueHeight + 4,
                          meterWidth,
                          area.height() - titleHeight - valueHeight - 10);

    const QRectF trackRect = QRectF(meterRect).adjusted(0.5, 0.5, -0.5, -0.5);
    painter.setPen(QPen(QColor(95, 95, 95), 1));
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

    painter.setPen(QPen(QColor(62, 64, 68), 1));
    for (double tickValue : std::as_const(tickValues)) {
        if (qFuzzyCompare(tickValue + 1.0, m_minimum + 1.0) ||
            qFuzzyCompare(tickValue + 1.0, m_maximum + 1.0)) {
            continue;
        }
        const int y = yForValue(meterRect, tickValue);
        painter.drawLine(meterRect.left() + 3, y, meterRect.right() - 3, y);
    }

    painter.setPen(QPen(QColor(140, 140, 140), 1));
    painter.drawLine(meterRect.left() + 2, zeroY, meterRect.right() - 2, zeroY);

    const int peakY = yForValue(meterRect, m_peakValue);
    painter.setPen(QPen(QColor(245, 245, 245), 2));
    painter.drawLine(meterRect.left() + 2, peakY, meterRect.right() - 2, peakY);

    painter.setPen(QColor(65, 65, 65));
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

QColor AudioLevelMeter::fillColorForValue(double value) const
{
    const double level = levelForThreshold(value);
    const double warning = qMin(m_warningThreshold, m_criticalThreshold);
    const double critical = qMax(m_warningThreshold, m_criticalThreshold);

    if (level >= critical) {
        return QColor(218, 74, 64);
    }

    if (level >= warning) {
        return QColor(228, 171, 48);
    }

    return QColor(57, 174, 101);
}
