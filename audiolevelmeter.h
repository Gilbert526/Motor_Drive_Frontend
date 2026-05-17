#ifndef AUDIOLEVELMETER_H
#define AUDIOLEVELMETER_H

#include <QColor>
#include <QTimer>
#include <QWidget>

/**
 * @brief Compact vertical gauge used to display live telemetry magnitudes.
 *
 * The widget paints its own title, numeric value, scale, filled level, zero line,
 * and peak-hold marker. Thresholds drive a simple Normal/Warning/Critical state
 * machine with optional hysteresis so the gauge does not flicker at boundaries.
 */
class AudioLevelMeter : public QWidget
{
    Q_OBJECT

public:
    /**
     * @brief Current visual severity of the gauge fill.
     */
    enum class ColorState {
        Normal,
        Warning,
        Critical
    };

    explicit AudioLevelMeter(QWidget *parent = nullptr);

    // Configuration setters update the internal state and schedule a repaint.
    void setTitle(const QString &title);
    void setUnit(const QString &unit);
    void setRange(double minimum, double maximum);
    void setThresholds(double warningThreshold, double criticalThreshold);
    void setThresholdHysteresisPercent(double percent);
    void setDivisionCount(int divisionCount);
    void setMajorTickCount(int tickCount);
    void setPeakHoldMs(int holdMs);
    void setValue(double value);

    double value() const { return m_value; }
    double peakValue() const { return m_peakValue; }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    /**
     * @brief Repaints the full meter using palette-aware colors.
     */
    void paintEvent(QPaintEvent *event) override;

private:
    // Geometry/value helpers keep painting code readable and centralize clamping.
    double clampedValue(double value) const;
    double normalizedValue(double value) const;
    int yForValue(const QRect &meterRect, double value) const;
    double levelForThreshold(double value) const;
    double hysteresisAmount() const;
    void updateColorState(double value);
    QString formattedValue(double value) const;
    QColor fillColorForValue(double value) const;

    QString m_title;
    QString m_unit;
    double m_minimum;
    double m_maximum;
    double m_value;
    double m_displayValue;
    double m_peakValue;
    double m_warningThreshold;
    double m_criticalThreshold;
    double m_thresholdHysteresisPercent;
    int m_divisionCount;
    ColorState m_colorState;
    int m_peakHoldMs;
    int m_peakHoldRemainingMs;
    QTimer m_peakDecayTimer;
    QTimer m_valueDisplayTimer;
};

#endif // AUDIOLEVELMETER_H
