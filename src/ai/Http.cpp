// ai/Http.cpp — see ai/Http.h.

#include "ai/Http.h"

#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QThreadStorage>
#include <QTimer>

#include <atomic>

namespace {

thread_local HttpAbort::Token threadAbort;

// 每线程一个 QNetworkAccessManager:同一网关的连续请求(流式/重试/回退)
// 复用连接与 TLS 会话,而不是每个请求都重新握手。QThreadStorage 在线程
// 结束时于本线程内销毁,满足 QObject 的线程亲和要求。
QNetworkAccessManager *sharedNam()
{
    static QThreadStorage<QNetworkAccessManager *> pool;
    if (!pool.hasLocalData())
        pool.setLocalData(new QNetworkAccessManager);
    return pool.localData();
}

} // namespace

namespace HttpAbort {

Scope::Scope(Token token) : previous_(std::move(threadAbort)) { threadAbort = std::move(token); }
Scope::~Scope() { threadAbort = std::move(previous_); }

bool consume()
{
    return threadAbort && threadAbort->load();
}

} // namespace HttpAbort

namespace {

QNetworkRequest makeRequest(const QUrl &url,
                             const QList<QPair<QByteArray, QByteArray>> &headers)
{
    QNetworkRequest req(url);
    req.setTransferTimeout(0);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::ManualRedirectPolicy);
    req.setMaximumRedirectsAllowed(0);
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  QStringLiteral("Mswrite-AI/0.1"));
    for (const auto &h : headers)
        req.setRawHeader(h.first, h.second);
    return req;
}

// SSE 原始体积封顶:只用于错误解析,超大响应没有留全量的意义
constexpr int kRawCap = 8 * 1024 * 1024;
constexpr int kJsonCap = 16 * 1024 * 1024;

} // namespace

namespace Http {

HttpResult get(const QUrl &url,
               const QList<QPair<QByteArray, QByteArray>> &headers,
               int timeoutMs,
               int maxBytes)
{
    if (HttpAbort::consume()) return {0, {}, QStringLiteral("已中断")};
    QNetworkAccessManager *nam = sharedNam();
    QNetworkReply *reply = nam->get(makeRequest(url, headers));
    reply->setReadBufferSize(64 * 1024);

    HttpResult r;
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);

    QTimer abortPoll;
    abortPoll.setInterval(100);
    bool aborted = false;
    QObject::connect(&abortPoll, &QTimer::timeout, [&]() {
        if (HttpAbort::consume()) {
            aborted = true;
            reply->abort();
        }
    });
    abortPoll.start();

    QByteArray buf;
    bool truncated = false;
    const qsizetype cap = maxBytes > 0 ? qMin(maxBytes, kJsonCap) : kJsonCap;
    const auto drain = [&]() {
        if (truncated) return;
        buf += reply->read(cap + 1 - buf.size());
        if (buf.size() > cap) { truncated = true; buf.clear(); reply->abort(); }
    };
    QObject::connect(reply, &QNetworkReply::readyRead, reply, drain);

    timer.start(timeoutMs);
    loop.exec();
    abortPoll.stop();

    if (aborted) {
        r.error = QStringLiteral("已中断");
        reply->deleteLater();
        return r;
    }
    if (!timer.isActive() && reply->isRunning()) {
        reply->abort();
        r.error = QStringLiteral("Error: HTTP timeout after %1 ms").arg(timeoutMs);
        reply->deleteLater();
        return r;
    }

    r.finalUrl = reply->url();
    r.contentType = QString::fromUtf8(
        reply->header(QNetworkRequest::ContentTypeHeader).toByteArray());
    r.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    drain();
    if (truncated) {
        r.error = QStringLiteral("Error: HTTP response exceeds %1 bytes").arg(cap);
        reply->deleteLater();
        return r;
    }
    r.body = buf;
    if (reply->error() != QNetworkReply::NoError
        && reply->error() != QNetworkReply::OperationCanceledError) {
        r.error = QStringLiteral("Error: HTTP %1").arg(reply->errorString());
    }
    reply->deleteLater();
    return r;
}

