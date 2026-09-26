#pragma once
// ai/AiDoctor.h — AI 连接自检:TLS 运行库 + 网关连通性(借鉴 MS-Agent /doctor)。

#include <QString>

class AiProvider;

namespace AiDoctor {

struct Result {
    // 本机 TLS
    bool sslSupported = false;
    QString sslRuntimeVersion;
    QString sslBuildVersion;
    // 网关连通(GET {base}/models,短超时)
    QString url;
    int status = 0;
    qint64 latencyMs = 0;
    QString error;
    QString bodyHead;
    bool ok() const { return status >= 200 && status < 300; }
};

Result run(const AiProvider &p);
QString format(const Result &r); // 人类可读的诊断文本

} // namespace AiDoctor
