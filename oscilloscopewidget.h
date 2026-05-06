#ifndef OSCILLOSCOPEWIDGET_H
#define OSCILLOSCOPEWIDGET_H

#include <QHash>
#include <QVector>
#include <QWidget>

#include "qcustomplot.h"

class QLabel;
class QPushButton;

class OscilloscopeWidget : public QWidget
{
    Q_OBJECT

public:
    explicit OscilloscopeWidget(QWidget *parent = nullptr);

    void setFields(const QStringList &fields);
    QStringList getFields() const { return m_fields; }
    void updatePlot(const QHash<QString, QVector<double>> &dataPool,
                    const QVector<double> &timeStamps, int maxPoints);
    void clear();
    void setTitle(const QString &title);

    void setMoveButtonsEnabled(bool upEnabled, bool downEnabled);
    void addField(const QString &fieldName);
    void setFieldColor(const QString &fieldName, const QColor &color);
    QColor getFieldColor(const QString &fieldName) const;
    void setColorList(const QList<QColor> &colors);

protected:
    void changeEvent(QEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dropEvent(QDropEvent *event) override;

signals:
    void fieldsChanged();
    void removeRequested();
    void addBelowRequested();
    void moveUpRequested();
    void moveDownRequested();
    void refreshRequested();

private slots:
    void onConfigure();
    void onToggleYLock();

private:
    struct RenderBuffers {
        QVector<double> x;
        QVector<double> y;
    };

    void updateYAxis(double yMin, double yMax, bool hasData);
    void applyTheme();
    void setupUi();

    QCustomPlot *m_plot;
    QStringList m_fields;
    QHash<QString, QCPGraph*> m_graphs;
    QList<QColor> m_colors;
    QHash<QString, RenderBuffers> m_renderBuffers;
    QLabel *m_titleLabel;
    QPushButton *m_configBtn;
    QPushButton *m_yLockBtn;
    QPushButton *m_moveUpBtn;
    QPushButton *m_moveDownBtn;
    bool m_yLocked;
};

#endif // OSCILLOSCOPEWIDGET_H
