#include "spacevectorwidget.h"

#include "qcustomplot.h"

#include <QEvent>
#include <QVBoxLayout>
#include <cmath>

namespace {
constexpr int kMaxTracePoints = 2000;
constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2.0 * kPi;
constexpr double kDirectionDeltaEpsilon = 1e-5;
constexpr double kDirectionChangeAngle = 0.01;
constexpr int kDirectionChangeSamples = 5;
constexpr int kMinimumAxisLabelPointSize = 9;
constexpr int kMaximumAxisLabelPointSize = 18;
constexpr double kAxisArrowGapPixels = 12.0;
constexpr double kAxisLabelGapPixels = 19.0;

double finiteOr(double value, double fallback)
{
    return std::isfinite(value) ? value : fallback;
}
}

SpaceVectorWidget::SpaceVectorWidget(QWidget *parent)
    : QFrame(parent),
      m_plot(new QCustomPlot(this)),
      m_hexagonCurve(nullptr),
      m_linearCircleCurve(nullptr),
      m_traceCurve(nullptr),
      m_latestPointGraph(nullptr),
      m_arrowItem(nullptr),
      m_vdc(1.0),
      m_lastUnwrappedTheta(0.0),
      m_rotationDirection(0),
      m_pendingRotationDirection(0),
      m_pendingDirectionSamples(0),
      m_pendingDirectionDelta(0.0),
      m_hasLastTheta(false),
      m_arrowVisible(true),
      m_geometryDirty(true)
{
    setMinimumSize(280, 280);
    setFrameShape(QFrame::StyledPanel);
    setFrameShadow(QFrame::Plain);
    setLineWidth(1);
    setupPlot();

    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_plot);
}

void SpaceVectorWidget::setupPlot()
{
    m_plot->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);
    m_plot->axisRect()->setupFullAxesBox(true);
    m_plot->axisRect()->setAutoMargins(QCP::msNone);
    m_plot->axisRect()->setMargins(QMargins(0, 0, 0, 0));
    for (QCPAxis *axis : {m_plot->xAxis, m_plot->yAxis, m_plot->xAxis2, m_plot->yAxis2}) {
        axis->setVisible(false);
        axis->grid()->setVisible(false);
    }
    m_plot->legend->setVisible(false);
    m_plot->setOpenGl(false);
    m_plot->setNoAntialiasingOnDrag(true);

    m_hexagonCurve = new QCPCurve(m_plot->xAxis, m_plot->yAxis);
    m_hexagonCurve->setPen(QPen(QColor(35, 35, 35), 2.0));
    m_hexagonCurve->setBrush(QBrush(QColor(255, 210, 0, 77)));
    m_linearCircleCurve = new QCPCurve(m_plot->xAxis, m_plot->yAxis);
    m_linearCircleCurve->setPen(Qt::NoPen);
    m_traceCurve = new QCPCurve(m_plot->xAxis, m_plot->yAxis);
    m_traceCurve->setPen(QPen(QColor(35, 170, 95), 1.6));
    m_latestPointGraph = m_plot->addGraph();
    m_latestPointGraph->setPen(Qt::NoPen);
    m_latestPointGraph->setScatterStyle(QCPScatterStyle(QCPScatterStyle::ssDisc,
                                                        QColor(25, 135, 75),
                                                        QColor(25, 135, 75),
                                                        7));

    const QList<QColor> axisColors = {
        QColor(210, 70, 70),
        QColor(70, 150, 80),
        QColor(70, 90, 210)
    };
    const QStringList axisLabels = {"a", "b", "c"};
    for (int i = 0; i < 3; ++i) {
        QCPItemLine *axis = new QCPItemLine(m_plot);
        QColor color = axisColors[i];
        color.setAlpha(128);
        axis->setPen(QPen(color, 1.0, Qt::SolidLine));
        axis->setHead(QCPLineEnding(QCPLineEnding::esSpikeArrow, 7, 9));
        axis->setTail(QCPLineEnding::esNone);
        m_axisItems.append(axis);

        QCPItemText *label = new QCPItemText(m_plot);
        label->setText(axisLabels[i]);
        QFont labelFont("Cambria Math", kMinimumAxisLabelPointSize);
        labelFont.setItalic(true);
        label->setFont(labelFont);
        label->setPositionAlignment(Qt::AlignCenter);
        label->setClipToAxisRect(false);
        m_axisLabels.append(label);
    }

    m_arrowItem = new QCPItemLine(m_plot);
    m_arrowItem->setPen(QPen(QColor(20, 130, 80), 2.0));
    m_arrowItem->setHead(QCPLineEnding::esSpikeArrow);

    applyTheme();
    updateReferenceGeometry();
}

