#pragma once
// ai/LlmCodec.h — Anthropic Messages + OpenAI Chat Completions(含工具/SSE 流式)。
// 自 MS-Agent LlmCodec 移植。

#include "ai/Types.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QPair>
#include <QUrl>
#include <QVector>

class LlmCodec {
public:
    // 回复上限默认值:0 = 用此默认值(长回答 + 工具写入会重复一遍正文,
    // 4096 太容易把工具参数截断)
    static constexpr int kDefaultMaxTokens = 1000448;

    static QUrl endpoint(Protocol p, const QString &baseUrl,
                         const QString &model = QString(), bool stream = false);
    static QByteArray requestBody(Protocol p,
                                  const QString &model,
                                  const QString &system,
                                  const QVector<ChatMessage> &history,
                                  const QVector<ToolSchema> &tools,
                                  bool stream = false,
                                  int maxTokens = kDefaultMaxTokens,
                                  const QString &thinkEffort = QString()); // "" = 关;否则原样档位名(low/high/max/自定义)
    static QList<QPair<QByteArray, QByteArray>> headers(Protocol p, const QString &apiKey);
    static ChatResponse parse(Protocol p, int httpStatus, const QByteArray &body);

    // DeepSeek 系模型把工具调用以 DSML 标签泄漏进正文 text 时的兜底:
    // 从 text 提取 Insert 调用为正式 ToolCall,并把裸标签清出正文。
    // 标签用全角竖线 U+FF5C(规范单竖线,兼容双竖线损坏变体)。
    static void extractStrayToolCalls(ChatResponse &resp);

    class StreamAssembler {
    public:
        explicit StreamAssembler(Protocol p, StreamSink sink = {});
        void feed(const QByteArray &data);
        ChatResponse finish(int httpStatus, const QByteArray &raw, const QString &transportError);
        int eventCount() const { return events_; }

    private:
        void applyAnthropic(const QJsonObject &obj);
        void applyOpenAi(const QJsonObject &obj);
        void applyResponses(const QJsonObject &obj);
        void applyGemini(const QJsonObject &obj);
        void emitThinking(const QString &delta);
        void emitText(const QString &delta);
        bool ensureToolSlot(int index); // 钳制 index,扩容 tools_;非法返回 false

        void emitTextRaw(const QString &delta);
        void splitThinkTags(const QString &delta);

        Protocol protocol_;
        StreamSink sink_;
        ChatResponse out_;
        struct PendingTool {
            QString id;
            QString name;
            QByteArray args;
            QString thoughtSignature;
        };
        QVector<PendingTool> tools_;
        // 网关拆帧容错:解析失败的 SSE 事件原文暂存,与下一条合并重试
        QByteArray pendingBad_;
        int droppedEvents_ = 0; // 彻底放弃的事件数(记进响应统计)
        int events_ = 0;
        bool sawSse_ = false;
        bool inThinkTag_ = false;
    };
};
