// ai/LlmCodec.cpp — see ai/LlmCodec.h.

#include "ai/LlmCodec.h"

#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QUuid>
#include <QRegularExpression>
#include <QUrlQuery>
#include <QDebug>

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

QJsonArray responsesInput(const QVector<ChatMessage> &history)
{
    QJsonArray input;
    for (const ChatMessage &m : history) {
        if (m.role == QLatin1String("tool")) {
            input.append(QJsonObject{{"type", "function_call_output"},
                                     {"call_id", m.toolCallId}, {"output", m.text}});
            continue;
        }
        if (m.role != QLatin1String("user") && m.role != QLatin1String("assistant")) continue;
        QJsonArray content;
        if (!m.text.isEmpty())
            content.append(QJsonObject{{"type", m.role == QLatin1String("assistant") ? "output_text" : "input_text"},
                                       {"text", m.text}});
        for (const AiAttach &image : m.images)
            content.append(QJsonObject{{"type", "input_image"},
                {"image_url", QStringLiteral("data:%1;base64,%2").arg(image.mime, image.base64)}});
        if (!content.isEmpty()) input.append(QJsonObject{{"role", m.role}, {"content", content}});
        for (const ToolCall &call : m.toolCalls)
            input.append(QJsonObject{{"type", "function_call"}, {"call_id", call.id},
                {"name", call.name}, {"arguments", QString::fromUtf8(QJsonDocument(call.input).toJson(QJsonDocument::Compact))}});
    }
    return input;
}

QJsonArray geminiContents(const QVector<ChatMessage> &history)
{
    QJsonArray contents;
    for (const ChatMessage &m : history) {
        if (m.role == QLatin1String("system")) continue;
        const QString role = m.role == QLatin1String("assistant") ? QStringLiteral("model") : QStringLiteral("user");
        QJsonArray parts;
        if (m.role == QLatin1String("tool")) {
            parts.append(QJsonObject{{"functionResponse", QJsonObject{{"name", m.toolName},
                {"response", QJsonObject{{"result", m.text}}}}}});
        } else {
            if (!m.text.isEmpty()) parts.append(QJsonObject{{"text", m.text}});
            for (const AiAttach &image : m.images)
                parts.append(QJsonObject{{"inlineData", QJsonObject{{"mimeType", image.mime}, {"data", image.base64}}}});
            for (const ToolCall &call : m.toolCalls) {
                QJsonObject part{{"functionCall", QJsonObject{{"name", call.name}, {"args", call.input}}}};
                if (!call.thoughtSignature.isEmpty()) part.insert("thoughtSignature", call.thoughtSignature);
                parts.append(part);
            }
        }
        if (parts.isEmpty()) continue;
        if (!contents.isEmpty() && contents.last().toObject().value("role").toString() == role) {
            QJsonObject last = contents.takeAt(contents.size() - 1).toObject();
            QJsonArray combined = last.value("parts").toArray();
            for (const QJsonValue &part : parts) combined.append(part);
            last.insert("parts", combined);
            contents.append(last);
        } else contents.append(QJsonObject{{"role", role}, {"parts", parts}});
    }
    return contents;
}

ChatResponse parseResponses(const QJsonObject &obj)
{
    ChatResponse r;
    r.model = obj.value("model").toString();
    const QJsonObject usage = obj.value("usage").toObject();
    r.inputTokens = usage.value("input_tokens").toVariant().toLongLong();
    r.outputTokens = usage.value("output_tokens").toVariant().toLongLong();
    r.stopReason = obj.value("status").toString();
    if (r.stopReason == QLatin1String("failed"))
        r.error = obj.value("error").toObject().value("message").toString("Response failed");
    else if (r.stopReason == QLatin1String("incomplete"))
        r.error = QStringLiteral("Response incomplete: %1").arg(obj.value("incomplete_details").toObject().value("reason").toString());
    for (const QJsonValue &value : obj.value("output").toArray()) {
        const QJsonObject item = value.toObject();
        const QString type = item.value("type").toString();
        if (type == QLatin1String("message")) {
            for (const QJsonValue &v : item.value("content").toArray()) {
                const QJsonObject part = v.toObject();
                if (part.value("type") == QLatin1String("output_text")) r.text += part.value("text").toString();
                if (part.value("type") == QLatin1String("refusal")) r.text += part.value("refusal").toString();
            }
        } else if (type == QLatin1String("reasoning")) {
            for (const QJsonValue &v : item.value("summary").toArray()) r.thinking += v.toObject().value("text").toString();
        } else if (type == QLatin1String("function_call")) {
            ToolCall call;
            call.id = item.value("call_id").toString();
            call.name = item.value("name").toString();
            call.input = argsToObject(item.value("arguments").toString());
            r.toolCalls.append(call);
        }
    }
    if (r.text.isEmpty() && r.toolCalls.isEmpty() && r.error.isEmpty()) r.error = QStringLiteral("Empty Responses output");
    return r;
}

