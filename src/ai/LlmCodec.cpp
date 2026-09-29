// ai/LlmCodec.cpp — see ai/LlmCodec.h.

#include "ai/LlmCodec.h"

#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QUuid>

namespace {

QJsonObject jsonSchema(const ToolSchema &t)
{
    return QJsonObject{
        { QStringLiteral("type"), QStringLiteral("object") },
        { QStringLiteral("properties"), t.parameters.value(QStringLiteral("properties")) },
        { QStringLiteral("required"), t.parameters.value(QStringLiteral("required")) },
    };
}

QJsonArray anthropicTools(const QVector<ToolSchema> &tools)
{
    QJsonArray a;
    for (const ToolSchema &t : tools) {
        a.append(QJsonObject{
            { QStringLiteral("name"), t.name },
            { QStringLiteral("description"), t.description },
            { QStringLiteral("input_schema"), t.parameters.isEmpty() ? jsonSchema(t) : t.parameters },
        });
    }
    return a;
}

QJsonArray openaiTools(const QVector<ToolSchema> &tools)
{
    QJsonArray a;
    for (const ToolSchema &t : tools) {
        a.append(QJsonObject{
            { QStringLiteral("type"), QStringLiteral("function") },
            { QStringLiteral("function"),
              QJsonObject{
                  { QStringLiteral("name"), t.name },
                  { QStringLiteral("description"), t.description },
                  { QStringLiteral("parameters"), t.parameters },
              } },
        });
    }
    return a;
}

QJsonArray anthropicMessages(const QVector<ChatMessage> &history)
{
    QJsonArray msgs;
    QJsonArray currentContent;
    QString currentRole;

    auto flush = [&]() {
        if (currentRole.isEmpty())
            return;
        msgs.append(QJsonObject{
            { QStringLiteral("role"), currentRole },
            { QStringLiteral("content"), currentContent },
        });
        currentContent = QJsonArray();
        currentRole.clear();
    };

    auto ensure = [&](const QString &role) {
        if (currentRole != role) {
            flush();
            currentRole = role;
        }
    };

    for (const ChatMessage &m : history) {
        if (m.role == QLatin1String("system"))
            continue;
        if (m.role == QLatin1String("user")) {
            ensure(QStringLiteral("user"));
            if (!m.text.isEmpty() || m.images.isEmpty()) {
                currentContent.append(QJsonObject{
                    { QStringLiteral("type"), QStringLiteral("text") },
                    { QStringLiteral("text"), m.text },
                });
            }
            for (const AiAttach &img : m.images) {
                currentContent.append(QJsonObject{
                    { QStringLiteral("type"), QStringLiteral("image") },
                    { QStringLiteral("source"), QJsonObject{
                        { QStringLiteral("type"), QStringLiteral("base64") },
                        { QStringLiteral("media_type"), img.mime },
                        { QStringLiteral("data"), img.base64 },
                    } },
                });
            }
        } else if (m.role == QLatin1String("assistant")) {
            ensure(QStringLiteral("assistant"));
            if (!m.text.isEmpty()) {
                currentContent.append(QJsonObject{
                    { QStringLiteral("type"), QStringLiteral("text") },
                    { QStringLiteral("text"), m.text },
                });
            }
            for (const ToolCall &c : m.toolCalls) {
                currentContent.append(QJsonObject{
                    { QStringLiteral("type"), QStringLiteral("tool_use") },
                    { QStringLiteral("id"), c.id },
                    { QStringLiteral("name"), c.name },
                    { QStringLiteral("input"), c.input },
                });
            }
        } else if (m.role == QLatin1String("tool")) {
            ensure(QStringLiteral("user"));
            currentContent.append(QJsonObject{
                { QStringLiteral("type"), QStringLiteral("tool_result") },
                { QStringLiteral("tool_use_id"), m.toolCallId },
                { QStringLiteral("content"), m.text },
            });
        }
    }
    flush();
    return msgs;
}

QJsonArray openaiMessages(const QString &system, const QVector<ChatMessage> &history)
{
    QJsonArray msgs;
    msgs.append(QJsonObject{
        { QStringLiteral("role"), QStringLiteral("system") },
        { QStringLiteral("content"), system },
    });
    for (const ChatMessage &m : history) {
        if (m.role == QLatin1String("system"))
            continue;
        if (m.role == QLatin1String("user")) {
            if (m.images.isEmpty()) {
                msgs.append(QJsonObject{
                    { QStringLiteral("role"), QStringLiteral("user") },
                    { QStringLiteral("content"), m.text },
                });
            } else {
                // 多模态:text + image_url(data:base64)
                QJsonArray content;
                if (!m.text.isEmpty())
                    content.append(QJsonObject{
                        { QStringLiteral("type"), QStringLiteral("text") },
                        { QStringLiteral("text"), m.text },
                    });
                for (const AiAttach &img : m.images) {
                    content.append(QJsonObject{
                        { QStringLiteral("type"), QStringLiteral("image_url") },
                        { QStringLiteral("image_url"), QJsonObject{
                            { QStringLiteral("url"),
                              QStringLiteral("data:%1;base64,%2").arg(img.mime, img.base64) },
                        } },
                    });
                }
                msgs.append(QJsonObject{
                    { QStringLiteral("role"), QStringLiteral("user") },
                    { QStringLiteral("content"), content },
                });
            }
        } else if (m.role == QLatin1String("assistant")) {
            QJsonObject o{ { QStringLiteral("role"), QStringLiteral("assistant") } };
            o.insert(QStringLiteral("content"), m.text);
            if (!m.toolCalls.isEmpty()) {
                QJsonArray tcs;
                for (const ToolCall &c : m.toolCalls) {
                    tcs.append(QJsonObject{
                        { QStringLiteral("id"), c.id },
                        { QStringLiteral("type"), QStringLiteral("function") },
                        { QStringLiteral("function"),
                          QJsonObject{
                              { QStringLiteral("name"), c.name },
                              { QStringLiteral("arguments"),
                                QString::fromUtf8(QJsonDocument(c.input).toJson(QJsonDocument::Compact)) },
                          } },
                    });
                }
                o.insert(QStringLiteral("tool_calls"), tcs);
            }
            msgs.append(o);
        } else if (m.role == QLatin1String("tool")) {
            msgs.append(QJsonObject{
                { QStringLiteral("role"), QStringLiteral("tool") },
                { QStringLiteral("tool_call_id"), m.toolCallId },
                { QStringLiteral("name"), m.toolName },
                { QStringLiteral("content"), m.text },
            });
        }
    }
    return msgs;
}

QJsonObject parseObject(const QByteArray &raw)
{
    const QJsonDocument d = QJsonDocument::fromJson(raw);
    if (d.isObject())
        return d.object();
    return {};
}

QJsonObject argsToObject(const QString &args)
{
    const QJsonDocument d = QJsonDocument::fromJson(args.toUtf8());
    return d.isObject() ? d.object() : QJsonObject{};
}

} // namespace

