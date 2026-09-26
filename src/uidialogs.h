#pragma once
#include <QString>
#include <QStringList>
class QWidget;
namespace UiDialogs {
enum class SaveChoice { Save, Discard, Cancel };
SaveChoice confirmSave(QWidget *parent, const QString &theme, const QStringList &documents, bool quitting);
void showShortcuts(QWidget *parent,const QString &theme);
void showAbout(QWidget *parent,const QString &theme);
void showSkills(QWidget *parent,const QString &theme);
}
