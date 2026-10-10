#pragma once

#include "fileservice.h"
#include "ai/AiProviders.h"
#include <QUuid>

#include <QMainWindow>
#include <QFutureWatcher>
#include <QPair>
#include <QPointer>
#include <QSet>
#include <QVector>
#include <functional>
#include <atomic>
#include <memory>

class WebViewHost;
class OutlineDock;
class SearchDock;
class AiChatDock;
class PdfViewWidget;

class WebViewHost;
class OutlineDock;
class SearchDock;
class QLabel;
class QTimer;
class QTabBar;
class QStackedWidget;
struct OutlineItem;

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr, const QString &initialPath = QString());
    ~MainWindow() override;

    void openFileFromArg(const QString &path);   // 命令行传入文件

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void openFile();
    void openFolder();
    void quickOpen();
    void newWindow();               // Ctrl+Shift+N
    void reopenClosedFile();        // Ctrl+Shift+T
    void showPrefs();               // Ctrl+,
    void showWelcome();             // 首次启动欢迎页
    QString modeLabelText(bool sourceMode) const;  // 状态栏模式位(座右铭优先)
    void toggleOutlinePanel();      // Ctrl+Shift+1
    void globalSearch();            // Ctrl+Shift+F
    void toggleSourceMode();        // Ctrl+/(源码/所见即所得切换)
    void toggleFullscreen();        // F11
    void openDevTools();            // Shift+F12
    void handlePickImage();         // Ctrl+Shift+I(来自页面)
    void handleReplaceDialog(const QString &prefill = QString()); // Ctrl+H(来自页面)
    void showEditorContextMenu(bool math = false, bool mathBold = false);
    void pasteFromClipboard();      // 菜单/右键"粘贴"(剪贴板由 C++ 读)
    bool saveFile();
    bool saveFileAs();
    void newTab();
    void closeTab(int index);
    void switchTab(int index);
    void exportHtml();
    void exportPdf();
    void exportWord();

    void onWebMessage(class WebViewHost *sender, const QJsonObject &obj);
    void onOutlineGoto(int index);
    void onSearchRequested(const QString &scope, const QString &query);
    void onSearchFileResult(const QString &path, const QString &lineText);
    void onSearchDocResult(int blockIndex);

