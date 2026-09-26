#pragma once
// ai/AiCcSwitch.h — 只读读取本机 cc-switch 的供应商(SQLite)。
// 绝不写库;Key 只进内存/自有配置,不落日志。移植自 MS-Agent CcSwitch。

#include "ai/Types.h"

#include <QString>
#include <QVector>

struct AiCcProvider {
    bool ok = false;
    QString error;
    QString id;
    QString name;
    QString appType; // claude | codex | gemini | …
    Protocol protocol = Protocol::Anthropic;
    QString apiKey;
    QString baseUrl;
    QString model;   // 可能带 [1M] 后缀,导入时拆分
};

class AiCcSwitch {
public:
    static QString dbPath(); // CC_SWITCH_HOME 或 ~/.cc-switch/cc-switch.db
    static QVector<AiCcProvider> listAll();
    static AiCcProvider loadCurrent(); // 优先 claude 系当前项
};
