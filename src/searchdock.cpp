#include "searchdock.h"

#include <QComboBox>
#include <QDir>
#include <QFileInfo>
#include <QJsonObject>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

SearchDock::SearchDock(QWidget *parent)
    : QDockWidget(tr("搜索"), parent)
{
    setObjectName(QStringLiteral("searchDock"));

    auto *panel = new QWidget(this);
    auto *lay = new QVBoxLayout(panel);
    lay->setContentsMargins(6, 6, 6, 6);
    lay->setSpacing(4);

    auto *top = new QHBoxLayout;
    m_scope = new QComboBox(panel);
    m_scope->addItem(tr("工作区"), QStringLiteral("workspace"));
    m_scope->addItem(tr("当前文档"), QStringLiteral("doc"));
    m_edit = new QLineEdit(panel);
    m_edit->setPlaceholderText(tr("搜索内容,回车开始"));
    m_edit->setClearButtonEnabled(true);
    auto *btn = new QPushButton(tr("搜索"), panel);
    top->addWidget(m_scope);
    top->addWidget(m_edit, 1);
    top->addWidget(btn);
    lay->addLayout(top);

    m_results = new QListWidget(panel);
    m_results->setWordWrap(true);
    lay->addWidget(m_results, 1);

    m_status = new QLabel(QStringLiteral(" "), panel);
    lay->addWidget(m_status);

    setWidget(panel);
    setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetClosable);

    connect(btn, &QPushButton::clicked, this, &SearchDock::onSearch);
    connect(m_edit, &QLineEdit::returnPressed, this, &SearchDock::onSearch);
    connect(m_edit, &QLineEdit::textChanged, this, [this](const QString &) { showStatus(tr("回车搜索")); });
    connect(m_scope, &QComboBox::currentIndexChanged, this, [this](int) {
        m_scopeValue = m_scope->currentData().toString();
        if (m_scopeValue.isEmpty())
            m_scopeValue = QStringLiteral("workspace");
    });
    connect(m_results, &QListWidget::itemActivated, this, &SearchDock::onResultActivated);
    connect(m_results, &QListWidget::itemDoubleClicked, this, &SearchDock::onResultActivated);
}

QString SearchDock::scope() const
{
    return m_scopeValue;
}

void SearchDock::focusInput()
{
    show();
    raise();
    m_edit->setFocus();
    m_edit->selectAll();
}

void SearchDock::prefillQuery(const QString &q)
{
    m_edit->setText(q);
}

void SearchDock::onSearch()
{
    const QString query = m_edit->text().trimmed();
    if (query.isEmpty())
        return;
    emit searchRequested(scope(), query);
}

void SearchDock::onResultActivated(QListWidgetItem *item)
{
    if (!item)
        return;
    const QString scope = item->data(Qt::UserRole + 1).toString();
    if (scope == QLatin1String("doc")) {
        emit docResultActivated(item->data(Qt::UserRole).toInt());
    } else {
        emit fileResultActivated(item->data(Qt::UserRole).toString(),
                                  item->data(Qt::UserRole + 2).toString());
    }
}

void SearchDock::showWorkspaceResults(const QVector<QPair<QString, QString>> &results)
{
    m_results->clear();
    for (const auto &r : results) {
        const QFileInfo fi(r.first);
        auto *item = new QListWidgetItem(
            QStringLiteral("%1  —  %2").arg(fi.fileName(), r.second), m_results);
        item->setData(Qt::UserRole, r.first);          // 文件路径
        item->setData(Qt::UserRole + 1, QStringLiteral("workspace"));
        item->setData(Qt::UserRole + 2, r.second);     // 行文本(用于跳转)
        item->setToolTip(QDir::toNativeSeparators(r.first));
    }
    m_status->setText(tr("工作区命中 %1 条").arg(results.size()));
}

void SearchDock::showDocResults(const QJsonArray &items)
{
    m_results->clear();
    for (const QJsonValue &v : items) {
        const QJsonObject o = v.toObject();
        auto *item = new QListWidgetItem(o.value(QStringLiteral("text")).toString(), m_results);
        item->setData(Qt::UserRole, o.value(QStringLiteral("i")).toInt());
        item->setData(Qt::UserRole + 1, QStringLiteral("doc"));
    }
    m_status->setText(tr("当前文档命中 %1 条").arg(items.size()));
}

void SearchDock::showStatus(const QString &text)
{
    m_status->setText(text);
}
