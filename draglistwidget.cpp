#include "DragListWidget.h"
#include <QMimeData>

DragListWidget::DragListWidget(QWidget *parent) : QListWidget(parent) {}

QMimeData* DragListWidget::mimeData(const QList<QListWidgetItem*> &items) const
{
    if (items.isEmpty())
        return nullptr;
    QMimeData *data = new QMimeData;
    QString text = items.first()->text();
    // 同时设置两种格式，确保兼容性
    data->setText(text);                           // 自动设置 text/plain
    data->setData("text/plain", text.toUtf8());    // 显式设置，双重保险
    // 可选：保留原始格式以便其他用途
    data->setData("application/x-qabstractitemmodeldatalist", 
                  QListWidget::mimeData(items)->data("application/x-qabstractitemmodeldatalist"));
    return data;
}