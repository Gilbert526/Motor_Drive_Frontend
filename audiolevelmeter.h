#ifndef AUDIOLEVELMETER_H
#define AUDIOLEVELMETER_H

#include <QColor>
#include <QTimer>
#include <QWidget>

class AudioLevelMeter : public QWidget
{
    Q_OBJECT

public:
    explicit AudioLevelMeter(QWidget *parent = nullptr);

    void setTitle(const QString &title);
    void setUnit(const QString &unit);
    void setRange(double minimum, double maximum);
    void setThresholds(double warningThreshold, double criticalThreshold);
    void setMajorTickCount(int tickCount);
    void setPeakHoldMs(int holdMs);
    void setValue(double value);

    double value() const { return m_value; }
    double peakValue() const { return m_peakValue; }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    double clampedValue(double value) const;
    double normalizedValue(double value) const;
    double levelForThreshold(double value) const;
    QString formattedValue(double value) const;
    QColor fillColorForValue(double value) const;

    QString m_title;
    QString m_unit;
    double m_minimum;
    double m_maximum;
    double m_value;
    double m_peakValue;
    double m_warningThreshold;
    double m_criticalThreshold;
    int m_majorTickCount;
    int m_peakHoldMs;
    int m_peakHoldRemainingMs;
    QTimer m_peakDecayTimer;
};

#endif // AUDIOLEVELMETER_H
