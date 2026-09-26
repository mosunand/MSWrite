#pragma once
// ai/Llm.h — 模型调用(流式优先,失败回退非流式)。

#include "ai/Types.h"

#include <QList>
#include <QPair>
#include <QString>
#include <QUrl>
#include <QVector>
#include <functional>

// 当前供应商生成的运行时配置
struct AiLlmConfig {
    QString apiKey;
    QString baseUrl;
    QString model;          // 原样发送:需要 [1M] 等后缀时直接写进模型名
    Protocol protocol = Protocol::Anthropic;
    int maxTokens = 4096;
    int timeoutMs = 120000;
    int thinkLevel = 3;     // 思考程度 0关/1低/2中/3高(默认高;模型不支持会自动降级重试)
};

class Llm {
public:
    using Transport = std::function<ChatResponse(Protocol,
                                                 const QUrl &,
                                                 const QByteArray &,
                                                 const QList<QPair<QByteArray, QByteArray>> &,
                                                 int)>;

    void setConfig(const AiLlmConfig &cfg) { cfg_ = cfg; }
    void setTransport(Transport t) { transport_ = std::move(t); } // 测试注入用

    ChatResponse complete(const QString &system,
                          const QVector<ChatMessage> &history,
                          const QVector<ToolSchema> &tools,
                          StreamSink sink = {}) const;

private:
    AiLlmConfig cfg_;
    Transport transport_;
};
