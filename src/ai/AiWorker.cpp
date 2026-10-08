// ai/AiWorker.cpp — see ai/AiWorker.h.

#include "ai/AiWorker.h"

#include "ai/AiChatDock.h"
#include "ai/LlmCodec.h"
#include "ai/MswriteSkill.h"

#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonObject>
#include <QMetaObject>
#include <QUuid>
#include <QJsonDocument>
#include <QSemaphore>
#include <QPointer>
#include <memory>

namespace {

constexpr int kMaxTurns = 8;
constexpr int kHistoryCap = 40; // 发给模型的最大历史条数(从用户消息边界裁)

// 从模型正文中提取泄漏的 DSML 工具调用(DeepSeek 系把调用写进 text 的兜底)。
// DSML 标签用全角竖线 U+FF5C 包裹,如:<｜DSML｜｜ invoke name="Insert">
// 修正则用 unicode 转义;行为:匹配到 Insert 调用 → 生成 ToolCall;text 清除裸标签。

namespace {

// DeepSeek 系模型偶尔把工具调用写进 text 而非 tool_calls:
// 提取逻辑在 LlmCodec::extractStrayToolCalls(实现与单测同处一文件)。

} // namespace

// 保留长历史的尾部,且从用户消息开始(API 不接受孤立的 tool 结果)
QVector<ChatMessage> trimmedHistory(const QVector<ChatMessage> &in, int cap)
{
    if (cap <= 0 || in.size() <= cap)
        return in;
    int start = in.size() - cap;
    while (start < in.size() && in.at(start).role != QLatin1String("user"))
        ++start;
    if (start >= in.size())
        start = in.size() - cap;
    return in.mid(start);
}

ToolSchema insertSchema()
{
    ToolSchema s;
    s.name = QStringLiteral("Insert");
    s.description = QStringLiteral(
        "Insert Markdown text at the user's caret in the current Mswrite "
        "document. This is the ONLY way to write document content. The caret "
        "advances after each insert, so consecutive calls write in order. "
        "Send Markdown source (not HTML) and do not wrap the payload in an "
        "outer code fence.");
    s.parameters = QJsonObject{
        { QStringLiteral("type"), QStringLiteral("object") },
        { QStringLiteral("properties"),
          QJsonObject{
              { QStringLiteral("text"),
                QJsonObject{ { QStringLiteral("type"), QStringLiteral("string") },
                             { QStringLiteral("description"),
                               QStringLiteral("Markdown source to insert") } } } } },
        { QStringLiteral("required"), QJsonArray{ QStringLiteral("text") } },
    };
    s.readOnly = false;
    return s;
}

ToolSchema readSchema()
{
    return {QStringLiteral("ReadDocument"),QStringLiteral(
        "Read the current document on demand. For PDF, page is 1-based and page_count is 1-4. "
        "Set include_images to inspect diagrams, layout or scans (at most two page images). "
        "For Markdown, start_line is 1-based and line_count is 1-2000. Request further ranges "
        "when the result says more content remains. Reading never changes the source file."),
        QJsonObject{{"type","object"},{"properties",QJsonObject{
            {"page",QJsonObject{{"type","integer"},{"minimum",1}}},
            {"page_count",QJsonObject{{"type","integer"},{"minimum",1},{"maximum",4}}},
            {"include_images",QJsonObject{{"type","boolean"}}},
            {"start_line",QJsonObject{{"type","integer"},{"minimum",1}}},
            {"line_count",QJsonObject{{"type","integer"},{"minimum",1},{"maximum",2000}}}}},
            {"required",QJsonArray{}}},true};
}

// 原始 HTTP/传输错误 → 可操作的中文提示(借鉴 MS-Agent)
QString translateApiError(const QString &raw, int httpStatus)
{
    if (httpStatus == 401 || httpStatus == 403)
        return QStringLiteral("API 密钥无效或无权限(HTTP %1)。请在 AI 设置里检查 Key 或地址。").arg(httpStatus);
    if (httpStatus == 429)
        return QStringLiteral("已被限流(HTTP 429)。稍等几秒再发送。");
    if (httpStatus == 402)
        return QStringLiteral("账户余额不足(HTTP 402)。请充值或换供应商。");
    if (httpStatus >= 500)
        return QStringLiteral("网关服务端错误(HTTP %1)。稍后再试。").arg(httpStatus);
    if (raw.contains(QLatin1String("timeout"), Qt::CaseInsensitive))
        return QStringLiteral("网络超时。检查网络连接或 AI 设置里的地址。");
    if (raw.contains(QLatin1String("Connection refused"), Qt::CaseInsensitive)
        || raw.contains(QLatin1String("Host not found"), Qt::CaseInsensitive))
        return QStringLiteral("无法连接到网关。检查网络与 AI 设置里的 API 地址。");
    return raw;
}

} // namespace

