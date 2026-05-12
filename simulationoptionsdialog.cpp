#include "simulationoptionsdialog.h"
#include "ui_simulationoptionsdialog.h"

#include <QBrush>
#include <QColor>
#include <QComboBox>
#include <QHeaderView>
#include <QTableWidgetItem>

SimulationOptionsDialog::SimulationOptionsDialog(QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::SimulationOptionsDialog)
{
    ui->setupUi(this);
    ui->comboBoxServerTargetType->addItems({"speed", "torque"});
    ui->comboBoxClientTargetType->addItems({"speed", "torque"});
    ui->comboBoxTimeUnit->addItems({"s", "ms", "us"});
    ui->spinBoxStartRow->setMinimum(1);

    connect(ui->comboBoxServerValueColumn, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &SimulationOptionsDialog::updateColumnHighlights);
    connect(ui->comboBoxClientValueColumn, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &SimulationOptionsDialog::updateColumnHighlights);
    connect(ui->comboBoxTimeColumn, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &SimulationOptionsDialog::updateColumnHighlights);
    connect(ui->spinBoxStartRow, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &SimulationOptionsDialog::updateColumnHighlights);
}

SimulationOptionsDialog::~SimulationOptionsDialog()
{
    delete ui;
}

void SimulationOptionsDialog::setCsvPreview(const QStringList &headers, const QVector<QStringList> &rows)
{
    ui->comboBoxServerValueColumn->clear();
    ui->comboBoxClientValueColumn->clear();
    ui->comboBoxTimeColumn->clear();
    for (int i = 0; i < headers.size(); ++i) {
        const QString label = QString("%1: %2").arg(i + 1).arg(headers.at(i));
        ui->comboBoxServerValueColumn->addItem(label);
        ui->comboBoxClientValueColumn->addItem(label);
        ui->comboBoxTimeColumn->addItem(label);
    }

    ui->spinBoxStartRow->setMaximum(qMax(1, rows.size()));
    const int previewRows = qMin(20, rows.size());
    ui->tableWidgetCsvPreview->clear();
    ui->tableWidgetCsvPreview->setColumnCount(headers.size());
    ui->tableWidgetCsvPreview->setRowCount(previewRows);
    ui->tableWidgetCsvPreview->setHorizontalHeaderLabels(headers);

    for (int row = 0; row < previewRows; ++row) {
        const QStringList textRow = rows.at(row);
        for (int column = 0; column < headers.size(); ++column) {
            ui->tableWidgetCsvPreview->setItem(
                row,
                column,
                new QTableWidgetItem(column < textRow.size() ? textRow.at(column) : QString()));
        }
    }

    ui->tableWidgetCsvPreview->resizeColumnsToContents();
    ui->tableWidgetCsvPreview->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    updateColumnHighlights();
}

void SimulationOptionsDialog::setMapping(const QString &serverTargetType,
                                         int serverValueColumn,
                                         const QString &clientTargetType,
                                         int clientValueColumn,
                                         int timeColumn,
                                         const QString &timeUnit,
                                         int startRow)
{
    ui->comboBoxServerTargetType->setCurrentText(serverTargetType);
    ui->comboBoxClientTargetType->setCurrentText(clientTargetType);

    if (serverValueColumn >= 0 && serverValueColumn < ui->comboBoxServerValueColumn->count()) {
        ui->comboBoxServerValueColumn->setCurrentIndex(serverValueColumn);
    }
    if (clientValueColumn >= 0 && clientValueColumn < ui->comboBoxClientValueColumn->count()) {
        ui->comboBoxClientValueColumn->setCurrentIndex(clientValueColumn);
    }
    if (timeColumn >= 0 && timeColumn < ui->comboBoxTimeColumn->count()) {
        ui->comboBoxTimeColumn->setCurrentIndex(timeColumn);
    }
    ui->comboBoxTimeUnit->setCurrentText(timeUnit);
    ui->spinBoxStartRow->setValue(qMax(1, startRow + 1));
    updateColumnHighlights();
}

QString SimulationOptionsDialog::serverTargetType() const
{
    return ui->comboBoxServerTargetType->currentText();
}

int SimulationOptionsDialog::serverValueColumn() const
{
    return ui->comboBoxServerValueColumn->currentIndex();
}

QString SimulationOptionsDialog::clientTargetType() const
{
    return ui->comboBoxClientTargetType->currentText();
}

int SimulationOptionsDialog::clientValueColumn() const
{
    return ui->comboBoxClientValueColumn->currentIndex();
}

int SimulationOptionsDialog::timeColumn() const
{
    return ui->comboBoxTimeColumn->currentIndex();
}

QString SimulationOptionsDialog::timeUnit() const
{
    return ui->comboBoxTimeUnit->currentText();
}

int SimulationOptionsDialog::startRow() const
{
    return ui->spinBoxStartRow->value() - 1;
}

void SimulationOptionsDialog::updateColumnHighlights()
{
    const int serverColumn = ui->comboBoxServerValueColumn->currentIndex();
    const int clientColumn = ui->comboBoxClientValueColumn->currentIndex();
    const int timeColumnIndex = ui->comboBoxTimeColumn->currentIndex();
    const int startRow = ui->spinBoxStartRow->value() - 1;

    const QColor serverColor("#d7ecff");
    const QColor clientColor("#ffe0e0");
    const QColor timeColor("#e3f6df");
    const QColor startColor("#efe4ff");
    const QColor overlapColor("#fff3bf");

    for (int row = 0; row < ui->tableWidgetCsvPreview->rowCount(); ++row) {
        for (int column = 0; column < ui->tableWidgetCsvPreview->columnCount(); ++column) {
            QTableWidgetItem *item = ui->tableWidgetCsvPreview->item(row, column);
            if (!item) {
                continue;
            }

            int matches = 0;
            QColor color = Qt::white;
            if (column == serverColumn) {
                color = serverColor;
                ++matches;
            }
            if (column == clientColumn) {
                color = clientColor;
                ++matches;
            }
            if (column == timeColumnIndex) {
                color = timeColor;
                ++matches;
            }
            if (row == startRow) {
                color = startColor;
                ++matches;
            }
            if (matches > 1) {
                color = overlapColor;
            }
            item->setBackground(QBrush(color));
        }
    }
}
