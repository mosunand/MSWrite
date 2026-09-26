#pragma once
// ai/Http.h — AI 工作线程用的阻塞式 HTTP(嵌套事件循环,与 MS-Agent 同款)。
// 停止按钮经 HttpAbort 置位,由轮询器消费。

#include <QByteArray>
#include <QList>
#include <QPair>
#include <QString>
#include <QUrl>
#include <functional>

struct HttpResult {
    int status = 0;
    QByteArray body;
    QString error;
    QString contentType;
    QUrl finalUrl;
    QUrl redirectUrl;
};

namespace HttpAbort {
void request();   // UI:停止按钮
bool consume();   // 轮询:置位后恰好返回一次 true
}

namespace Http {

HttpResult get(const QUrl &url,
               const QList<QPair<QByteArray, QByteArray>> &headers,
               int timeoutMs,
               int maxBytes);

HttpResult postJson(const QUrl &url,
                    const QByteArray &body,
                    const QList<QPair<QByteArray, QByteArray>> &headers,
                    int timeoutMs);

HttpResult postSse(const QUrl &url,
                   const QByteArray &body,
                   const QList<QPair<QByteArray, QByteArray>> &headers,
                   int timeoutMs,
                   const std::function<void(const QByteArray &data)> &onData);

} // namespace Http
