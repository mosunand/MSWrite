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
    QString model;         // 旧格式兼容:当前模型 id 的镜像(新格式以 models 为准)
    QString contextWindow; // 历史遗留字段,仅兼容旧文件,发送时不再拼接
    Protocol protocol = Protocol::Anthropic;
    int maxTokens = 0;     // 旧格式兼容:当前模型回复上限的镜像;0 = 默认 1000448
    QVector<AiModelCfg> models;   // 该网址下的模型列表(可为空=旧格式)
    QString currentModel;         // 当前选中的模型 id(空 = 第一个启用的)
    QString importedFrom;  // 来源标记(cc-switch:app/name),不参与业务
    QString importedId;    // 稳定来源标识(cc-switch:app/id)，改名后仍可去重
    QJsonObject sourceConfig;

    // 有效当前模型:models 优先(currentModel → 第一个启用项),旧字段兜底
    const AiModelCfg *effectiveModel() const;
    QString effectiveModelId() const;
    int effectiveMaxTokens() const;         // 0 = 默认 1000448
    QStringList effectiveThinkLevels() const; // "关"不在此列,由 UI 自行添加
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
    QString setCurrentModel(const QString &providerName, const QString &modelId); // 切换某供应商的当前模型
    bool autoImportCcSwitch() const { return autoImportCcSwitch_; }
    QString setAutoImportCcSwitch(bool enabled);
    QString ccSwitchApp() const { return ccSwitchApp_; }
    QString setCcSwitchApp(const QString &app);
    int importFromMsAgent(QString *report);   // 从 ~/.ms-agent/providers.json 复制副本
    int importFromCcSwitch(QString *report, bool automatic = false);

private:
    bool save(QString *err) const;
    void ensureCurrent();

    QString path_;
    QString current_;
    QVector<AiProvider> items_;
    bool autoImportCcSwitch_ = true;
    QString ccSwitchApp_ = QStringLiteral("claude");
    QStringList ignoredCcSwitch_; // 用户删除的来源，自动导入时不恢复
    QString lastFollowedCcId_;    // 上次自动跟随的 cc-switch 来源 id:
                                  // 未变化时不覆盖用户在应用内选的当前供应商
    QString loadError_;           // 配置损坏/不可读时禁止自动覆盖
};