AiWorker::AiWorker(QObject *parent)
    : QObject(parent)
{
}

void AiWorker::applyConfig(const AiLlmConfig &cfg)
{
    cfg_ = cfg;
    llm_.setConfig(cfg_);
}

QString AiWorker::runInsert(const QString &text)
{
    if (!dock_ || stop_.load())
        return QStringLiteral("Error: no document window");
    // 跨线程回调 GUI 执行插入并回传结果:Queued + 信号量等待(与 readDocument
    // 同模式),带超时兜底。原先的 BlockingQueuedConnection 在 GUI 忙于模态
    // 对话框/退出时会让工作线程永久挂起,wait() 超时后销毁仍在运行的 QThread
    // 直接 qFatal(退出偶发闪退的根源)。
    struct InsertState { QSemaphore ready; QString result; };
    auto state = std::make_shared<InsertState>();
    QMetaObject::invokeMethod(dock_, [guard = QPointer<AiChatDock>(dock_), state, text] {
        if (!guard) {
            state->result = QStringLiteral("Error: document window closed");
            state->ready.release();
            return;
        }
        state->result = guard->insertAtCursor(text);
        state->ready.release();
    }, Qt::QueuedConnection);
    // 5 秒未完成视为 GUI 不可达:放弃插入,轮次以错误收尾而不是挂死
    if (!state->ready.tryAcquire(1, 5000))
        return QStringLiteral("Error: document window busy; insert skipped");
    return state->result;
}

AiDocumentResult AiWorker::readDocument(const QJsonObject &request)
{
    if(!dock_) return {QStringLiteral("Error: no document window"),{}};
    struct ReadState { QSemaphore ready; AiDocumentResult result; };
    auto state=std::make_shared<ReadState>();
    QMetaObject::invokeMethod(dock_,[guard=QPointer<AiChatDock>(dock_),request,state] {
        if(!guard) {state->result.text=QStringLiteral("Error: document window closed");state->ready.release();return;}
        guard->readDocument(request,[state](AiDocumentResult result) {
            state->result=std::move(result);state->ready.release();
        });
    },Qt::QueuedConnection);
    if(!state->ready.tryAcquire(1,2000))
        return {QStringLiteral("Error: document is not ready; retry reading. No cached text was substituted."),{}};
    return state->result;
}

