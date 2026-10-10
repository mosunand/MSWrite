#pragma once
// ai/Http.h — AI 工作线程用的阻塞式 HTTP(嵌套事件循环,与 MS-Agent 同款)。
// 停止按钮设置当前工作线程的取消标志,轮询器读取且保留该状态。

#include <QByteArray>
#include <QList>
#include <QPair>
#include <QString>
#include <QUrl>
#include <functional>
#include <atomic>
#include <memory>

struct HttpResult {
    int status = 0;
    QByteArray body;
    QString error;
    QString contentType;
    QUrl finalUrl;
    QUrl redirectUrl;
};

namespace HttpAbort {
using Token = std::shared_ptr<std::atomic_bool>;
// Each AI turn binds its own cancellation flag to its worker thread. Keeping
// it set also prevents retries and subsequent tool calls after Stop.
class Scope {
public:
    explicit Scope(Token token);
    ~Scope();
    Scope(const Scope &) = delete;
    Scope &operator=(const Scope &) = delete;
private:
    Token previous_;
};
bool consume();
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
