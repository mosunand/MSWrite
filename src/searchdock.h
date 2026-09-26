#pragma once

#include <QDockWidget>
#include <QJsonArray>
#include <QVector>
#include <QPair>

class QLineEdit;
class QComboBox;
class QListWidget;
class QListWidgetItem;
class QLabel;

// 全文搜索面板:工作区(递归 *.md)/ 当前文档 两个范围
class SearchDock : public QDockWidget
{
    Q_OBJECT
public:
    explicit SearchDock(QWidget *parent = nullptr);

    // scope: "workspace" / "doc"
    QString scope() const;
    void focusInput();
    void prefillQuery(const QString &q);   // 用当前选中文本预填搜索词

    // 工作区结果:文件路径 + 命中行文本
    void showWorkspaceResults(const QVector<QPair<QString, QString>> &results);
    // 当前文档结果:[{i: 块索引, text: 块文本}]
    void showDocResults(const QJsonArray &items);
    void showStatus(const QString &text);

signals:
    void searchRequested(const QString &scope, const QString &query);
    // 工作区结果被点击
    void fileResultActivated(const QString &path, const QString &lineText);
    // 当前文档结果被点击
    void docResultActivated(int blockIndex);

private slots:
    void onSearch();
    void onResultActivated(QListWidgetItem *item);

private:
    QLineEdit *m_edit = nullptr;
    QComboBox *m_scope = nullptr;
    QListWidget *m_results = nullptr;
    QLabel *m_status = nullptr;
    QString m_scopeValue = QStringLiteral("workspace");
};