void SpaceVectorWidget::appendSample(double alpha, double beta, double vdc, bool capToBoundary)
{
    if (!std::isfinite(alpha) || !std::isfinite(beta)) {
        return;
    }

    const double nextVdc = qMax(0.001, finiteOr(std::fabs(vdc), m_vdc));
    if (!qFuzzyCompare(nextVdc, m_vdc)) {
        m_vdc = nextVdc;
        m_geometryDirty = true;
    }

    double cappedAlpha = alpha;
    double cappedBeta = beta;
    const double theta = normalizedAngle(std::atan2(beta, alpha));
    if (capToBoundary) {
        const double mag = std::hypot(alpha, beta);
        const double limit = boundaryRadiusForAngle(theta);
        if (mag > limit && mag > 0.0) {
            cappedAlpha *= limit / mag;
            cappedBeta *= limit / mag;
        }
    }

    const double cappedTheta = normalizedAngle(std::atan2(cappedBeta, cappedAlpha));
    const double unwrappedTheta = unwrapAngle(cappedTheta);
    m_lastUnwrappedTheta = unwrappedTheta;
    m_hasLastTheta = true;

    m_trace.append({cappedAlpha, cappedBeta, cappedTheta, unwrappedTheta});

    if (m_trace.size() > 1) {
        while (!m_trace.isEmpty()) {
            const double oldest = m_trace.first().unwrappedTheta;
            bool shouldRemove = false;
            if (m_rotationDirection > 0) {
                shouldRemove = oldest < unwrappedTheta - kTwoPi || oldest > unwrappedTheta;
            } else if (m_rotationDirection < 0) {
                shouldRemove = oldest > unwrappedTheta + kTwoPi || oldest < unwrappedTheta;
            } else {
                shouldRemove = oldest < unwrappedTheta - kTwoPi || oldest > unwrappedTheta + kTwoPi;
            }
            if (!shouldRemove) {
                break;
            }
            m_trace.removeFirst();
        }
    }

    compactTraceIfNeeded();
}

void SpaceVectorWidget::clearTrace()
{
    m_trace.clear();
    m_hasLastTheta = false;
    m_lastUnwrappedTheta = 0.0;
    m_rotationDirection = 0;
    m_pendingRotationDirection = 0;
    m_pendingDirectionSamples = 0;
    m_pendingDirectionDelta = 0.0;
    m_traceCurve->data()->clear();
    m_latestPointGraph->data()->clear();
    m_arrowItem->setVisible(false);
    m_plot->replot(QCustomPlot::rpQueuedReplot);
}

void SpaceVectorWidget::refreshPlot()
{
    if (m_geometryDirty) {
        updateReferenceGeometry();
    }
    updateTraceGraph();
    m_plot->replot(QCustomPlot::rpQueuedReplot);
}

void SpaceVectorWidget::setArrowVisible(bool visible)
{
    m_arrowVisible = visible;
    m_arrowItem->setVisible(visible && !m_trace.isEmpty());
    m_plot->replot(QCustomPlot::rpQueuedReplot);
}

void SpaceVectorWidget::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::PaletteChange ||
        event->type() == QEvent::ApplicationPaletteChange) {
        applyTheme();
    }
    QWidget::changeEvent(event);
}

void SpaceVectorWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    m_geometryDirty = true;
    updateAxisLabelFonts();
}

