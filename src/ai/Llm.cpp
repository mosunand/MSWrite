// ai/Llm.cpp — see ai/Llm.h.

#include "ai/Llm.h"

#include "ai/Http.h"
#include "ai/LlmCodec.h"

#include <QJsonDocument>
#include <QJsonObject>

namespace {

// 网关对未知字段(enable_thinking 等)报错时识别出来,剥掉重试
bool looksLikeUnknownField(const ChatResponse &r)
{
    const QString e = r.error.toLower();
    return e.contains(QLatin1String("enable_thinking"))
        || e.contains(QLatin1String("unknown field"))
        || e.contains(QLatin1String("unrecognized"))
        || e.contains(QLatin1String("extra inputs are not permitted"))
        || e.contains(QLatin1String("unexpected keyword"));
}

} // namespace

ChatResponse Llm::complete(const QString &system,
                           const QVector<ChatMessage> &history,
                           const QVector<ToolSchema> &tools,
                           StreamSink sink) const
{
    if (cfg_.protocol == Protocol::Unsupported) {
        ChatResponse error;
        error.error = QStringLiteral("Unsupported AI protocol");
        return error;
    }
    const QUrl url = LlmCodec::endpoint(cfg_.protocol, cfg_.baseUrl, cfg_.model, false);
    const auto headers = LlmCodec::headers(cfg_.protocol, cfg_.apiKey);
    const QString model = cfg_.model; // 原样发送,不拼任何后缀

    if (transport_) {
        const QByteArray body = LlmCodec::requestBody(cfg_.protocol, model, system, history, tools, false, cfg_.maxTokens, cfg_.thinkEffort);
        ChatResponse raw = transport_(cfg_.protocol, url, body, headers, cfg_.timeoutMs);
        if (!raw.raw.isEmpty()) {
            ChatResponse parsed = LlmCodec::parse(cfg_.protocol, raw.httpStatus, raw.raw);
            if (!raw.error.isEmpty()) { parsed.error = raw.error; parsed.toolCalls.clear(); }
            return parsed;
        }
        return raw;
    }

    auto runStream = [&](bool extras) {
        QByteArray streamBody = LlmCodec::requestBody(cfg_.protocol, model, system, history, tools, true, cfg_.maxTokens, cfg_.thinkEffort);
        if (!extras) {
            QJsonObject obj = QJsonDocument::fromJson(streamBody).object();
            obj.remove(QStringLiteral("enable_thinking"));
            obj.remove(QStringLiteral("thinking"));
            obj.remove(QStringLiteral("reasoning_effort"));
            obj.remove(QStringLiteral("reasoning"));
            streamBody = QJsonDocument(obj).toJson(QJsonDocument::Compact);
        }
        LlmCodec::StreamAssembler assembler(cfg_.protocol, sink);
        const QUrl streamUrl = LlmCodec::endpoint(cfg_.protocol, cfg_.baseUrl, cfg_.model, true);
        const HttpResult hr = Http::postSse(streamUrl, streamBody, headers, cfg_.timeoutMs,
                                            [&](const QByteArray &data) { assembler.feed(data); });
        return assembler.finish(hr.status, hr.body, hr.error);
    };

    ChatResponse streamed = runStream(true);
    if (looksLikeUnknownField(streamed) && streamed.sseEvents == 0)
        streamed = runStream(false);

    if (streamed.sseEvents > 0 || !streamed.text.isEmpty() || !streamed.toolCalls.isEmpty())
        return streamed;
    if (streamed.error == QStringLiteral("已中断") || streamed.error.contains(QLatin1String("timeout"), Qt::CaseInsensitive))
        return streamed;
    if (!streamed.error.isEmpty() && streamed.httpStatus >= 400 && !looksLikeUnknownField(streamed))
        return streamed;

    // 流式完全没起来:回退非流式
    const QByteArray body = LlmCodec::requestBody(cfg_.protocol, model, system, history, tools, false, cfg_.maxTokens, cfg_.thinkEffort);
    const HttpResult plain = Http::postJson(url, body, headers, cfg_.timeoutMs);
    ChatResponse parsed = LlmCodec::parse(cfg_.protocol, plain.status, plain.body);
    if (!plain.error.isEmpty()) {
        parsed.error = plain.error;
        parsed.toolCalls.clear(); // A partial HTTP body must never authorize document writes.
        return parsed;
    }
    if (!parsed.thinking.isEmpty() && sink.onThinking)
        sink.onThinking(parsed.thinking);
    if (!parsed.text.isEmpty() && sink.onText && !parsed.streamedText)
        sink.onText(parsed.text);
    return parsed;
}
