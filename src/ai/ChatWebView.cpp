#include "ai/ChatWebView.h"
#include "ai/ChatView.h"
#include <QApplication>
#include <QClipboard>
#include <QDateTime>
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
    // AI 面板是用户显式打开的常驻窗口;失焦/被遮挡时 WebView2 会挂起
    // 页面渲染进程,之后所有 ExecuteScript 返回空值 —— 这正是"页面
    // 空闲数秒后 update 全部失败、重载后永久卡死"的根源。导出页用
    // setAlwaysVisible 解决同类问题,聊天页同款处理。
    setAlwaysVisible(true);
    // 主题跟随主窗口 (不再读 aiTheme 旧覆盖值：主窗口浅色/AI 深色的
    // 不一致即源于此)。主窗口切主题时由 followHostTheme 实时同步。
    m_light = QSettings().value(QStringLiteral("theme"), QStringLiteral("light"))
                  .toString() != QLatin1String("dark");
    showLoading(m_light ? "light" : "dark", tr("正在加载对话…"));
    connect(this, &WebViewHost::loadingRetry, this, [this] {
        // 重载启动:全状态复位。watchdog 不在此刻武装 —— 重载本身需要
        // 时间,若重载等待期就武装,15s 后又会 retryLoading,形成
        // 重载→等待→再重载 的永动机(主题切换后"正在加载对话…"卡死的
        // 真正闭环)。等 chatReady 到达、发出首个 update 后再武装。
        m_ready = false; m_rendered = false; m_renderPending = false;
        m_reset = true; m_inFlight = false; m_failures = 0;
        ++m_sequence;
        m_watchdog.stop();
        if (!m_timer.isActive())
            m_timer.start();
    });
    // 自愈看门狗:首帧渲染(发出 update 后)15 秒无 chatRendered 才重载。
    // 最多自动重试 2 次,之后保留手动"重新加载"
    m_watchdog.setSingleShot(true);
    m_watchdog.setInterval(15000);
    connect(&m_watchdog, &QTimer::timeout, this, [this] {
        if (!isVisible()) {
            m_watchdog.stop();
            return;
        }
        // 只有"正在等待本帧渲染确认"才算卡死;重载等待期(未发过 update)
        // 不算,直接返回。
        if (!m_renderPending)
            return;
        qWarning() << "Mswrite: 对话页 15s 未渲染,自动重载(" << m_autoRetries + 1 << "/2)"
                   << "envErr=" << WebViewHost::environmentError();
        if (++m_autoRetries > 2) {
            qWarning() << "Mswrite: 对话页进入死态,整链重建";
            m_ready = false;
            m_rendered = false;
            m_renderPending = false;
            m_reset = true;
            m_inFlight = false;
            m_failures = 0;
            m_autoRetries = 0;
            m_watchdog.stop();
            ++m_sequence;
            showLoading(m_light ? "light" : "dark", tr("正在加载对话…"));
            recreateBrowser();
            return;
        }
        retryLoading();
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
            schedule();
        } else if(type==QLatin1String("chatRendered")) {
            if (o.value("sequence").toInt() != m_sequence) return;
            m_rendered=true;
            m_renderPending=false;
            m_failures=0;
            m_autoRetries=0;
            m_watchdog.stop();
            finishLoading();
        } else if(type==QLatin1String("chatError")) {
            const int errorSequence = o.value(QStringLiteral("sequence")).toInt(-1);
            if (errorSequence >= 0 && errorSequence != m_sequence)
                return;
            // 无 sequence 且页面早已稳定渲染：属于旧页面/主题切换的瞬时错误。
            // 绝不能因此挂上加载遮罩，否则用户会永久卡在"正在加载对话…"。
            if (errorSequence < 0 && m_rendered && !m_renderPending) {
                qWarning() << "Mswrite: 忽略稳定页面上的过期聊天错误"
                           << o.value("message").toString();
                return;
            }
            qWarning()<<"Mswrite: chat page error"<<o.value("message").toString();
            // 页面尚未渲染完成时的错误多半来自半加载状态(与 sync 空结果
            // 同一策略):静默重试,不挂遮罩不重载,避免加载窗口内死循环。
            if (!m_rendered) {
                if (++m_failures < 6) {
                    QTimer::singleShot(300, this, [guard = QPointer<ChatWebView>(this)] {
                        if (!guard || guard->m_rendered) return;
                        guard->schedule();
                    });
                    return;
                }
                m_failures = 0;
                retryLoading();
                return;
            }
            m_rendered=false;
            m_renderPending=false;
            m_watchdog.stop();
            m_timer.stop();
            if (++m_failures < 3) {
                showLoading(m_light ? "light" : "dark", tr("正在加载对话…"));
                QTimer::singleShot(500, this, [guard = QPointer<ChatWebView>(this)] {
                    if (!guard || guard->m_rendered || !guard->isLoading())
                        return;
                    guard->retryLoading();
                });
            } else {
                showLoadingError(tr("对话排版失败，请重新加载。对话内容仍然保留。"));
            }
        } else if (type == QLatin1String("chatCopy")) {
            QApplication::clipboard()->setText(o.value("text").toString());
        } else if (type == QLatin1String("chatRegenerate") && !m_busy) {
            const int row = o.value("row").toInt(-1);
            const auto *m = m_model->msgAt(row);
            if (m && m->kind == ChatMsg::Assistant && m->finalized)
                emit regenerateRequested(row);
        } else if (type == QLatin1String("chatAskEdit")) {
            // 用户消息"复制进询问框":原文回填输入框,继续追问
            emit askEditRequested(o.value("text").toString());
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
          QDir(web).absolutePath(), QStringLiteral("https://chat.local/chat.html?v=9"));
}