HttpResult postJson(const QUrl &url,
                    const QByteArray &body,
                    const QList<QPair<QByteArray, QByteArray>> &headers,
                    int timeoutMs)
{
    if (HttpAbort::consume()) return {0, {}, QStringLiteral("已中断")};
    auto hdrs = headers;
    bool hasCt = false;
    for (const auto &h : hdrs) {
        if (h.first.toLower() == "content-type")
            hasCt = true;
    }
    if (!hasCt)
        hdrs.append(qMakePair(QByteArray("Content-Type"), QByteArray("application/json")));

    QNetworkAccessManager *nam = sharedNam();
    QNetworkReply *reply = nam->post(makeRequest(url, hdrs), body);
    reply->setReadBufferSize(64 * 1024);

    HttpResult r;
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);

    QByteArray responseBody;
    bool tooLarge = false;
    const auto drain = [&]() {
        if (tooLarge) return;
        responseBody += reply->read(kJsonCap + 1 - responseBody.size());
        if (responseBody.size() > kJsonCap) {
            tooLarge = true;
            responseBody.clear();
            reply->abort();
        }
    };
    QObject::connect(reply, &QNetworkReply::readyRead, reply, drain);

    QTimer abortPoll;
    abortPoll.setInterval(100);
    bool aborted = false;
    QObject::connect(&abortPoll, &QTimer::timeout, [&]() {
        if (HttpAbort::consume()) {
            aborted = true;
            reply->abort();
        }
    });
    abortPoll.start();

    timer.start(timeoutMs);
    loop.exec();
    abortPoll.stop();

    if (aborted) {
        r.error = QStringLiteral("已中断");
        reply->deleteLater();
        return r;
    }
    if (!timer.isActive() && reply->isRunning()) {
        reply->abort();
        r.error = QStringLiteral("Error: HTTP timeout after %1 ms").arg(timeoutMs);
        reply->deleteLater();
        return r;
    }

    r.finalUrl = reply->url();
    r.contentType = QString::fromUtf8(
        reply->header(QNetworkRequest::ContentTypeHeader).toByteArray());
    r.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    drain();
    if (tooLarge) {
        r.error = QStringLiteral("Error: HTTP response exceeds 16 MB");
        reply->deleteLater();
        return r;
    }
    r.body = responseBody;
    if (reply->error() != QNetworkReply::NoError
        && reply->error() != QNetworkReply::OperationCanceledError) {
        r.error = QStringLiteral("Error: HTTP %1").arg(reply->errorString());
    }
    reply->deleteLater();
    return r;
}