ChatResponse parseGemini(const QJsonObject &obj)
{
    ChatResponse r;
    r.model = obj.value("modelVersion").toString();
    const QJsonObject usage = obj.value("usageMetadata").toObject();
    r.inputTokens = usage.value("promptTokenCount").toVariant().toLongLong();
    r.outputTokens = usage.value("candidatesTokenCount").toVariant().toLongLong();
    const QJsonArray candidates = obj.value("candidates").toArray();
    if (candidates.isEmpty()) {
        r.error = QStringLiteral("Gemini returned no candidates: %1")
            .arg(obj.value("promptFeedback").toObject().value("blockReason").toString());
        return r;
    }
    const QJsonObject candidate = candidates.first().toObject();
    r.stopReason = candidate.value("finishReason").toString();
    for (const QJsonValue &v : candidate.value("content").toObject().value("parts").toArray()) {
        const QJsonObject part = v.toObject();
        if (part.value("thought").toBool()) r.thinking += part.value("text").toString();
        else r.text += part.value("text").toString();
        if (part.contains("functionCall")) {
            const QJsonObject function = part.value("functionCall").toObject();
            ToolCall call;
            call.id = function.value("id").toString(QUuid::createUuid().toString(QUuid::WithoutBraces));
            call.name = function.value("name").toString();
            call.input = function.value("args").toObject();
            call.thoughtSignature = part.value("thoughtSignature").toString();
            r.toolCalls.append(call);
        }
    }
    if (r.stopReason == QLatin1String("SAFETY") || r.stopReason == QLatin1String("RECITATION")
        || r.stopReason == QLatin1String("MALFORMED_FUNCTION_CALL"))
        r.error = QStringLiteral("Gemini stopped: %1").arg(r.stopReason);
    return r;
}

} // namespace

