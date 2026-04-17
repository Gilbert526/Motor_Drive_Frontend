#ifndef DRAGLISTWIDGET_H
#define DRAGLISTWIDGET_H

#include <QListWidget>

class DragListWidget : public QListWidget
{
    Q_OBJECT
public:
    explicit DragListWidget(QWidget *parent = nullptr);
protected:
    QMimeData* mimeData(const QList<QListWidgetItem*> &items) const override;
};

#endif // DRAGLISTWIDGET_H