HttpResult postSse(const QUrl &url,
                   const QByteArray &body,
                   const QList<QPair<QByteArray, QByteArray>> &headers,
                   int timeoutMs,
                   const std::function<void(const QByteArray &data)> &onData)
{
    if (HttpAbort::consume()) return {0, {}, QStringLiteral("已中断")};
    auto hdrs = headers;
    bool hasCt = false;
    bool hasAccept = false;
    for (const auto &h : hdrs) {
        const QByteArray k = h.first.toLower();
        if (k == "content-type")
            hasCt = true;
        if (k == "accept")
            hasAccept = true;
    }
    if (!hasCt)
        hdrs.append(qMakePair(QByteArray("Content-Type"), QByteArray("application/json")));
    if (!hasAccept)
        hdrs.append(qMakePair(QByteArray("Accept"), QByteArray("text/event-stream")));

    QNetworkAccessManager *nam = sharedNam();
    QNetworkReply *reply = nam->post(makeRequest(url, hdrs), body);
    reply->setReadBufferSize(64 * 1024);

    HttpResult r;
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);

    QTimer abortPoll;
    abortPoll.setInterval(100);
    bool aborted = false;
    QObject::connect(&abortPoll, &QTimer::timeout, [&]() {
        if (HttpAbort::consume()) {
            aborted = true;
            reply->abort();
        }
    });
    abortPoll.start();

    QByteArray pending;
    QByteArray all;
    QByteArray eventData;
    // 单段待切分字节 / 单个事件的累积上限:正常事件是几 KB 的 JSON。
    // 端点若从不发 '\n',或一条事件永不闭合,这两个缓冲没有上限就会无限吃内存
    constexpr int kPendingCap = 2 * 1024 * 1024;
    constexpr int kEventCap = 4 * 1024 * 1024;
    bool malformedStream = false;
    qsizetype totalBytes = 0;
    auto flushEvent = [&]() {
        if (eventData.isEmpty()) return;
        if (onData) onData(eventData);
        eventData.clear();
    };
    auto consumeChunk = [&](const QByteArray &chunk) {
        if (malformedStream) return;
        totalBytes += chunk.size();
        if (totalBytes > 64 * 1024 * 1024) {
            malformedStream = true; reply->abort(); return;
        }
        pending += chunk;
        if (pending.size() > kPendingCap) {
            // Stop at a malformed frame; accepting its tail as a fresh event
            // can corrupt generated text or tool arguments.
            pending.clear();
            eventData.clear(); malformedStream = true; reply->abort();
            return;
        }
        if (all.size() < kRawCap)
            all += chunk.left(kRawCap - all.size());
        int idx;
        while ((idx = pending.indexOf('\n')) >= 0) {
            QByteArray line = pending.left(idx);
            pending.remove(0, idx + 1);
            if (line.endsWith('\r'))
                line.chop(1);
            if (line.startsWith("data:")) {
                QByteArray data = line.mid(5);
                if (data.startsWith(' '))
                    data = data.mid(1);
                // SSE 规范允许多行 data:,但本应用的 data 一律是单行 JSON。
                // 网关按固定宽度拆行时,规范拼接(加 '\n')会把换行塞进 JSON
                // 字符串内部 —— 解析"成功"但内容被污染(Qt 容忍串内原始换行),
                // 整条增量被静默吞掉,工具调用参数残缺报错。这里按字节直连,
                // 交给 LlmCodec 侧的容错兜底
                eventData += data;
                if (eventData.size() > kEventCap) {
                    // Refuse the entire event; never interpret a truncated tail.
                    eventData.clear();
                    pending.clear();
                    malformedStream = true; reply->abort();
                    return;
                }
            } else if (line.isEmpty()) flushEvent();
        }
    };

    // 超时语义 = 空闲超时:每收到字节就重置计时。SSE 长回复(含长思考)不再
    // 受"总时长"封顶,只有连接在 timeoutMs 内一个字节都不来才掐断;
    // 网关的心跳/注释行同样会触发 readyRead,能防正常空闲误断
    QObject::connect(reply, &QNetworkReply::readyRead, reply, [&]() {
        timer.start();   // 重置空闲计时(保持原 interval)
        consumeChunk(reply->readAll());
    });

    timer.start(timeoutMs);
    loop.exec();
    abortPoll.stop();

    if (aborted) {
        r.error = QStringLiteral("已中断");
        reply->deleteLater();
        return r;
    }
    if (!timer.isActive() && reply->isRunning()) {
        reply->abort();
        r.error = QStringLiteral("Error: HTTP timeout after %1 ms").arg(timeoutMs);
        reply->deleteLater();
        return r;
    }

    consumeChunk(reply->readAll());
    if (malformedStream) {
        r.error = QStringLiteral("Error: oversized or malformed SSE frame");
        reply->deleteLater();
        return r;
    }
    if (pending.startsWith("data:")) {
        QByteArray tail = pending.mid(5);
        if (tail.startsWith(' ')) tail.remove(0, 1);
        eventData += tail;
        if (eventData.size() > kEventCap) {
            r.error = QStringLiteral("Error: oversized or malformed SSE frame");
            reply->deleteLater(); return r;
        }
    }
    flushEvent();
    r.finalUrl = reply->url();
    r.contentType = QString::fromUtf8(
        reply->header(QNetworkRequest::ContentTypeHeader).toByteArray());
    r.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    r.body = all;
    if (reply->error() != QNetworkReply::NoError
        && reply->error() != QNetworkReply::OperationCanceledError) {
        r.error = QStringLiteral("Error: HTTP %1").arg(reply->errorString());
    }
    reply->deleteLater();
    return r;
}

} // namespace Http
