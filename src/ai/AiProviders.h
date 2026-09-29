#pragma once
// ai/AiProviders.h — Mswrite 自己的 AI 供应商列表(格式对齐 MS-Agent 的
// providers.json,可一键导入)。存放在 %APPDATA%/Mswrite/ai-providers.json。

#include "ai/Types.h"

#include <QString>
#include <QStringList>
#include <QVector>

struct AiProvider {
    QString id;            // 稳定 id(导入/更新时定位)
    QString name;
    QString apiKey;
    QString baseUrl;
    QString model;         // 原样发送(需要 [1M] 等网关后缀时直接写进模型名)
    QString contextWindow; // 历史遗留字段,仅兼容旧文件,发送时不再拼接
    Protocol protocol = Protocol::Anthropic;
    int maxTokens = 0;     // 0 = 默认 4096
    QString importedFrom;  // 来源标记(cc-switch:app/name),不参与业务
    QString importedId;    // 稳定来源标识(cc-switch:app/id)，改名后仍可去重
};

class AiProviderStore {
public:
    // 打开(或新建)%APPDATA%/Mswrite/ai-providers.json
    static AiProviderStore load();
    static QString defaultFilePath();

    explicit AiProviderStore(const QString &path = QString());

    QVector<AiProvider> all() const { return items_; }
    QString currentName() const { return current_; }
    const AiProvider *current() const;
    const AiProvider *find(const QString &name) const;   // 大小写不敏感
    AiProvider *findMut(const QString &name);

    QString add(const AiProvider &p);        // 空 = 成功
    QString upsert(const AiProvider &p, const QString &originalName);
    QString remove(const QString &name);      // 删除当前项时选择下一项可用配置
    QString setCurrent(const QString &name);
    bool autoImportCcSwitch() const { return autoImportCcSwitch_; }
    QString setAutoImportCcSwitch(bool enabled);
    int importFromMsAgent(QString *report);   // 从 ~/.ms-agent/providers.json 复制副本
    int importFromCcSwitch(QString *report, bool automatic = false);

private:
    bool save(QString *err) const;
    void ensureCurrent();

    QString path_;
    QString current_;
    QVector<AiProvider> items_;
    bool autoImportCcSwitch_ = true;
    QStringList ignoredCcSwitch_; // 用户删除的来源，自动导入时不恢复
    QString loadError_;           // 配置损坏/不可读时禁止自动覆盖
};
