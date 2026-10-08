#pragma once
// ai/AiCcSwitch.h — 只读读取本机 cc-switch 的供应商(SQLite)。
// 绝不写库;Key 只进内存/自有配置,不落日志。移植自 MS-Agent CcSwitch。

#include "ai/Types.h"

#include <QString>
#include <QVector>

struct AiCcProvider {
    bool ok = false;
    bool isCurrent = false;
    QString error;
    QString id;
    QString name;
    QString appType; // claude | codex | gemini | …
    Protocol protocol = Protocol::Anthropic;
    QString apiKey;
    QString baseUrl;
    QString model;   // 原样保留，包括 [1M] 等网关后缀
    QJsonObject sourceConfig;
};

class AiCcSwitch {
public:
    static QString dbPath(); // CC_SWITCH_HOME 或 ~/.cc-switch/cc-switch.db
    static QVector<AiCcProvider> listAll(QString *error = nullptr);
    static AiCcProvider loadCurrent(); // 优先 claude 系当前项
};
