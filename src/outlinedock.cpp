#include "outlinedock.h"

#include <QListWidget>
#include <QFont>
#include <QScrollBar>
#include <QString>

OutlineDock::OutlineDock(QWidget *parent)
    : QDockWidget(tr("大纲"), parent)
{
    setObjectName(QStringLiteral("outlineDock"));
    setMinimumWidth(180);

    m_list = new QListWidget(this);
    m_list->setWordWrap(true);
    m_list->setUniformItemSizes(false);
    m_list->setAlternatingRowColors(false);
    m_list->setSpacing(2);
    connect(m_list, &QListWidget::itemClicked, this, [this](QListWidgetItem *it) {
        emit gotoRequested(static_cast<int>(m_list->row(it)));
    });

    setWidget(m_list);
    setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetClosable);
}

void OutlineDock::setItems(const QVector<OutlineItem> &items)
{
    // 大纲在每次输入后都会重建:内容没变就别动列表,
    // 否则长文档打字时侧栏滚动条会被反复弹回顶部
    if (items.size() == m_items.size()) {
        bool same = true;
        for (int i = 0; i < items.size(); ++i) {
            if (items[i].level != m_items[i].level || items[i].text != m_items[i].text) {
                same = false;
                break;
            }
        }
        if (same)
            return;
    }
    m_items = items;

    QScrollBar *bar = m_list->verticalScrollBar();
    const int keep = bar ? bar->value() : 0;

    m_list->clear();
    for (const OutlineItem &it : items) {
        QString prefix;
        // 层级缩进,一级最醒目
        const int level = qBound(1, it.level, 6);
        if (level > 1)
            prefix = QString((level - 1) * 2, QLatin1Char(' '));
        auto *row = new QListWidgetItem(prefix + it.text, m_list);
        if (it.level == 1)
            row->setFont([](QFont f) { f.setBold(true); return f; }(row->font()));
    }
    // 内容确实变了:尽量把用户看的位置还回去
    if (bar)
        bar->setValue(keep);
}

void OutlineDock::clear()
{
    m_items.clear();
    m_list->clear();
}

void OutlineDock::onClicked(int row)
{
    emit gotoRequested(row);
}
