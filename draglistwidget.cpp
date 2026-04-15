#include "DragListWidget.h"
#include <QMimeData>

DragListWidget::DragListWidget(QWidget *parent) : QListWidget(parent) {}

QMimeData* DragListWidget::mimeData(const QList<QListWidgetItem*> items) const
{
    if (items.isEmpty())
        return nullptr;
    QMimeData *data = new QMimeData;
    // 将选中项的文本作为纯文本设置到 mimeData
    data->setText(items.first()->text());
    return data;
}