QUrl LlmCodec::endpoint(Protocol p, const QString &baseUrl, const QString &model, bool stream)
{
    QUrl url(baseUrl.trimmed());
    QString b = url.path();
    while (b.endsWith(QLatin1Char('/')))
        b.chop(1);
    if (p == Protocol::Unsupported) return {};
    if (p == Protocol::Gemini) {
        const int models = b.indexOf(QStringLiteral("/models/"));
        if (models >= 0) b = b.left(models);
        if (!b.endsWith(QLatin1String("/v1beta")) && !b.endsWith(QLatin1String("/v1"))) b += QStringLiteral("/v1beta");
        QString modelId = model;
        if (modelId.startsWith(QLatin1String("models/"))) modelId.remove(0, 7);
        b += QStringLiteral("/models/%1:%2").arg(modelId,
            stream ? QStringLiteral("streamGenerateContent") : QStringLiteral("generateContent"));
        QUrlQuery query(url);
        if (stream) { query.removeAllQueryItems("alt"); query.addQueryItem("alt", "sse"); }
        url.setQuery(query);
    } else {
        const QString suffix = p == Protocol::Anthropic ? QStringLiteral("/messages")
                             : p == Protocol::OpenAiResponses ? QStringLiteral("/responses")
                             : QStringLiteral("/chat/completions");
        if (!b.endsWith(suffix)) {
            if (!b.endsWith(QLatin1String("/v1"))) b += QStringLiteral("/v1");
            b += suffix;
        }
    }
    url.setPath(b);
    return url;
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
    } else if (p == Protocol::Gemini) {
        add("x-goog-api-key", apiKey.toUtf8());
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
                                 const QString &thinkEffort)
{
    // thinkEffort:"" = 关;档位名(low/medium/high/max/自定义)按协议映射,
    // 自定义名(OpenAI 系)原样透传 —— 思考档位现在是每模型可配置列表
    const bool thinkOn = !thinkEffort.isEmpty();
    // Anthropic 思考预算需要具体数字:已知档映射,自定义按 high 处理
    const int thinkBudget = thinkEffort == QLatin1String("low") ? 1024
                          : thinkEffort == QLatin1String("medium") ? 4096
                          : thinkEffort == QLatin1String("max") ? 24576
                          : 8192; // high / 其它自定义
    QJsonObject body;
    body.insert(QStringLiteral("model"), model);
    if (stream)
        body.insert(QStringLiteral("stream"), true);
    if (p == Protocol::Anthropic) {
        body.insert(QStringLiteral("max_tokens"), maxTokens > 0 ? maxTokens : kDefaultMaxTokens);
        body.insert(QStringLiteral("system"), system);
        body.insert(QStringLiteral("messages"), anthropicMessages(history));
        body.insert(QStringLiteral("tools"), anthropicTools(tools));
        if (thinkOn) {
            // 思考预算(anthropic 约定);必须小于 max_tokens,留 512 余量
            const int capped = qMin(thinkBudget, (maxTokens > 0 ? maxTokens : kDefaultMaxTokens) - 512);
            if (capped >= 1024) {
                body.insert(QStringLiteral("thinking"), QJsonObject{
                    { QStringLiteral("type"), QStringLiteral("enabled") },
                    { QStringLiteral("budget_tokens"), capped },
                });
            }
        }
    } else if (p == Protocol::OpenAiResponses) {
        body.insert("instructions", system);
        body.insert("input", responsesInput(history));
        body.insert("store", false);
        body.insert("max_output_tokens", maxTokens > 0 ? maxTokens : kDefaultMaxTokens);
        QJsonArray functions;
        for (const ToolSchema &tool : tools)
            functions.append(QJsonObject{{"type", "function"}, {"name", tool.name},
                {"description", tool.description}, {"parameters", tool.parameters}, {"strict", false}});
        if (!functions.isEmpty()) { body.insert("tools", functions); body.insert("tool_choice", "auto"); }
        if (thinkOn) body.insert("reasoning", QJsonObject{{"effort", thinkEffort}});
    } else if (p == Protocol::Gemini) {
        body.remove("model");
        body.remove("stream");
        body.insert("systemInstruction", QJsonObject{{"parts", QJsonArray{QJsonObject{{"text", system}}}}});
        body.insert("contents", geminiContents(history));
        body.insert("generationConfig", QJsonObject{{"maxOutputTokens", maxTokens > 0 ? maxTokens : kDefaultMaxTokens}});
        QJsonArray functions;
        for (const ToolSchema &tool : tools)
            functions.append(QJsonObject{{"name", tool.name}, {"description", tool.description}, {"parameters", tool.parameters}});
        if (!functions.isEmpty()) body.insert("tools", QJsonArray{QJsonObject{{"functionDeclarations", functions}}});
    } else if (p == Protocol::OpenAi) {
        body.insert(QStringLiteral("messages"), openaiMessages(system, history));
        body.insert(QStringLiteral("tools"), openaiTools(tools));
        body.insert(QStringLiteral("tool_choice"), QStringLiteral("auto"));
        body.insert(QStringLiteral("max_tokens"), maxTokens > 0 ? maxTokens : kDefaultMaxTokens);
        if (model.contains(QLatin1String("glm"), Qt::CaseInsensitive) && thinkOn)
            body.insert(QStringLiteral("enable_thinking"), true);
        if (thinkOn)
            body.insert(QStringLiteral("reasoning_effort"), thinkEffort);
    }
    return QJsonDocument(body).toJson(QJsonDocument::Compact);
}

