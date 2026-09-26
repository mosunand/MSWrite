#include "mainwindow.h"
#include "pdfview.h"
#include "ai/ChatView.h"
#include "ai/ChatWebView.h"
#include "ai/AiChatDock.h"
#include "ai/AiInputEdit.h"
#include "ai/AiWorker.h"
#include "ai/Http.h"
#include <QTcpServer>
#include <QTcpSocket>
#include <QRegularExpression>
#include "ai/MswriteSkill.h"
#include "uidialogs.h"
#include <QtConcurrent/QtConcurrentRun>
#include <QDialog>
#include <QLineEdit>
#include <QTreeWidget>
#include <QJsonDocument>
#include <QJsonArray>
#include <QSignalSpy>
#include <QApplication>
#include <QAction>
#include <QClipboard>
#include <QMimeData>
#include <QPainter>
#include <QPdfWriter>
#include <QQuickWidget>
#include <QQuickItem>
#include <QSettings>
#include <QTabBar>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>
#include <QTest>
#include <QElapsedTimer>
#include <QFile>
#include <QTextBrowser>
#include <QScrollArea>
#include <QScrollBar>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QStackedWidget>
#include <QCloseEvent>
#include <objbase.h>
#include <memory>

static bool waitFor(const std::function<bool()> &f, int timeoutMs=10000)
{
    QElapsedTimer timer; timer.start();
    while(timer.elapsed()<timeoutMs) { if(f()) return true; QTest::qWait(10); }
    return false;
}

static QString evaluate(WebViewHost &host, const QString &script)
{
    auto result=std::make_shared<QPair<bool,QString>>(false, QString());
    host.evalWithResult(script,[result](const QString &value){result->first=true;result->second=value;});
    waitFor([&]{return result->first;});
    return result->second;
}

