#pragma once
// ai/AiWorker.h — 独立线程上的 AI 对话循环。
// ReadDocument 按需读取当前文档; Insert 经 GUI 线程写入光标处。

#include <QObject>
#include <QString>
#include <QVector>

#include <atomic>

#include "ai/Llm.h"
#include "ai/Types.h"

class AiChatDock;

class AiWorker : public QObject {
    Q_OBJECT
public:
    explicit AiWorker(QObject *parent = nullptr);

    void setDock(AiChatDock *dock) { dock_ = dock; }

    void applyConfig(const AiLlmConfig &cfg);
    void setTransport(Llm::Transport transport) { llm_.setTransport(std::move(transport)); }
    bool hasProvider() const { return !cfg_.apiKey.isEmpty(); }
    // GUI 侧关窗/退出前调用:跨线程原子置位,worker 立即放弃后续 GUI 回调,
    // 不再卡在 BlockingQueuedConnection 里拖死线程退出
    void requestStop() { stop_.store(true); }
    void clearHistory() { history_.clear(); }
    // 会话持久化(均要求工作线程空闲时调用)
    QVector<ChatMessage> historySnapshot() const { return history_; }
    void restoreHistory(const QVector<ChatMessage> &h) { history_ = h; }
    void truncateHistory(int keepCount)
    {
        if (keepCount >= 0 && keepCount < history_.size())
            history_.resize(keepCount);
    }

public slots:
    void run(const QString &userText, const QString &docMarkdown,
             int writeMode, const QString &thinkEffort, const QVector<AiAttach> &images);

signals:
    void textDelta(const QString &text);
    void thinkingDelta(const QString &text); // 思维链增量(面板滚动显示,避免"无响应"观感)
    void turnStats(qint64 thinkMs, double tps, qint64 outTokens, qint64 inTokens); // 轮末统计
    void turnRendered(const QString &fullText); // 本轮原始全文(供面板重渲染)
    void turnFinished(const QString &error);   // 空 = 正常结束
    void busyChanged(bool busy);

private:
    QString runInsert(const QString &text); // 阻塞回调到 GUI 线程
    AiDocumentResult readDocument(const QJsonObject &request);

    AiLlmConfig cfg_;
    Llm llm_;
    QVector<ChatMessage> history_;
    AiChatDock *dock_ = nullptr;
    std::atomic_bool stop_ = false; // GUI 请求终止:GUI 回调走超时放行,不再无限阻塞
};
