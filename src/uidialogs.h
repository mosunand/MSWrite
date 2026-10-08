#pragma once
#include <QString>
#include <QStringList>
class QWidget;
class QValidator;
namespace UiDialogs {
enum class SaveChoice { Save, Discard, Cancel };
SaveChoice confirmSave(QWidget *parent, const QString &theme, const QStringList &documents, bool quitting);
void showShortcuts(QWidget *parent,const QString &theme);
void showAbout(QWidget *parent,const QString &theme);
void showSkills(QWidget *parent,const QString &theme);
// 偏好设置:正文行距(1.0~2.2,默认 1.0)+ 座右铭(可留空取消)。写 QSettings:
// lineHeight / motto。返回 true 表示用户点了确定(调用方随后应用新值)。
bool showPrefs(QWidget *parent, const QString &theme);
// 座右铭宽度:1 个汉字(含全角/emoji)= 1,2 个半角字母/数字 = 1,上限 19
double mottoWidth(const QString &text);
QValidator *makeMottoValidator(QObject *parent);
}
