#ifndef SIMULATIONOPTIONSDIALOG_H
#define SIMULATIONOPTIONSDIALOG_H

#include <QDialog>
#include <QStringList>
#include <QVector>

namespace Ui {
class SimulationOptionsDialog;
}

class SimulationOptionsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SimulationOptionsDialog(QWidget *parent = nullptr);
    ~SimulationOptionsDialog();

    void setCsvPreview(const QStringList &headers, const QVector<QStringList> &rows);
    void setMapping(const QString &serverTargetType,
                    int serverValueColumn,
                    const QString &clientTargetType,
                    int clientValueColumn,
                    int timeColumn,
                    const QString &timeUnit,
                    int startRow);

    QString serverTargetType() const;
    int serverValueColumn() const;
    QString clientTargetType() const;
    int clientValueColumn() const;
    int timeColumn() const;
    QString timeUnit() const;
    int startRow() const;

private slots:
    void updateColumnHighlights();

private:
    Ui::SimulationOptionsDialog *ui;
};

#endif // SIMULATIONOPTIONSDIALOG_H