// DeepSeek V3.2 泄漏进正文的工具调用。规范格式(官方 encoding_dsv32.py,
// 逐字节核实):每侧【一根】全角竖线 U+FF5C:
//   <｜DSML｜function_calls>\n<｜DSML｜invoke name="Insert">\n
//   <｜DSML｜parameter name="text" string="true">内容</｜DSML｜parameter>\n
//   </｜DSML｜invoke>\n</｜DSML｜function_calls>
// 真实泄漏里还出现过双竖线损坏变体(<｜｜DSML｜｜ calls>,cline #14057),
// 故竖线按 1-2 根兼容;外层块名 V3.2=function_calls、V4=tool_calls、
// V4.1 带前导空格,invoke/parameter 关键字前允许空白。
// 历史注记:旧实现的闭合标签 "\uFF5C\uFF5C/uFF5C?" 少了反斜杠,匹配的
// 是字面量 "uFF5C",从未真正闭合过——2026-10 修正。
void LlmCodec::extractStrayToolCalls(ChatResponse &resp)
{
    const QChar bar(0xFF5C);
    const QString tag = QStringLiteral("<") + bar + QStringLiteral("{1,2}DSML") + bar
                        + QStringLiteral("{1,2}\\s*(?:function_calls|tool_calls|calls)?");
    const QString invokeOpen = tag + QStringLiteral("\\s*invoke\\s+name=\"([^\"]+)\"");
    const QString paramOpen = tag + QStringLiteral("\\s*parameter\\s+name=\"([^\"]*)\"[^>]*>");
    // 闭合标签:</｜DSML｜ parameter> 等;斜杠后同样 1-2 根竖线
    const QString anyClose = QStringLiteral("</") + bar + QStringLiteral("{1,2}DSML")
                             + bar + QStringLiteral("{1,2}\\s*(?:invoke|parameter|function_calls|tool_calls|calls)?\\s*>");
    static const QRegularExpression invokeRe(
        invokeOpen + QStringLiteral("[\\s\\S]*?") + paramOpen
                     + QStringLiteral("([\\s\\S]*?)") + anyClose);
    static const QRegularExpression tagRe(
        QStringLiteral("<") + bar + QStringLiteral("{1,2}DSML") + bar
        + QStringLiteral("{1,2}[^>]*>|</") + bar + QStringLiteral("{1,2}DSML")
        + bar + QStringLiteral("{1,2}[^>]*>"));

    bool found = false;
    QString cleaned;
    cleaned.reserve(resp.text.size());
    qsizetype prev = 0;
    auto it = invokeRe.globalMatch(resp.text);
    while (it.hasNext()) {
        const auto m = it.next();
        found = true;
        const QString toolName = m.captured(1);
        const QString paramName = m.captured(2);
        const QString paramValue = m.captured(3);

        if (toolName == QLatin1String("Insert") && paramName == QLatin1String("text")) {
            ToolCall tc;
            tc.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            tc.name = toolName;
            QJsonObject input;
            input.insert(paramName, paramValue);
            tc.input = input;
            resp.toolCalls.push_back(tc);
            qWarning() << "Mswrite: DSML 正文工具调用已提取(" << paramValue.size() << "字)";
        }
        cleaned += resp.text.mid(prev, m.capturedStart() - prev);
        prev = m.capturedEnd();
    }
    if (!found)
        return;
    cleaned += resp.text.mid(prev);

    // 清除所有残漏的 DSML 标签(包装 calls / 独立 parameter 闭标签等)
    cleaned.remove(tagRe);
    resp.text = cleaned.trimmed();
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

    if (p == Protocol::OpenAiResponses || p == Protocol::Gemini) {
        ChatResponse parsed = p == Protocol::OpenAiResponses ? parseResponses(obj) : parseGemini(obj);
        parsed.httpStatus = httpStatus;
        parsed.raw = body;
        return parsed;
    }
    if (p == Protocol::Unsupported) { r.error = QStringLiteral("Unsupported AI protocol"); return r; }
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

// 工具索引硬上限:远高于任何正规工具调用轮次,同时挡住畸形/恶意流里的
// 巨大 index 造成的内存膨胀。超过即丢弃该事件。
static constexpr int kMaxToolSlots = 256;

// 越界/越负的 index 一律拒绝:tools_ 用 operator[] 访问,负值会在 release
// 下静默越界读写堆内存。
bool LlmCodec::StreamAssembler::ensureToolSlot(int index)
{
    if (index < 0 || index >= kMaxToolSlots) {
        qWarning() << "Mswrite: 丢弃非法工具索引" << index;
        ++droppedEvents_;
        return false;
    }
    while (tools_.size() <= index)
        tools_.push_back({});
    return true;
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
        const int index = obj.value(QStringLiteral("index")).toInt(-1);
        if (btype == QLatin1String("tool_use")) {
            if (!ensureToolSlot(index))
                return;
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
        const int index = obj.value(QStringLiteral("index")).toInt(-1);
        if (dtype == QLatin1String("thinking_delta")
            || dtype == QLatin1String("reasoning_delta")
            || delta.contains(QStringLiteral("thinking")))
            emitThinking(delta.value(QStringLiteral("thinking")).toString());
        else if (dtype == QLatin1String("text_delta") || delta.contains(QStringLiteral("text")))
            emitText(delta.value(QStringLiteral("text")).toString());
        else if (dtype == QLatin1String("input_json_delta")) {
            // 单条工具参数上限:防止无限流累积吃光内存(8 MB 远超正常工具参数)
    static constexpr int kMaxToolArgs = 8 * 1024 * 1024;
    if (!ensureToolSlot(index))
                return;
            tools_[index].args += delta.value(QStringLiteral("partial_json")).toString().toUtf8();
            if (tools_[index].args.size() > kMaxToolArgs) {
                qWarning() << "Mswrite: 工具参数超过上限,截断";
                tools_[index].args.truncate(kMaxToolArgs);
            }
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
        int index = o.value(QStringLiteral("index")).toInt(-1);
        if (!ensureToolSlot(index))
            continue;
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

void LlmCodec::StreamAssembler::applyResponses(const QJsonObject &obj)
{
    const QString type = obj.value("type").toString();
    if (type == QLatin1String("response.output_text.delta") || type == QLatin1String("response.refusal.delta")) {
        emitText(obj.value("delta").toString());
    } else if (type == QLatin1String("response.reasoning_summary_text.delta")) {
        emitThinking(obj.value("delta").toString());
    } else if (type == QLatin1String("response.output_item.added") || type == QLatin1String("response.output_item.done")) {
        const QJsonObject item = obj.value("item").toObject();
        if (item.value("type") != QLatin1String("function_call")) return;
        const int index = obj.value("output_index").toInt(-1);
        if (index < 0 || index > 1024) { out_.error = QStringLiteral("Invalid Responses tool index"); return; }
        while (tools_.size() <= index) tools_.append(PendingTool{});
        tools_[index].id = item.value("call_id").toString();
        tools_[index].name = item.value("name").toString();
        if (item.contains("arguments")) tools_[index].args = item.value("arguments").toString().toUtf8();
    } else if (type == QLatin1String("response.function_call_arguments.delta")
               || type == QLatin1String("response.function_call_arguments.done")) {
        const int index = obj.value("output_index").toInt(-1);
        if (index < 0 || index > 1024) { out_.error = QStringLiteral("Invalid Responses tool index"); return; }
        while (tools_.size() <= index) tools_.append(PendingTool{});
        if (type.endsWith(QLatin1String(".done"))) tools_[index].args = obj.value("arguments").toString().toUtf8();
        else tools_[index].args += obj.value("delta").toString().toUtf8();
    } else if (type == QLatin1String("response.completed") || type == QLatin1String("response.failed")
               || type == QLatin1String("response.incomplete")) {
        const ChatResponse final = parseResponses(obj.value("response").toObject());
        out_.inputTokens = final.inputTokens;
        out_.outputTokens = final.outputTokens;
        out_.model = final.model;
        out_.stopReason = final.stopReason;
        if (out_.text.isEmpty()) emitText(final.text);
        if (out_.thinking.isEmpty()) emitThinking(final.thinking);
        if (!final.error.isEmpty()) out_.error = final.error;
        if (tools_.isEmpty()) out_.toolCalls = final.toolCalls;
    } else if (type == QLatin1String("error") || obj.contains("error")) {
        out_.error = obj.value("message").toString();
        if (out_.error.isEmpty()) out_.error = obj.value("error").toObject().value("message").toString("Responses stream error");
    }
}

void LlmCodec::StreamAssembler::applyGemini(const QJsonObject &obj)
{
    if (obj.contains("error")) {
        out_.error = obj.value("error").toObject().value("message").toString("Gemini stream error");
        return;
    }
    const ChatResponse part = parseGemini(obj);
    emitText(part.text);
    emitThinking(part.thinking);
    out_.toolCalls += part.toolCalls;
    if (obj.contains("usageMetadata")) { out_.inputTokens = part.inputTokens; out_.outputTokens = part.outputTokens; }
    if (!part.model.isEmpty()) out_.model = part.model;
    if (!part.stopReason.isEmpty()) out_.stopReason = part.stopReason;
    if (!part.error.isEmpty()) out_.error = part.error;
}

void LlmCodec::StreamAssembler::feed(const QByteArray &data)
{
    if (data.trimmed() == "[DONE]")
        return;
    QJsonParseError err{};
    QJsonObject obj = QJsonDocument::fromJson(data, &err).object();
    if (err.error != QJsonParseError::NoError
        && (data.contains('\n') || data.contains('\r'))) {
        // 合法 SSE 允许一条事件拆多行 data:(postSse 用 '\n' 拼接),但 JSON
        // 字符串里不能有真换行 —— 某些网关按固定宽度拆行,必须按"无换行
        // 拼接"重试一次
        QByteArray joined = data;
        joined.replace('\n', QByteArray());
        joined.replace('\r', QByteArray());
        obj = QJsonDocument::fromJson(joined, &err).object();
    }
    if (err.error != QJsonParseError::NoError && !pendingBad_.isEmpty()) {
        // 上一条事件解析失败,可能是一条事件被网关拆成多个 SSE 帧;先合并重试
        const QByteArray merged = pendingBad_ + data;
        QJsonParseError merr{};
        QJsonObject m = QJsonDocument::fromJson(merged, &merr).object();
        if (merr.error == QJsonParseError::NoError) {
            pendingBad_.clear();
            obj = m;
            err.error = QJsonParseError::NoError;
        } else {
            pendingBad_ = merged;
            if (pendingBad_.size() > 256 * 1024) {  // 防爆:残段过大就放弃
                pendingBad_.clear();
                ++droppedEvents_;
            }
            return;
        }
    }
    if (err.error != QJsonParseError::NoError) {
        // 本条解析失败且无残段可拼:先留到下一次 feed 合并;若它本来就是
        // 一条完整的坏事件,下一条能独立解析时会被丢弃(不会污染好数据)
        pendingBad_ = data;
        return;
    }
    if (!pendingBad_.isEmpty()) {
        // 上一条其实独立失败(不是被拆帧):丢弃,统计,不吞本条
        pendingBad_.clear();
        ++droppedEvents_;
    }
    sawSse_ = true;
    ++events_;
    if (protocol_ == Protocol::Anthropic)
        applyAnthropic(obj);
    else if (protocol_ == Protocol::OpenAiResponses)
        applyResponses(obj);
    else if (protocol_ == Protocol::Gemini)
        applyGemini(obj);
    else
        applyOpenAi(obj);
}

ChatResponse LlmCodec::StreamAssembler::finish(int httpStatus, const QByteArray &raw,
                                                const QString &transportError)
{
    out_.httpStatus = httpStatus;
    out_.raw = raw;
    out_.sseEvents = events_;
    if (!pendingBad_.isEmpty() || droppedEvents_ > 0)
        qWarning() << "Mswrite: AI 流式帧解析容错:丢弃" << droppedEvents_
                   << "条,尾部残段" << pendingBad_.size() << "字节";
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
        out_.error = transportError;
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
        const QJsonDocument args = QJsonDocument::fromJson(t.args);
        if (tc.name.isEmpty() || !args.isObject()) {
            out_.error = QStringLiteral("Incomplete or invalid streamed tool call");
            continue;
        }
        tc.input = args.object();
        tc.thoughtSignature = t.thoughtSignature;
        out_.toolCalls.push_back(tc);
    }
    return out_;
}