QUrl LlmCodec::endpoint(Protocol p, const QString &baseUrl)
{
    QString b = baseUrl;
    while (b.endsWith(QLatin1Char('/')))
        b.chop(1);
    if (p == Protocol::Anthropic) {
        if (b.endsWith(QLatin1String("/v1/messages")))
            return QUrl(b);
        if (b.endsWith(QLatin1String("/v1")))
            return QUrl(b + QStringLiteral("/messages"));
        return QUrl(b + QStringLiteral("/v1/messages"));
    }
    if (b.endsWith(QLatin1String("/chat/completions")))
        return QUrl(b);
    if (b.endsWith(QLatin1String("/v1")))
        return QUrl(b + QStringLiteral("/chat/completions"));
    return QUrl(b + QStringLiteral("/v1/chat/completions"));
}

QList<QPair<QByteArray, QByteArray>> LlmCodec::headers(Protocol p, const QString &apiKey)
{
    QList<QPair<QByteArray, QByteArray>> h;
    auto add = [&](const char *k, const QByteArray &v) {
        h.append(qMakePair(QByteArray(k), v));
    };
    add("Content-Type", QByteArray("application/json"));
    if (p == Protocol::Anthropic) {
        add("x-api-key", apiKey.toUtf8());
        add("anthropic-version", QByteArray("2023-06-01"));
        add("Authorization", QByteArray("Bearer ") + apiKey.toUtf8());
    } else {
        add("Authorization", QByteArray("Bearer ") + apiKey.toUtf8());
    }
    return h;
}