void SpaceVectorWidget::applyTheme()
{
    const QPalette pal = palette();
    const QColor windowColor = pal.color(QPalette::Window);
    const QColor textColor = pal.color(QPalette::WindowText);
    const QColor midColor = pal.color(QPalette::Mid);
    const QColor gridColor = midColor;
    const QColor subGridColor = midColor.lighter(115);
    const QColor boundaryColor = textColor;
    QColor bandColor = QColor(255, 210, 0);
    bandColor.setAlpha(77);
    QColor traceColor = textColor.lightness() < 128
                            ? QColor(45, 170, 95)
                            : QColor(0, 95, 45);
    QColor pointColor = traceColor.darker(115);
    QColor arrowColor = traceColor.darker(125);
    const QList<QColor> axisBaseColors = {
        QColor(220, 70, 70),
        QColor(70, 170, 90),
        QColor(80, 110, 230)
    };

    m_plot->setBackground(windowColor);
    m_plot->axisRect()->setBackground(windowColor);
    m_hexagonCurve->setPen(QPen(boundaryColor, 2.0));
    m_hexagonCurve->setBrush(QBrush(bandColor));
    m_linearCircleCurve->setBrush(QBrush(windowColor));
    m_traceCurve->setPen(QPen(traceColor, 1.6));
    m_latestPointGraph->setScatterStyle(QCPScatterStyle(QCPScatterStyle::ssDisc,
                                                        pointColor,
                                                        pointColor,
                                                        7));
    m_arrowItem->setPen(QPen(arrowColor, 2.0));
    for (int i = 0; i < m_axisItems.size(); ++i) {
        QColor color = axisBaseColors[i % axisBaseColors.size()];
        if (textColor.lightness() < 128) {
            color = color.lighter(125);
        }
        color.setAlpha(128);
        m_axisItems[i]->setPen(QPen(color, 1.0, Qt::SolidLine));
        if (i < m_axisLabels.size()) {
            QColor labelColor = color;
            labelColor.setAlpha(220);
            m_axisLabels[i]->setColor(labelColor);
            m_axisLabels[i]->setBrush(Qt::NoBrush);
            m_axisLabels[i]->setPen(Qt::NoPen);
        }
    }
    for (QCPAxis *axis : {m_plot->xAxis, m_plot->yAxis}) {
        axis->setBasePen(QPen(textColor));
        axis->setTickPen(QPen(textColor));
        axis->setSubTickPen(QPen(textColor));
        axis->setTickLabelColor(textColor);
        axis->setLabelColor(textColor);
        axis->grid()->setPen(QPen(gridColor, 0, Qt::DotLine));
        axis->grid()->setSubGridPen(QPen(subGridColor, 0, Qt::DotLine));
        axis->grid()->setZeroLinePen(QPen(textColor, 0));
        axis->setVisible(false);
        axis->grid()->setVisible(false);
    }
    m_plot->replot(QCustomPlot::rpQueuedReplot);
}

void SpaceVectorWidget::updateReferenceGeometry()
{
    const double hexRadius = 2.0 * m_vdc / 3.0;
    const double linearRadius = m_vdc / std::sqrt(3.0);
    updateAxisLabelFonts();
    const int plotWidth = qMax(1, m_plot->axisRect()->width());
    const int plotHeight = qMax(1, m_plot->axisRect()->height());
    const int limitingPixels = qMax(1, qMin(plotWidth, plotHeight));
    const double labelMarginPixels = m_axisLabels.isEmpty()
                                         ? 10.0
                                         : qMax(10.0, m_axisLabels.first()->font().pointSizeF());
    const double initialContentRadius = hexRadius * 1.25;
    const double unitsPerPixel = 2.0 * initialContentRadius / limitingPixels;
    const double axisRadius = hexRadius + kAxisArrowGapPixels * unitsPerPixel;
    const double labelRadius = axisRadius + kAxisLabelGapPixels * unitsPerPixel;
    const double contentRadius = labelRadius + labelMarginPixels * unitsPerPixel;
    const double aspect = static_cast<double>(plotWidth) / plotHeight;
    if (aspect >= 1.0) {
        m_plot->xAxis->setRange(-contentRadius * aspect, contentRadius * aspect);
        m_plot->yAxis->setRange(-contentRadius, contentRadius);
    } else {
        m_plot->xAxis->setRange(-contentRadius, contentRadius);
        m_plot->yAxis->setRange(-contentRadius / aspect, contentRadius / aspect);
    }

    QVector<double> t;
    QVector<double> x;
    QVector<double> y;
    t.reserve(7);
    x.reserve(7);
    y.reserve(7);
    for (int i = 0; i <= 6; ++i) {
        const double angle = i * kPi / 3.0;
        t.append(i);
        x.append(hexRadius * std::cos(angle));
        y.append(hexRadius * std::sin(angle));
    }
    m_hexagonCurve->setData(t, x, y, true);

    t.clear();
    x.clear();
    y.clear();
    t.reserve(181);
    x.reserve(181);
    y.reserve(181);
    for (int i = 0; i <= 180; ++i) {
        const double angle = i * kTwoPi / 180.0;
        t.append(i);
        x.append(linearRadius * std::cos(angle));
        y.append(linearRadius * std::sin(angle));
    }
    m_linearCircleCurve->setData(t, x, y, true);

    for (int i = 0; i < m_axisItems.size(); ++i) {
        const double angle = -2.0 * kPi * i / 3.0;
        QCPItemLine *axis = m_axisItems[i];
        axis->start->setCoords(-axisRadius * std::cos(angle), -axisRadius * std::sin(angle));
        axis->end->setCoords(axisRadius * std::cos(angle), axisRadius * std::sin(angle));
        if (i < m_axisLabels.size()) {
            m_axisLabels[i]->position->setCoords(labelRadius * std::cos(angle),
                                                 labelRadius * std::sin(angle));
        }
    }

    m_geometryDirty = false;
}

