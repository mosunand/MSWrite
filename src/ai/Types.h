#pragma once
// ai/Types.h — 消息、工具调用、模型响应(自 MS-Agent 移植)。

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>

struct ToolCall {
    QString id;
    QString name;
    QJsonObject input;
    QString thoughtSignature; // Gemini requires this opaque value on tool continuations.
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

// 单个模型的独立配置(一个供应商/网址下可挂多个模型,逐个配置)
struct AiModelCfg {
    QString id;                 // 模型名(原样发送;需要 [1M] 等后缀时直接写进 id)
    int contextWindow = 0;      // 上下文窗口 token 数(0 = 未设置,仅展示)
    int maxOutput = 0;          // 回复上限(0 = 默认,见 LlmCodec::kDefaultMaxTokens)
    bool smartConfig = true;    // 智能配置:开 = 推荐值,关闭才可手改
    QStringList thinkLevels{ QStringLiteral("low"), QStringLiteral("high") }; // 支持的思考档,可自定义追加
    bool inText = true;         // 文本输入(恒可输入,UI 锁定)
    bool inImage = false;       // 图片输入(视觉模型)
    bool inVideo = false;       // 视频(预留)
    bool inPdf = false;         // PDF(预留)
    bool capStructured = false; // 结构化输出
    bool capSearch = false;     // 原生联网搜索
    bool capSystem = false;     // 对话中系统消息
    bool enabled = true;
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
    OpenAiResponses,
    Gemini,
    Unsupported,
};

inline QString protocolName(Protocol p)
{
    switch (p) {
    case Protocol::Anthropic: return QStringLiteral("anthropic");
    case Protocol::OpenAi: return QStringLiteral("openai");
    case Protocol::OpenAiResponses: return QStringLiteral("openai-responses");
    case Protocol::Gemini: return QStringLiteral("gemini");
    case Protocol::Unsupported: return QStringLiteral("unsupported");
    }
    return QStringLiteral("unsupported");
}

inline Protocol protocolFromName(const QString &name)
{
    const QString value = name.trimmed().toLower();
    if (value == QLatin1String("anthropic")) return Protocol::Anthropic;
    if (value == QLatin1String("openai") || value == QLatin1String("openai_chat")) return Protocol::OpenAi;
    if (value == QLatin1String("openai-responses") || value == QLatin1String("openai_responses")) return Protocol::OpenAiResponses;
    if (value == QLatin1String("gemini")) return Protocol::Gemini;
    return Protocol::Unsupported;
}

Q_DECLARE_METATYPE(AiAttach)
Q_DECLARE_METATYPE(QVector<AiAttach>)
Q_DECLARE_METATYPE(QVector<ChatMessage>)
