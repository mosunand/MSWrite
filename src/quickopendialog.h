#pragma once

#include <QDialog>
#include <QVector>

class QLineEdit;
class QListWidget;
struct QFileInfo;

// Ctrl+P 快速打开:最近文件 + 工作区 Markdown 文件,输入即过滤
class QuickOpenDialog : public QDialog
{
    Q_OBJECT
public:
    QuickOpenDialog(const QStringList &recentFiles, const QString &workspace,
                     QWidget *parent = nullptr);

    QString selectedPath() const { return m_selected; }

private:
    struct Entry {
        QString name;   // 显示名(文件名)
        QString dir;    // 目录(辅助显示)
        QString path;   // 绝对路径
        bool recent = false;
    };

    void refill(const QString &filter);
    void acceptCurrent();

    QLineEdit *m_edit = nullptr;
    QListWidget *m_list = nullptr;
    QVector<Entry> m_all;
    bool m_workspaceEntriesCollected = false;
    QString m_selected;
};