private:
    struct Tab {
        WebViewHost *host = nullptr;      // Markdown 编辑器(host 和 pdf 二选一)
        PdfViewWidget *pdf = nullptr;     // PDF 阅读器
        QString path;                  // 空 = 未命名
        QString aiId=QUuid::createUuid().toString(QUuid::WithoutBraces);
        QString mode = QStringLiteral("ir");
        bool dirty = false;
        bool awaitingContent = false;  // 正等该 host 回传全文(保存)
        bool closeAfterSave = false;   // 保存完成后关闭此标签
        bool choosingSavePath = false;
        QTimer *autoSave = nullptr;
        int saveRequest = 0;
        QString saveError;
        int rev = 0;                   // 页面修订号:判定保存快照是否已过期
        FileService::Encoding enc = FileService::Encoding::Utf8; // 原文件编码
        bool crlf = false;             // 原文件行尾:CRLF 保存时原样保留
        double zoom = 1.0;             // 整页缩放(Ctrl+滚轮,每标签独立)
        QString docLang;               // 本文档默认代码语言(新建代码块自动带上)
        QString stats;                 // 最近统计文本(切标签时恢复)
        QString docHost;               // 本标签文档目录的虚拟主机名(doc{n}.local)
        QString docDir;                // 本标签文档所在目录(与 docHost 配对,用于落盘还原)
        QVector<QPair<QString, QString>> imageMappings; // Previous document roots survive Save As.
        QSet<QString> copiedImageTargets;
        QString titleHint;             // 文档开头正文的清洗标题(未命名文档的默认文件名候选)
    };

    void buildMenus();
    void buildStatusBar();
    void changeFontSize(int delta);             // 1 增大 / -1 减小 / 0 重置
    void applyDocLang(Tab &tab, const QString &lang); // 文档默认语言:应用+持久化+下发
    void applyDocDir(Tab &tab);                 // 设置文档目录映射并通知页面
    void attachTabCloseButton(int index);       // 标签挂自定义关闭按钮(紫/悬浮红)
    void updateTabText(int index);              // 标签文字(含 ● 脏标记)
    QString imagePoolDir() const;               // 中央图片库(Typora 同款位置)

    int addPdfTab(const QString &path);           // 打开 PDF 标签页
    int addTab(const QString &path, const QString &content,
               FileService::Encoding enc = FileService::Encoding::Utf8,
               bool crlf = false);
    int findTabByPath(const QString &path) const;
    int currentTabIndex() const;
    Tab *currentTab();
    Tab *tabForHost(WebViewHost *host);
    int indexOfHost(WebViewHost *host) const;

    bool openPath(const QString &path);           // 已打开则激活,否则新标签
    void requestContent(Tab &tab);
    bool flushSaves(const QVector<WebViewHost *> &hosts);
    void markDirty(int index, bool dirty);
    void updateTitle();
    void syncStatusFromTab();
    // 未命名文档落盘:标题冲突时加序号;无可用标题回退时间戳名
    QString uniqueUntitledPath(const QString &title) const;
    // 另存为/导出前的默认名兜底:firstLine 消息未到时同步向页面取一次首行
    void refreshTitleHint(Tab &tab);

    void applyTheme(const QString &theme, bool persist = true);
    void broadcastTheme();
    void updateSaveIndicator(bool dirty);    // 状态栏:未保存红/已保存白(加粗)
    void updateZoomLabel(double factor);     // 状态栏:缩放百分比胶囊
    void applyZoom(Tab &tab, double factor); // 整页缩放:同步 host/状态栏/持久化
    bool eventFilter(QObject *obj, QEvent *ev) override;  // 编辑区 Ctrl+滚轮缩放

    void saveImageFromWeb(Tab &tab, int rid, const QString &mime, const QString &base64);
    QString saveImageAsset(const Tab &tab, const QString &base64, const QString &mime,
                           QString *error);

    // 图片池:默认中央库(可由用户在菜单中更改),旧池目录保留映射以便旧图显示
    struct ImgMap { QString host; QString dir; };
    QStringList m_imgDirs;
    QVector<ImgMap> m_imgMaps;
    int m_imgHostSeq = 0;
    void syncImgMaps();

    // 把内部虚拟域 URL 还原为绝对路径(imgpool/doc 主机),保证落盘/导出数据干净
    QString restoreImagePaths(const Tab &tab, QString md) const;
    bool copyImageAssets(Tab &tab, const QString &markdown, const QString &rawMarkdown,
                         const QString &path, QString *error) const;

    // 导出
    QString composeExportHtml(const QString &bodyHtml, const Tab &tab);
    void requestExport(int kind);                // 0=html 1=pdf 2=word
    void ensureExportHost();
    void finishExport(const QString &bodyHtml, Tab *tab);
    void resetExport();
    QString welcomeContent() const;

    // 组合键处理(编辑区获得焦点时)
    bool handleAccelerator(int vk, bool ctrl, bool shift, bool alt);

    // ---------- AI 写作助手 ----------
    void ensureAiDock();                          // 懒创建右侧对话面板
    void toggleAiDock();
    void openAiConfig(QWidget *owner = nullptr, const QString &theme = QString());
    void applyCurrentAiProvider();               // 当前供应商 → 面板标签 + 工作线程
    QString insertAiText(const QString &documentId,const QString &text);
    QJsonObject aiDocumentContext();
    void readAiDocument(const QJsonObject &request,std::function<void(AiDocumentResult)> cb);
    void openAiSkills();
    void runAiDoctor();                           // 连接诊断(后台线程跑,弹窗显示)
    AiChatDock *m_aiDock = nullptr;
    QAction *m_aiToggleAction = nullptr;
    AiProviderStore m_aiStore;

    WebViewHost *m_exportHost = nullptr;         // 隐藏:导出 PDF 用
    bool m_exportNavPending = false;
    QString m_exportNavUrl;
    QString m_exportPdfTarget;
    bool m_exportAlternates = false;
    int m_exportKind = -1;                       // 待完成的导出类型
    bool m_exportBusy = false;
    int m_exportRequest = 0;
    QPointer<WebViewHost> m_exportSource;

    QTabBar *m_tabbar = nullptr;
    QStackedWidget *m_stack = nullptr;
    QVector<Tab> m_tabs;

    OutlineDock *m_outline = nullptr;
    SearchDock *m_search = nullptr;
    FileService *m_files = nullptr;
    QString m_workspace;          // 工作区目录(搜索范围/快速打开,不再有文件树)

    QLabel *m_statsLabel = nullptr;
    QLabel *m_modeLabel = nullptr;
    QLabel *m_saveLabel = nullptr;   // 状态栏保存状态指示
    QLabel *m_zoomLabel = nullptr;   // 状态栏缩放百分比(点击回 100%)
    QLabel *m_themeLabel = nullptr;

    QString m_theme = QStringLiteral("light");
    QString m_motto;                     // 座右铭(空 = 状态栏显示「所见即所得」)
    int m_fontSize = 16;
    bool m_lineNumbers = false;          // 代码块行号(默认关:MarkText 式无行号卡片)
    int m_docHostSeq = 0;
    QFutureWatcher<QVector<QPair<QString, QString>>> m_searchWatcher;
    std::shared_ptr<std::atomic_bool> m_searchCancelled;
    bool m_focusMode = false;
    bool m_typewriter = false;
    bool m_shuttingDown = false;
    bool m_flushingSaves = false;
    bool m_pdfActive = false;
    bool m_outlineBeforePdf = true;
    bool m_runtimeWarned = false;   // 已提示过 WebView2 运行时异常
    // 自动保存开关缓存:changed/content/markDirty 等高频路径不再每次开注册表,
    // 写入方(菜单动作)负责同步本成员
    bool m_autoSave = true;
    QStringList m_closedFiles;      // 已关闭文件栈(Ctrl+Shift+T 重开)
};