QByteArray LlmCodec::requestBody(Protocol p,
                                 const QString &model,
                                 const QString &system,
                                 const QVector<ChatMessage> &history,
                                 const QVector<ToolSchema> &tools,
                                 bool stream,
                                 int maxTokens,
                                 int thinkLevel)
{
    QJsonObject body;
    body.insert(QStringLiteral("model"), model);
    if (stream)
        body.insert(QStringLiteral("stream"), true);
    if (p == Protocol::Anthropic) {
        body.insert(QStringLiteral("max_tokens"), maxTokens > 0 ? maxTokens : 4096);
        body.insert(QStringLiteral("system"), system);
        body.insert(QStringLiteral("messages"), anthropicMessages(history));
        body.insert(QStringLiteral("tools"), anthropicTools(tools));
        if (thinkLevel > 0) {
            // 思考预算(anthropic 约定);必须小于 max_tokens,留 512 余量
            const int budget = thinkLevel >= 3 ? 8192 : (thinkLevel == 2 ? 4096 : 1024);
            const int capped = qMin(budget, (maxTokens > 0 ? maxTokens : 4096) - 512);
            if (capped >= 1024) {
                body.insert(QStringLiteral("thinking"), QJsonObject{
                    { QStringLiteral("type"), QStringLiteral("enabled") },
                    { QStringLiteral("budget_tokens"), capped },
                });
            }
        }
    } else {
        body.insert(QStringLiteral("messages"), openaiMessages(system, history));
        body.insert(QStringLiteral("tools"), openaiTools(tools));
        body.insert(QStringLiteral("tool_choice"), QStringLiteral("auto"));
        if (maxTokens > 0)
            body.insert(QStringLiteral("max_tokens"), maxTokens);
        if (model.contains(QLatin1String("glm"), Qt::CaseInsensitive) && thinkLevel > 0)
            body.insert(QStringLiteral("enable_thinking"), true);
        if (thinkLevel > 0)
            body.insert(QStringLiteral("reasoning_effort"),
                        thinkLevel >= 3 ? QStringLiteral("high")
                                        : (thinkLevel == 2 ? QStringLiteral("medium")
                                                           : QStringLiteral("low")));
    }
    return QJsonDocument(body).toJson(QJsonDocument::Compact);
}

