#pragma once
// ai/Types.h — 消息、工具调用、模型响应(自 MS-Agent 移植)。

#include <QJsonObject>
#include <QString>
#include <QVector>
#include <functional>

struct ToolCall {
    QString id;
    QString name;
    QJsonObject input;
};

// 发给模型的多模态图片附件(截图粘贴 / 文件上传)
struct AiAttach {
    QString name;    // 显示名(来源文件名)
    QString mime;    // image/png 等
    QString base64;  // 图片数据
};

struct AiDocumentResult {
    QString text;
    QVector<AiAttach> images;
};

struct ChatMessage {
    QString role;          // system | user | assistant | tool
    QString text;
    QVector<ToolCall> toolCalls; // assistant
    QString toolCallId;    // tool result
    QString toolName;
    QVector<AiAttach> images;    // user 消息的图片附件
};

struct StreamSink {
    std::function<void(const QString&)> onThinking;
    std::function<void(const QString&)> onText;
};

struct ChatResponse {
    QString text;
    QString thinking;
    QVector<ToolCall> toolCalls;
    QString stopReason;
    QString error;
    int httpStatus = 0;
    QByteArray raw;
    bool streamedText = false;
    int sseEvents = 0;
    qint64 inputTokens = 0;
    qint64 outputTokens = 0;
    QString model; // 实际请求的模型名回显
};

struct ToolSchema {
    QString name;
    QString description;
    QJsonObject parameters;
    bool readOnly = false;
};

enum class Protocol {
    Anthropic,
    OpenAi,
};

inline QString protocolName(Protocol p)
{
    return p == Protocol::Anthropic ? QStringLiteral("anthropic")
                                    : QStringLiteral("openai");
}

Q_DECLARE_METATYPE(AiAttach)
Q_DECLARE_METATYPE(QVector<AiAttach>)
