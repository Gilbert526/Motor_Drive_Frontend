#ifndef AUDIOLEVELMETER_H
#define AUDIOLEVELMETER_H

#include <QColor>
#include <QPointF>
#include <QTimer>
#include <QVector>
#include <QWidget>

class QPainter;

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

    enum class DisplayMode {
        VerticalBar,
        Circular
    };

    explicit AudioLevelMeter(QWidget *parent = nullptr);

    // Configuration setters update the internal state and schedule a repaint.
    void setDisplayMode(DisplayMode mode);
    void setTitle(const QString &title);
    void setUnit(const QString &unit);
    void setRange(double minimum, double maximum);
    void setThresholds(double warningThreshold, double criticalThreshold);
    void setThresholdHysteresisPercent(double percent);
    void setDivisionCount(int divisionCount);
    void setValueDecimals(int decimals);
    void setMajorTickCount(int tickCount);
    void setPeakHoldMs(int holdMs);
    void setPeakTrackingEnabled(bool enabled);
    void setSecondaryColorTracksPrimary(bool enabled);
    void setPeakValue(double value);
    void setValue(double value);

    double value() const { return m_value; }
    double peakValue() const { return m_peakValue; }
    DisplayMode displayMode() const { return m_displayMode; }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    /**
     * @brief Repaints the full meter using palette-aware colors.
     */
    void paintEvent(QPaintEvent *event) override;

private:
    // Geometry/value helpers keep painting code readable and centralize clamping.
    QVector<double> tickValues() const;
    double clampedValue(double value) const;
    double normalizedValue(double value) const;
    double angleForValue(double value) const;
    QPointF pointOnCircle(const QPointF &center, double radius, double angleDegrees) const;
    int yForValue(const QRect &meterRect, double value) const;
    void paintVerticalBar(QPainter &painter, const QRect &area);
    void paintCircularGauge(QPainter &painter, const QRect &area);
    void drawNeedle(QPainter &painter,
                    const QPointF &center,
                    double angleDegrees,
                    double length,
                    const QColor &color,
                    double width);
    void drawSecondaryArrow(QPainter &painter,
                            const QPointF &center,
                            double angleDegrees,
                            double innerRadius,
                            double outerRadius,
                            const QColor &color);
    void drawLcdValue(QPainter &painter,
                      const QRectF &rect,
                      const QString &numberText,
                      const QString &unitText,
                      const QColor &textColor);
    double levelForThreshold(double value) const;
    double hysteresisAmount() const;
    void updateColorState(double value);
    QString formattedNumber(double value) const;
    QString formattedValue(double value) const;
    QColor colorForState(ColorState state) const;
    QColor colorForDisplayValue(double value) const;
    QColor fillColorForValue(double value) const;
    QColor secondaryMarkerColor() const;

    QString m_title;
    QString m_unit;
    DisplayMode m_displayMode;
    double m_minimum;
    double m_maximum;
    double m_value;
    double m_displayValue;
    double m_peakValue;
    double m_warningThreshold;
    double m_criticalThreshold;
    double m_thresholdHysteresisPercent;
    int m_divisionCount;
    int m_valueDecimals;
    ColorState m_colorState;
    int m_peakHoldMs;
    int m_peakHoldRemainingMs;
    bool m_peakTrackingEnabled;
    bool m_secondaryColorTracksPrimary;
    QTimer m_peakDecayTimer;
    QTimer m_valueDisplayTimer;
};

#endif // AUDIOLEVELMETER_H