ChatResponse LlmCodec::parse(Protocol p, int httpStatus, const QByteArray &body)
{
    ChatResponse r;
    r.httpStatus = httpStatus;
    r.raw = body;
    const QJsonObject obj = parseObject(body);
    if (httpStatus >= 400 || obj.contains(QStringLiteral("error"))) {
        const QJsonValue err = obj.value(QStringLiteral("error"));
        if (err.isObject())
            r.error = err.toObject().value(QStringLiteral("message")).toString();
        else if (err.isString())
            r.error = err.toString();
        if (r.error.isEmpty())
            r.error = QStringLiteral("HTTP %1: %2").arg(httpStatus).arg(QString::fromUtf8(body.left(400)));
        return r;
    }

    if (p == Protocol::Anthropic) {
        r.stopReason = obj.value(QStringLiteral("stop_reason")).toString();
        {
            const QJsonObject u = obj.value(QStringLiteral("usage")).toObject();
            r.inputTokens = u.value(QStringLiteral("input_tokens")).toVariant().toLongLong();
            r.outputTokens = u.value(QStringLiteral("output_tokens")).toVariant().toLongLong();
        }
        const QJsonArray content = obj.value(QStringLiteral("content")).toArray();
        for (const QJsonValue &v : content) {
            const QJsonObject c = v.toObject();
            const QString type = c.value(QStringLiteral("type")).toString();
            if (type == QLatin1String("text")) {
                r.text += c.value(QStringLiteral("text")).toString();
            } else if (type == QLatin1String("thinking") || type == QLatin1String("redacted_thinking")) {
                r.thinking += c.value(QStringLiteral("thinking")).toString();
            } else if (type == QLatin1String("tool_use")) {
                ToolCall tc;
                tc.id = c.value(QStringLiteral("id")).toString();
                tc.name = c.value(QStringLiteral("name")).toString();
                tc.input = c.value(QStringLiteral("input")).toObject();
                r.toolCalls.push_back(tc);
            }
        }
        return r;
    }

    const QJsonArray choices = obj.value(QStringLiteral("choices")).toArray();
    if (choices.isEmpty()) {
        r.error = QStringLiteral("empty choices in OpenAI response");
        return r;
    }
    const QJsonObject msg = choices.at(0).toObject().value(QStringLiteral("message")).toObject();
    r.text = msg.value(QStringLiteral("content")).toString();
    {
        const QJsonObject u = obj.value(QStringLiteral("usage")).toObject();
        r.inputTokens = u.value(QStringLiteral("prompt_tokens")).toVariant().toLongLong();
        r.outputTokens = u.value(QStringLiteral("completion_tokens")).toVariant().toLongLong();
        if (r.inputTokens == 0 && u.contains(QStringLiteral("input_tokens")))
            r.inputTokens = u.value(QStringLiteral("input_tokens")).toVariant().toLongLong();
        if (r.outputTokens == 0 && u.contains(QStringLiteral("output_tokens")))
            r.outputTokens = u.value(QStringLiteral("output_tokens")).toVariant().toLongLong();
    }
    if (msg.value(QStringLiteral("reasoning_content")).isString())
        r.thinking += msg.value(QStringLiteral("reasoning_content")).toString();
    if (msg.value(QStringLiteral("reasoning")).isString())
        r.thinking += msg.value(QStringLiteral("reasoning")).toString();
    r.stopReason = choices.at(0).toObject().value(QStringLiteral("finish_reason")).toString();
    {
        // 网关偶尔把思维链以 <think>...</think> 内联在 content 里:拆出来
        const int a = r.text.indexOf(QStringLiteral("<think>"));
        const int b = r.text.indexOf(QStringLiteral("</think>"));
        if (a >= 0 && b > a) {
            r.thinking += r.text.mid(a + 7, b - a - 7);
            r.text = (r.text.left(a) + r.text.mid(b + 8)).trimmed();
        }
    }
    const QJsonArray tcs = msg.value(QStringLiteral("tool_calls")).toArray();
    for (const QJsonValue &v : tcs) {
        const QJsonObject o = v.toObject();
        const QJsonObject fn = o.value(QStringLiteral("function")).toObject();
        ToolCall tc;
        tc.id = o.value(QStringLiteral("id")).toString();
        if (tc.id.isEmpty())
            tc.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        tc.name = fn.value(QStringLiteral("name")).toString();
        tc.input = argsToObject(fn.value(QStringLiteral("arguments")).toString());
        r.toolCalls.push_back(tc);
    }
    return r;
}

LlmCodec::StreamAssembler::StreamAssembler(Protocol p, StreamSink sink)
    : protocol_(p)
    , sink_(std::move(sink))
{
}

void LlmCodec::StreamAssembler::emitThinking(const QString &delta)
{
    if (delta.isEmpty())
        return;
    out_.thinking += delta;
    if (sink_.onThinking)
        sink_.onThinking(delta);
}

void LlmCodec::StreamAssembler::emitTextRaw(const QString &delta)
{
    if (delta.isEmpty())
        return;
    out_.text += delta;
    out_.streamedText = true;
    if (sink_.onText)
        sink_.onText(delta);
}

void LlmCodec::StreamAssembler::splitThinkTags(const QString &delta)
{
    QString s = delta;
    while (!s.isEmpty()) {
        if (inThinkTag_) {
            const int end = s.indexOf(QStringLiteral("</think>"));
            if (end < 0) {
                emitThinking(s);
                return;
            }
            emitThinking(s.left(end));
            s = s.mid(end + 8);
            inThinkTag_ = false;
            continue;
        }
        const int start = s.indexOf(QStringLiteral("<think>"));
        if (start < 0) {
            emitTextRaw(s);
            return;
        }
        if (start > 0)
            emitTextRaw(s.left(start));
        s = s.mid(start + 7);
        inThinkTag_ = true;
    }
}

void LlmCodec::StreamAssembler::emitText(const QString &delta)
{
    if (delta.isEmpty())
        return;
    if (inThinkTag_ || delta.contains(QStringLiteral("<think>")))
        splitThinkTags(delta);
    else
        emitTextRaw(delta);
}