void SpaceVectorWidget::updateTraceGraph()
{
    QVector<double> t;
    QVector<double> x;
    QVector<double> y;
    const int plottedPoints = qMin(kMaxTracePoints, m_trace.size());
    t.reserve(plottedPoints);
    x.reserve(plottedPoints);
    y.reserve(plottedPoints);
    for (int i = 0; i < plottedPoints; ++i) {
        const int sourceIndex = plottedPoints > 1
                                    ? static_cast<int>(std::llround(static_cast<double>(i) * (m_trace.size() - 1) / (plottedPoints - 1)))
                                    : 0;
        const TracePoint &point = m_trace[sourceIndex];
        t.append(i);
        x.append(point.alpha);
        y.append(point.beta);
    }
    m_traceCurve->setData(t, x, y, true);

    if (m_trace.isEmpty()) {
        m_latestPointGraph->data()->clear();
        m_arrowItem->setVisible(false);
        return;
    }

    const TracePoint &latest = m_trace.last();
    m_latestPointGraph->setData({latest.alpha}, {latest.beta}, true);
    m_arrowItem->start->setCoords(0.0, 0.0);
    m_arrowItem->end->setCoords(latest.alpha, latest.beta);
    m_arrowItem->setVisible(m_arrowVisible);
}

void SpaceVectorWidget::updateAxisLabelFonts()
{
    const int shortSide = qMin(m_plot->axisRect()->width(), m_plot->axisRect()->height());
    const int pointSize = qBound(kMinimumAxisLabelPointSize,
                                 shortSide / 24,
                                 kMaximumAxisLabelPointSize);
    QFont labelFont("Cambria Math", pointSize);
    labelFont.setItalic(true);
    for (QCPItemText *label : m_axisLabels) {
        label->setFont(labelFont);
    }
}

void SpaceVectorWidget::compactTraceIfNeeded()
{
    if (m_trace.size() <= kMaxTracePoints * 2) {
        return;
    }

    QVector<TracePoint> compacted;
    compacted.reserve(kMaxTracePoints);
    for (int i = 0; i < kMaxTracePoints; ++i) {
        const int sourceIndex = static_cast<int>(std::llround(static_cast<double>(i) * (m_trace.size() - 1) / (kMaxTracePoints - 1)));
        compacted.append(m_trace[sourceIndex]);
    }
    m_trace = compacted;
}

double SpaceVectorWidget::boundaryRadiusForAngle(double theta) const
{
    const double hexRadius = 2.0 * m_vdc / 3.0;
    const double inRadius = m_vdc / std::sqrt(3.0);
    double local = std::fmod(theta, kPi / 3.0);
    if (local < 0.0) {
        local += kPi / 3.0;
    }
    local -= kPi / 6.0;
    const double denominator = std::max(0.001, std::cos(local));
    return qMin(hexRadius, inRadius / denominator);
}

double SpaceVectorWidget::normalizedAngle(double theta) const
{
    double normalized = std::fmod(theta, kTwoPi);
    if (normalized < 0.0) {
        normalized += kTwoPi;
    }
    return normalized;
}

double SpaceVectorWidget::unwrapAngle(double theta)
{
    if (!m_hasLastTheta) {
        m_rotationDirection = 0;
        m_pendingRotationDirection = 0;
        m_pendingDirectionSamples = 0;
        m_pendingDirectionDelta = 0.0;
        return theta;
    }

    double delta = theta - normalizedAngle(m_lastUnwrappedTheta);
    if (delta > kPi) {
        delta -= kTwoPi;
    } else if (delta < -kPi) {
        delta += kTwoPi;
    }
    updateRotationDirection(delta);
    return m_lastUnwrappedTheta + delta;
}

void SpaceVectorWidget::updateRotationDirection(double delta)
{
    if (std::fabs(delta) < kDirectionDeltaEpsilon) {
        return;
    }

    const int sampleDirection = delta > 0.0 ? 1 : -1;
    if (m_rotationDirection == 0 || sampleDirection == m_rotationDirection) {
        m_rotationDirection = sampleDirection;
        m_pendingRotationDirection = 0;
        m_pendingDirectionSamples = 0;
        m_pendingDirectionDelta = 0.0;
        return;
    }

    if (sampleDirection != m_pendingRotationDirection) {
        m_pendingRotationDirection = sampleDirection;
        m_pendingDirectionSamples = 1;
        m_pendingDirectionDelta = std::fabs(delta);
        return;
    }

    ++m_pendingDirectionSamples;
    m_pendingDirectionDelta += std::fabs(delta);
    if (m_pendingDirectionSamples >= kDirectionChangeSamples ||
        m_pendingDirectionDelta >= kDirectionChangeAngle) {
        m_rotationDirection = m_pendingRotationDirection;
        m_pendingRotationDirection = 0;
        m_pendingDirectionSamples = 0;
        m_pendingDirectionDelta = 0.0;
    }
}