void ChatWebView::showEvent(QShowEvent *event)
{
    WebViewHost::showEvent(event);
    // 可见性变化不会使 DOM 或用户滚动位置失效
    schedule();
}

void ChatWebView::hideEvent(QHideEvent *event)
{
    m_watchdog.stop();
    WebViewHost::hideEvent(event);
}

void ChatWebView::schedule()
{
    m_pendingSync=true;
    if (!m_timer.isActive()) m_timer.start();
    // 只有"本帧 update 已发出、等待 chatRendered 确认"才武装看门狗。
    // 重载等待期(页面未就绪,sync 被跳过)绝不武装:否则重载必然超时,
    // 触发 重载→再超时→再重载 的死循环。
    if (m_renderPending && isVisible() && !m_watchdog.isActive())
        m_watchdog.start();
}

void ChatWebView::setLightTheme(bool light)
{
    const bool changed = m_light != light;
    m_light = light;
    setLoadingTheme(light ? "light" : "dark");
    if (!changed)
        return;

    const quint64 epoch = ++m_themeEpoch;
    // 只有页面稳定时才直连主题脚本，避免打在半初始化的 DOM 上；
    // 其他时候只保留最新 light，等下一次 payload 自然携带。
    const bool stable = m_ready && isPageReady() && m_rendered
                        && !m_renderPending && !m_inFlight;
    if (!stable) {
        schedule();
        return;
    }

    // 让主题立刻写入 DOM；回调只认当前 epoch，过期结果不覆盖状态。
    const QString script = QStringLiteral(
        "(()=>{try{document.documentElement.dataset.theme='%1';"
        "if(window.chatView&&typeof window.chatView.setTheme==='function')"
        "{window.chatView.setTheme(%2)};return true;"
        "}catch(e){return false}})()")
        .arg(light ? QLatin1String("light") : QLatin1String("dark"),
             light ? QLatin1String("true") : QLatin1String("false"));
    evalWithResult(script, [guard = QPointer<ChatWebView>(this), epoch](const QString &result) {
        if (!guard || epoch != guard->m_themeEpoch || result != QLatin1String("true"))
            return;
        guard->m_appliedThemeEpoch = epoch;
    });
}
void ChatWebView::setBusy(bool busy) { m_busy = busy; schedule(); }

void ChatWebView::scrollToBottom(bool force)
{
    if (m_ready)
        runScript(force ? QStringLiteral("window.chatView.scrollToBottom(true)")
                        : QStringLiteral("window.chatView.scrollToBottom(false)"));
}

