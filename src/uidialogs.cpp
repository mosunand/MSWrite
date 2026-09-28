#include "uidialogs.h"
#include "ai/MswriteSkill.h"
#include <QApplication>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QIcon>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QTabWidget>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <QStyleFactory>
#include <windows.h>

namespace {
QDialog *dialog(QWidget *parent,const QString &theme,const QString &name,const QString &title)
{
    auto *d=new QDialog(parent);
    d->setObjectName(name);d->setWindowTitle(title);
    d->setAttribute(Qt::WA_DeleteOnClose);d->setWindowModality(Qt::WindowModal);
    d->resize(760,610);d->setMinimumSize(480,400);
    d->setFont(QFont(QStringLiteral("Microsoft YaHei UI"),10));
    const bool dark=theme==QLatin1String("dark");
    d->setStyleSheet(QStringLiteral(R"(
QDialog { background:%1; color:%2; }
QLabel { color:%2; background:transparent; }
QLabel[muted="true"] { color:%5; }
QLabel[heading="true"] { font-size:24px; font-weight:600; }
QLineEdit { background:%3; color:%2; border:1px solid %4; border-radius:6px; padding:9px 12px; font-size:13px; selection-background-color:#3b67ce; }
QLineEdit:focus { border-color:#6784d7; }
QTreeWidget { background:%3; color:%2; border:1px solid %4; border-radius:6px; font-size:13px; outline:none; }
QTreeWidget::item { padding:9px 6px; border-bottom:1px solid %4; }
QTreeWidget::item:selected { background:%4; color:%2; }
QHeaderView::section { background:%1; color:%5; border:0; padding:10px 6px; text-align:left; }
QPushButton { background:%3; color:%2; border:1px solid %4; border-radius:6px; padding:8px 14px; font-size:13px; }
QPushButton:hover { border-color:#6784d7; }
QPushButton:default { background:#315ec7; border-color:#315ec7; color:white; }
)").arg(dark ? "#181a20" : "#f6f7fa",dark ? "#eceef2" : "#202631",
           dark ? "#20232b" : "#ffffff",dark ? "#343945" : "#e0e4ec",dark ? "#a1aab9" : "#687386"));
    auto *layout=new QVBoxLayout(d);layout->setContentsMargins(28,24,28,20);layout->setSpacing(16);
    auto *heading=new QLabel(title,d);heading->setProperty("heading",true);layout->addWidget(heading);
    return d;
}
QLabel *label(const QString &text,QWidget *parent,bool muted=false)
{
    auto *l=new QLabel(text,parent);l->setWordWrap(true);l->setProperty("muted",muted);
    l->setTextInteractionFlags(Qt::TextSelectableByMouse);return l;
}
void closeButton(QDialog *d)
{
    auto *buttons=new QDialogButtonBox(QDialogButtonBox::Close,d);
    buttons->button(QDialogButtonBox::Close)->setText(QObject::tr("关闭"));
    QObject::connect(buttons,&QDialogButtonBox::rejected,d,&QDialog::close);
    d->layout()->addWidget(buttons);
}
}

UiDialogs::SaveChoice UiDialogs::confirmSave(QWidget *parent, const QString &theme,
                                            const QStringList &documents, bool quitting)
{
    QDialog d(parent);
    d.setObjectName(QStringLiteral("saveChangesDialog"));
    d.setWindowTitle(QObject::tr("保存更改"));
    d.setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    d.setWindowModality(Qt::WindowModal);
    auto *style=QStyleFactory::create(QStringLiteral("Fusion"));
    style->setParent(&d); d.setStyle(style);
    const bool dark=theme==QLatin1String("dark");
    const QString base=dark ? "#1c2028" : theme==QLatin1String("paper") ? "#fffdf8" : "#ffffff";
    const QString ink=dark ? "#edf0f5" : "#253041";
    QPalette palette=d.palette();
    palette.setColor(QPalette::Window,QColor(base));
    palette.setColor(QPalette::WindowText,QColor(ink));
    d.setPalette(palette);
    d.setFont(QFont(QStringLiteral("Microsoft YaHei UI"),10));
    d.setStyleSheet(QStringLiteral(R"(
QDialog { background:%1; }
QLabel { color:%2; background:transparent; }
QLabel#saveHeading { font-size:20px; font-weight:600; }
QLabel#saveDetail { color:%3; font-size:13px; }
QLabel#saveDocuments { background:%4; border:1px solid %5; border-radius:8px; padding:12px; }
QPushButton { color:%2; background:%1; border:1px solid %5; border-radius:6px; padding:9px 15px; min-width:60px; }
QPushButton:hover { background:%4; border-color:#6b88cb; }
QPushButton:focus { border:2px solid #6b88cb; padding:8px 14px; }
QPushButton#saveConfirm { color:white; background:#315ec7; border-color:#315ec7; }
QPushButton#saveConfirm:hover { background:#264da9; }
)").arg(base,ink,dark ? "#a7b2c5" : "#65738a",dark ? "#252b36" : "#f5f7fb",dark ? "#3a4454" : "#dfe5ee"));
    auto *layout=new QVBoxLayout(&d);
    layout->setContentsMargins(24,24,24,20);layout->setSpacing(16);
    auto *heading=new QLabel(quitting ? QObject::tr("退出前保存更改？") : QObject::tr("关闭前保存更改？"),&d);
    heading->setObjectName("saveHeading");layout->addWidget(heading);
    auto *detail=new QLabel(QObject::tr("以下文档的最新修改尚未写入磁盘。选择“不保存”将丢弃这些修改。"),&d);
    detail->setObjectName("saveDetail");detail->setWordWrap(true);layout->addWidget(detail);
    QStringList names=documents.mid(0,4);
    if(documents.size()>4) names.append(QObject::tr("另有 %1 个文档").arg(documents.size()-4));
    auto *files=new QLabel(names.join('\n'),&d);
    files->setTextFormat(Qt::PlainText);files->setWordWrap(true);files->setObjectName("saveDocuments");layout->addWidget(files);
    auto *buttons=new QHBoxLayout;buttons->setSpacing(8);buttons->addStretch();
    auto *discard=new QPushButton(QObject::tr("不保存"),&d);discard->setObjectName("saveDiscard");
    auto *cancel=new QPushButton(QObject::tr("继续编辑"),&d);cancel->setObjectName("saveCancel");
    auto *save=new QPushButton(quitting ? QObject::tr("保存并退出") : QObject::tr("保存并关闭"),&d);save->setObjectName("saveConfirm");
    buttons->addWidget(discard);buttons->addWidget(cancel);buttons->addWidget(save);layout->addLayout(buttons);
    SaveChoice result=SaveChoice::Cancel;
    QObject::connect(discard,&QPushButton::clicked,&d,[&]{result=SaveChoice::Discard;d.accept();});
    QObject::connect(cancel,&QPushButton::clicked,&d,&QDialog::reject);
    QObject::connect(save,&QPushButton::clicked,&d,[&]{result=SaveChoice::Save;d.accept();});
    save->setDefault(true);cancel->setFocus();d.setFixedWidth(460);
    // The Windows caption follows the app's selected theme, not the OS theme.
    using SetAttribute=HRESULT(WINAPI *)(HWND,DWORD,LPCVOID,DWORD);
    static HMODULE dwm=LoadLibraryW(L"dwmapi.dll");
    static auto setAttribute=dwm ? reinterpret_cast<SetAttribute>(GetProcAddress(dwm,"DwmSetWindowAttribute")) : nullptr;
    if(setAttribute) {
        const HWND hwnd=reinterpret_cast<HWND>(d.winId());
        const BOOL useDark=dark;setAttribute(hwnd,20,&useDark,sizeof(useDark));
        const QColor bg(base),fg(ink);
        const COLORREF caption=RGB(bg.red(),bg.green(),bg.blue()),text=RGB(fg.red(),fg.green(),fg.blue());
        setAttribute(hwnd,35,&caption,sizeof(caption));setAttribute(hwnd,36,&text,sizeof(text));
    }
    d.exec();
    return result;
}

void UiDialogs::showShortcuts(QWidget *parent,const QString &theme)
{
    auto *d=dialog(parent,theme,"shortcutsDialog",QObject::tr("快捷键"));
    auto *layout=qobject_cast<QVBoxLayout *>(d->layout());
    auto *search=new QLineEdit(d);search->setObjectName("shortcutSearch");
    search->setPlaceholderText(QObject::tr("搜索操作或快捷键"));search->setClearButtonEnabled(true);layout->addWidget(search);
    auto *tree=new QTreeWidget(d);tree->setObjectName("shortcutList");tree->setColumnCount(2);
    tree->setHeaderLabels({QObject::tr("操作"),QObject::tr("快捷键")});
    tree->header()->setStretchLastSection(false);
    tree->header()->setSectionResizeMode(0,QHeaderView::Stretch);
    tree->header()->setSectionResizeMode(1,QHeaderView::ResizeToContents);
    struct Entry {const char *group;const char *name;const char *key;};
    const Entry entries[]={
        {"文档","新建标签","Ctrl+N"},{"文档","打开文件","Ctrl+O"},{"文档","快速打开","Ctrl+P"},
        {"文档","保存","Ctrl+S"},{"文档","另存为","Ctrl+Shift+S"},{"文档","关闭标签","Ctrl+W"},
        {"文档","重开关闭的文件","Ctrl+Shift+T"},{"文档","偏好设置","Ctrl+,"},
        {"编辑","撤销 / 重做","Ctrl+Z / Ctrl+Y"},{"编辑","复制 / 粘贴","Ctrl+C / Ctrl+V"},
        {"编辑","纯文本粘贴","Ctrl+Shift+V"},{"编辑","查找 / 替换","Ctrl+F / Ctrl+H"},
        {"编辑","加粗 / 斜体","Ctrl+B / Ctrl+I"},{"编辑","链接","Ctrl+K"},
        {"编辑","一级至四级标题","Ctrl+1 / 2 / 3 / 4"},{"编辑","插入表格","Ctrl+T"},
        {"编辑","代码块","Ctrl+Shift+K"},{"编辑","数学公式块","Ctrl+Shift+M"},
        {"编辑","标题升级 / 降级","Ctrl+Shift+Up / Down"},
        {"视图","源码模式","Ctrl+/"},{"视图","放大 / 缩小 / 适配","Ctrl+= / Ctrl+- / Ctrl+0"},
        {"视图","字号增大 / 缩小 / 复原","Ctrl+Alt+= / - / 0"},{"视图","大纲","Ctrl+Shift+1"},
        {"视图","全屏","F11"},{"PDF 阅读","上一页 / 下一页","PageUp / PageDown"},
        {"PDF 阅读","首 / 末页","Ctrl+Home / Ctrl+End"},{"PDF 阅读","查找下 / 上一个","F3 / Shift+F3"},
        {"PDF 阅读","复制选区","Ctrl+C"},{"导出","HTML / PDF / Word","Ctrl+Shift+E / P / W"},
        {"AI 助手","打开助手","F9"},{"AI 助手","发送 / 换行","Enter / Shift+Enter"},
        {"AI 助手","粘贴截图","Ctrl+V"},{"AI 助手","输入历史","Up / Down"}};
    QTreeWidgetItem *group=nullptr;QString previous;
    for(const auto &entry:entries) {
        const QString category=QString::fromUtf8(entry.group);
        if(category!=previous) {
            group=new QTreeWidgetItem(tree,{category});group->setFirstColumnSpanned(true);
            QFont f=group->font(0);f.setBold(true);group->setFont(0,f);previous=category;
        }
        new QTreeWidgetItem(group,{QString::fromUtf8(entry.name),QString::fromUtf8(entry.key)});
    }
    tree->expandAll();layout->addWidget(tree,1);
    QObject::connect(search,&QLineEdit::textChanged,d,[tree](const QString &query) {
        for(int i=0;i<tree->topLevelItemCount();++i) {
            auto *group=tree->topLevelItem(i);bool any=false;
            for(int j=0;j<group->childCount();++j) {
                auto *item=group->child(j);
                const bool match=(group->text(0)+item->text(0)+item->text(1)).contains(query.trimmed(),Qt::CaseInsensitive);
                item->setHidden(!match);any|=match;
            }
            group->setHidden(!any);
        }
    });
    closeButton(d);d->show();search->setFocus();
}

void UiDialogs::showAbout(QWidget *parent,const QString &theme)
{
    auto *d=dialog(parent,theme,"aboutDialog",QStringLiteral("Mswrite"));d->resize(650,520);
    auto *layout=qobject_cast<QVBoxLayout *>(d->layout());
    auto *brand=new QHBoxLayout;
    auto *logo=new QLabel(d);logo->setPixmap(QIcon(":/logo.ico").pixmap(64,64));logo->setFixedSize(72,72);
    brand->addWidget(logo);
    brand->addWidget(label(QObject::tr("所见即所得 Markdown 编辑器\n写作、阅读与 AI，在同一个工作空间。"),d),1);
    layout->addLayout(brand);
    layout->addWidget(label(QObject::tr("编辑与排版"),d));
    layout->addWidget(label(QObject::tr("Markdown 即时预览 · LaTeX 数学公式 · 代码高亮 · 表格与图表\n多标签、自动保存，以及 HTML、PDF、Word 导出。"),d,true));
    layout->addWidget(label(QObject::tr("阅读与 AI"),d));
    layout->addWidget(label(QObject::tr("PDF 文字选择、搜索与目录导航。AI 可按需读取当前文档，支持选区翻译、分析，以及自定义技能。"),d,true));
    layout->addStretch();
    layout->addWidget(label(QObject::tr("版本 2.0.0  ·  Qt %1  ·  Windows\n编辑内核：WebView2 / Vditor / Lute / KaTeX\n文档保存在本机；AI 请求使用你配置的服务。致敬 Typora。")
        .arg(QStringLiteral(QT_VERSION_STR)),d,true));
    auto *folder=new QPushButton(QObject::tr("打开程序目录"),d);
    QObject::connect(folder,&QPushButton::clicked,d,[]{QDesktopServices::openUrl(QUrl::fromLocalFile(QCoreApplication::applicationDirPath()));});
    layout->addWidget(folder,0,Qt::AlignLeft);closeButton(d);d->show();
}

void UiDialogs::showSkills(QWidget *parent,const QString &theme)
{
    auto *d=dialog(parent,theme,"skillsDialog",QObject::tr("AI 技能"));
    auto *layout=qobject_cast<QVBoxLayout *>(d->layout());
    auto *row=new QHBoxLayout;
    auto *path=new QLineEdit(MswriteSkill::directory(),d);path->setObjectName("skillsDirectory");
    auto *browse=new QPushButton(QObject::tr("选择目录…"),d);row->addWidget(path,1);row->addWidget(browse);layout->addLayout(row);
    auto *tree=new QTreeWidget(d);tree->setHeaderLabels({QObject::tr("技能文件"),QObject::tr("大小")});
    tree->header()->setStretchLastSection(false);
    tree->header()->setSectionResizeMode(1,QHeaderView::ResizeToContents);
    tree->setRootIsDecorated(false);tree->header()->setSectionResizeMode(0,QHeaderView::Stretch);
    layout->addWidget(tree,1);
    auto *status=label(QString(),d,true);layout->addWidget(status);
    auto refresh=[path,tree,status]{
        tree->clear();const auto files=MswriteSkill::files(path->text());
        for(const auto &file:files) new QTreeWidgetItem(tree,{QDir(path->text()).relativeFilePath(file),QStringLiteral("%1 KB").arg(QFileInfo(file).size()/1024.0,0,'f',1)});
        status->setText(QDir(path->text()).exists() ? QObject::tr("%1 个可用技能 · 下次提问生效").arg(files.size()) : QObject::tr("目录不存在"));
    };
    QObject::connect(path,&QLineEdit::textChanged,d,refresh);
    QObject::connect(browse,&QPushButton::clicked,d,[d,path]{const auto dir=QFileDialog::getExistingDirectory(d,QObject::tr("选择技能目录"),path->text());if(!dir.isEmpty())path->setText(dir);});
    auto *buttons=new QDialogButtonBox(QDialogButtonBox::Save|QDialogButtonBox::Cancel,d);
    buttons->button(QDialogButtonBox::Save)->setText(QObject::tr("保存"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QObject::tr("取消"));
    QObject::connect(buttons,&QDialogButtonBox::accepted,d,[d,path,status]{
        const QFileInfo folder(path->text());
        if(!folder.isDir() || !folder.isReadable()){status->setText(QObject::tr("请选择可读取的目录"));return;}
        QSettings().setValue(QStringLiteral("aiSkillsDir"),folder.absoluteFilePath());d->close();
    });
    QObject::connect(buttons,&QDialogButtonBox::rejected,d,&QDialog::close);
    layout->addWidget(buttons);refresh();d->show();
}
