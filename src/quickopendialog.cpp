#include "quickopendialog.h"

#include <QDirIterator>
#include <QFileInfo>
#include <QFont>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QSet>
#include <QShortcut>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include <utility>

namespace {
constexpr int kMaxWorkspaceEntries = 400;
}

QuickOpenDialog::QuickOpenDialog(const QStringList &recentFiles, const QString &workspace,
                                 QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("快速打开"));
    setModal(true);
    resize(620, 420);

    m_all.reserve(recentFiles.size() + 64);

    // 1) 最近文件(优先)
    for (const QString &f : recentFiles) {
        if (!QFileInfo::exists(f))
            continue;
        const QFileInfo fi(f);
        m_all.append({ fi.fileName(), fi.absolutePath(), fi.absoluteFilePath(), true });
    }

    auto *lay = new QVBoxLayout(this);
    auto *top = new QHBoxLayout;
    top->addWidget(new QLabel(tr("打开文件:"), this));
    m_edit = new QLineEdit(this);
    m_edit->setPlaceholderText(tr("输入文件名过滤,回车打开"));
    m_edit->setClearButtonEnabled(true);
    top->addWidget(m_edit, 1);
    lay->addLayout(top);

    m_list = new QListWidget(this);
    m_list->setUniformItemSizes(false);
    lay->addWidget(m_list, 1);

    connect(m_edit, &QLineEdit::textChanged, this, [this](const QString &t) { refill(t); });
    connect(m_edit, &QLineEdit::returnPressed, this, &QuickOpenDialog::acceptCurrent);
    connect(m_list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *) {
        acceptCurrent();
    });
    auto *up = new QShortcut(QKeySequence(Qt::Key_Up), m_edit, nullptr, nullptr, Qt::WidgetShortcut);
    auto *down = new QShortcut(QKeySequence(Qt::Key_Down), m_edit, nullptr, nullptr, Qt::WidgetShortcut);
    connect(up, &QShortcut::activated, this, [this] {
        const int r = qMax(0, m_list->currentRow() - 1);
        m_list->setCurrentRow(r);
    });
    connect(down, &QShortcut::activated, this, [this] {
        const int r = qMin(m_list->count() - 1, m_list->currentRow() + 1);
        m_list->setCurrentRow(r);
    });

    refill(QString());
    m_edit->setFocus();

    // 工作区收集放显示之后,并且放到后台线程:大目录递归扫描既不会阻塞
    // 对话框出现,也不会卡住输入(旧实现在 GUI 线程里一次性扫完)
    QTimer::singleShot(0, this, [this, workspace] {
        if (m_workspaceEntriesCollected)
            return;
        m_workspaceEntriesCollected = true;
        if (workspace.isEmpty() || !QFileInfo(workspace).isDir())
            return;
        QStringList known;
        for (const auto &e : std::as_const(m_all))
            known << e.path;

        auto *watcher = new QFutureWatcher<QVector<Entry>>(this);
        connect(watcher, &QFutureWatcher<QVector<Entry>>::finished, this, [this, watcher] {
            const QVector<Entry> found = watcher->result();
            watcher->deleteLater();
            if (found.isEmpty())
                return;
            m_all += found;
            refill(m_edit->text()); // 收集完成,按当前过滤词刷新列表
        });
        watcher->setFuture(QtConcurrent::run([workspace, known]() -> QVector<Entry> {
            QVector<Entry> out;
            QSet<QString> listed(known.constBegin(), known.constEnd());
            QDirIterator it(workspace,
                            { QStringLiteral("*.md"), QStringLiteral("*.markdown"),
                              QStringLiteral("*.mdown"), QStringLiteral("*.txt"),
                              QStringLiteral("*.pdf") },
                            QDir::Files, QDirIterator::Subdirectories);
            int count = 0;
            while (it.hasNext() && count < kMaxWorkspaceEntries) {
                const QString p = QDir::cleanPath(it.next());
                const QFileInfo fi(p);
                if (fi.suffix().compare(QLatin1String("pdf"), Qt::CaseInsensitive) != 0
                    && fi.suffix().compare(QLatin1String("txt"), Qt::CaseInsensitive) != 0
                    && !p.endsWith(QLatin1String(".md"), Qt::CaseInsensitive)
                    && !p.endsWith(QLatin1String(".markdown"), Qt::CaseInsensitive)
                    && !p.endsWith(QLatin1String(".mdown"), Qt::CaseInsensitive))
                    continue;
                if (listed.contains(p))
                    continue;
                // 跳过隐藏目录与资源目录
                const QString rel = QDir(workspace).relativeFilePath(p);
                if (rel.startsWith(QLatin1Char('.')) || rel.startsWith(QStringLiteral("assets")))
                    continue;
                out.append({ fi.fileName(), fi.absolutePath(), p, false });
                ++count;
            }
            std::sort(out.begin(), out.end(), [](const Entry &a, const Entry &b) {
                return a.path.compare(b.path, Qt::CaseInsensitive) < 0;
            });
            return out;
        }));
    });
}

void QuickOpenDialog::refill(const QString &filter)
{
    m_list->clear();
    const QString f = filter.trimmed();
    const QString lower = f.toLower();
    int shown = 0;
    for (const Entry &e : std::as_const(m_all)) {
        if (!f.isEmpty()
            && !e.name.toLower().contains(lower)
            && !e.path.toLower().contains(lower))
            continue;
        QString label = e.name + QStringLiteral("   ")
                      + QDir::toNativeSeparators(e.dir);
        auto *item = new QListWidgetItem(label, m_list);
        item->setData(Qt::UserRole, e.path);
        if (e.recent) {
            QFont font = item->font();
            font.setBold(true);
            item->setFont(font);
        }
        if (++shown >= 100)
            break;
    }
    if (m_list->count() > 0)
        m_list->setCurrentRow(0);
}

void QuickOpenDialog::acceptCurrent()
{
    QListWidgetItem *item = m_list->currentItem();
    if (!item)
        item = m_list->item(0);
    if (!item)
        return;
    m_selected = item->data(Qt::UserRole).toString();
    accept();
}
