#pragma once
#include "webviewhost.h"
#include <QSet>
#include <QTimer>

class ChatModel;

// One browser surface for selectable Markdown, code and KaTeX. Only changed
// messages cross the bridge; the page owns scrolling and text selection.
class ChatWebView : public WebViewHost {
    Q_OBJECT
public:
    explicit ChatWebView(ChatModel *model, QWidget *parent = nullptr);
    void setLightTheme(bool light);
    void setBusy(bool busy);
    void scrollToBottom(bool force = false);

signals:
    void regenerateRequested(int row);
    void exportRequested();
    void askEditRequested(const QString &text); // 用户消息"复制进询问框"

protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    void sync();
    void schedule();
    ChatModel *m_model;
    QTimer m_timer;
    QSet<int> m_dirty;
    bool m_reset = true;
    bool m_ready = false;
    bool m_light = false;
    bool m_busy = false;
    bool m_rendered = false;
    bool m_renderPending = false;
    bool m_inFlight = false;
    bool m_pendingSync = false;
    int m_failures = 0;
    int m_sequence = 0;
    quint64 m_themeEpoch = 0;
    quint64 m_appliedThemeEpoch = 0;  // 最后一次应用的主题版本号
    QTimer m_watchdog;   // 对话页 15s 未渲染 → 自动重载(最多 2 次)
    int m_autoRetries = 0;
    qint64 m_lastSkipLog = 0;   // sync 跳过原因的诊断日志节流
};