void LlmCodec::StreamAssembler::applyAnthropic(const QJsonObject &obj)
{
    const QString type = obj.value(QStringLiteral("type")).toString();
    if (type == QLatin1String("message_start")) {
        const QJsonObject u = obj.value(QStringLiteral("message")).toObject()
                                 .value(QStringLiteral("usage")).toObject();
        out_.inputTokens = u.value(QStringLiteral("input_tokens")).toVariant().toLongLong();
        return;
    }
    if (type == QLatin1String("content_block_start")) {
        const QJsonObject block = obj.value(QStringLiteral("content_block")).toObject();
        const QString btype = block.value(QStringLiteral("type")).toString();
        const int index = obj.value(QStringLiteral("index")).toInt();
        if (btype == QLatin1String("tool_use")) {
            while (tools_.size() <= index)
                tools_.push_back({});
            tools_[index].id = block.value(QStringLiteral("id")).toString();
            tools_[index].name = block.value(QStringLiteral("name")).toString();
            const QJsonValue inputVal = block.value(QStringLiteral("input"));
            if (inputVal.isObject() && !inputVal.toObject().isEmpty())
                tools_[index].args = QJsonDocument(inputVal.toObject()).toJson(QJsonDocument::Compact);
        } else if (btype == QLatin1String("thinking") || btype == QLatin1String("redacted_thinking")) {
            emitThinking(block.value(QStringLiteral("thinking")).toString());
        } else if (btype == QLatin1String("text")) {
            emitText(block.value(QStringLiteral("text")).toString());
        }
        return;
    }
    if (type == QLatin1String("content_block_delta")) {
        const QJsonObject delta = obj.value(QStringLiteral("delta")).toObject();
        const QString dtype = delta.value(QStringLiteral("type")).toString();
        const int index = obj.value(QStringLiteral("index")).toInt();
        if (dtype == QLatin1String("thinking_delta")
            || dtype == QLatin1String("reasoning_delta")
            || delta.contains(QStringLiteral("thinking")))
            emitThinking(delta.value(QStringLiteral("thinking")).toString());
        else if (dtype == QLatin1String("text_delta") || delta.contains(QStringLiteral("text")))
            emitText(delta.value(QStringLiteral("text")).toString());
        else if (dtype == QLatin1String("input_json_delta")) {
            while (tools_.size() <= index)
                tools_.push_back({});
            tools_[index].args += delta.value(QStringLiteral("partial_json")).toString().toUtf8();
        }
        return;
    }
    if (type == QLatin1String("message_delta")) {
        const QJsonObject delta = obj.value(QStringLiteral("delta")).toObject();
        const QString stop = delta.value(QStringLiteral("stop_reason")).toString();
        if (!stop.isEmpty())
            out_.stopReason = stop;
        const QJsonObject u = obj.value(QStringLiteral("usage")).toObject();
        if (u.contains(QStringLiteral("output_tokens")))
            out_.outputTokens = u.value(QStringLiteral("output_tokens")).toVariant().toLongLong();
        return;
    }
    if (obj.contains(QStringLiteral("error"))) {
        const QJsonValue err = obj.value(QStringLiteral("error"));
        if (err.isObject())
            out_.error = err.toObject().value(QStringLiteral("message")).toString();
        else
            out_.error = err.toString();
    }
}

