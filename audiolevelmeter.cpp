#include "audiolevelmeter.h"

#include <QFontMetrics>
#include <QPainter>
#include <QSizePolicy>
#include <QtMath>

AudioLevelMeter::AudioLevelMeter(QWidget *parent)
    : QWidget(parent),
      m_minimum(0.0),
      m_maximum(100.0),
      m_value(0.0),
      m_peakValue(0.0),
      m_warningThreshold(65.0),
      m_criticalThreshold(85.0),
      m_majorTickCount(6),
      m_peakHoldMs(1000),
      m_peakHoldRemainingMs(0)
{
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    setMinimumSize(90, 180);

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

void AudioLevelMeter::setMajorTickCount(int tickCount)
{
    m_majorTickCount = qMax(2, tickCount);
    update();
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
    return QSize(110, 240);
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
                     formattedValue(m_value));

    painter.setFont(titleFont);
    const int titleHeight = 22;
    painter.drawText(QRect(area.left(), area.bottom() - titleHeight + 1, area.width(), titleHeight),
                     Qt::AlignCenter,
                     m_title);

    painter.setFont(valueFont);
    const int scaleWidth = 34;
    const int gap = 6;
    const QRect meterRect(area.left() + scaleWidth + gap,
                          area.top() + valueHeight + 4,
                          area.width() - scaleWidth - gap,
                          area.height() - titleHeight - valueHeight - 10);

    const QRectF trackRect = QRectF(meterRect).adjusted(0.5, 0.5, -0.5, -0.5);
    painter.setPen(QPen(QColor(95, 95, 95), 1));
    painter.setBrush(QColor(28, 30, 33));
    painter.drawRoundedRect(trackRect, 4, 4);

    const double ratio = normalizedValue(m_value);
    const int fillHeight = qRound(meterRect.height() * ratio);
    QRect fillRect(meterRect.left() + 3,
                   meterRect.bottom() - fillHeight + 1,
                   meterRect.width() - 5,
                   fillHeight - 3);

    if (fillRect.height() > 0) {
        painter.fillRect(fillRect, fillColorForValue(m_value));
    }

    painter.setPen(QPen(QColor(62, 64, 68), 1));
    for (int i = 1; i < 10; ++i) {
        const int y = meterRect.bottom() - qRound(meterRect.height() * (i / 10.0));
        painter.drawLine(meterRect.left() + 3, y, meterRect.right() - 3, y);
    }

    const int peakY = meterRect.bottom() - qRound(meterRect.height() * normalizedValue(m_peakValue));
    painter.setPen(QPen(QColor(245, 245, 245), 2));
    painter.drawLine(meterRect.left() + 2, peakY, meterRect.right() - 2, peakY);

    painter.setPen(QColor(65, 65, 65));
    const QFontMetrics fm(painter.font());
    for (int i = 0; i < m_majorTickCount; ++i) {
        const double tickRatio = (m_majorTickCount == 1) ? 0.0 : i / double(m_majorTickCount - 1);
        const double tickValue = m_maximum - tickRatio * (m_maximum - m_minimum);
        const int y = meterRect.top() + qRound(meterRect.height() * tickRatio);
        const QString label = QString::number(tickValue, 'f', (m_maximum - m_minimum <= 10.0) ? 1 : 0);

        painter.drawLine(meterRect.left() - 5, y, meterRect.left() - 1, y);
        painter.drawText(QRect(area.left(), y - fm.height() / 2, scaleWidth, fm.height()),
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
