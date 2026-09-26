// ai/Http.cpp — see ai/Http.h.

#include "ai/Http.h"

#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>

#include <atomic>

namespace {

std::atomic<bool> g_abort{false};

} // namespace

namespace HttpAbort {

void request()
{
    g_abort.store(true);
}

bool consume()
{
    return g_abort.exchange(false);
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

} // namespace

namespace Http {

HttpResult get(const QUrl &url,
               const QList<QPair<QByteArray, QByteArray>> &headers,
               int timeoutMs,
               int maxBytes)
{
    QNetworkAccessManager nam;
    QNetworkReply *reply = nam.get(makeRequest(url, headers));

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
    if (maxBytes > 0) {
        QObject::connect(reply, &QNetworkReply::readyRead, reply, [&]() {
            if (truncated)
                return;
            buf += reply->readAll();
            if (buf.size() > maxBytes) {
                buf.truncate(maxBytes);
                truncated = true;
                reply->abort();
            }
        });
    }

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
    if (maxBytes > 0)
        r.body = buf;
    else
        r.body = reply->readAll();
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
    QNetworkAccessManager nam;
    auto hdrs = headers;
    bool hasCt = false;
    for (const auto &h : hdrs) {
        if (h.first.toLower() == "content-type")
            hasCt = true;
    }
    if (!hasCt)
        hdrs.append(qMakePair(QByteArray("Content-Type"), QByteArray("application/json")));

    QNetworkReply *reply = nam.post(makeRequest(url, hdrs), body);

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
    r.body = reply->readAll();
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
    QNetworkAccessManager nam;
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

    QNetworkReply *reply = nam.post(makeRequest(url, hdrs), body);

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
    auto consumeChunk = [&](const QByteArray &chunk) {
        pending += chunk;
        if (all.size() < kRawCap)
            all += chunk;
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
                if (onData)
                    onData(data);
            }
        }
    };

    QObject::connect(reply, &QNetworkReply::readyRead, reply, [&]() {
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
