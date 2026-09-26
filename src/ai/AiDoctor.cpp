// ai/AiDoctor.cpp — see ai/AiDoctor.h.

#include "ai/AiDoctor.h"

#include "ai/AiProviders.h"
#include "ai/Http.h"
#include "ai/LlmCodec.h"

#include <QSslSocket>
#include <QDateTime>
#include <QUrl>

namespace AiDoctor {

Result run(const AiProvider &p)
{
    Result r;
    r.sslSupported = QSslSocket::supportsSsl();
    r.sslRuntimeVersion = QSslSocket::sslLibraryVersionString();
    r.sslBuildVersion = QSslSocket::sslLibraryBuildVersionString();

    // GET {endpoint 的根}/models:打真实网关但不消耗 token
    r.url = LlmCodec::endpoint(p.protocol, p.baseUrl).toString();
    if (r.url.endsWith(QStringLiteral("/messages")))
        r.url.chop(9);
    else if (r.url.endsWith(QStringLiteral("/chat/completions")))
        r.url.chop(17);
    r.url += QStringLiteral("/models");

    const qint64 t0 = QDateTime::currentMSecsSinceEpoch();
    const HttpResult hr = Http::get(QUrl(r.url),
                                     LlmCodec::headers(p.protocol, p.apiKey),
                                     6000, 4096);
    r.latencyMs = QDateTime::currentMSecsSinceEpoch() - t0;
    r.status = hr.status;
    r.error = hr.error;
    r.bodyHead = QString::fromUtf8(hr.body.left(400));
    return r;
}

QString format(const Result &r)
{
    QString s;
    s += QStringLiteral("TLS 运行库: %1(需要 OpenSSL,缺失则所有 https 都会失败)\n")
             .arg(r.sslSupported
                      ? QStringLiteral("可用,%1").arg(r.sslRuntimeVersion)
                      : QStringLiteral("不可用!运行目录缺少 OpenSSL DLL"));
    s += QStringLiteral("连接测试: GET %1\n").arg(r.url);
    if (r.ok()) {
        s += QStringLiteral("结果: HTTP %1 · %2 ms · 连接正常\n").arg(r.status).arg(r.latencyMs);
    } else if (r.status > 0) {
        s += QStringLiteral("结果: HTTP %1 · %2 ms(网关可达;若 401/403 是 Key 问题)\n%3\n")
                 .arg(r.status)
                 .arg(r.latencyMs)
                 .arg(r.bodyHead);
    } else {
        s += QStringLiteral("结果: 失败(%1)\n").arg(r.error.isEmpty() ? QStringLiteral("无响应") : r.error);
        if (r.error.contains(QLatin1String("TLS"), Qt::CaseInsensitive))
            s += QStringLiteral("→ 这是 TLS 问题:Mswrite.exe 旁边缺少 OpenSSL DLL"
                                "(libssl/libcrypto),从 Qt 的 bin 目录复制即可。\n");
        else if (r.error.contains(QLatin1String("Host not found"), Qt::CaseInsensitive)
                 || r.error.contains(QLatin1String("Connection refused"), Qt::CaseInsensitive))
            s += QStringLiteral("→ 无法连到网关:检查网络 / API 地址是否填对。\n");
    }
    return s;
}

} // namespace AiDoctor
