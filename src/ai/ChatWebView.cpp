#include "ai/ChatWebView.h"
#include "ai/ChatView.h"
#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QUrl>
#include <QPointer>
#include <QSettings>
#include <algorithm>

ChatWebView::ChatWebView(ChatModel *model, QWidget *parent)
    : WebViewHost(parent), m_model(model)
{
    m_light = QSettings().value(QStringLiteral("aiTheme"),
        QSettings().value(QStringLiteral("theme"), QStringLiteral("light")).toString() == QLatin1String("dark") ? 0 : 1).toInt() == 1;
    showLoading(m_light ? "light" : "dark", tr("正在加载对话…"));
    connect(this, &WebViewHost::loadingRetry, this, [this] {
        m_ready = false; m_rendered = false; m_reset = true; m_inFlight = false; m_failures = 0;
        ++m_sequence;
    });
    m_timer.setSingleShot(true);
    m_timer.setInterval(60);
    connect(&m_timer, &QTimer::timeout, this, &ChatWebView::sync);
    connect(model, &QAbstractItemModel::rowsInserted, this,
            [this](const QModelIndex &, int first, int last) {
        for (int i = first; i <= last; ++i) m_dirty.insert(i);
        schedule();
    });
    connect(model, &QAbstractItemModel::dataChanged, this,
            [this](const QModelIndex &first, const QModelIndex &last) {
        for (int i = first.row(); i <= last.row(); ++i) m_dirty.insert(i);
        schedule();
    });
    connect(model, &QAbstractItemModel::rowsRemoved, this, [this] { schedule(); });
    connect(model, &QAbstractItemModel::modelReset, this, [this] {
        m_reset = true;
        m_dirty.clear();
        schedule();
    });
    connect(this, &WebViewHost::message, this, [this](const QJsonObject &o) {
        const QString type = o.value("t").toString();
        if (type == QLatin1String("chatReady")) {
            m_ready = true;
            m_reset = true;
            sync();
        } else if(type==QLatin1String("chatRendered")) {
            if (o.value("sequence").toInt() != m_sequence) return;
            m_rendered=true;
            m_failures=0;
            finishLoading();
        } else if(type==QLatin1String("chatError")) {
            qWarning()<<"Mswrite: chat page error"<<o.value("message").toString();
            m_rendered=false;
            showLoading(m_light ? "light" : "dark", tr("正在加载对话…"));
            showLoadingError(tr("对话排版失败，请重新加载。对话内容仍然保留。"));
        } else if (type == QLatin1String("chatCopy")) {
            QApplication::clipboard()->setText(o.value("text").toString());
        } else if (type == QLatin1String("chatRegenerate") && !m_busy) {
            const int row = o.value("row").toInt(-1);
            const auto *m = m_model->msgAt(row);
            if (m && m->kind == ChatMsg::Assistant && m->finalized)
                emit regenerateRequested(row);
        } else if (type == QLatin1String("chatExport")) {
            emit exportRequested();
        } else if (type == QLatin1String("chatLink")) {
            const QUrl url(o.value("url").toString());
            if (url.scheme() == QLatin1String("https") || url.scheme() == QLatin1String("http"))
                QDesktopServices::openUrl(url);
        }
    }, Qt::QueuedConnection); // Leave the WebView2 COM callback before clipboard/dialog operations.
    const QString exe = QCoreApplication::applicationDirPath();
    QString web = exe + QStringLiteral("/resources/web");
#ifdef MSWRITE_SOURCE_DIR
    if (!QFileInfo::exists(web + QStringLiteral("/chat.html")))
        web = QStringLiteral(MSWRITE_SOURCE_DIR) + QStringLiteral("/resources/web");
#endif
    start(exe + QStringLiteral("/webview-data"), QStringLiteral("chat.local"),
          QDir(web).absolutePath(), QStringLiteral("https://chat.local/chat.html?v=3"));
}

void ChatWebView::showEvent(QShowEvent *event)
{
    WebViewHost::showEvent(event);
    // Visibility changes do not invalidate the DOM or the user's scroll position.
    schedule();
}

void ChatWebView::schedule()
{
    m_pendingSync=true;
    if (!m_timer.isActive()) m_timer.start();
}

void ChatWebView::setLightTheme(bool light) { m_light = light; setLoadingTheme(light ? "light" : "dark"); schedule(); }
void ChatWebView::setBusy(bool busy) { m_busy = busy; schedule(); }

void ChatWebView::scrollToBottom(bool force)
{
    if (m_ready)
        runScript(force ? QStringLiteral("window.chatView.scrollToBottom(true)")
                        : QStringLiteral("window.chatView.scrollToBottom(false)"));
}

void ChatWebView::sync()
{
    if (!m_ready || m_inFlight || !isVisible()) return;
    m_pendingSync=false;
    QJsonArray rows;
    QList<int> changed;
    if (m_reset) {
        changed.reserve(m_model->rowCount());
        for (int i = 0; i < m_model->rowCount(); ++i) changed.append(i);
    } else {
        changed = m_dirty.values();
        std::sort(changed.begin(), changed.end());
    }
    for (int i : changed) {
        const auto *m = m_model->msgAt(i);
        if (!m) continue;
        rows.append(QJsonObject{{"id", i}, {"kind", int(m->kind)}, {"text", m->text},
            {"meta", m->meta}, {"role", m->role}, {"finalized", m->finalized},
            {"fullText", m->fullText}, {"expandable", m->expandable}, {"preferLatex",m->preferLatex}});
    }
    const int sequence = ++m_sequence;
    const QJsonObject payload{{"reset", m_reset}, {"count", m_model->rowCount()}, {"sequence", sequence},
                             {"rows", rows}, {"light", m_light}, {"busy", m_busy}};
    m_dirty.clear();
    m_reset = false;
    m_inFlight=true;
    evalWithResult(QStringLiteral("(()=>{if(!window.chatView)return false;window.chatView.update(%1);return true;})()")
        .arg(QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact))),
        [guard=QPointer<ChatWebView>(this), sequence](const QString &result){
            if(!guard || sequence != guard->m_sequence)return;
            guard->m_inFlight=false;
            if(result!=QLatin1String("true")) {
                guard->m_rendered=false;guard->m_reset=true;
                guard->showLoading(guard->m_light ? "light" : "dark", tr("正在加载对话…"));
                if(++guard->m_failures<3) guard->m_timer.start(250);
                else guard->showLoadingError(tr("对话排版失败，请重新加载。对话内容仍然保留。"));
                return;
            }
            if(guard->m_pendingSync || guard->m_reset || !guard->m_dirty.isEmpty()) guard->schedule();
        });
}
