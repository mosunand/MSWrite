#pragma once

#include <QDockWidget>
#include <QVector>

class QListWidget;

struct OutlineItem {
    int level = 1;      // 1..6
    QString text;
};

// 原生大纲侧栏:标题列表,点击跳转到对应标题
class OutlineDock : public QDockWidget
{
    Q_OBJECT
public:
    explicit OutlineDock(QWidget *parent = nullptr);

    void setItems(const QVector<OutlineItem> &items);
    void clear();

signals:
    void gotoRequested(int index);   // 按 items 顺序的第 index 个标题

private slots:
    void onClicked(int row);

private:
    QListWidget *m_list = nullptr;
    QVector<OutlineItem> m_items;   // 上次内容:相同则不动列表(保住滚动位置)
};
