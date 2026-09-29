#pragma once
// ai/AiWorker.h — 独立线程上的 AI 对话循环。
// ReadDocument 按需读取当前文档; Insert 经 GUI 线程写入光标处。

#include <QObject>
#include <QPointer>
#include <QString>
#include <QVector>

#include "ai/Llm.h"
#include "ai/Types.h"

class AiChatDock;

class AiWorker : public QObject {
    Q_OBJECT
public:
    explicit AiWorker(QObject *parent = nullptr);

    // dock 用 QPointer:窗口先一步销毁(退出托管回收路径)时自动置空,
    // 裸指针在 BlockingQueued/排队回调里会悬垂
    void setDock(AiChatDock *dock) { dock_ = dock; }

    void applyConfig(const AiLlmConfig &cfg);
    void setTransport(Llm::Transport transport) { llm_.setTransport(std::move(transport)); }
    bool hasProvider() const { return !cfg_.apiKey.isEmpty(); }
    void clearHistory() { history_.clear(); }
    // 会话持久化(均要求工作线程空闲时调用)
    QVector<ChatMessage> historySnapshot() const { return history_; }
    void restoreHistory(const QVector<ChatMessage> &h) { history_ = h; }
    void truncateHistory(int keepCount)
    {
        if (keepCount >= 0 && keepCount < history_.size())
            history_.resize(keepCount);
    }
    // "重新生成"用:截到第 occurrence 条文本等于 text 的用户消息(含)。
    // GUI 行号与 worker 历史不是同一序列(思考/通知行、tool 消息都无对应),
    // 旧实现拿 GUI 行号直接当下标会切错位置。按内容+序数定位与显示层无关。
    void truncateAtUserMessage(const QString &text, int occurrence)
    {
        int seen = 0;
        for (int i = 0; i < history_.size(); ++i) {
            const ChatMessage &m = history_.at(i);
            if (m.role == QLatin1String("user") && m.text == text) {
                ++seen;
                if (seen == occurrence) {
                    history_.resize(i + 1);
                    return;
                }
            }
        }
        // 找不到(历史被裁剪过等):不截断,保留现状 —— 错切会造出
        // "有 tool_use 没 tool_result"的非法序列,网关直接 400
    }

public slots:
    void run(const QString &userText, const QString &docMarkdown,
             int writeMode, int thinkLevel, const QVector<AiAttach> &images);

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
    QPointer<AiChatDock> dock_ = nullptr;
};
