#pragma once
// ai/MswriteSkill.h — 把 Mswrite 的写作能力写成"技能"注入系统提示:
// 模型由此知道编辑器支持的格式,并加载用户的 Markdown 技能指令。

#include <QString>
#include <QStringList>

namespace MswriteSkill {

// 基础技能(身份 + 工作方式 + Mswrite Markdown 规范)
QString basePrompt();

// 写入模式说明(0=不写入 1=AI 决定 2=强制全写)
QString writeModeSection(int writeMode);

// 完整系统提示 = 基础技能 + 写入模式 + 当前文档内容(全文,不截断)
QString withDocument(const QString &docMarkdown, int writeMode = 1);
QString directory();
QStringList files(const QString &directory);
QString customInstructions(const QString &directory);
QString withContext(const QString &context, int writeMode, const QString &skills);

} // namespace MswriteSkill