void ChatWebView::sync()
{
    if (!m_ready || !isVisible()) {
        // 排障：转圈挂着却发不出同步时，记下卡在哪一环 (5s 节流防刷屏)
        if (isLoading() && !m_rendered) {
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            if (now - m_lastSkipLog > 5000) {
                m_lastSkipLog = now;
                qWarning() << "Mswrite: 对话同步被跳过"
                           << "ready=" << m_ready << "inFlight=" << m_inFlight
                           << "visible=" << isVisible();
            }
        }
        return;
    }
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
        QJsonObject row{{"id", i}, {"kind", int(m->kind)}, {"text", m->text},
            {"meta", m->meta}, {"role", m->role}, {"finalized", m->finalized},
            {"fullText", m->fullText}, {"expandable", m->expandable}, {"preferLatex",m->preferLatex}};
        // 用户消息的图片附件（截图等）以 data URL 直出，页面只渲染不转存
        if (!m->images.isEmpty()) {
            QJsonArray imgs;
            for (const AiAttach &a : m->images)
                imgs.append(QJsonObject{{"n", a.name},
                    {"src", QStringLiteral("data:%1;base64,%2").arg(a.mime, a.base64)}});
            row.insert(QStringLiteral("images"), imgs);
        }
        rows.append(row);
    }
    const int sequence = ++m_sequence;
    const QJsonObject payload{{"reset", m_reset}, {"count", m_model->rowCount()}, {"sequence", sequence},
                             {"rows", rows}, {"light", m_light}, {"busy", m_busy}};
    m_dirty.clear();
    m_reset = false;
    m_renderPending=true;
    m_inFlight=true;
    // 回调自保:WebView2 在页面导航/重载边缘会静默丢弃 ExecuteScript 的
    // 完成回调(handler 永远不被 Invoke)。原实现 m_inFlight 会因此永久卡
    // true,后续 sync 全被跳过。这里用超时兜底:无回调视为丢失,复位
    // inFlight 并重试。绝不因回调丢失而重载页面 —— 页面本身是健康的
    // (BOOT/chatReady/chatRendered 均正常),只是 C++ 侧的回执丢了。
    // 4s:远大于正常全量渲染耗时(46KB 约 0.3s),只为兜住真丢失。
    QTimer::singleShot(4000, this, [guard = QPointer<ChatWebView>(this), sequence]() {
        if (!guard || guard->m_sequence != sequence || !guard->m_inFlight)
            return;   // 回调已正常到达(或已有更新一轮),无需处理
        qWarning() << "Mswrite: 对话 update 回调丢失,超时复位重试";
        guard->m_inFlight = false;
        guard->m_renderPending = false;
        guard->m_reset = true;
        guard->schedule();
    });
    evalWithResult(QStringLiteral("(()=>{if(!window.chatView)return false;window.chatView.update(%1);return true;})()")
        .arg(QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact))),
        [guard=QPointer<ChatWebView>(this), sequence](const QString &result){
            if(!guard || sequence != guard->m_sequence)return;
            guard->m_inFlight=false;
            if(result!=QLatin1String("true")) {
                // null/空 = 执行上下文已死(主题切换触发样式重算时渲染侧崩溃,
                // 实测 navigate() 无法复活 —— 重载后 BOOT 永不再来);"false" =
                // chatView 未就绪。前 2 次静默重试对付瞬态;第 3 次必须整链
                // 重建控制器 —— 唯一被验证能复活页面的手段。
                guard->m_renderPending=false;
                guard->m_reset=true;   // 下次全量重发,主题/消息变更不丢
                if (++guard->m_failures <= 2) {
                    QTimer::singleShot(300, guard, [guard] {
                        if (!guard || guard->m_rendered) return;
                        guard->schedule();
                    });
                    return;
                }
                qWarning() << "Mswrite: 对话 update 连续失败,整链重建恢复";
                guard->showLoading(guard->m_light ? "light" : "dark", tr("正在加载对话…"));
                guard->m_failures = 0;
                guard->m_ready = false;
                guard->m_rendered = false;
                guard->m_inFlight = false;
                guard->m_watchdog.stop();
                ++guard->m_sequence;
                // 防御:此刻仍在 ExecuteScript 完成回调的 COM 栈内,控制器
                // 销毁必须退回事件循环再执行,否则有重入风险。
                QTimer::singleShot(0, guard, [guard] {
                    if (!guard || guard->isClosing()) return;
                    guard->recreateBrowser();
                });
                return;
            }
            if(guard->m_pendingSync || guard->m_reset || !guard->m_dirty.isEmpty()) guard->schedule();
        });
}
