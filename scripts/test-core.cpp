#include "fileservice.h"
#include "securitypolicy.h"
#include "ai/Http.h"
#include "ai/LlmCodec.h"
#include "ai/Llm.h"
#include "ai/AiMath.h"
#include "deferredrequest.h"
#include <QJsonDocument>
#include <QJsonArray>
#include <QEventLoop>
#include <thread>
#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <iostream>
#include <stdexcept>

namespace {
int checks = 0;
void check(bool success, const char *name) {
    if (!success) throw std::runtime_error(name);
    ++checks; std::cout << "PASS " << name << '\n';
}
void rawFile(const QString &path, const QByteArray &data) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size())
        throw std::runtime_error("fixture write failed");
}
void policyChecks() {
    const QUrl page("https://app.local/editor.html?v=168");
    check(SecurityPolicy::trustedPage(QUrl("https://app.local/editor.html?v=other#anchor"), page), "own page accepted");
    const QUrl blank("about:blank"), exportPage("https://export.local/export-a.html");
    check(SecurityPolicy::trustedNavigation(blank, blank), "PDF blank bootstrap allowed");
    check(!SecurityPolicy::trustedPage(blank, blank), "blank page has no native message privileges");
    check(!SecurityPolicy::trustedNavigation(blank, page), "editor cannot navigate to a blank page");
    check(SecurityPolicy::trustedNavigation(exportPage, exportPage), "native export page navigation allowed");
    check(!SecurityPolicy::trustedNavigation(QUrl("https://export.local/other.html"), exportPage), "unrequested export navigation blocked");
    for (const auto *bad : {"http://app.local/editor.html", "https://app.local.evil/editor.html",
        "https://user@app.local/editor.html", "https://app.local:444/editor.html",
        "https://doc1.local/editor.html", "https://app.local/chat.html", "file:///editor.html",
        "https://app.local/%2feditor.html", "https://app.local/editor.html/extra"})
        check(!SecurityPolicy::trustedPage(QUrl(QString::fromLatin1(bad)), page), bad);
    check(SecurityPolicy::externalUrl(QUrl("https://example.com/")), "HTTPS links accepted");
    check(SecurityPolicy::externalUrl(QUrl("mailto:test@example.com")), "mail links accepted");
    for (const auto *bad : {"file:///C:/Windows/System32/cmd.exe", "javascript:alert(1)",
        "data:text/html,hello", "ms-settings:privacy", "https://user:secret@example.com", "relative.md"})
        check(!SecurityPolicy::externalUrl(QUrl(QString::fromLatin1(bad))), bad);
    QJsonObject reply{{"md", "text"}, {"rev", 2}, {"request", 7}};
    check(SecurityPolicy::contentReply(reply, 7), "valid save response accepted");
    check(!SecurityPolicy::contentReply(reply, 8), "stale request rejected");
    reply.remove("md"); check(!SecurityPolicy::contentReply(reply, 7), "missing body rejected");
    reply.insert("md", 0); check(!SecurityPolicy::contentReply(reply, 7), "numeric body rejected");
    reply.insert("md", ""); check(SecurityPolicy::contentReply(reply, 7), "intentional empty body accepted");
    reply.insert("rev", -1); check(!SecurityPolicy::contentReply(reply, 7), "negative revision rejected");
    reply.insert("rev", 1.5); check(!SecurityPolicy::contentReply(reply, 7), "fractional revision rejected");
    reply.insert("rev", 1e30); check(!SecurityPolicy::contentReply(reply, 7), "overflow revision rejected");
    check(SecurityPolicy::oneBasedIndex(QJsonValue(QJsonValue::Undefined), 5, 3) == 2, "missing page uses current-page fallback");
    check(SecurityPolicy::oneBasedIndex(1, 5) == 0, "first line index accepted");
    for (const QJsonValue value : {QJsonValue(-2147483648.0), QJsonValue(2147483647.0), QJsonValue(1.5), QJsonValue("1"), QJsonValue(0)})
        check(SecurityPolicy::oneBasedIndex(value, 5) == -1, "invalid page/line rejected before subtraction");
}
void fileChecks() {
    QTemporaryDir directory;
    if (!directory.isValid()) throw std::runtime_error("temporary directory failed");
    const QString file = directory.filePath("document.md");
    const QString text = QString::fromUtf8("中文\r\nemoji 😀\r\n正文");
    for (auto encoding : {FileService::Encoding::Utf8, FileService::Encoding::Utf8Bom,
        FileService::Encoding::Utf16LE, FileService::Encoding::Utf16BE, FileService::Encoding::Gbk}) {
        check(FileService::writeFile(file, text, encoding), "encoding save succeeds");
        bool ok = false, crlf = false;
        auto detected = FileService::Encoding::Utf8;
        const QString read = FileService::readFile(file, &ok, &detected, &crlf);
        check(ok && read == text && crlf, "encoding round trip preserves Unicode and CRLF");
        check(detected == encoding, "encoding preserved");
    }
    for (const auto &bad : {QByteArray::fromHex("efbbbfe4b8"), QByteArray::fromHex("fffe6100ff"),
        QByteArray::fromHex("feff0061d800"), QByteArray::fromHex("ff")}) {
        rawFile(file, bad); bool ok = true;
        check(FileService::readFile(file, &ok).isEmpty() && !ok, "damaged input rejected");
        QFile original(file); original.open(QIODevice::ReadOnly);
        check(original.readAll() == bad, "rejected file remains untouched");
    }
    rawFile(file, "original");
    check(!FileService::writeFile(file, "replacement", static_cast<FileService::Encoding>(999)), "invalid encoding cannot erase original");
    QByteArray bounded;
    check(FileService::readBytes(file, 8, &bounded) && bounded == "original", "bounded file read preserves content");
    check(!FileService::readBytes(file, 7, &bounded) && bounded.isEmpty(), "oversized binary read returns no partial data");
    check(!FileService::readBytes(directory.path(), 8, &bounded), "directory cannot be read as an attachment");
    check(!FileService::writeFile(file, QString(QChar(0xd800))), "invalid UTF16 refused");
    QFile original(file); original.open(QIODevice::ReadOnly);
    check(original.readAll() == "original", "failed save preserves original bytes"); original.close();
    QFile large(file); large.open(QIODevice::WriteOnly); large.resize(64 * 1024 * 1024 + 1); large.close();
    bool ok = true; FileService::readFile(file, &ok); check(!ok, "oversized document refused");
    rawFile(file, ""); ok = false;
    check(FileService::readFile(file, &ok).isEmpty() && ok, "valid empty document opens");
}
HttpResult response(const QByteArray &body, bool sse = false, int timeout = 2000, bool stall = false) {
    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost)) throw std::runtime_error("loopback server failed");
    QObject::connect(&server, &QTcpServer::newConnection, &server, [&] {
        auto *socket = server.nextPendingConnection();
        QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket, body, sse, stall] {
            socket->readAll();
            if (socket->property("replied").toBool() || stall) return;
            socket->setProperty("replied", true);
            socket->write("HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: "
                + QByteArray(sse ? "text/event-stream" : "application/json")
                + "\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body);
            socket->disconnectFromHost();
        });
    });
    const QUrl url(QStringLiteral("http://127.0.0.1:%1/").arg(server.serverPort()));
    QList<QByteArray> events;
    const HttpResult result = sse ? Http::postSse(url, "{}", {}, timeout, [&](const QByteArray &event) { events.append(event); })
                                 : Http::postJson(url, "{}", {}, timeout);
    if (sse && body == "data: {\"a\":1}\n\n") check(events == QList<QByteArray>{"{\"a\":1}"}, "SSE event delivered once");
    return result;
}
void asynchronousChecks() {
    const auto first = std::make_shared<std::atomic_bool>(false);
    const auto second = std::make_shared<std::atomic_bool>(false);
    first->store(true);
    bool firstStopped = false, secondStopped = true;
    std::thread a([&] { HttpAbort::Scope scope(first); firstStopped = HttpAbort::consume() && HttpAbort::consume(); });
    std::thread b([&] { HttpAbort::Scope scope(second); secondStopped = HttpAbort::consume(); });
    a.join(); b.join();
    check(firstStopped && !secondStopped, "cancellation isolated between worker threads and remains sticky");
    {
        HttpAbort::Scope scope(first);
        check(response("{}").error == QStringLiteral("已中断"), "cancelled request does not start network work");
        { HttpAbort::Scope nested(second); check(!HttpAbort::consume(), "nested cancellation scope isolated"); }
        check(HttpAbort::consume(), "previous cancellation scope restored");
    }
    check(!HttpAbort::consume(), "completed scope does not poison future requests");
    auto delayed = std::make_shared<DeferredRequest>(1);
    bool inserted = false;
    QEventLoop loop;
    QTimer::singleShot(15, &loop, [&] { if (delayed->active()) inserted = true; loop.quit(); });
    loop.exec();
    check(!inserted, "expired queued GUI write skipped");
    DeferredRequest cancelled(1000); cancelled.cancel();
    check(!cancelled.active(), "explicitly cancelled GUI write skipped");
}
QByteArray eventJson(const QJsonObject &object) { return QJsonDocument(object).toJson(QJsonDocument::Compact); }
void streamChecks() {
    const auto openAiArgs = [](const QString &arguments, int index = 0) {
        const QJsonObject tool{{"index", index}, {"id", "call-1"},
            {"function", QJsonObject{{"name", "Insert"}, {"arguments", arguments}}}};
        const QJsonObject delta{{"tool_calls", QJsonArray{tool}}};
        return eventJson({{"choices", QJsonArray{QJsonObject{{"delta", delta}}}}});
    };
    LlmCodec::StreamAssembler normal(Protocol::OpenAi);
    normal.feed(openAiArgs("{\"text\":\"正文\"}"));
    const auto valid = normal.finish(200, {}, {});
    check(valid.error.isEmpty() && valid.toolCalls.size() == 1 && valid.toolCalls[0].input.value("text") == QStringLiteral("正文"), "valid streamed tool remains usable");
    LlmCodec::StreamAssembler oversized(Protocol::OpenAi);
    for (int i = 0; i < 9; ++i) oversized.feed(openAiArgs(QString(1024 * 1024, QLatin1Char('x'))));
    const auto refused = oversized.finish(200, {}, {});
    check(refused.error.contains("exceed 8 MB") && refused.toolCalls.isEmpty(), "oversized OpenAI arguments refused without truncation");
    LlmCodec::StreamAssembler invalid(Protocol::OpenAiResponses);
    invalid.feed(eventJson({{"type", "response.function_call_arguments.delta"}, {"output_index", 999999}, {"delta", "{}"}}));
    check(invalid.finish(200, {}, {}).error.contains("Invalid streamed tool index"), "Responses tool index bounded");
    LlmCodec::StreamAssembler text(Protocol::OpenAi);
    const QByteArray delta = eventJson({{"choices", QJsonArray{QJsonObject{{"delta", QJsonObject{{"content", QString(1024 * 1024, QLatin1Char('a'))}}}}}}});
    for (int i = 0; i < 34; ++i) text.feed(delta);
    check(text.finish(200, {}, {}).error.contains("assembly limit"), "total streamed text bounded");
    LlmCodec::StreamAssembler damaged(Protocol::OpenAi);
    damaged.feed(openAiArgs("{}")); damaged.feed("broken event");
    damaged.feed(eventJson({{"choices", QJsonArray{}}}));
    check(!damaged.finish(200, {}, {}).error.isEmpty(), "damaged stream cannot approve a partial document tool call");
    const auto plainTool = [](const QJsonValue &arguments, const QString &reason = QString()) {
        return eventJson({{"choices", QJsonArray{QJsonObject{{"finish_reason", reason},
            {"message", QJsonObject{{"tool_calls", QJsonArray{QJsonObject{{"id", "call-1"},
                {"function", QJsonObject{{"name", "ReadDocument"}, {"arguments", arguments}}}}}}}}}}}});
    };
    for (const QJsonValue args : {QJsonValue("broken"), QJsonValue("[]"), QJsonValue("null"), QJsonValue(4), QJsonValue(QJsonObject{})}) {
        const auto parsed = LlmCodec::parse(Protocol::OpenAi, 200, plainTool(args));
        check(!parsed.error.isEmpty() && parsed.toolCalls.isEmpty(), "malformed plain arguments cannot become an empty read request");
    }
    check(LlmCodec::parse(Protocol::OpenAi, 200, plainTool("{}")).toolCalls.size() == 1, "valid empty argument object remains usable");
    const auto truncated = LlmCodec::parse(Protocol::OpenAi, 200, plainTool("{}", "length"));
    check(!truncated.error.isEmpty() && truncated.toolCalls.isEmpty(), "token-truncated response cannot authorize tool calls");
    LlmCodec::StreamAssembler streamedTruncated(Protocol::OpenAi);
    streamedTruncated.feed(openAiArgs("{}"));
    streamedTruncated.feed(eventJson({{"choices", QJsonArray{QJsonObject{{"finish_reason", "length"}}}}}));
    check(streamedTruncated.finish(200, {}, {}).toolCalls.isEmpty(), "streamed token-truncated tools refused");
    QJsonArray many;
    for (int i = 0; i < 257; ++i) many.append(QJsonObject{{"type", "function_call"}, {"name", "ReadDocument"}, {"arguments", "{}"}});
    LlmCodec::StreamAssembler completed(Protocol::OpenAiResponses);
    completed.feed(eventJson({{"type", "response.completed"}, {"response", QJsonObject{{"status", "completed"}, {"output", many}}}}));
    const auto tooMany = completed.finish(200, {}, {});
    check(!tooMany.error.isEmpty() && tooMany.toolCalls.isEmpty(), "Responses completed fallback cannot bypass tool count limit");
    LlmCodec::StreamAssembler anthropic(Protocol::Anthropic);
    anthropic.feed(eventJson({{"type", "content_block_start"}, {"index", 0}, {"content_block", QJsonObject{{"type", "tool_use"}, {"name", "ReadDocument"}, {"input", QJsonObject{}}}}}));
    check(anthropic.finish(200, {}, {}).toolCalls.size() == 1, "Anthropic empty-input tool remains usable");
    for (const auto protocol : {Protocol::Anthropic, Protocol::Gemini, Protocol::OpenAiResponses}) {
        QByteArray body;
        if (protocol == Protocol::Anthropic) body = eventJson({{"content", QJsonArray{QJsonObject{{"type", "tool_use"}, {"name", "ReadDocument"}, {"input", QJsonArray{}}}}}});
        if (protocol == Protocol::Gemini) body = eventJson({{"candidates", QJsonArray{QJsonObject{{"content", QJsonObject{{"parts", QJsonArray{QJsonObject{{"functionCall", QJsonObject{{"name", "ReadDocument"}, {"args", QJsonArray{}}}}}}}}}}}}});
        if (protocol == Protocol::OpenAiResponses) body = eventJson({{"status", "completed"}, {"output", QJsonArray{QJsonObject{{"type", "function_call"}, {"name", "ReadDocument"}, {"arguments", "broken"}}}}});
        const auto parsed = LlmCodec::parse(protocol, 200, body);
        check(!parsed.error.isEmpty() && parsed.toolCalls.isEmpty(), "all protocols refuse malformed tool arguments");
    }
    Llm llm;
    AiLlmConfig config; config.protocol = Protocol::OpenAi; llm.setConfig(config);
    llm.setTransport([&](Protocol, const QUrl &, const QByteArray &, const QList<QPair<QByteArray, QByteArray>> &, int) {
        ChatResponse raw; raw.httpStatus = 200; raw.raw = plainTool("{}"); raw.error = "test transport failure"; return raw;
    });
    const auto failed = llm.complete({}, {}, {});
    check(failed.error == "test transport failure" && failed.toolCalls.isEmpty(), "transport error cannot approve a seemingly valid partial response");
}
void networkChecks() {
    const auto normal = response("{\"ok\":true}");
    check(normal.error.isEmpty() && normal.status == 200 && normal.body == "{\"ok\":true}", "normal JSON response intact");
    const auto oversized = response(QByteArray(16 * 1024 * 1024 + 1, 'x'));
    check(oversized.error.contains("exceeds 16 MB") && oversized.body.isEmpty(), "oversized JSON explicitly fails");
    check(response("data: {\"a\":1}\n\n", true).error.isEmpty(), "normal SSE succeeds");
    check(response(QByteArray(2 * 1024 * 1024 + 1, 'x'), true).error.contains("malformed SSE"), "oversized SSE frame explicitly fails");
    check(response({}, false, 30, true).error.contains("timeout"), "HTTP idle timeout remains effective");
}
void mathChecks() {
    check(!MathRender::render(QStringLiteral("\\frac{1}{2}+x^2"), false).isEmpty(), "normal terminal formula still renders");
    const QString oversized(64 * 1024 + 1, QLatin1Char('x'));
    check(MathRender::render(oversized, true) == oversized, "oversized formula retains source without expensive layout");
    QString repeated;
    for (int i = 0; i < 500; ++i) repeated += QStringLiteral("\\begin{matrix}x\\end{matrix}");
    const QString rendered = MathRender::render(repeated, true);
    check(!rendered.isEmpty() && rendered.contains(QStringLiteral("\\begin{matrix}")), "repeated matrix recursion bounded without dropping remaining source");
    const QString body = QStringLiteral("x\\\\").repeated(300);
    check(MathRender::render(QStringLiteral("\\begin{matrix}") + body + QStringLiteral("\\end{matrix}"), true) == body, "matrix row expansion bounded");
    check(!MathRender::render(QString(300, '{') + "x" + QString(300, '}'), true).isEmpty(), "deep braces remain bounded");
}
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try { policyChecks(); fileChecks(); networkChecks(); asynchronousChecks(); streamChecks(); mathChecks(); std::cout << checks << " native checks passed.\n"; }
    catch (const std::exception &error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
    return 0;
}
