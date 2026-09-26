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
    static QUrl endpoint(Protocol p, const QString &baseUrl);
    static QByteArray requestBody(Protocol p,
                                  const QString &model,
                                  const QString &system,
                                  const QVector<ChatMessage> &history,
                                  const QVector<ToolSchema> &tools,
                                  bool stream = false,
                                  int maxTokens = 4096,
                                  int thinkLevel = 0); // 0关 1低 2中 3高
    static QList<QPair<QByteArray, QByteArray>> headers(Protocol p, const QString &apiKey);
    static ChatResponse parse(Protocol p, int httpStatus, const QByteArray &body);

    class StreamAssembler {
    public:
        explicit StreamAssembler(Protocol p, StreamSink sink = {});
        void feed(const QByteArray &data);
        ChatResponse finish(int httpStatus, const QByteArray &raw, const QString &transportError);
        int eventCount() const { return events_; }

    private:
        void applyAnthropic(const QJsonObject &obj);
        void applyOpenAi(const QJsonObject &obj);
        void emitThinking(const QString &delta);
        void emitText(const QString &delta);

        void emitTextRaw(const QString &delta);
        void splitThinkTags(const QString &delta);

        Protocol protocol_;
        StreamSink sink_;
        ChatResponse out_;
        struct PendingTool {
            QString id;
            QString name;
            QByteArray args;
        };
        QVector<PendingTool> tools_;
        int events_ = 0;
        bool sawSse_ = false;
        bool inThinkTag_ = false;
    };
};