int main(int argc, char **argv)
{
    const auto oleResult=OleInitialize(nullptr);
    qInstallMessageHandler([](QtMsgType,const QMessageLogContext &,const QString &message){QTextStream(stdout)<<message<<Qt::endl;});
    QTextStream(stdout)<<"OLE_INIT "<<long(oleResult)<<Qt::endl;
    QApplication app(argc,argv);
    app.setQuitOnLastWindowClosed(false);
    app.setOrganizationName(QStringLiteral("MswriteTests"));
    app.setApplicationName(QStringLiteral("ExperienceRegression"));
    QTemporaryDir temp;
    const QByteArray originalAppData=qgetenv("APPDATA");
    qputenv("APPDATA",temp.path().toUtf8());
    struct AppDataRestore { QByteArray value; ~AppDataRestore(){qputenv("APPDATA",value);} } appDataRestore{originalAppData};
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat,QSettings::UserScope,temp.path());
    QSettings().setValue(QStringLiteral("imgDir"),temp.path());
    QSettings().setValue(QStringLiteral("saveDir"),temp.filePath("saved-documents"));
    const bool clipboardAvailable=OpenClipboard(nullptr);
    if(clipboardAvailable) CloseClipboard();
    auto *saved=new QMimeData;
    if(clipboardAvailable) if(const auto *old=app.clipboard()->mimeData())
        for(const auto &format:old->formats()) saved->setData(format,old->data(format));
    struct Restore { QMimeData *data; bool available; ~Restore(){if(available)QApplication::clipboard()->setMimeData(data);else delete data;} } restore{saved,clipboardAvailable};
    QTimer::singleShot(0, &app, [&] {
    QTextStream out(stdout);
    int passed=0,failed=0;
    auto check=[&](bool ok,const char *name){out<<(ok?"PASS ":"FAIL ")<<name<<'\n';out.flush();ok?++passed:++failed;};

    if(app.arguments().contains("--save-tests")) {
        const auto path=temp.filePath("autosave.md");
        FileService::writeFile(path,"# Saved document\n\n");
        QSettings().setValue("theme","light");QSettings().setValue("autoSave",true);
        MainWindow window(nullptr,path);window.setAttribute(Qt::WA_DontShowOnScreen);window.resize(1080,760);window.show();
        auto *host=window.findChild<WebViewHost *>();
        auto *status=window.findChild<QLabel *>("saveStatus");
        auto *tabs=window.findChild<QTabBar *>();
        auto *autoSave=window.findChild<QAction *>("autoSave");
        check(waitFor([&]{return host->isPageReady();},30000),"save fixture editor ready");
        QTest::qWait(450);
        auto append=[&](WebViewHost *editor,const QString &text){
            evaluate(*editor,QStringLiteral("window.msbridge.focus();var r=document.createRange();r.selectNodeContents(document.querySelector('.vditor-ir pre.vditor-reset'));r.collapse(false);window.getSelection().removeAllRanges();window.getSelection().addRange(r);window.msbridge.insertText(%1)")
                .arg(QString::fromUtf8(QJsonDocument(QJsonArray{text}).toJson(QJsonDocument::Compact)).mid(1).chopped(1)));
        };
        append(host,"\n\nAuto save marker");
        check(waitFor([&]{return FileService::readFile(path).contains("Auto save marker");},5000),"named document automatically writes actual editor content to disk");
        check(waitFor([&]{return status->text().contains(QStringLiteral("已保存"));}),"saved status follows successful disk write");
        check(status->toolTip().contains("autosave.md"),"save status exposes the actual file location");

        autoSave->trigger();append(host,"\n\nDisabled marker");QTest::qWait(2300);
        check(!FileService::readFile(path).contains("Disabled marker"),"disabled auto save leaves disk unchanged");
        QTimer::singleShot(40,[&]{
            if(auto *d=window.findChild<QDialog *>("saveChangesDialog")) {
                auto *cancel=d->findChild<QPushButton *>("saveCancel");
                check(d->palette().color(QPalette::Window).lightness()>220 && cancel->text()==QStringLiteral("继续编辑")
                    && d->findChild<QPushButton *>("saveConfirm")->text()==QStringLiteral("保存并退出")
                    && d->findChild<QPushButton *>("saveDiscard")->text()==QStringLiteral("不保存"),"light exit dialog uses readable surface and Chinese actions");
                if(app.arguments().contains("--screenshots"))d->grab().save(QCoreApplication::applicationDirPath()+"/save-light.png");
                cancel->click();
            }
        });
        check(!window.close() && window.isVisible(),"continue editing cancels exit without losing changes");
        autoSave->trigger();
        check(waitFor([&]{return FileService::readFile(path).contains("Disabled marker");},5000),"enabling auto save schedules existing unsaved edits");

        QMetaObject::invokeMethod(&window,"newTab");
        auto *draft=qobject_cast<WebViewHost *>(window.findChild<QStackedWidget *>()->currentWidget());
        check(draft && waitFor([&]{return draft->isPageReady();},30000),"new document editor ready");QTest::qWait(350);
        append(draft,"New document auto-save marker");
        QString newPath;
        check(waitFor([&]{
            const auto files=QDir(temp.filePath("saved-documents")).entryList({"*.md"},QDir::Files);
            if(files.isEmpty())return false;
            newPath=temp.filePath("saved-documents/")+files.first();
            return FileService::readFile(newPath).contains("New document auto-save marker");
        },5000),"new unnamed document receives a unique saved Markdown file");
        check(status->text().contains(QStringLiteral("已保存")) && !tabs->tabText(tabs->currentIndex()).contains(QStringLiteral("未命名 ●")),"new document reports saved only after file creation");
        tabs->setCurrentIndex(0);append(host,"\n\nBackground tab marker");tabs->setCurrentIndex(1);
        QElapsedTimer typing;typing.start();
        while(typing.elapsed()<2800) {append(draft," continued");QTest::qWait(350);}
        const bool backgroundSaved=FileService::readFile(path).contains("Background tab marker");
        if(!backgroundSaved)out<<"BACKGROUND_DISK "<<FileService::readFile(path)<<"\nBACKGROUND_EDITOR "<<evaluate(*host,"window.mswValue()")<<'\n';
        check(backgroundSaved,"typing in another tab cannot postpone a background document save");
        const QString finalText=QJsonDocument::fromJson(("["+evaluate(*draft,"window.mswValue()")+"]").toUtf8()).array().first().toString();
        check(window.close(),"closing immediately flushes pending automatic saves without a prompt");
        check(FileService::readFile(newPath)==finalText,"closing writes the final edits rather than an older snapshot");

        const auto failureDir=temp.filePath("unwritable-parent");QDir().mkpath(failureDir);
        const auto failurePath=failureDir+"/failure.md";
        FileService::writeFile(failurePath,"Original\n");
        MainWindow failure(nullptr,failurePath);failure.setAttribute(Qt::WA_DontShowOnScreen);failure.show();
        auto *failureHost=failure.findChild<WebViewHost *>();
        check(waitFor([&]{return failureHost->isPageReady();},30000),"failure fixture editor ready");QTest::qWait(350);
        QFile::remove(failurePath);QDir().rmdir(failureDir); // Only the temporary test fixture.
        append(failureHost,"\nUnsaved failure marker");
        auto *failureStatus=failure.findChild<QLabel *>("saveStatus");
        check(waitFor([&]{return failureStatus->text().contains(QStringLiteral("保存失败"));},5000),"write failure remains visibly unsaved");
        check(evaluate(*failureHost,"window.mswValue()").contains("Unsaved failure marker"),"write failure retains the editor contents");
        QTimer cancelFailedClose;cancelFailedClose.setInterval(40);
        QObject::connect(&cancelFailedClose,&QTimer::timeout,&failure,[&]{if(auto *d=failure.findChild<QDialog *>("saveChangesDialog")) {
            cancelFailedClose.stop();d->findChild<QPushButton *>("saveCancel")->click();
        }});cancelFailedClose.start();
        check(!failure.close(),"failed automatic save cannot silently close the document");
        QDir().mkpath(failureDir);QTest::mouseClick(failureStatus,Qt::LeftButton);
        check(waitFor([&]{return FileService::readFile(failurePath).contains("Unsaved failure marker");}),"clicking failed save retries and persists the retained text");
        failure.close();

        QTimer::singleShot(40,[&]{for(auto *widget:app.topLevelWidgets())if(auto *d=qobject_cast<QDialog *>(widget);d && d->objectName()=="saveChangesDialog") {
            check(d->palette().color(QPalette::Window).lightness()<80,"dark exit dialog follows the selected theme");
            if(app.arguments().contains("--screenshots"))d->grab().save(QCoreApplication::applicationDirPath()+"/save-dark.png");
            QTest::keyClick(d,Qt::Key_Escape);
        }});
        check(UiDialogs::confirmSave(nullptr,"dark",{QStringLiteral("项目笔记.md")},false)==UiDialogs::SaveChoice::Cancel,"Escape safely cancels save dialog");
        out<<passed<<" save integration checks passed; "<<failed<<" failed.\n";
        app.exit(failed?1:0);return;
    }

    const auto startupPath=temp.filePath(QStringLiteral("startup.md"));
    { QFile f(startupPath); f.open(QIODevice::WriteOnly); f.write("# Startup document\n\nContent appears on the first editor render.\n\n## Notes\n\nA readable workspace with clear tabs and a quiet outline.\n\n- Select text\n- Keep writing\n\n```cpp\nint main() { return 0; }\n```\n"); }
    QSettings().setValue(QStringLiteral("lastFile"), temp.filePath(QStringLiteral("missing.md")));
    QSettings().setValue(QStringLiteral("theme"), QStringLiteral("light"));
    {
        QElapsedTimer startup; startup.start();
        MainWindow window(nullptr, startupPath);
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1080,760); window.show();
        auto *host=window.findChild<WebViewHost *>();
        const auto *preview=host->findChild<QTextBrowser *>(QStringLiteral("startupText"));
        check(preview && preview->toPlainText().contains("Startup document"), "readable content exists before browser startup");
        out << "STARTUP_PREVIEW_MS " << startup.elapsed() << '\n'; out.flush();
        if (app.arguments().contains(QStringLiteral("--screenshots")))
            window.grab().save(QCoreApplication::applicationDirPath()+QStringLiteral("/startup-review.png"));
        check(window.findChild<QTabBar *>()->count()==1, "explicit startup file creates only one tab");
        check(waitFor([&]{return host->isPageReady();}), "startup editor navigation completes");
        check(waitFor([&]{return evaluate(*host,QStringLiteral("document.querySelector('.vditor-ir')?.textContent || ''")).contains("Startup document");}), "initial document renders without setContent round trip");
        out << "STARTUP_CONTENT_MS " << startup.elapsed() << '\n'; out.flush();
        check(evaluate(*host,QStringLiteral("!!window.msInitialState"))=="true", "document initialization is injected before page scripts");
        check(evaluate(*host,QStringLiteral("document.body.dataset.msTheme"))=="\"light\"", "saved theme applies at startup");
        check(waitFor([&]{return host->findChild<QWidget *>(QStringLiteral("startupPreview"))==nullptr;}), "preview gives way to the ready editor");
        if (app.arguments().contains(QStringLiteral("--screenshots"))) {
            QTest::qWait(200);
            window.grab().save(QCoreApplication::applicationDirPath()+QStringLiteral("/workspace-review.png"));
        }
        for(auto *action:window.findChildren<QAction *>())
            if(action->shortcut()==QKeySequence(QStringLiteral("F9"))) action->trigger();
        AiChatDock *dock=nullptr;
        for(auto *widget:app.topLevelWidgets()) if(auto *candidate=qobject_cast<AiChatDock *>(widget)) dock=candidate;
        check(dock!=nullptr,"assistant opens for current Markdown");
        if(dock) {
            QObject::disconnect(dock,&AiChatDock::runRequested,nullptr,nullptr);
            AiProvider provider;provider.apiKey="test-only";provider.baseUrl="http://127.0.0.1";provider.model="test";
            dock->applyProvider(provider);
            QSignalSpy dispatch(dock,&AiChatDock::runRequested);
            auto *input=dock->findChild<QPlainTextEdit *>("aiInputInner");
            input->setPlainText("Read current document");QTest::keyClick(input,Qt::Key_Return);
            check(dispatch.size()==1,"one user input dispatches one AI turn");
            const auto metadata=QJsonDocument::fromJson(dispatch.at(0).at(1).toString().toUtf8()).object();
            check(metadata.value("kind")=="markdown" && !dispatch.at(0).at(1).toString().contains("Content appears"),"initial AI request contains document metadata only");
            QJsonObject request{{"document_id",metadata.value("id")},{"start_line",1},{"line_count",2}};
            bool done=false;AiDocumentResult result;
            dock->readDocument(request,[&](AiDocumentResult r){result=std::move(r);done=true;});
            check(waitFor([&]{return done;}) && result.text.contains("Startup document") && result.text.contains("Next start_line: 3"),"AI reads live Markdown with explicit continuation");
            QMetaObject::invokeMethod(&window,"newTab");
            done=false;dock->readDocument(request,[&](AiDocumentResult r){result=std::move(r);done=true;});
            check(done && result.text.startsWith("Error:"),"switching tabs rejects an old document read");
        }
    }

    const auto pdfPath=temp.filePath(QStringLiteral("test.pdf"));
    { QPdfWriter writer(pdfPath); writer.setResolution(72); QPainter p(&writer); p.drawText(40,80,QStringLiteral("Native PDF menu selection.")); }
    {
        QElapsedTimer pdfStartup; pdfStartup.start();
        MainWindow window(nullptr, pdfPath);
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1050,800);
        window.show();
        auto *pdf=window.findChild<PdfViewWidget *>();
        check(pdf!=nullptr,"PDF tab uses reader");
        check(window.findChild<QTabBar *>()->count()==1 && window.findChildren<WebViewHost *>().isEmpty(), "PDF startup does not create an unused browser editor");
        auto *quick=pdf->findChild<QQuickWidget *>();
        check(waitFor([&]{return quick->rootObject()->property("pageReady").toBool();}),"PDF page in main window renders");
        out << "PDF_CONTENT_MS " << pdfStartup.elapsed() << '\n'; out.flush();
        auto shortcut=[&](QKeySequence key){
            for(auto *a:window.findChildren<QAction *>())
                if(a->shortcut()==key){a->trigger();return true;}
            return false;
        };
        shortcut(QKeySequence::SelectAll);
        check(waitFor([&]{return pdf->selectedText().contains("Native PDF");}),"main menu selects PDF text");
        if(clipboardAvailable) {
            shortcut(QKeySequence::Copy);
            check(waitFor([&]{return app.clipboard()->text().contains("Native PDF");}),"main menu copies PDF text");
        } else out<<"SKIP main menu clipboard check: Windows OpenClipboard unavailable\n";
        shortcut(QKeySequence::Find);
        check(quick->rootObject()!=nullptr,"main menu searches PDF without editor dereference");
        for(auto *a:window.findChildren<QAction *>()) {
            if(a->text().contains(QStringLiteral("Dark"))) { a->trigger(); break; }
        }
        check(pdf->palette().color(QPalette::Text).lightness()>180,"theme changes reach PDF tab");
        shortcut(QKeySequence::Undo);
        shortcut(QKeySequence::Redo);
        shortcut(QKeySequence::Cut);
        shortcut(QKeySequence(QStringLiteral("Ctrl+/")));
        shortcut(QKeySequence(QStringLiteral("Ctrl+Alt+=")));
        check(true,"editing commands tolerate PDF tabs");
        auto *tabs=window.findChild<QTabBar *>();
        QMetaObject::invokeMethod(&window, "newTab");
        tabs->setCurrentIndex(1);
        tabs->setCurrentIndex(0);
        check(pdf->isVisible(),"switching between Markdown and PDF");
        pdf->aiSelectionRequested(QStringLiteral("Native PDF menu selection."),0,true);
        AiChatDock *dock=nullptr;
        for(auto *widget:app.topLevelWidgets()) if(auto *candidate=qobject_cast<AiChatDock *>(widget)) dock=candidate;
        check(dock && dock->isVisible(),"PDF translation opens assistant automatically");
        if(dock) {
            auto *messages=dock->findChild<ChatModel *>();
            check(messages && messages->msgAt(messages->rowCount()-1)->text.contains(QStringLiteral("没有连接服务器")),"selection without API key reports missing connection");
            QObject::disconnect(dock,&AiChatDock::runRequested,nullptr,nullptr);
            AiProvider provider;provider.apiKey="test-only";provider.baseUrl="http://127.0.0.1";provider.model="test";dock->applyProvider(provider);
            QSignalSpy dispatch(dock,&AiChatDock::runRequested);
            dock->findChild<QPlainTextEdit *>("aiInputInner")->setPlainText("Draft is preserved");
            pdf->aiSelectionRequested(QStringLiteral("Native PDF menu selection."),0,false);
            check(dispatch.size()==1 && dispatch.at(0).at(0).toString().contains("Native PDF menu selection.") && dispatch.at(0).at(2).toInt()==0,"PDF analysis dispatches exact selection in read-only mode");
            check(dock->findChild<QPlainTextEdit *>("aiInputInner")->toPlainText()=="Draft is preserved","selection action preserves input draft");
            const auto metadata=QJsonDocument::fromJson(dispatch.at(0).at(1).toString().toUtf8()).object();
            AiDocumentResult result;
            dock->readDocument({{"document_id",metadata.value("id")},{"page",1},{"include_images",true}},[&](AiDocumentResult r){result=std::move(r);});
            const auto image=result.images.isEmpty() ? QImage() : QImage::fromData(QByteArray::fromBase64(result.images.first().base64.toLatin1()));
            check(result.text.contains("Native PDF menu selection.") && result.text.contains("[Page 1]") && !image.isNull(),"AI receives PDF page text and real page image");
            dock->readDocument({{"document_id",metadata.value("id")},{"page",100}},[&](AiDocumentResult r){result=std::move(r);});
            check(result.text.startsWith("Error:"),"invalid PDF page returns explicit error");
        }
    }
    {
        MainWindow restored;
        check(restored.findChild<PdfViewWidget *>()!=nullptr,"restart restores PDF as PDF");
    }

    {
        ChatModel model;
        QElapsedTimer chatStartup; chatStartup.start();
        QWidget chatWindow;
        chatWindow.setAttribute(Qt::WA_DontShowOnScreen);
        ChatWebView chat(&model,&chatWindow);
        chat.setAlwaysVisible(true);
        chatWindow.resize(800,700); chat.setGeometry(chatWindow.rect());
        chatWindow.show();
        const int id=model.append(ChatMsg::Assistant,QStringLiteral("# Native chat\n\n$$\\frac{x^2}{y_1}$$\n\n```cpp\nint x = 2;\n```"));
        model.msgAt(id)->finalized=true;
        bool ready=false;
        QObject::connect(&chat,&WebViewHost::message,[&](const QJsonObject &o){if(o.value("t")=="chatReady")ready=true;});
        check(waitFor([&]{return ready;},30000),"native WebView2 chat loads local assets");
        out << "CHAT_READY_MS " << chatStartup.elapsed() << '\n'; out.flush();
        if(!ready) out << "CHAT_DIAGNOSTIC " << evaluate(chat,QStringLiteral("JSON.stringify({url:location.href,state:document.readyState,title:document.title,lute:typeof Lute,chat:typeof chatView,body:document.body?.innerText.slice(0,300)})")) << '\n';
        if(ready) {
            QTest::qWait(200);
            check(evaluate(chat,QStringLiteral("document.querySelectorAll('.katex').length"))=="1","native chat renders KaTeX");
            check(evaluate(chat,QStringLiteral("document.querySelectorAll('.code-block').length"))=="1","native chat renders code block");
            if(clipboardAvailable) {
                evaluate(chat,QStringLiteral("document.querySelector('[data-action=copy]').click()"));
                check(waitFor([&]{return app.clipboard()->text().contains("# Native chat");}),"browser copy action reaches native clipboard");
            } else out<<"SKIP native chat clipboard check: Windows OpenClipboard unavailable\n";
            model.append(ChatMsg::Assistant,QStringLiteral("Streaming first"));
            QTest::qWait(100);
            model.msgAt(1)->text+=QStringLiteral(" and second."); model.touch(1);
            QTest::qWait(150);
            check(evaluate(chat,QStringLiteral("document.querySelector('article[data-id=\"1\"]').textContent")).contains("and second"),"native model deltas reach conversation");
            model.removeFrom(1); QTest::qWait(100);
            check(evaluate(chat,QStringLiteral("document.querySelectorAll('article').length"))=="1","removed messages disappear");
            model.clearAll(); QTest::qWait(100);
            check(evaluate(chat,QStringLiteral("document.querySelectorAll('article').length"))=="0","new conversation clears rendered history");
        }
    }
    {
        const QByteArray oldAppData=qgetenv("APPDATA");
        qputenv("APPDATA",temp.path().toUtf8());
        struct RestoreAppData { QByteArray value; ~RestoreAppData(){qputenv("APPDATA",value);} } restoreAppData{oldAppData};
        QSettings().setValue(QStringLiteral("aiTheme"),1);
        AiChatDock welcome;
        welcome.setAttribute(Qt::WA_DontShowOnScreen);
        welcome.resize(883,1096); welcome.show(); QTest::qWait(300);
        auto *scroll=welcome.findChild<QScrollArea *>(QStringLiteral("aiWelcomeScroll"));
        auto *grid=welcome.findChild<QGridLayout *>(QStringLiteral("aiWelcomeGrid"));
        auto *title=welcome.findChild<QLabel *>(QStringLiteral("aiWelcomeTitle"));
        check(scroll && grid && title && scroll->isVisible(),"empty conversation displays welcome page");
        auto columns=[&] {int row,col,rowSpan,colSpan;grid->getItemPosition(2,&row,&col,&rowSpan,&colSpan);return col;};
        check(columns()==2 && title->y()>180,"wide welcome has three columns and room for motif");
        const bool screenshots=app.arguments().contains(QStringLiteral("--screenshots"));
        const auto output=QCoreApplication::applicationDirPath();
        if(screenshots) welcome.grab().save(output+QStringLiteral("/welcome-light.png"));
        for(auto *button:welcome.findChildren<QPushButton *>())
            if(button->text()==QStringLiteral("🌙")) {button->click();break;}
        QTest::qWait(100);
        if(screenshots) welcome.grab().save(output+QStringLiteral("/welcome-dark.png"));
        welcome.resize(460,520); QTest::qWait(300);
        check(columns()==0 && scroll->widget()->width()<=scroll->viewport()->width(),"narrow welcome reflows without horizontal overflow");
        scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
        const auto last=grid->itemAt(5)->widget();
        check(scroll->viewport()->rect().contains(last->mapTo(scroll->viewport(),last->rect().bottomRight())),
            "last welcome card remains reachable in a short window");
        if(screenshots) welcome.grab().save(output+QStringLiteral("/welcome-narrow.png"));
        auto *input=static_cast<AiInputEdit *>(welcome.findChild<QPlainTextEdit *>("aiInputInner"));
        auto *attach=welcome.findChild<QPushButton *>("aiAttach");
        check(attach && !attach->icon().isNull() && attach->mapTo(&welcome,QPoint()).x()<input->mapTo(&welcome,QPoint()).x(),"attachment icon is visible on left of composer");
        QSignalSpy sends(&welcome,&AiChatDock::runRequested);
        QInputMethodEvent preedit(QStringLiteral("nihao"),{});app.sendEvent(input,&preedit);
        check(input->isComposing() && input->placeholderText().isEmpty(),"IME preedit hides placeholder");
        QTest::keyClick(input,Qt::Key_Return);
        check(sends.isEmpty(),"IME confirmation does not send message");
        if(screenshots) welcome.grab().save(output+QStringLiteral("/ime-review.png"));
        QInputMethodEvent commit;commit.setCommitString(QStringLiteral("你好"));app.sendEvent(input,&commit);
        check(!input->isComposing() && input->toPlainText().contains(QStringLiteral("你好")),"IME commit retains entered text");
        input->clear();QInputMethodEvent cancel;app.sendEvent(input,&cancel);
        check(!input->placeholderText().isEmpty(),"empty composer restores placeholder");
        auto *conversation=welcome.findChild<ChatWebView *>();
        auto *model=welcome.findChild<ChatModel *>();
        const int reply=model->append(ChatMsg::Assistant,QStringLiteral("Visible reply in the real assistant window.\n\n$x^2$"));
        model->msgAt(reply)->finalized=true;model->touch(reply);
        check(waitFor([&]{return conversation->isPageReady();},30000),"assistant conversation surface loads after welcome transition");
        check(waitFor([&]{return evaluate(*conversation,QStringLiteral("document.querySelector('#messages')?.textContent || ''")).contains("Visible reply");}),"real assistant renders its message model");
        check(evaluate(*conversation,QStringLiteral("document.visibilityState"))=="\"visible\"","assistant browser surface is actually visible");
        welcome.hide();welcome.show();QTest::qWait(150);
        check(evaluate(*conversation,QStringLiteral("document.visibilityState"))=="\"visible\"","reopening assistant restores browser visibility");
    }
    {
        QWidget parent;
        UiDialogs::showShortcuts(&parent,"light");
        auto *dialog=parent.findChild<QDialog *>("shortcutsDialog");
        auto *tree=dialog->findChild<QTreeWidget *>("shortcutList");
        auto *search=dialog->findChild<QLineEdit *>("shortcutSearch");
        search->setText(QStringLiteral("下一页"));
        int visible=0;for(int i=0;i<tree->topLevelItemCount();++i) visible+=!tree->topLevelItem(i)->isHidden();
        check(visible==1,"shortcut search filters grouped operations");
        search->clear();QTest::qWait(100);
        if(app.arguments().contains("--screenshots")) dialog->grab().save(QCoreApplication::applicationDirPath()+"/shortcuts-review.png");
        dialog->close();
        UiDialogs::showAbout(&parent,"dark");QTest::qWait(100);
        if(app.arguments().contains("--screenshots")) parent.findChild<QDialog *>("aboutDialog")->grab().save(QCoreApplication::applicationDirPath()+"/about-review.png");
        parent.findChild<QDialog *>("aboutDialog")->close();
        const auto skillDir=temp.filePath("skills");QDir().mkpath(skillDir+"/custom");
        {QFile file(skillDir+"/custom/SKILL.md");file.open(QIODevice::WriteOnly);file.write("Use the phrase regression-skill-marker when analyzing.");}
        QSettings().setValue("aiSkillsDir",skillDir);
        check(MswriteSkill::directory()==skillDir && MswriteSkill::customInstructions(skillDir).contains("regression-skill-marker"),"configured skill directory loads nested skill files");
        {QFile file(skillDir+"/custom/SKILL.md");file.open(QIODevice::WriteOnly);file.write("Updated regression-skill-marker");}
        check(MswriteSkill::customInstructions(skillDir).startsWith("\n## Skill:") && MswriteSkill::customInstructions(skillDir).contains("Updated"),"skill edits apply on next request without restart");
        UiDialogs::showSkills(&parent,"light");QTest::qWait(100);
        if(app.arguments().contains("--screenshots")) parent.findChild<QDialog *>("skillsDialog")->grab().save(QCoreApplication::applicationDirPath()+"/skills-review.png");
        parent.findChild<QDialog *>("skillsDialog")->close();

        AiChatDock dock;int reads=0,inserts=0;QString requestedId;
        dock.setDocumentReader([&](const QJsonObject &request,auto done){++reads;requestedId=request.value("document_id").toString();done(AiDocumentResult{"Page body marker",{{"Page 1","image/png","aW1hZ2U="}}});});
        dock.setInsertHandler([&](const QString &,const QString &){++inserts;return QString();});
        AiWorker worker;worker.setDock(&dock);
        QSignalSpy rendered(&worker,&AiWorker::turnRendered);
        AiLlmConfig config;config.apiKey="test-only";config.baseUrl="http://127.0.0.1";config.model="test";config.protocol=Protocol::OpenAi;worker.applyConfig(config);
        QVector<QByteArray> requests;
        worker.setTransport([&](Protocol,const QUrl &,const QByteArray &body,const auto &,int){
            requests.append(body);ChatResponse response;
            if(requests.size()==1) response.toolCalls={{"read1","ReadDocument",{{"document_id","wrong-id"},{"include_images",true}}},{"write1","Insert",{{"text","forbidden"}}}};
            else response.text="Analysis complete";
            return response;
        });
        auto future=QtConcurrent::run([&]{worker.run("Read the page",R"({"id":"pinned-id","kind":"pdf"})",0,0,{});});
        check(waitFor([&]{return future.isFinished();},12000),"AI tool loop completes with local mock transport");
        future.waitForFinished();
        check(waitFor([&]{return !rendered.isEmpty();}) && rendered.last().first().toString()=="Analysis complete","non-streaming tool reply reaches final visible response");
        check(reads==1 && requestedId=="pinned-id" && inserts==0,"read tool pins file identity and write-off rejects Insert");
        check(requests.size()==2 && !requests[0].contains("Page body marker") && requests[1].contains("Page body marker") && requests[1].contains("image_url"),"document text and image reach model only after tool request");
        check(requests[0].contains("regression-skill-marker"),"configured skills reach model system prompt");
    }
    {
        // Exercise the real send -> HTTP stream -> worker -> model -> WebView
        // path, without contacting a provider or reading real credentials.
        QTcpServer server;server.listen(QHostAddress::LocalHost);
        int requests=0;
        QObject::connect(&server,&QTcpServer::newConnection,&server,[&]{
            auto *socket=server.nextPendingConnection();
            auto received=std::make_shared<QByteArray>();
            QObject::connect(socket,&QTcpSocket::readyRead,socket,[&,socket,received]{
                *received+=socket->readAll();
                const int headerEnd=received->indexOf("\r\n\r\n");
                if(headerEnd<0)return;
                const auto length=QRegularExpression(QStringLiteral("content-length: (\\d+)"),QRegularExpression::CaseInsensitiveOption)
                    .match(QString::fromLatin1(received->left(headerEnd))).captured(1).toInt();
                if(received->size()<headerEnd+4+length)return;
                QObject::disconnect(socket,&QTcpSocket::readyRead,nullptr,nullptr);++requests;
                const QString reply=QStringLiteral("Visible AI reply %1.\\$f(x)\\$\n\n```latex\n$$x^2$$\n```").arg(requests);
                const QJsonObject chunk{{"choices",QJsonArray{QJsonObject{{"delta",QJsonObject{{"content",reply}}}}}}};
                const QByteArray body="data: "+QJsonDocument(chunk).toJson(QJsonDocument::Compact)+"\n\ndata: [DONE]\n\n";
                socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\nContent-Length: "+QByteArray::number(body.size())+"\r\n\r\n"+body);
                socket->disconnectFromHost();
            });
        });
        QSettings().setValue("preferLatex",true);
        {
            AiChatDock dock;dock.clearConversation();dock.setAttribute(Qt::WA_DontShowOnScreen);dock.resize(800,700);dock.show();
            AiProvider provider;provider.apiKey="test-only";provider.baseUrl=QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort());provider.model="test";provider.protocol=Protocol::OpenAi;
            dock.applyProvider(provider);HttpAbort::consume();
            auto *input=dock.findChild<QPlainTextEdit *>("aiInputInner");
            auto *view=dock.findChild<ChatWebView *>();
            auto *model=dock.findChild<ChatModel *>();
            input->setPlainText("Visible user message");QTest::keyClick(input,Qt::Key_Return);
            check(waitFor([&]{for(int i=0;i<model->rowCount();++i)if(model->msgAt(i)->finalized && model->msgAt(i)->text.contains("Visible AI reply"))return true;return false;},15000),"real assistant receives local HTTP streamed reply");
            check(waitFor([&]{return view->isPageReady();},30000),"real assistant chat page navigates");
            check(waitFor([&]{const auto content=evaluate(*view,QStringLiteral("document.querySelector('#messages')?.textContent || ''"));return content.contains("Visible user message") && content.contains("Visible AI reply");}),"user and AI messages both render through the full pipeline");
            check(evaluate(*view,QStringLiteral("document.querySelectorAll('.katex').length"))=="2","assistant applies LaTeX preference to the actual streamed response");
            check(waitFor([&]{
                QFile saved(temp.filePath("Mswrite/ai-session.json"));
                if(!saved.open(QIODevice::ReadOnly))return false;
                const auto rows=QJsonDocument::fromJson(saved.readAll()).object().value("rows").toArray();
                for(const auto &row:rows)if(row.toObject().value("t").toString().contains("Visible AI reply"))
                    return row.toObject().value("preferLatex").toBool();
                return false;
            }),"first-run session is saved with the per-message LaTeX preference");
            check(waitFor([&]{return view->findChild<QWidget *>("startupPreview")==nullptr;}),"text fallback yields only after chat render acknowledgement");
            evaluate(*view,QStringLiteral("window.__savedChat=window.chatView;window.chatView=null"));
            model->append(ChatMsg::User,QStringLiteral("Fallback user message"));
            check(waitFor([&]{auto *preview=view->findChild<QTextBrowser *>("startupText");return preview && preview->toPlainText().contains("Fallback user message") && preview->toPlainText().contains("Visible AI reply");}),"renderer failure preserves readable user and AI messages");
            auto *fallback=view->findChild<QWidget *>("startupPreview");
            check(fallback && fallback->isVisibleTo(&dock) && fallback->width()>300 && fallback->height()>200,
                  "readable fallback occupies the visible conversation area");
            if(fallback && app.arguments().contains("--screenshots"))
                fallback->grab().save(QCoreApplication::applicationDirPath()+"/chat-fallback-review.png");
            evaluate(*view,QStringLiteral("window.chatView=window.__savedChat;window.chrome.webview.postMessage({t:'chatReady'})"));
            check(waitFor([&]{return view->findChild<QWidget *>("startupPreview")==nullptr;}),"recovered renderer restores chat without losing messages");
        }
        QSettings().setValue("preferLatex",false);
        {
            AiChatDock restored;restored.setAttribute(Qt::WA_DontShowOnScreen);restored.resize(800,700);restored.show();
            auto *view=restored.findChild<ChatWebView *>();
            check(waitFor([&]{return view->isPageReady();},30000) && waitFor([&]{return evaluate(*view,QStringLiteral("document.querySelectorAll('.katex').length"))=="2";}),"restored conversation keeps converted formulas after preference is disabled");
        }
    }
    out<<passed<<" native integration checks passed; "<<failed<<" failed.\n";
    app.exit(failed?1:0);
    });
    return app.exec();
}
