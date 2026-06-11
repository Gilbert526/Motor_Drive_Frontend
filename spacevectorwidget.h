#ifndef SPACEVECTORWIDGET_H
#define SPACEVECTORWIDGET_H

#include <QColor>
#include <QVector>
#include <QFrame>

class QCPGraph;
class QCPItemLine;
class QCPItemText;
class QCPCurve;
class QCustomPlot;

class SpaceVectorWidget : public QFrame
{
    Q_OBJECT

public:
    explicit SpaceVectorWidget(QWidget *parent = nullptr);

    void appendSample(double alpha, double beta, double vdc, bool capToBoundary = false);
    void clearTrace();
    void refreshPlot();
    void setArrowVisible(bool visible);

protected:
    void changeEvent(QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    struct TracePoint {
        double alpha = 0.0;
        double beta = 0.0;
        double theta = 0.0;
        double unwrappedTheta = 0.0;
    };

    void setupPlot();
    void applyTheme();
    void updateReferenceGeometry();
    void updateTraceGraph();
    void updateAxisLabelFonts();
    void compactTraceIfNeeded();
    double boundaryRadiusForAngle(double theta) const;
    double normalizedAngle(double theta) const;
    double unwrapAngle(double theta);
    void updateRotationDirection(double delta);

    QCustomPlot *m_plot;
    QCPCurve *m_hexagonCurve;
    QCPCurve *m_linearCircleCurve;
    QCPCurve *m_traceCurve;
    QCPGraph *m_latestPointGraph;
    QVector<QCPItemLine*> m_axisItems;
    QVector<QCPItemText*> m_axisLabels;
    QCPItemLine *m_arrowItem;
    QVector<TracePoint> m_trace;
    double m_vdc;
    double m_lastUnwrappedTheta;
    int m_rotationDirection;
    int m_pendingRotationDirection;
    int m_pendingDirectionSamples;
    double m_pendingDirectionDelta;
    bool m_hasLastTheta;
    bool m_arrowVisible;
    bool m_geometryDirty;
};

#endif // SPACEVECTORWIDGET_H
