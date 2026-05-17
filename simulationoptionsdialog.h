#ifndef SIMULATIONOPTIONSDIALOG_H
#define SIMULATIONOPTIONSDIALOG_H

#include <QDialog>
#include <QStringList>
#include <QVector>

namespace Ui {
class SimulationOptionsDialog;
}

/**
 * @brief CSV mapping dialog for synchronized simulation playback.
 *
 * The dialog lets the user choose which CSV columns drive the server target,
 * client target, and time axis. It also displays a small preview table and
 * highlights the selected columns/start row so mapping mistakes are visible
 * before the simulation is started.
 */
class SimulationOptionsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SimulationOptionsDialog(QWidget *parent = nullptr);
    ~SimulationOptionsDialog();

    /**
     * @brief Populate the preview table and column selectors from loaded CSV data.
     */
    void setCsvPreview(const QStringList &headers, const QVector<QStringList> &rows);

    /**
     * @brief Restore a previously chosen mapping into the dialog controls.
     */
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
