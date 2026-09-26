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

protected:
    void showEvent(QShowEvent *event) override;

private:
    void sync();
    void schedule();
    void updateFallback();
    ChatModel *m_model;
    QTimer m_timer;
    QSet<int> m_dirty;
    bool m_reset = true;
    bool m_ready = false;
    bool m_light = false;
    bool m_busy = false;
    bool m_rendered = false;
    bool m_inFlight = false;
    bool m_pendingSync = false;
    int m_failures = 0;
};
