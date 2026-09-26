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
#include <QLabel>
#include <algorithm>

ChatWebView::ChatWebView(ChatModel *model, QWidget *parent)
    : WebViewHost(parent), m_model(model)
{
    updateFallback();
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
            m_rendered=true;
            m_failures=0;
        } else if(type==QLatin1String("chatError")) {
            qWarning()<<"Mswrite: chat page error"<<o.value("message").toString();
            m_rendered=false;updateFallback();
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
          QDir(web).absolutePath(), QStringLiteral("https://chat.local/chat.html?v=2"));
}

void ChatWebView::updateFallback()
{
    if(m_rendered) return;
    QStringList messages;
    int size=0;
    for(int i=m_model->rowCount()-1;i>=0 && size<28000;--i) {
        const auto *message=m_model->msgAt(i);
        if(message->kind==ChatMsg::Thinking) continue;
        const QString text=QStringLiteral("## %1\n\n%2").arg(message->kind==ChatMsg::User ? tr("我") : tr("AI"),message->text);
        messages.prepend(text);size+=text.size();
    }
    showStartupPreview(messages.join("\n\n"),m_light ? "light" : "dark",16);
    if(auto *hint=findChild<QLabel *>(QStringLiteral("startupHint")))
        hint->setText(tr("对话内容已保留 · 正在准备排版…"));
}

void ChatWebView::showEvent(QShowEvent *event)
{
    WebViewHost::showEvent(event);
    // Resynchronize after hidden-window navigation or a browser reload.
    m_reset=true;schedule();
}

void ChatWebView::schedule()
{
    m_pendingSync=true;
    if (!m_timer.isActive()) m_timer.start();
}

void ChatWebView::setLightTheme(bool light) { m_light = light; schedule(); }
void ChatWebView::setBusy(bool busy) { m_busy = busy; schedule(); }

void ChatWebView::scrollToBottom(bool force)
{
    if (m_ready)
        runScript(force ? QStringLiteral("window.chatView.scrollToBottom(true)")
                        : QStringLiteral("window.chatView.scrollToBottom(false)"));
}

void ChatWebView::sync()
{
    updateFallback();
    if (!m_ready || m_inFlight) return;
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
    const QJsonObject payload{{"reset", m_reset}, {"count", m_model->rowCount()},
                             {"rows", rows}, {"light", m_light}, {"busy", m_busy}};
    m_dirty.clear();
    m_reset = false;
    m_inFlight=true;
    evalWithResult(QStringLiteral("(()=>{if(!window.chatView)return false;window.chatView.update(%1);return true;})()")
        .arg(QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact))),
        [guard=QPointer<ChatWebView>(this)](const QString &result){
            if(!guard)return;
            guard->m_inFlight=false;
            if(result!=QLatin1String("true")) {
                guard->m_rendered=false;guard->m_reset=true;guard->updateFallback();
                if(++guard->m_failures<3) guard->m_timer.start(250);
                return;
            }
            if(guard->m_pendingSync || guard->m_reset || !guard->m_dirty.isEmpty()) guard->schedule();
        });
}