void AiWorker::run(const QString &userText, const QString &docMarkdown,
                   int writeMode, const QString &thinkEffort, const QVector<AiAttach> &images)
{
    if (userText.trimmed().isEmpty() && images.isEmpty()) {
        emit turnFinished(QStringLiteral("请输入文字或添加图片"));
        emit busyChanged(false);
        return;
    }
    qWarning() << "Mswrite: AI 轮次开始(输入" << userText.size()
               << "字,文档" << docMarkdown.size()
               << "字,写入模式" << writeMode << ",思考" << thinkEffort
               << ",图片" << images.size() << "张)";
    // 防御:配置未生效(Key 为空)时不发 doomed 请求,给出明确提示
    if (cfg_.apiKey.isEmpty()) {
        emit busyChanged(true);
        emit turnFinished(QStringLiteral(
            "当前供应商的配置还没生效(API Key 为空)。打开「设置…」重新选一次当前供应商即可。"));
        emit busyChanged(false);
        return;
    }
    emit busyChanged(true);

    // 思考程度随轮次生效(模型不支持时 Llm 会剥参自动降级重试)
    cfg_.thinkEffort = thinkEffort;
    llm_.setConfig(cfg_);

    ChatMessage u;
    u.role = QStringLiteral("user");
    u.text = userText;
    u.images = images;
    history_.push_back(u);

    static const ToolSchema schema = insertSchema();
    // 写入模式 0 = 不提供 Insert 工具(模型只能在面板回答)
    QVector<ToolSchema> tools{readSchema()};
    if(writeMode!=0) tools.append(schema);
    const QJsonObject context=QJsonDocument::fromJson(docMarkdown.toUtf8()).object();
    const QString skills=MswriteSkill::customInstructions(MswriteSkill::directory());

    QString raw;     // 本轮流式正文(面板先按原文显示,结束再重渲染)
    QString error;

    // 轮内计时:思考时长(首个思考增量→首个正文增量)+ 生成速度
    qint64 thinkMs = 0;
    qint64 textChars = 0;
    qint64 outTokens = 0, inTokens = 0;
    QElapsedTimer thinkClock, textClock;
    bool thinkRunning = false, textRunning = false;
    bool retried = false; // 网络/服务端错误自动重试(仅一次)

    for (int turn = 1; turn <= kMaxTurns; ++turn) {
        // GUI 已请求终止(关窗/退出):不再发起请求与 GUI 回调,立即收尾
        if (stop_.load()) {
            error = QStringLiteral("已中断");
            break;
        }
        // 文档内容随轮次重建:AI 自己插入的内容也在其历史(tool 结果)里
        const QString system = MswriteSkill::withContext(docMarkdown, writeMode,skills);

        StreamSink sink;
        QString streamedTurnText;
        // 文字经信号直发面板;raw 同步累积,轮末整体重渲染。
        // 思维链也实时上报:glm 先思考十几秒,不显示会被当成"没连上"
        sink.onThinking = [&](const QString &d) {
            if (!thinkRunning) {
                thinkClock.start();
                thinkRunning = true;
            }
            emit thinkingDelta(d);
        };
        sink.onText = [&](const QString &d) {
            streamedTurnText+=d;
            if (thinkRunning) {
                thinkMs += thinkClock.elapsed();
                thinkRunning = false;
            }
            if (!textRunning) {
                textClock.start();
                textRunning = true;
            }
            textChars += d.size();
            raw += d;
            emit textDelta(d);
        };

        ChatResponse resp = llm_.complete(system, trimmedHistory(history_, kHistoryCap),
                                          tools, sink);
        inTokens = resp.inputTokens; // 最后一次的输入 ≈ 上下文占用
        outTokens += resp.outputTokens;
        if (resp.error.contains(QStringLiteral("已中断"))) {
            error = QStringLiteral("已中断");
            break;
        }
        if (!resp.error.isEmpty()) {
            // 网络/服务端类错误自动重试一次(4xx 客户端错误不重试)
            const bool retryable = resp.httpStatus == 0   // 超时/断连
                                 || resp.httpStatus >= 500; // 网关故障
            if (retryable && turn == 1 && !retried) {
                retried = true;
                emit thinkingDelta(QStringLiteral("网络波动,自动重试…"));
                qWarning() << "Mswrite: AI 请求失败(可重试) http=" << resp.httpStatus
                           << "重试中";
                --turn; // 本轮重跑
                continue;
            }
            error = translateApiError(resp.error, resp.httpStatus);
            // 原始错误落 mswrite.log:面板只显示翻译,排障要原文
            qWarning() << "Mswrite: AI 请求失败 http=" << resp.httpStatus
                       << "sse=" << resp.sseEvents << "原始错误:" << resp.error;
            break;
        }

        // DeepSeek 系模型偶尔把工具调用写进 text 而非 tool_calls:
        // 从正文提取成正式 ToolCall,清除裸 DSML 标签
        LlmCodec::extractStrayToolCalls(resp);
        // Some gateways return a complete response without streaming deltas.
        // The final response must still reach the conversation model.
        if(streamedTurnText.isEmpty() && !resp.text.isEmpty()) sink.onText(resp.text);

        history_.push_back([&resp] {
            ChatMessage m;
            m.role = QStringLiteral("assistant");
            m.text = resp.text;
            m.toolCalls = resp.toolCalls;
            return m;
        }());

        if (resp.toolCalls.isEmpty())
            break;

        QVector<AiAttach> pageImages;
        for (const ToolCall &call : resp.toolCalls) {
            QString result;
            if(call.name==QLatin1String("ReadDocument")) {
                QJsonObject request=call.input;
                request.insert(QStringLiteral("document_id"),context.value(QStringLiteral("id")));
                const auto document=readDocument(request);
                result=document.text;
                pageImages+=document.images;
            } else if (call.name == QLatin1String("Insert") && writeMode!=0) {
                const QString text = call.input.value(QStringLiteral("text")).toString();
                if (text.isEmpty()) {
                    result = QStringLiteral("Error: text is empty");
                } else {
                    result = runInsert(text);
                }
            } else {
                result = QStringLiteral("Error: unknown tool %1").arg(call.name);
            }

            ChatMessage m;
            m.role = QStringLiteral("tool");
            m.toolCallId = call.id;
            m.toolName = call.name;
            m.text = result;
            history_.push_back(m);
        }
        if(!pageImages.isEmpty()) {
            ChatMessage pages;
            pages.role=QStringLiteral("user");
            QStringList names; for(const auto &image:pageImages) names.append(image.name);
            pages.text=QStringLiteral("Requested ReadDocument page images (reference material): %1").arg(names.join(", "));
            pages.images=pageImages;
            history_.push_back(pages);
        }
    }

    // 统计:思考时长 + 生成速度(~3.5 字符/token,与 MS-Agent 同估算)
    double tps = 0;
    if (textRunning) {
        const double sec = textClock.elapsed() / 1000.0;
        if (sec > 0.2)
            tps = (textChars / 3.5) / sec;
    }
    qWarning() << "Mswrite: AI 轮次结束 error=" << error << "文字" << raw.size()
               << "字 think=" << thinkMs << "ms";
    emit turnStats(thinkMs, tps, outTokens, inTokens);
    emit turnRendered(raw.trimmed());
    emit turnFinished(error);
    emit busyChanged(false);
}