void LlmCodec::StreamAssembler::applyOpenAi(const QJsonObject &obj)
{
    if (obj.contains(QStringLiteral("error"))) {
        const QJsonValue err = obj.value(QStringLiteral("error"));
        if (err.isObject())
            out_.error = err.toObject().value(QStringLiteral("message")).toString();
        else
            out_.error = err.toString();
        return;
    }
    if (obj.contains(QStringLiteral("usage"))) {
        const QJsonObject u = obj.value(QStringLiteral("usage")).toObject();
        out_.inputTokens = u.value(QStringLiteral("prompt_tokens")).toVariant().toLongLong();
        out_.outputTokens = u.value(QStringLiteral("completion_tokens")).toVariant().toLongLong();
        if (out_.inputTokens == 0 && u.contains(QStringLiteral("input_tokens")))
            out_.inputTokens = u.value(QStringLiteral("input_tokens")).toVariant().toLongLong();
        if (out_.outputTokens == 0 && u.contains(QStringLiteral("output_tokens")))
            out_.outputTokens = u.value(QStringLiteral("output_tokens")).toVariant().toLongLong();
    }
    const QJsonArray choices = obj.value(QStringLiteral("choices")).toArray();
    if (choices.isEmpty())
        return;
    const QJsonObject ch = choices.at(0).toObject();
    const QString fr = ch.value(QStringLiteral("finish_reason")).toString();
    if (!fr.isEmpty())
        out_.stopReason = fr;
    const QJsonObject delta = ch.contains(QStringLiteral("delta"))
        ? ch.value(QStringLiteral("delta")).toObject()
        : ch.value(QStringLiteral("message")).toObject();
    emitText(delta.value(QStringLiteral("content")).toString());
    const auto takeThink = [&](const QJsonValue &v) {
        if (v.isString())
            emitThinking(v.toString());
        else if (v.isObject()) {
            const QJsonObject o = v.toObject();
            if (o.value(QStringLiteral("content")).isString())
                emitThinking(o.value(QStringLiteral("content")).toString());
            if (o.value(QStringLiteral("text")).isString())
                emitThinking(o.value(QStringLiteral("text")).toString());
        }
    };
    takeThink(delta.value(QStringLiteral("reasoning_content")));
    takeThink(delta.value(QStringLiteral("reasoning")));
    takeThink(delta.value(QStringLiteral("reasoning_text")));
    takeThink(delta.value(QStringLiteral("thinking")));
    const QJsonArray tcs = delta.value(QStringLiteral("tool_calls")).toArray();
    for (const QJsonValue &v : tcs) {
        const QJsonObject o = v.toObject();
        int index = o.value(QStringLiteral("index")).toInt();
        if (index < 0)
            index = 0;
        while (tools_.size() <= index)
            tools_.push_back({});
        const QString id = o.value(QStringLiteral("id")).toString();
        if (!id.isEmpty())
            tools_[index].id = id;
        const QJsonObject fn = o.value(QStringLiteral("function")).toObject();
        const QString name = fn.value(QStringLiteral("name")).toString();
        if (!name.isEmpty())
            tools_[index].name = name;
        tools_[index].args += fn.value(QStringLiteral("arguments")).toString().toUtf8();
    }
}

void LlmCodec::StreamAssembler::feed(const QByteArray &data)
{
    if (data.trimmed() == "[DONE]")
        return;
    const QJsonObject obj = QJsonDocument::fromJson(data).object();
    if (obj.isEmpty())
        return;
    sawSse_ = true;
    ++events_;
    if (protocol_ == Protocol::Anthropic)
        applyAnthropic(obj);
    else
        applyOpenAi(obj);
}

ChatResponse LlmCodec::StreamAssembler::finish(int httpStatus, const QByteArray &raw,
                                                const QString &transportError)
{
    out_.httpStatus = httpStatus;
    out_.raw = raw;
    out_.sseEvents = events_;
    // 4xx/5xx:响应体里的错误信息远比传输层错误有用(如 DeepSeek 的
    // "you passed deepseek-flash[512K]"),优先解析它,没有才退回传输错误
    if (httpStatus >= 400) {
        const ChatResponse parsed = LlmCodec::parse(protocol_, httpStatus, raw);
        if (!parsed.error.isEmpty())
            out_.error = parsed.error;
        else if (!transportError.isEmpty())
            out_.error = transportError;
        else
            out_.error = QStringLiteral("HTTP %1").arg(httpStatus);
    } else if (!transportError.isEmpty()) {
        // 传输层错误必须透传,不能只在"一字未收"时采纳 —— 流中途断开时
        // 已收到的部分文本不是完整答案,静默当完整回复返回会截断内容
        if (transportError == QLatin1String("已中断")) {
            out_.error = transportError;   // 用户主动中断
        } else if (!out_.text.isEmpty() || !out_.toolCalls.isEmpty() || sawSse_) {
            out_.error = transportError + QStringLiteral("(回复可能不完整)");
        } else {
            out_.error = transportError;
        }
    }
    if (!sawSse_ && out_.text.isEmpty() && tools_.isEmpty() && out_.error.isEmpty())
        return LlmCodec::parse(protocol_, httpStatus, raw);

    for (PendingTool &t : tools_) {
        if (t.name.isEmpty() && t.args.isEmpty())
            continue;
        ToolCall tc;
        tc.id = t.id;
        if (tc.id.isEmpty())
            tc.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        tc.name = t.name;
        tc.input = argsToObject(QString::fromUtf8(t.args));
        out_.toolCalls.push_back(tc);
    }
    return out_;
}
