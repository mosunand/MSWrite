#include "mainwindow.h"

#include "webviewhost.h"
#include "bridge.h"
#include "fileservice.h"
#include "outlinedock.h"
#include "searchdock.h"
#include "quickopendialog.h"
#include "pdfview.h"
#include "uidialogs.h"
#include "ai/AiChatDock.h"
#include "ai/AiConfigDialog.h"
#include "ai/AiDoctor.h"

#include <QActionGroup>
#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QDateTime>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHash>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QColor>
#include <QPainter>
#include <QPixmap>
#include <QCursor>
#include <QInputDialog>
#include <QProcess>
#include <QPointer>
#include <QRegularExpression>
#include <QSettings>
#include <QSaveFile>
#include <QScopeGuard>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyleHints>
#include <QTabBar>
#include <QToolButton>
#include <QTextStream>
#include <QTime>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>

namespace {
constexpr const char *kVirtualHost = "app.local";
constexpr int kExportHtml = 0, kExportPdf = 1, kExportWord = 2;
// 退出前等待落盘的上限(超时只提示,不再静默退出)
constexpr int kSaveWaitMs = 20000;

QString pageUrl()
{
    // ?v= 与 bridge.js 版本同步递增:editor.html 本体也绕过缓存
    QString url = QStringLiteral("https://") + QLatin1String(kVirtualHost)
                + QStringLiteral("/editor.html?v=139");
    // 开发态才把排障开关传给页面(按键记录器等),生产环境不启用
    if (qEnvironmentVariableIsSet("MSWRITE_DEV"))
        url += QStringLiteral("&dev=1");
    return url;
}

// 页面资源目录:优先 exe 旁的 resources(发布形态),回退源码树(开发形态)
QString resourcesWebDir()
{
    const QString exeDir = QCoreApplication::applicationDirPath();
    const QString beside = exeDir + QStringLiteral("/resources/web");
    if (QFileInfo::exists(beside + QStringLiteral("/editor.html")))
        return QDir(beside).absolutePath();
#ifdef MSWRITE_SOURCE_DIR
    const QString dev = QStringLiteral(MSWRITE_SOURCE_DIR) + QStringLiteral("/resources/web");
    if (QFileInfo::exists(dev + QStringLiteral("/editor.html")))
        return QDir(dev).absolutePath();
#endif
    return beside;
}

QString readAll(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}

QString katexExportCss(const QString &webDir)
{
    // KaTeX 字体随工程分发,永不变更:内联结果按 webDir 缓存,
    // 避免每次导出都重读全部字体文件并 base64(数百 KB IO+编码)
    static QHash<QString, QString> cache;
    const auto hit = cache.find(webDir);
    if (hit != cache.end())
        return *hit;
    const QDir katexDir(webDir + QStringLiteral("/vditor/dist/js/katex"));
    QString css = readAll(katexDir.filePath(QStringLiteral("katex.min.css")));
    static const QRegularExpression urls(QStringLiteral(R"(url\(([^)]+)\))"));
    auto matches = urls.globalMatch(css);
    QString result;
    qsizetype offset = 0;
    while (matches.hasNext()) {
        const auto match = matches.next();
        QString name = match.captured(1);
        name.remove(QLatin1Char('"'));
        name.remove(QLatin1Char('\''));
        QFile font(katexDir.filePath(name));
        if (!font.open(QIODevice::ReadOnly))
            continue;
        const QString suffix = QFileInfo(name).suffix();
        const QString mime = suffix == QLatin1String("ttf") ? QStringLiteral("font/ttf")
                           : suffix == QLatin1String("woff") ? QStringLiteral("font/woff")
                                                             : QStringLiteral("font/woff2");
        result += css.mid(offset, match.capturedStart() - offset);
        result += QStringLiteral("url(data:") + mime + QStringLiteral(";base64,")
                + QString::fromLatin1(font.readAll().toBase64()) + QLatin1Char(')');
        offset = match.capturedEnd();
    }
    result += css.mid(offset);
    cache.insert(webDir, result);
    return result;
}

// MIME(或裸扩展名)-> 落盘扩展名:截图/拖入的 jpg、webp 不再一律存成 .png
QString imageExtForMime(const QString &mime)
{
    const QString m = mime.toLower();
    if (m.contains(QLatin1String("jpeg")) || m.contains(QLatin1String("jpg")))
        return QStringLiteral("jpg");
    if (m.contains(QLatin1String("gif")))  return QStringLiteral("gif");
    if (m.contains(QLatin1String("webp"))) return QStringLiteral("webp");
    if (m.contains(QLatin1String("bmp")))  return QStringLiteral("bmp");
    if (m.contains(QLatin1String("svg")))  return QStringLiteral("svg");
    if (m.contains(QLatin1String("avif"))) return QStringLiteral("avif");
    return QStringLiteral("png");
}

// ExecuteScript 的结果是 JSON 序列化文本:字符串带引号与转义,包一层数组解析
QString unquoteJsonString(const QString &v)
{
    QString s = v.trimmed();
    if (s.size() >= 2 && s.startsWith(QLatin1Char('"')) && s.endsWith(QLatin1Char('"'))) {
        const QJsonDocument d = QJsonDocument::fromJson(
            QStringLiteral("[%1]").arg(s).toUtf8());
        if (d.isArray() && d.array().size() == 1)
            return d.array().at(0).toString();
    }
    return v;
}

} // namespace

MainWindow::MainWindow(QWidget *parent, const QString &initialPath)
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("Mswrite"));

    m_files = new FileService(this);
    // Resolve appearance before constructing any controls or starting the editor.
    const QString savedTheme = m_files->lastTheme();
    m_theme = !savedTheme.isEmpty() ? savedTheme
        : (QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark
            ? QStringLiteral("dark") : QStringLiteral("light"));
    m_fontSize = qBound(12, QSettings().value(QStringLiteral("fontSize"), 16).toInt(), 28);
    m_lineNumbers = QSettings().value(QStringLiteral("codeLineNumbers"), false).toBool();
    m_autoSave = QSettings().value(QStringLiteral("autoSave"), true).toBool();
    m_aiStore = AiProviderStore::load(); // AI 助手的供应商(面板懒创建)

    // ---------- 标签栏 + 编辑区栈 ----------
    m_tabbar = new QTabBar(this);
    m_tabbar->setTabsClosable(true);
    m_tabbar->setMovable(true);
    m_tabbar->setExpanding(false);
    m_tabbar->setDrawBase(false);
    m_tabbar->setUsesScrollButtons(true);
    m_tabbar->setElideMode(Qt::ElideMiddle);
    connect(m_tabbar, &QTabBar::currentChanged, this, &MainWindow::switchTab);
    connect(m_tabbar, &QTabBar::tabCloseRequested, this, &MainWindow::closeTab);
    // 可拖动标签必须同步 m_tabs / 编辑区栈,否则拖完后索引对不上:
    // 关 A 实际关掉 B、保存写到另一份文档。tabMoved 先于 currentChanged。
    connect(m_tabbar, &QTabBar::tabMoved, this, [this](int from, int to) {
        if (from < 0 || to < 0 || from == to)
            return;
        if (from >= m_tabs.size() || to >= m_tabs.size())
            return;
        m_tabs.move(from, to);
        QWidget *w = m_stack->widget(from);
        if (!w)
            return;
        m_stack->removeWidget(w);
        m_stack->insertWidget(to, w);
    });

    m_stack = new QStackedWidget(this);

    auto *central = new QWidget(this);
    auto *centralLay = new QVBoxLayout(central);
    centralLay->setContentsMargins(0, 0, 0, 0);
    centralLay->setSpacing(0);
    centralLay->addWidget(m_tabbar);
    centralLay->addWidget(m_stack, 1);
    setCentralWidget(central);

    // ---------- 侧栏 ----------
    m_outline = new OutlineDock(this);
    addDockWidget(Qt::LeftDockWidgetArea, m_outline);
    m_search = new SearchDock(this);
    addDockWidget(Qt::BottomDockWidgetArea, m_search);
    m_search->hide();
    // 后台搜索结果回传
    connect(&m_searchWatcher, &QFutureWatcher<QVector<QPair<QString, QString>>>::finished,
            this, [this] {
                m_search->showWorkspaceResults(m_searchWatcher.result());
            });

    connect(m_outline, &OutlineDock::gotoRequested, this, &MainWindow::onOutlineGoto);
    connect(m_search, &SearchDock::searchRequested, this, &MainWindow::onSearchRequested);
    connect(m_search, &SearchDock::fileResultActivated, this, &MainWindow::onSearchFileResult);
    connect(m_search, &SearchDock::docResultActivated, this, &MainWindow::onSearchDocResult);

    buildMenus();
    buildStatusBar();

    // ---------- 窗口几何恢复 ----------
    QSettings geo;
    restoreGeometry(geo.value(QStringLiteral("geometry")).toByteArray());
    restoreState(geo.value(QStringLiteral("windowState")).toByteArray());

    // 恢复工作区(供搜索范围与快速打开使用)
    const QString ws = m_files->lastWorkspace();
    if (!ws.isEmpty() && QFileInfo(ws).isDir())
        m_workspace = ws;

    // ---------- 首个标签 ----------
    m_imgDirs.append(imagePoolDir()); // 中央图片池(用户可在菜单中更改)
    // An explicit file replaces session restoration, avoiding a second browser tab.
    const QString last = initialPath.isEmpty() ? m_files->lastOpenedFile()
                                              : QFileInfo(initialPath).absoluteFilePath();
    if (!last.isEmpty() && QFileInfo::exists(last)) {
        if (QFileInfo(last).suffix().compare(QLatin1String("pdf"), Qt::CaseInsensitive) == 0) {
            if (addPdfTab(last) < 0) addTab(QString(), welcomeContent());
        } else {
            bool ok = false;
            FileService::Encoding enc = FileService::Encoding::Utf8;
            bool crlf = false;
            const QString content = FileService::readFile(last, &ok, &enc, &crlf);
            if (ok) {
                addTab(last, content, enc, crlf);
                m_files->pushRecentFile(last);
                m_files->setLastOpenedFile(last);
            } else {
                addTab(QString(), welcomeContent());
            }
        }
    } else {
        addTab(QString(), welcomeContent());
    }
    updateTitle();

    // 运行时看门狗:WebView2 缺失 / 环境创建失败时给出明确提示,
    // 否则用户看到的只是一个永远空白的编辑区(旧实现只写日志)
    QTimer::singleShot(12000, this, [this] {
        if (m_shuttingDown || m_runtimeWarned || m_tabs.isEmpty())
            return;
        for (const Tab &t : m_tabs) {
            if (t.pdf || (t.host && t.host->isPageReady()))
                return;
        }
        m_runtimeWarned = true;
        const QString detail = WebViewHost::environmentError();
        qCritical() << "Mswrite: 编辑器内核未就绪" << detail;
        QMessageBox::warning(this, tr("编辑器内核未能启动"),
            tr("WebView2 内核没有就绪,编辑区无法使用。\n\n%1\n\n"
               "请确认系统已安装 Microsoft Edge WebView2 Runtime"
               "(Windows 10/11 通常自带),并检查程序目录下的 WebView2Loader.dll 是否完整。")
                .arg(detail.isEmpty() ? tr("原因:等待超时") : tr("原因:%1").arg(detail)));
    });
}

MainWindow::~MainWindow()
{
    delete m_aiDock;
}

QString MainWindow::welcomeContent() const
{
    return readAll(QStringLiteral(":/welcome.md"));
}

// 中央图片库:与 Typora 默认行为一致,且允许用户自选目录
// (设置键 imgDir;历史用过的目录保留映射,旧文档图片不受换库影响)
QString MainWindow::imagePoolDir() const
{
    // 用户指定过的优先;否则跟随文档保存目录(默认 exe/MSWriteData)
    QString dir = QSettings().value(QStringLiteral("imgDir")).toString();
    if (!dir.isEmpty() && QDir().mkpath(dir))
        return QDir::fromNativeSeparators(dir);
    dir = m_files->defaultSaveDir() + QStringLiteral("/images");
    QDir().mkpath(dir);
    return dir;
}

// 为所有图片池目录建立虚拟主机映射,并同步给每个编辑页
void MainWindow::syncImgMaps()
{
    for (const QString &dir : m_imgDirs) {
        bool found = false;
        for (const ImgMap &m : m_imgMaps) {
            if (m.dir == dir) { found = true; break; }
        }
        if (!found)
            m_imgMaps.append({ QStringLiteral("imgpool%1.local").arg(++m_imgHostSeq), dir });
    }

    QJsonArray arr;
    for (const ImgMap &m : m_imgMaps)
        arr.append(QJsonObject{ { "host", m.host }, { "prefix", m.dir } });
    const QString js = Bridge::call(QStringLiteral("setImgDirs"), { arr });

    for (const Tab &t : m_tabs) {
        if (!t.host) continue;
        for (const ImgMap &m : m_imgMaps)
            t.host->addHostMapping(m.host, m.dir);
        t.host->runScript(js);
    }
}

// 保存粘贴图片:统一入池,返回相对路径
QString MainWindow::saveImageAsset(const Tab &tab, const QString &base64, const QString &mime,
                                   QString *error)
{
    if (base64.isEmpty()) {
        *error = QStringLiteral("空数据");
        return {};
    }
    // 图片存文档同级 assets,插入相对路径(可移植,用户指定模式)
    QString baseDir;
    if (!tab.path.isEmpty())
        baseDir = QFileInfo(tab.path).absolutePath();
    else if (!m_workspace.isEmpty())
        baseDir = m_workspace;
    else {
        *error = QStringLiteral("请先保存文档(图片将存到文档旁 assets 目录)");
        return {};
    }

    const QDir assetsDir(baseDir + QStringLiteral("/assets"));
    if (!assetsDir.exists() && !QDir().mkpath(assetsDir.absolutePath())) {
        *error = QStringLiteral("无法创建 assets 目录");
        return {};
    }

    const QString stamp = QDateTime::currentDateTime()
                              .toString(QStringLiteral("yyyyMMddHHmmsszzz"));
    const QString ext = imageExtForMime(mime);
    QString name = QStringLiteral("image-%1.%2").arg(stamp, ext);
    for (int i = 0; assetsDir.exists(name); ++i)
        name = QStringLiteral("image-%1-%2.%3").arg(stamp).arg(i).arg(ext);

    const QByteArray bytes = QByteArray::fromBase64(base64.toLatin1());
    QFile f(assetsDir.filePath(name));
    if (!f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size()) {
        *error = QStringLiteral("写入图片失败");
        return {};
    }
    return QStringLiteral("./assets/") + name;
}

// ---------------------------------------------------------------------------
// 标签管理
// ---------------------------------------------------------------------------

int MainWindow::addTab(const QString &path, const QString &content,
                       FileService::Encoding enc, bool crlf)
{
    Tab t;
    t.path = path;
    t.enc = enc;
    t.crlf = crlf;
    t.host = new WebViewHost(this);
    t.autoSave = new QTimer(t.host);
    t.autoSave->setSingleShot(true);
    // 打开既有文档:标题从内容直接算(页面编辑中再由 firstLine 消息维护)
    t.titleHint = FileService::titleFromMarkdown(content);
    connect(t.autoSave, &QTimer::timeout, this, [this,host=t.host] {
        auto *tab=tabForHost(host);
        if(tab && tab->dirty && m_autoSave && !tab->choosingSavePath)
            requestContent(*tab);
    });
    t.host->showStartupPreview(content, m_theme, m_fontSize);
    const int idx = m_stack->addWidget(t.host);
    Q_UNUSED(idx);

    connect(t.host, &WebViewHost::message, this,
            [this, host = t.host](const QJsonObject &o) { onWebMessage(host, o); }, Qt::QueuedConnection);

    const QString exeDir = QCoreApplication::applicationDirPath();
    const QJsonObject initial{{"content", content}, {"theme", m_theme},
        {"fontSize", m_fontSize}, {"lineNumbers", m_lineNumbers},
        {"preferLatex",QSettings().value(QStringLiteral("preferLatex"),false).toBool()}};
    const QString bootstrap = QStringLiteral("if(location.origin==='https://app.local'){window.msInitialState=%1;}")
        .arg(QString::fromUtf8(QJsonDocument(initial).toJson(QJsonDocument::Compact)));
    t.host->start(exeDir + QStringLiteral("/webview-data"),
                  QString::fromLatin1(kVirtualHost),
                  resourcesWebDir(),
                  pageUrl(), bootstrap);
    t.host->setAcceleratorFilter([this, host = QPointer<WebViewHost>(t.host)](int vk, bool ctrl, bool shift, bool alt) {
        if (!ctrl) return false;
        const bool handled = alt ? (vk == 0xBB || vk == 0xBD || vk == '0')
            : shift ? QStringLiteral("NSOTFEPW12").contains(QChar(vk))
            : (QStringLiteral("SOPNWQF0").contains(QChar(vk))
               || vk == 0xBF || vk == 0xBB || vk == 0xBD || vk == 0xBC);
        if (!handled) return false;
        // WebView2 accelerator callbacks are synchronous. Opening a modal
        // dialog or closing its controller here can deadlock/reenter COM.
        QTimer::singleShot(0, this, [this, host, vk, ctrl, shift, alt] {
            if (host && currentTab() && currentTab()->host == host)
                handleAccelerator(vk, ctrl, shift, alt);
        });
        return true;
    });
    // 恢复该文档上次使用的整页缩放(按路径持久化)
    t.zoom = QSettings().value(QStringLiteral("zoom/") + path, 1.0).toDouble();
    if (!qFuzzyCompare(t.zoom, 1.0))
        t.host->setZoomFactor(t.zoom);
    // 本文档默认代码语言(按路径持久化,标签页各自独立)
    t.docLang = QSettings().value(QStringLiteral("doclang/") + path).toString();

    // 文档目录虚拟主机:页面内 ./assets/* 图片经它读取
    applyDocDir(t);

    const int tabIndex = m_tabs.size();
    m_tabs.append(t);

    const QString name = path.isEmpty() ? tr("未命名")
                                        : QFileInfo(path).fileName();
    m_tabbar->addTab(name);
    m_tabbar->setTabToolTip(tabIndex, QDir::toNativeSeparators(path));
    attachTabCloseButton(tabIndex);
    m_tabbar->setCurrentIndex(tabIndex);
    m_stack->setCurrentWidget(t.host);

    // 图片池映射:必须在 append 之后,新 host 才能收到(否则新标签显示不了池内图片)
    syncImgMaps();

    // 内容下发(runScript 在 C++ 与 JS 两级都有就绪门,乱序安全)
    // Only used if the runtime could not register the initial script.
    t.host->runScript(QStringLiteral("if(!window.msInitialState){%1%2%3%4}")
        .arg(Bridge::call(QStringLiteral("setContent"), { content }),
             Bridge::call(QStringLiteral("setTheme"), { m_theme }),
             Bridge::call(QStringLiteral("setFontSize"), { m_fontSize }),
             Bridge::call(QStringLiteral("setLineNumbers"), { m_lineNumbers })));
    if (!t.docLang.isEmpty())
        t.host->runScript(Bridge::call(QStringLiteral("setDocLang"), { t.docLang }));
    if (m_focusMode)
        t.host->runScript(Bridge::call(QStringLiteral("setFocusMode"), { true }));
    if (m_typewriter)
        t.host->runScript(Bridge::call(QStringLiteral("setTypewriter"), { true }));
    return tabIndex;
}

int MainWindow::findTabByPath(const QString &path) const
{
    if (path.isEmpty())
        return -1;
    const QString canon = QFileInfo(path).absoluteFilePath();
    for (int i = 0; i < m_tabs.size(); ++i) {
        if (!m_tabs[i].path.isEmpty()
            && QFileInfo(m_tabs[i].path).absoluteFilePath().compare(canon, Qt::CaseInsensitive) == 0)
            return i;
    }
    return -1;
}

int MainWindow::currentTabIndex() const
{
    return m_tabbar->currentIndex();
}

MainWindow::Tab *MainWindow::currentTab()
{
    const int i = currentTabIndex();
    return (i >= 0 && i < m_tabs.size()) ? &m_tabs[i] : nullptr;
}

// 未命名文档落盘路径:标题(已清洗)直接作名,冲突加 -2/-3 序号;
// 没有可用标题(空文档/纯代码围栏开头)回退时间戳+短 UUID,保持唯一
QString MainWindow::uniqueUntitledPath(const QString &title) const
{
    const QString dir = m_files->defaultSaveDir();
    QString name = FileService::sanitizeFileName(title);
    if (name.isEmpty())
        name = QStringLiteral("未命名-%1-%2")
                   .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")),
                        QUuid::createUuid().toString(QUuid::Id128).left(8));
    QString path = dir + QLatin1Char('/') + name + QStringLiteral(".md");
    for (int i = 2; QFileInfo::exists(path); ++i)
        path = dir + QLatin1Char('/') + name + QLatin1Char('-')
             + QString::number(i) + QStringLiteral(".md");
    return path;
}

// 另存为/导出对话框的默认名:titleHint 平时由页面 firstLine 消息维护,
// 但它有 300ms 防抖 —— 打完字立刻保存时可能还没到。这里同步向页面
// 要一次文档首行(mswFirstLine),保证对话框默认名总是正确的。
// 立即回传时事件循环零等待;页面无响应时 800ms 兜底放弃。
void MainWindow::refreshTitleHint(Tab &tab)
{
    if (!tab.host || !tab.host->isPageReady())
        return;
    const QPointer<WebViewHost> host(tab.host);
    QEventLoop loop;
    QTimer::singleShot(800, &loop, &QEventLoop::quit);
    host->evalWithResult(
        QStringLiteral("window.mswTitleSource ? window.mswTitleSource() : null"),
        [guard = QPointer<QEventLoop>(&loop)](const QString &v) {
            if (!guard) return; // A late response must never touch a dead stack.
            guard->setProperty("result", v);
            guard->quit();
        });
    if (!loop.property("result").isValid())
        loop.exec(QEventLoop::ExcludeUserInputEvents);
    const QString raw = loop.property("result").toString();
    // The nested loop can close/reorder tabs. Re-resolve by host identity.
    if (host && raw.startsWith(QLatin1Char('"')))
        if (auto *liveTab = tabForHost(host))
            liveTab->titleHint = FileService::titleFromMarkdown(unquoteJsonString(raw));
}

MainWindow::Tab *MainWindow::tabForHost(WebViewHost *host)
{
    for (Tab &t : m_tabs)
        if (t.host == host)
            return &t;
    return nullptr;
}

int MainWindow::indexOfHost(WebViewHost *host) const
{
    for (int i = 0; i < m_tabs.size(); ++i)
        if (m_tabs[i].host == host)
            return i;
    return -1;
}

bool MainWindow::openPath(const QString &path)
{
    if (path.isEmpty())
        return false;
    const int existing = findTabByPath(path);
    if (existing >= 0) {
        m_tabbar->setCurrentIndex(existing);
        return true;
    }
    // PDF 文件走独立阅读器(非 WebView 编辑)
    const QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix == QLatin1String("pdf")) {
        const int idx = addPdfTab(path);
        return idx >= 0;
    }
    bool ok = false;
    FileService::Encoding enc = FileService::Encoding::Utf8;
    bool crlf = false;
    const QString content = FileService::readFile(path, &ok, &enc, &crlf);
    if (!ok) {
        QMessageBox::warning(this, tr("打开失败"), tr("无法读取文件:\n%1").arg(path));
        return false;
    }
    addTab(path, content, enc, crlf);
    m_files->pushRecentFile(path);
    m_files->setLastOpenedFile(path);
    statusBar()->showMessage(tr("已打开 %1").arg(QDir::toNativeSeparators(path)), 4000);
    return true;
}

// PDF 标签页:独立阅读器(WebViewHost 互斥)
int MainWindow::addPdfTab(const QString &path)
{
    auto *view = new PdfViewWidget(this);
    view->setTheme(m_theme);
    if (!view->load(path)) {
        delete view;
        QMessageBox::warning(this, tr("打开失败"),
            tr("无法读取 PDF 文件:\n%1").arg(path));
        return -1;
    }
    Tab t;
    t.pdf = view;
    t.path = path;
    const int idx = m_stack->addWidget(view);

    connect(view, &PdfViewWidget::pageChanged, this,
            [this, view](int page, int total) {
        // 状态栏联动(仅当该标签是当前标签)
        const int cur = currentTabIndex();
        if (cur >= 0 && cur < m_tabs.size() && m_tabs[cur].pdf == view) {
            m_statsLabel->setText(tr("第 %1 / %2 页").arg(page + 1).arg(total));
        }
    });
    connect(view, &PdfViewWidget::zoomChanged, this, [this, view](double factor) {
        const auto *tab = currentTab();
        if (tab && tab->pdf == view) updateZoomLabel(factor);
    });
    connect(view,&PdfViewWidget::aiSelectionRequested,this,[this,view,path](const QString &text,int page,bool translate) {
        if(!currentTab() || currentTab()->pdf!=view) return;
        ensureAiDock();
        m_aiDock->requestSelection(text,QFileInfo(path).fileName(),page,translate);
    });

    const int tabIndex = m_tabs.size();
    m_tabs.append(t);
    const QString name = QFileInfo(path).fileName();
    m_tabbar->addTab(name);
    m_tabbar->setTabToolTip(tabIndex, QDir::toNativeSeparators(path));
    attachTabCloseButton(tabIndex);
    m_tabbar->setCurrentIndex(tabIndex);
    m_stack->setCurrentWidget(view);

    m_files->pushRecentFile(path);
    m_files->setLastOpenedFile(path);
    statusBar()->showMessage(tr("已打开 %1").arg(QDir::toNativeSeparators(path)), 4000);
    return tabIndex;
}

void MainWindow::newTab()
{
    addTab(QString(), QString());
}

void MainWindow::closeTab(int index)
{
    if (m_flushingSaves || index < 0 || index >= m_tabs.size())
        return;
    if (m_tabs[index].choosingSavePath) return;
    WebViewHost *host=m_tabs[index].host;
    if(host && !m_flushingSaves && m_tabs[index].dirty && m_autoSave) {
        flushSaves({host});
        index=indexOfHost(host);
        if(index<0)return;
    }
    if (m_tabs[index].dirty) {
        if(m_tabs[index].autoSave)m_tabs[index].autoSave->stop();
        const auto ret = UiDialogs::confirmSave(this,m_theme,
            {m_tabs[index].path.isEmpty() ? tr("未命名文档") : QFileInfo(m_tabs[index].path).fileName()},false);
        index = indexOfHost(host);
        if (index < 0) return;
        if (ret == UiDialogs::SaveChoice::Cancel) {
            if (m_autoSave && m_tabs[index].dirty) m_tabs[index].autoSave->start(2000);
            return;
        }
        if (ret == UiDialogs::SaveChoice::Save) {
            // 先立"存完即关"的意图,再走保存:未命名文档要经另存为,
            // 期间内容回传时靠 closeAfterSave 才能真正把标签关掉
            m_tabs[index].closeAfterSave = true;
            if (m_tabs[index].path.isEmpty()) {
                // 未命名文档:先另存为,成功后再走保存关闭流程
                const QString activeId = currentTab() ? currentTab()->aiId : QString();
                m_tabbar->setCurrentIndex(index);
                const bool okAs = saveFileAs();
                for (int i = 0; i < m_tabs.size(); ++i)
                    if (m_tabs[i].aiId == activeId) { m_tabbar->setCurrentIndex(i); break; }
                if (!okAs) {
                    if (auto *live = tabForHost(host)) live->closeAfterSave = false;
                }
                return;
            }
            requestContent(m_tabs[index]);
            return; // 等保存回传后真正关闭
        }
    }
    Tab &t = m_tabs[index];
    if (t.autoSave) t.autoSave->stop();
    if (m_exportSource == host && m_exportKind >= 0) resetExport();
    // 立即关闭(记录到重开栈)
    if (!t.path.isEmpty())
        m_closedFiles.append(t.path);
    QWidget *page = t.pdf ? static_cast<QWidget *>(t.pdf)
                          : static_cast<QWidget *>(t.host);
    m_tabs.removeAt(index);
    m_tabbar->removeTab(index);
    m_stack->removeWidget(page);
    page->deleteLater();

    if (m_tabs.isEmpty() && !m_shuttingDown) {
        addTab(QString(), QString()); // 至少保留一个标签
    } else if (currentTabIndex() >= m_tabs.size()) {
        m_tabbar->setCurrentIndex(m_tabs.size() - 1);
    }
    updateTitle();
    // 关闭的是当前标签时,状态栏还显示着被关文档的统计/缩放,切到
    // 相邻标签后要同步一次(currentChanged 不会为此再触发)
    syncStatusFromTab();
}

void MainWindow::switchTab(int index)
{
    if (index < 0 || index >= m_tabs.size())
        return;
    const Tab &t = m_tabs[index];
    if (t.pdf) {
        if (!m_pdfActive) m_outlineBeforePdf = m_outline->isVisible();
        m_pdfActive = true;
        m_outline->hide();
        m_stack->setCurrentWidget(t.pdf);
        t.pdf->setFocus();
        updateTitle();
        // PDF 标签状态栏显示页码
        m_statsLabel->setText(tr("第 %1 / %2 页")
                                  .arg(t.pdf->currentPage() + 1)
                                  .arg(t.pdf->pageCount()));
        m_modeLabel->setText(tr("PDF 阅读"));
        updateSaveIndicator(false);
        return;
    }
    if (m_pdfActive) m_outline->setVisible(m_outlineBeforePdf);
    m_pdfActive = false;
    m_stack->setCurrentWidget(t.host);
    updateTitle();
    syncStatusFromTab();
    // 请求该文档重推统计与大纲(否则侧栏还停留在上一个标签的内容)
    m_tabs[index].host->runScript(QStringLiteral("window.msbridge.refresh()"));
    m_tabs[index].host->runScript(QStringLiteral("window.msbridge.focus()"));
}

// Neutral close buttons stay readable in all themes.
void MainWindow::attachTabCloseButton(int index)
{
    auto *btn = new QToolButton(m_tabbar);
    btn->setObjectName(QStringLiteral("tabClose"));
    btn->setFixedSize(22, 22);
    btn->setCursor(Qt::PointingHandCursor);
    btn->setToolTip(tr("关闭标签页 (Ctrl+W)"));
    btn->setAccessibleName(tr("关闭标签页"));
    btn->setText(QStringLiteral("×"));
    btn->setAutoRaise(true);

    // 点击关闭:按钮在标签里的位置可能因拖动变化,实时取
    connect(btn, &QToolButton::clicked, this, [this, btn] {
        for (int i = 0; i < m_tabbar->count(); ++i) {
            if (m_tabbar->tabButton(i, QTabBar::RightSide) == btn) {
                closeTab(i);
                return;
            }
        }
    });

    m_tabbar->setTabButton(index, QTabBar::RightSide, btn);
}

void MainWindow::applyDocDir(Tab &tab)
{
    // 每次生成新主机名再映射:避免"变更既有映射对当前页可能不生效"的限制
    tab.docHost = QStringLiteral("doc%1.local").arg(++m_docHostSeq);
    tab.docDir = tab.path.isEmpty()
                     ? QString()
                     : QFileInfo(tab.path).absolutePath();
    if (!tab.docDir.isEmpty())
        tab.host->addHostMapping(tab.docHost, tab.docDir);
    tab.host->runScript(Bridge::call(QStringLiteral("setDocHost"), { tab.docHost }));
}

// 还原:JS 显示层会把 C:\... 改写成 https://imgpoolN.local/...,
// Vditor 内部随之被污染;落盘/导出前统一换回绝对路径
QString MainWindow::restoreImagePaths(const Tab &tab, QString md) const
{
    // 显示层把 ./assets/x.png 改写成 https://docN.local/assets/x.png,
    // 落盘时还原回 ./ 相对路径(保持文档可移植)
    if (!tab.docHost.isEmpty())
        md.replace(QStringLiteral("https://") + tab.docHost + QLatin1Char('/'),
                   QStringLiteral("./"));
    // 历史遗留的中央池绝对路径图片:还原为原生绝对路径
    for (const ImgMap &m : m_imgMaps) {
        const QString nativePrefix = QDir::toNativeSeparators(m.dir) + QChar(u'\\');
        md.replace(QStringLiteral("https://") + m.host + QLatin1Char('/'), nativePrefix);
    }
    return md;
}

// 标签文字(含 ● 脏标记):所有改标签名的路径都走这里,
// 避免"另存为"把 ● 抹掉(旧实现直接 setTabText 会丢标记)
void MainWindow::updateTabText(int index)
{
    if (index < 0 || index >= m_tabs.size() || index >= m_tabbar->count())
        return;
    const Tab &t = m_tabs[index];
    const QString name = t.path.isEmpty() ? tr("未命名")
                                         : QFileInfo(t.path).fileName();
    m_tabbar->setTabText(index, t.dirty ? QStringLiteral("● ") + name : name);
}

void MainWindow::markDirty(int index, bool dirty)
{
    if (index < 0 || index >= m_tabs.size())
        return;
    if (m_tabs[index].dirty == dirty) {
        if(index==currentTabIndex())updateSaveIndicator(dirty);
        return;
    }
    m_tabs[index].dirty = dirty;
    updateTabText(index);   // 标签文字同步显示/隐藏 ●(与窗口标题一致)
    if (index == currentTabIndex()) {
        updateTitle();
        updateSaveIndicator(dirty);   // 状态栏:未保存红 / 已保存白
    }
}


void MainWindow::updateTitle()
{
    const int i = currentTabIndex();
    if (i < 0) {
        setWindowTitle(QStringLiteral("Mswrite"));
        return;
    }
    const Tab &t = m_tabs[i];
    const QString name = t.path.isEmpty() ? tr("未命名")
                                         : QFileInfo(t.path).fileName();
    setWindowTitle(QStringLiteral("%1%2 — Mswrite")
                       .arg(name, t.dirty ? QStringLiteral(" ●") : QString()));
}

void MainWindow::syncStatusFromTab()
{
    const Tab *t = currentTab();
    if (!t)
        return;
    if (t->pdf) {
        // PDF 标签:状态栏显示页码,不是文字统计
        m_statsLabel->setText(tr("第 %1 / %2 页")
                                  .arg(t->pdf->currentPage() + 1)
                                  .arg(t->pdf->pageCount()));
        m_modeLabel->setText(tr("PDF 阅读"));
        return;
    }
    m_statsLabel->setText(t->stats.isEmpty() ? QStringLiteral("    ") : t->stats);
    m_modeLabel->setText(t->mode == QLatin1String("sv") ? tr("源码模式")
                                                        : tr("所见即所得"));
    updateSaveIndicator(t->dirty);
    updateZoomLabel(t->zoom);   // 切标签:缩放胶囊跟随该标签
}

// 状态栏保存指示:已保存=白色加粗 / 未保存=红色加粗
// 状态栏缩放胶囊:显示当前标签的缩放百分比;100% 灰色低调,非 100% 蓝色
void MainWindow::updateZoomLabel(double factor)
{
    if (!m_zoomLabel)
        return;
    const int pct = qRound(factor * 100);
    m_zoomLabel->setText(QStringLiteral("%1%").arg(pct));
    m_zoomLabel->setStyleSheet(QStringLiteral(
        "padding:2px 9px; margin-right:4px; border-radius:5px; color:%1; background:%2;")
        .arg(m_theme == QLatin1String("dark") ? "#a3adbd" : "#52637a",
             qFuzzyCompare(factor, 1.0) ? "transparent" : m_theme == QLatin1String("dark") ? "#253e5e" : "#e1edff"));
    m_zoomLabel->setToolTip(tr("页面缩放 %1%(Ctrl+滚轮调节,点击回到 100%)").arg(pct));
}

// Keep saved state quiet; unsaved content remains clearly marked in both themes.
void MainWindow::updateSaveIndicator(bool dirty)
{
    if (!m_saveLabel)
        return;
    const Tab *tab=currentTab();
    const QString text = tab && tab->pdf ? tr("PDF 只读")
        : tab && !tab->saveError.isEmpty() ? tr("保存失败 · 点击重试")
        : tab && tab->awaitingContent ? tr("正在保存…")
        : dirty ? (m_autoSave ? tr("等待自动保存…") : tr("未保存 · 自动保存已关闭"))
        : tab && tab->path.isEmpty() ? tr("新文档") : tr("✓ 已保存");
    m_saveLabel->setText(text);
    m_saveLabel->setToolTip(tab && !tab->saveError.isEmpty() ? tab->saveError
        : tab && !tab->path.isEmpty() ? tr("保存位置：%1").arg(QDir::toNativeSeparators(tab->path))
        : tr("新文档停笔 2 秒后自动保存到：%1").arg(QDir::toNativeSeparators(m_files->defaultSaveDir())));
    m_saveLabel->setStyleSheet(QStringLiteral(
        "padding:2px 9px; margin-right:4px; border-radius:5px; color:%1; background:%2;")
        .arg(dirty ? (m_theme == QLatin1String("dark") ? "#ffd19b" : "#955511")
                   : (m_theme == QLatin1String("dark") ? "#9bb8a9" : "#527465"),
             dirty ? (m_theme == QLatin1String("dark") ? "#503719" : "#fff1dc") : "transparent"));
}

// ---------------------------------------------------------------------------
// 文件操作
// ---------------------------------------------------------------------------

void MainWindow::openFile()
{
    const Tab *t = currentTab();
    const QString dir = (t && !t->path.isEmpty()) ? QFileInfo(t->path).absolutePath()
                                                  : m_files->lastWorkspace();
    const QString path = QFileDialog::getOpenFileName(this, tr("打开"), dir,
        FileService::markdownFilters().join(QStringLiteral(";;")));
    if (!path.isEmpty())
        openPath(path);
}

void MainWindow::openFolder()
{
    const QString dir = QFileDialog::getExistingDirectory(this, tr("打开工作区文件夹"),
                                                           m_files->lastWorkspace());
    if (dir.isEmpty())
        return;
    m_workspace = dir;
    m_files->setLastWorkspace(dir);
    statusBar()->showMessage(
        tr("工作区已设为 %1(Ctrl+F 可全库搜索,Ctrl+P 可快速打开)").arg(dir), 4000);
}

void MainWindow::quickOpen()
{
    QuickOpenDialog dlg(m_files->recentFiles(), m_workspace, this);
    if (dlg.exec() == QDialog::Accepted && !dlg.selectedPath().isEmpty())
        openPath(dlg.selectedPath());
}

bool MainWindow::saveFile()
{
    Tab *t = currentTab();
    if (!t)
        return false;
    if (t->pdf)
        return true; // PDF 是只读的,无保存操作
    if (t->path.isEmpty())
        return saveFileAs();
    if (t->awaitingContent)
        return true; // 已在请求中
    requestContent(*t);
    return true;
}

bool MainWindow::saveFileAs()
{
    Tab *t = currentTab();
    if (!t)
        return false;
    if (t->pdf) {
        const QString sourcePath = t->path;
        const QString dest = QFileDialog::getSaveFileName(this, tr("PDF 另存为"), sourcePath, tr("PDF (*.pdf)"));
        if (dest.isEmpty()) return false;
        if (QFileInfo(dest).absoluteFilePath() == QFileInfo(sourcePath).absoluteFilePath()) return true;
        QFile source(sourcePath);
        QSaveFile target(dest);
        if (!source.open(QIODevice::ReadOnly) || !target.open(QIODevice::WriteOnly)) return false;
        while (!source.atEnd()) {
            const QByteArray data = source.read(1024 * 1024);
            if (source.error() != QFile::NoError || target.write(data) != data.size()) return false;
        }
        return target.commit();
    }
    const QPointer<WebViewHost> selfHost(t->host);
    if (t->choosingSavePath) return false;
    t->choosingSavePath = true;
    const auto restoreAutoSave = qScopeGuard([this, selfHost] {
        if (auto *live = selfHost ? tabForHost(selfHost) : nullptr) {
            live->choosingSavePath = false;
            if (m_autoSave && live->dirty && !live->awaitingContent) live->autoSave->start(2000);
        }
    });
    // Do not let a pending automatic save close this tab or change its path
    // while the Save As dialog is running its nested event loop.
    const bool closeAfterSave = t->closeAfterSave;
    t->closeAfterSave = false;
    t->autoSave->stop();
    if (t->awaitingContent && !flushSaves({selfHost})) return false;
    t = selfHost ? tabForHost(selfHost) : nullptr;
    if (!t) return false;
    QString suggested = t->path;
    if (suggested.isEmpty()) {
        // 新建文档:默认名取文档开头正文;firstLine 消息未到时同步兜底取一次
        // (打完字立刻 Ctrl+S 也能拿到正确默认名)。保存目录默认 exe/MSWriteData
        refreshTitleHint(*t);
        t = selfHost ? tabForHost(selfHost) : nullptr;
        if (!t) return false;
        suggested = m_files->defaultSaveDir() + QLatin1Char('/')
                  + (t->titleHint.isEmpty() ? tr("未命名") : t->titleHint)
                  + QStringLiteral(".md");
    }
    QString path = QFileDialog::getSaveFileName(this, tr("另存为"), suggested,
        tr("Markdown 文档 (*.md *.markdown *.mdown *.txt);;所有文件 (*.*)"));
    t = selfHost ? tabForHost(selfHost) : nullptr;
    if (!t) return false;
    if (path.isEmpty()) {
        if (m_autoSave && t->dirty) t->autoSave->start(2000);
        return false;
    }
    if (QFileInfo(path).suffix().isEmpty())
        path += QStringLiteral(".md");

    // 另存为到"另一个已打开标签"的路径:直接覆盖会让两个标签指向同一文件,
    // 之后两边各自保存互相踩。先关掉那个已开标签。
    int existing = findTabByPath(path);
    if (existing >= 0 && m_tabs[existing].host != selfHost) {
        if (m_tabs[existing].dirty) {
            const auto ret = QMessageBox::question(this, tr("文件已打开"),
                tr("「%1」已在另一个标签中打开且有未保存更改。\n"
                   "关闭那个标签并覆盖吗?").arg(QFileInfo(path).fileName()),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (ret != QMessageBox::Yes)
                return false;
            t = selfHost ? tabForHost(selfHost) : nullptr;
            if (!t) return false;
        }
        // 用户已确认覆盖:不要再走 closeTab 的保存询问,否则弹两次。
        // host 指针先记下 —— closeTab 会挪动 QVector,旧 Tab* 立刻失效。
        existing = findTabByPath(path);
        if (existing >= 0 && m_tabs[existing].host != selfHost) {
            if (m_tabs[existing].choosingSavePath) return false;
            m_tabs[existing].dirty = false;
            m_tabs[existing].awaitingContent = false;
            m_tabs[existing].closeAfterSave = false;
            closeTab(existing);
        }
        t = tabForHost(selfHost);
        if (!t)
            return false;
    }

    const int idx = indexOfHost(t->host);
    t->path = path;
    t->closeAfterSave = closeAfterSave;
    // 未命名时设的文档默认语言随另存为落到新路径
    if (!t->docLang.isEmpty())
        QSettings().setValue(QStringLiteral("doclang/") + path, t->docLang);
    applyDocDir(*t);
    updateTabText(idx);     // 保留 ● 脏标记(此刻尚未落盘)
    m_tabbar->setTabToolTip(idx, QDir::toNativeSeparators(path));
    updateTitle();
    m_files->pushRecentFile(path);
    m_files->setLastOpenedFile(path);
    requestContent(*t);
    return true;
}

void MainWindow::requestContent(Tab &tab)
{
    if (tab.awaitingContent || !tab.host)
        return;
    tab.awaitingContent = true;
    tab.saveError.clear();
    tab.autoSave->stop();
    const int request=++tab.saveRequest;
    tab.host->runScript(QStringLiteral("window.msbridge.requestContent(%1)").arg(request));
    if(currentTab()==&tab)updateSaveIndicator(tab.dirty);
    QTimer::singleShot(8000,this,[this,host=QPointer<WebViewHost>(tab.host),request] {
        auto *tab=host ? tabForHost(host) : nullptr;
        if(!tab || !tab->awaitingContent || tab->saveRequest!=request)return;
        tab->awaitingContent=false;tab->closeAfterSave=false;
        tab->saveError=tr("编辑器未响应，内容尚未保存。请按 Ctrl+S 重试。");
        markDirty(indexOfHost(host),true);
        statusBar()->showMessage(tab->saveError);
    });
}

bool MainWindow::flushSaves(const QVector<WebViewHost *> &hosts)
{
    if(m_flushingSaves)return false;
    m_flushingSaves=true;
    for(auto *host:hosts)if(auto *tab=tabForHost(host)) {
        if(tab->dirty)requestContent(*tab);
    }
    QEventLoop loop;
    QTimer poll;poll.setInterval(25);
    connect(&poll,&QTimer::timeout,&loop,[&] {
        for(auto *host:hosts)if(auto *tab=tabForHost(host);tab && tab->awaitingContent)return;
        loop.quit();
    });
    poll.start();QTimer::singleShot(kSaveWaitMs,&loop,&QEventLoop::quit);
    loop.exec(QEventLoop::ExcludeUserInputEvents);
    m_flushingSaves=false;
    for(auto *host:hosts)if(auto *tab=tabForHost(host);tab && (tab->dirty || tab->awaitingContent))return false;
    return true;
}

void MainWindow::openFileFromArg(const QString &path)
{
    if (!path.isEmpty() && QFileInfo::exists(path))
        openPath(QDir::cleanPath(path));
}

// ---------------------------------------------------------------------------
// 菜单 / 状态栏
// ---------------------------------------------------------------------------

void MainWindow::buildMenus()
{
    QMenu *file = menuBar()->addMenu(tr("文件(&F)"));
    file->addAction(tr("新建标签页(&T)"), QKeySequence(QStringLiteral("Ctrl+N")),
                    this, &MainWindow::newTab);
    file->addAction(tr("新建窗口(&W)..."), QKeySequence(QStringLiteral("Ctrl+Shift+N")),
                    this, &MainWindow::newWindow);
    file->addAction(tr("快速打开(&P)..."), QKeySequence(QStringLiteral("Ctrl+P")),
                    this, &MainWindow::quickOpen);
    file->addAction(tr("打开(&O)..."), QKeySequence::Open, this, &MainWindow::openFile);
    file->addAction(tr("打开文件夹(&W)..."), QKeySequence(QStringLiteral("Ctrl+Shift+O")),
                    this, &MainWindow::openFolder);
    file->addSeparator();
    file->addAction(tr("保存(&S)"), QKeySequence::Save, this, &MainWindow::saveFile);
    file->addAction(tr("另存为(&A)..."), QKeySequence::SaveAs, this, &MainWindow::saveFileAs);
    file->addSeparator();
    QMenu *recent = file->addMenu(tr("最近文件(&R)"));
    connect(recent, &QMenu::aboutToShow, this, [this, recent] {
        recent->clear();
        const QStringList files = m_files->recentFiles();
        if (files.isEmpty()) {
            QAction *empty = recent->addAction(tr("(空)"));
            empty->setEnabled(false);
            return;
        }
        for (const QString &f : files) {
            const QFileInfo fi(f);
            recent->addAction(fi.fileName()
                                  + QStringLiteral("  (")
                                  + QDir::toNativeSeparators(fi.absolutePath())
                                  + QStringLiteral(")"),
                              this, [this, f] { openPath(f); });
        }
    });
    file->addSeparator();
    file->addAction(tr("重新打开关闭的文件(&R)"), QKeySequence(QStringLiteral("Ctrl+Shift+T")),
                    this, &MainWindow::reopenClosedFile);
    file->addSeparator();
    file->addAction(tr("偏好设置(&S)..."), QKeySequence(QStringLiteral("Ctrl+,")),
                    this, &MainWindow::showPrefs);
    file->addSeparator();
    file->addAction(tr("关闭标签页(&C)"), QKeySequence(QStringLiteral("Ctrl+W")), this, [this] {
        closeTab(currentTabIndex());
    });
    file->addAction(tr("退出(&Q)"), QKeySequence(QStringLiteral("Ctrl+Q")), this, &MainWindow::close);

    QMenu *edit = menuBar()->addMenu(tr("编辑(&E)"));
    // 撤销/重做必须走 Vditor 自己的撤销栈:document.execCommand('undo') 在
    // IR(contenteditable)里绕过它、实测静默失败,而真实 Ctrl+Z 由 Vditor
    // 的 keydown 处理器接管(vditor.undo.undo/redo)。菜单要和按键同一条路
    edit->addAction(tr("撤销(&U)"), QKeySequence::Undo, this, [this] {
        Tab *t = currentTab();
        if (t && t->host) t->host->runScript(QStringLiteral(
            "try{window.msbridge.editUndo()}catch(e){document.execCommand('undo')}")); });
    edit->addAction(tr("重做(&R)"), QKeySequence::Redo, this, [this] {
        Tab *t = currentTab();
        if (t && t->host) t->host->runScript(QStringLiteral(
            "try{window.msbridge.editRedo()}catch(e){document.execCommand('redo')}")); });
    edit->addSeparator();
    edit->addAction(tr("剪切(&T)"), QKeySequence::Cut, this, [this] {
        Tab *t = currentTab();
        if (t && t->host) t->host->runScript(QStringLiteral("document.execCommand('cut')")); });
    edit->addAction(tr("复制(&C)"), QKeySequence::Copy, this, [this] {
        Tab *t = currentTab();
        if (t && t->pdf) t->pdf->copySelection();
        else if (t && t->host) t->host->runScript(QStringLiteral("document.execCommand('copy')")); });
    edit->addAction(tr("粘贴(&P)"), QKeySequence::Paste, this, &MainWindow::pasteFromClipboard);
    edit->addSeparator();
    edit->addAction(tr("全选(&A)"), QKeySequence::SelectAll, this, [this] {
        Tab *t = currentTab();
        if (t && t->pdf) t->pdf->selectAll();
        else if (t && t->host) t->host->runScript(QStringLiteral("window.msbridge.selectAll()")); });

    QMenu *view = menuBar()->addMenu(tr("视图(&V)"));
    view->addAction(tr("切换源码模式(&S)"), QKeySequence(QStringLiteral("Ctrl+/")),
                    this, &MainWindow::toggleSourceMode);
    view->addAction(tr("放大页面(&B)"), QKeySequence(QStringLiteral("Ctrl+=")), this, [this] {
        Tab *t = currentTab();
        if (t && t->pdf) t->pdf->zoomIn();
        else if (t) applyZoom(*t, t->zoom + 0.05);
    });
    view->addAction(tr("缩小页面(&X)"), QKeySequence(QStringLiteral("Ctrl+-")), this, [this] {
        Tab *t = currentTab();
        if (t && t->pdf) t->pdf->zoomOut();
        else if (t) applyZoom(*t, t->zoom - 0.05);
    });
    view->addAction(tr("重置页面缩放(&Z)"), QKeySequence(QStringLiteral("Ctrl+0")), this, [this] {
        Tab *t = currentTab();
        if (t && t->pdf) t->pdf->zoomFitWidth();
        else if (t) applyZoom(*t, 1.0);
    });
    view->addSeparator();
    view->addAction(tr("放大字号(&B2)"), QKeySequence(QStringLiteral("Ctrl+Alt+=")), this, [this] { changeFontSize(+1); });
    view->addAction(tr("缩小字号(&S)"), QKeySequence(QStringLiteral("Ctrl+Alt+-")), this, [this] { changeFontSize(-1); });
    view->addAction(tr("重置字号(&R)"), QKeySequence(QStringLiteral("Ctrl+Alt+0")), this, [this] { changeFontSize(0); });
    view->addSeparator();
    QAction *focusAct = view->addAction(tr("专注模式(&F)"));
    focusAct->setCheckable(true);
    QAction *typewriterAct = view->addAction(tr("打字机模式(&W)"));
    typewriterAct->setCheckable(true);
    view->addSeparator();
    view->addAction(tr("大纲(&O)"), this, &MainWindow::toggleOutlinePanel);
    view->addAction(tr("查找(&F)"), QKeySequence(QStringLiteral("Ctrl+F")), this, [this] {
        Tab *t = currentTab();
        if (t && t->pdf) t->pdf->startSearch();
        else if (t && t->host) t->host->runScript(QStringLiteral("window.msbridge.openFind()"));
    });
    view->addAction(tr("全局搜索(&G)"), QKeySequence(QStringLiteral("Ctrl+Shift+F")),
                    this, &MainWindow::globalSearch);
    view->addSeparator();
    view->addAction(tr("大纲面板(&O)"), QKeySequence(QStringLiteral("Ctrl+Shift+1")),
                    this, &MainWindow::toggleOutlinePanel);
    view->addAction(tr("文档列表(&D)"), QKeySequence(QStringLiteral("Ctrl+Shift+2")),
                    this, &MainWindow::quickOpen);
    view->addSeparator();
    view->addAction(tr("全屏(&F)"), QKeySequence(QStringLiteral("F11")),
                    this, &MainWindow::toggleFullscreen);
    view->addAction(tr("开发者工具(&V)"), QKeySequence(QStringLiteral("Shift+F12")),
                    this, &MainWindow::openDevTools);

    QMenu *exportMenu = menuBar()->addMenu(tr("导出(&X)"));
    exportMenu->addAction(tr("HTML(&H)..."), QKeySequence(QStringLiteral("Ctrl+Shift+E")), this, &MainWindow::exportHtml);
    exportMenu->addAction(tr("PDF(&P)..."), QKeySequence(QStringLiteral("Ctrl+Shift+P")), this, &MainWindow::exportPdf);
    exportMenu->addAction(tr("Word 文档(&W)..."), QKeySequence(QStringLiteral("Ctrl+Shift+W")), this, &MainWindow::exportWord);

    QMenu *theme = menuBar()->addMenu(tr("主题(&M)"));
    const QList<QPair<QString, QString>> themeDefs = {
        { QStringLiteral("light"), tr("浅色(&L) — GitHub") },
        { QStringLiteral("dark"),  tr("夜间(&D) — Dark") },
        { QStringLiteral("paper"), tr("纸白(&W) — WeChat") },
    };
    QActionGroup *themes = new QActionGroup(this);
    for (const auto &d : themeDefs) {
        QAction *act = theme->addAction(d.second);
        act->setCheckable(true);
        act->setChecked(d.first == m_theme);
        themes->addAction(act);
        const QString id = d.first;
        connect(act, &QAction::triggered, this, [this, id] { applyTheme(id); });
    }

    // ---------- 设置菜单(帮助之前) ----------
    QMenu *settings = menuBar()->addMenu(tr("设置(&S)"));
    auto *preferLatex=settings->addAction(tr("优先 LaTeX 风格"));
    preferLatex->setObjectName(QStringLiteral("preferLatex"));
    preferLatex->setCheckable(true);
    preferLatex->setChecked(QSettings().value(QStringLiteral("preferLatex"),false).toBool());
    connect(preferLatex,&QAction::toggled,this,[this](bool enabled){
        QSettings().setValue(QStringLiteral("preferLatex"),enabled);
        for(const auto &tab:m_tabs) if(tab.host)
            tab.host->runScript(Bridge::call(QStringLiteral("setPreferLatex"),{enabled}));
    });
    settings->addAction(tr("修复当前文档的 LaTeX"),this,[this]{
        if(auto *tab=currentTab();tab && tab->host)
            tab->host->runScript(Bridge::call(QStringLiteral("repairLatex")));
    });
    settings->addSeparator();
    QAction *autoSaveAct = settings->addAction(tr("自动保存(&A)(停笔 2 秒)"), this, [this](bool on) {
        QSettings().setValue(QStringLiteral("autoSave"), on);
        m_autoSave = on;   // 高频路径读成员,不再每次开注册表
        for(Tab &tab:m_tabs)if(tab.autoSave) {
            if(on && tab.dirty)tab.autoSave->start(2000);
            else if(!on)tab.autoSave->stop();
        }
        if(auto *tab=currentTab())updateSaveIndicator(tab->dirty);
    });
    autoSaveAct->setCheckable(true);
    autoSaveAct->setObjectName(QStringLiteral("autoSave"));
    autoSaveAct->setChecked(QSettings().value(QStringLiteral("autoSave"), true).toBool());
    settings->addSeparator();
    settings->addAction(tr("放大字号(&B)"), QKeySequence(QStringLiteral("Ctrl+Alt+=")), this, [this] { changeFontSize(+1); });
    settings->addAction(tr("缩小字号(&S)"), QKeySequence(QStringLiteral("Ctrl+Alt+-")), this, [this] { changeFontSize(-1); });
    settings->addAction(tr("重置字号(&R)"), QKeySequence(QStringLiteral("Ctrl+Alt+0")), this, [this] { changeFontSize(0); });
    settings->addSeparator();
    QAction *focusAct2 = settings->addAction(tr("专注模式(&F)"));
    focusAct2->setCheckable(true);
    QAction *typewriterAct2 = settings->addAction(tr("打字机模式(&W)"));
    typewriterAct2->setCheckable(true);
    auto bindToggle = [this](QAction *a, QAction *b, bool *flag, const QString &fn) {
        auto apply = [this, a, b, flag, fn](bool on) {
            if (*flag == on) {
                if (a->isChecked() != on) a->setChecked(on);
                if (b->isChecked() != on) b->setChecked(on);
                return;
            }
            *flag = on;
            if (a->isChecked() != on) a->setChecked(on);
            if (b->isChecked() != on) b->setChecked(on);
            for (const Tab &t : m_tabs)
                if (t.host) t.host->runScript(Bridge::call(fn, { on }));
        };
        connect(a, &QAction::toggled, this, apply);
        connect(b, &QAction::toggled, this, apply);
    };
    bindToggle(focusAct, focusAct2, &m_focusMode, QStringLiteral("setFocusMode"));
    bindToggle(typewriterAct, typewriterAct2, &m_typewriter, QStringLiteral("setTypewriter"));
    settings->addSeparator();
    // 代码块行号:两套独立渲染系统 —— 勾上=自研行号列(左槽 40px+行号+编辑高亮列),
    // 不勾=MarkText 式纯代码卡片(左槽 16px,无行号)。默认不勾(默认无行号)。
    QAction *lineNumAct = settings->addAction(tr("代码块行号(&N)"), this, [this](bool on) {
        QSettings().setValue(QStringLiteral("codeLineNumbers"), on);
        m_lineNumbers = on;   // 新开标签读这个成员,漏了它就拿到切换前的旧状态
        for (const Tab &t : m_tabs)
            if (t.host) t.host->runScript(Bridge::call(QStringLiteral("setLineNumbers"), { on }));
    });
    lineNumAct->setCheckable(true);
    lineNumAct->setChecked(QSettings().value(QStringLiteral("codeLineNumbers"), false).toBool());
    settings->addSeparator();
    // 本文档默认代码语言:列表底部有快捷入口,这里是正式设置入口
    settings->addAction(tr("本文档默认代码语言(&G)..."), this, [this] {
        Tab *t = currentTab();
        if (!t)
            return;
        const QString none = tr("(无默认)");
        const QStringList langs = {
            none,
            QStringLiteral("python"), QStringLiteral("javascript"), QStringLiteral("typescript"),
            QStringLiteral("java"), QStringLiteral("cpp"), QStringLiteral("c"), QStringLiteral("csharp"),
            QStringLiteral("go"), QStringLiteral("rust"), QStringLiteral("php"), QStringLiteral("ruby"),
            QStringLiteral("swift"), QStringLiteral("kotlin"), QStringLiteral("html"), QStringLiteral("css"),
            QStringLiteral("json"), QStringLiteral("yaml"), QStringLiteral("sql"), QStringLiteral("bash"),
            QStringLiteral("powershell"), QStringLiteral("markdown"), QStringLiteral("lua"), QStringLiteral("r"),
            QStringLiteral("matlab"), QStringLiteral("perl"), QStringLiteral("haskell"), QStringLiteral("dart"),
        };
        const QString docName = t->path.isEmpty() ? tr("未命名文档")
                                                   : QFileInfo(t->path).fileName();
        int cur = langs.indexOf(t->docLang);
        if (cur < 0)
            cur = 0;   // 未设置(或自定义值不在表里) → 显示"无默认",选中后可手输
        bool ok = false;
        const QString picked = QInputDialog::getItem(this, tr("本文档默认代码语言"),
            tr("%1 中新建的代码块将自动使用该语言(仍可手动修改,\n也可直接输入其他语言):").arg(docName),
            langs, cur, true, &ok);
        if (!ok)
            return;
        const QString lang = (picked.isEmpty() || picked == none) ? QString() : picked.toLower();
        applyDocLang(*t, lang);
    });
    settings->addSeparator();
    // 主题:同步勾选到"主题"菜单(同一组 QAction,两处入口共享状态)
    QActionGroup *settingsThemes = new QActionGroup(this);
    const QList<QPair<QString, QString>> sthemeDefs = {
        { QStringLiteral("light"), tr("浅色(&L) — GitHub") },
        { QStringLiteral("dark"),  tr("夜间(&D) — Dark") },
        { QStringLiteral("paper"), tr("纸白(&W) — WeChat") },
    };
    for (const auto &d : sthemeDefs) {
        QAction *act = settings->addAction(d.second);
        act->setCheckable(true);
        act->setChecked(d.first == m_theme);
        settingsThemes->addAction(act);
        const QString id = d.first;
        connect(act, &QAction::triggered, this, [this, id] { applyTheme(id); });
    }
    settings->addSeparator();
    settings->addAction(tr("图片目录(&I)..."), this, [this] {
        const QString dir = QFileDialog::getExistingDirectory(
            this, tr("选择图片存放目录"),
            QSettings().value(QStringLiteral("imgDir")).toString());
        if (!dir.isEmpty()) {
            QSettings().setValue(QStringLiteral("imgDir"), dir);
            if (!m_imgDirs.contains(dir)) {
                m_imgDirs.append(dir);
                syncImgMaps();
            }
            statusBar()->showMessage(
                tr("已登记历史图片目录 %1(新截图仍存文档旁 assets/)").arg(dir), 4000);
        }
    });
    settings->addSeparator();
    // 文档默认保存位置(新建文件/另存为的起始目录;也是图片池的默认根)
    settings->addAction(tr("文档保存位置(&S)..."), this, [this] {
        const QString current = m_files->defaultSaveDir();
        const QString dir = QFileDialog::getExistingDirectory(
            this, tr("选择文档保存位置"), current);
        if (dir.isEmpty())
            return;
        m_files->setDefaultSaveDir(dir);
        // 图片池未手动设置过时也跟随过去
        if (QSettings().value(QStringLiteral("imgDir")).toString().isEmpty()) {
            m_imgDirs.append(imagePoolDir()); // 新池目录进映射表
            syncImgMaps();
        }
        statusBar()->showMessage(
            tr("文档默认保存到 %1(新建文件与图片都存这里)").arg(dir), 4000);
    });
    // AI 助手的供应商(模型/协议/密钥),与 AI 菜单里的入口同一对话框
    settings->addAction(tr("AI 供应商(&A)..."), this, [this] { openAiConfig(); });
    settings->addAction(tr("AI 技能目录…"),this,&MainWindow::openAiSkills);

    QMenu *help = menuBar()->addMenu(tr("帮助(&H)"));
    help->addAction(tr("快捷键说明(&K)..."), QKeySequence::HelpContents, this, [this] {
        UiDialogs::showShortcuts(this,m_theme); });
    help->addSeparator();
    help->addAction(tr("关于 Mswrite(&A)..."), this, [this] {
        UiDialogs::showAbout(this,m_theme); });

    // ---------- AI 菜单(帮助之后) ----------
    QMenu *aiMenu = menuBar()->addMenu(tr("AI(&I)"));
    m_aiToggleAction = aiMenu->addAction(tr("我的AI助手(&M)"),
                                        QKeySequence(QStringLiteral("F9")),
                                        this, [this] { toggleAiDock(); });
    m_aiToggleAction->setCheckable(true);
    aiMenu->addAction(tr("AI 供应商(&S)..."), this, [this] { openAiConfig(); });
    aiMenu->addAction(tr("连接诊断(&D)..."), this, [this] { runAiDoctor(); });
    aiMenu->addSeparator();
    aiMenu->addAction(tr("清空对话(&C)"), this, [this] {
        if (m_aiDock)
            m_aiDock->clearConversation();
    });
}

// 整页缩放:WebView2 原生 ZoomFactor(等比缩放整页,含图片/公式渲染),
// 每标签独立;按文档路径持久化,重开恢复
void MainWindow::applyZoom(Tab &tab, double factor)
{
    if (tab.pdf) { tab.pdf->zoomFitWidth(); return; }
    factor = qBound(0.5, factor, 2.0);
    tab.zoom = factor;
    if (tab.host) {
        tab.host->setZoomFactor(factor);
        // 缩放是异步生效的:等它落定后让页面重排行号/高亮,
        // 否则行号列还停在旧缩放的坐标上
        QTimer::singleShot(80, tab.host, [host = tab.host]() {
            host->runScript(QStringLiteral("window.msbridge.resyncDecor()"));
        });
    }
    if (!tab.path.isEmpty()) {
        if (qFuzzyCompare(factor, 1.0))
            QSettings().remove(QStringLiteral("zoom/") + tab.path);
        else
            QSettings().setValue(QStringLiteral("zoom/") + tab.path, factor);
    }
    if (m_zoomLabel) {
        updateZoomLabel(factor);
        return;
    }
}

// 编辑区 Ctrl+滚轮 = 整页缩放。滚轮事件被 WebView2 的 Chromium 子窗口吃掉,
// Qt 事件系统收不到 —— 所以过滤器装在每个 host 的原生 HWND 上(installEventFilter
// 对原生窗口无效,改由 WebViewHost 的 wheelEvent 桥接;见 webviewhost.cpp)
bool MainWindow::eventFilter(QObject *obj, QEvent *ev)
{
    if(obj==m_saveLabel && ev->type()==QEvent::MouseButtonRelease) {
        saveFile();return true;
    }
    // 点击缩放胶囊:回 100%(Ctrl+滚轮的缩放在页面桥接层上报,见 bridge.js)
    if (obj == m_zoomLabel && ev->type() == QEvent::MouseButtonRelease) {
        Tab *t = currentTab();
        if (t && !qFuzzyCompare(t->zoom, 1.0))
            applyZoom(*t, 1.0);
        return true;
    }
    return QMainWindow::eventFilter(obj, ev);
}

void MainWindow::buildStatusBar()
{
    m_statsLabel = new QLabel(QStringLiteral("    "), this);
    m_modeLabel = new QLabel(tr("所见即所得"), this);
    m_saveLabel = new QLabel(this);
    m_saveLabel->setObjectName(QStringLiteral("saveStatus"));
    m_saveLabel->setCursor(Qt::PointingHandCursor);
    m_saveLabel->installEventFilter(this);
    m_themeLabel = new QLabel(this);
    // 缩放百分比胶囊:100% 时灰色低调显示,非 100% 变蓝;点击回 100%
    m_zoomLabel = new QLabel(this);
    m_zoomLabel->setCursor(Qt::PointingHandCursor);
    statusBar()->addWidget(m_statsLabel, 1);
    statusBar()->addPermanentWidget(m_modeLabel);
    // 保存状态指示:放在最右侧,一眼可见(配色见 updateSaveIndicator)
    statusBar()->addPermanentWidget(m_saveLabel);
    statusBar()->addPermanentWidget(m_zoomLabel);
    statusBar()->addPermanentWidget(m_themeLabel);
    updateSaveIndicator(false);   // 初始:已保存
    applyTheme(m_theme, false);
    // 点击缩放胶囊回到 100%
    m_zoomLabel->installEventFilter(this);
}

void MainWindow::applyTheme(const QString &theme, bool persist)
{
    m_theme = theme;
    const bool dark = theme == QLatin1String("dark");
    const bool paper = theme == QLatin1String("paper");
    const QString surface = dark ? "#202329" : paper ? "#f5f2eb" : "#f5f7fa";
    const QString base = dark ? "#181b20" : paper ? "#fffdf8" : "#ffffff";
    const QString ink = dark ? "#e5e7eb" : "#253041";
    const QString muted = dark ? "#a3adbd" : "#66748a";
    const QString line = dark ? "#353c49" : "#e0e5ed";
    const QString hover = dark ? "#2c3340" : "#eaf0f8";
    const QString accent = dark ? "#8bbcff" : "#2563eb";
    QPalette p = palette();
    p.setColor(QPalette::Window, QColor(surface));
    p.setColor(QPalette::Base, QColor(base));
    p.setColor(QPalette::AlternateBase, QColor(hover));
    p.setColor(QPalette::Text, QColor(ink));
    p.setColor(QPalette::WindowText, QColor(ink));
    p.setColor(QPalette::Button, QColor(surface));
    p.setColor(QPalette::ButtonText, QColor(ink));
    p.setColor(QPalette::Mid, QColor(line));
    p.setColor(QPalette::Highlight, QColor(dark ? "#31527e" : "#dbeafe"));
    p.setColor(QPalette::HighlightedText, QColor(ink));
    p.setColor(QPalette::PlaceholderText, QColor(muted));
    setPalette(p);
    for (auto *child : findChildren<QWidget *>()) child->setPalette(p);
    setStyleSheet(QStringLiteral(R"(
QMainWindow { background:%1; }
QMenuBar { background:%1; color:%3; padding:4px 8px; border-bottom:1px solid %5; }
QMenuBar::item { padding:5px 10px; border-radius:5px; background:transparent; }
QMenuBar::item:selected { background:%6; }
QMenu { background:%2; color:%3; border:1px solid %5; padding:5px; }
QMenu::item { padding:7px 26px 7px 12px; border-radius:4px; }
QMenu::item:selected { background:%6; }
QMenu::item:disabled { color:%4; }
QMenu::separator { height:1px; background:%5; margin:5px 8px; }
QTabBar { background:%1; }
QTabBar::tab { color:%4; background:%1; min-width:90px; max-width:240px; padding:9px 12px; border:0; border-bottom:2px solid transparent; }
QTabBar::tab:selected { background:%2; color:%3; border-bottom:2px solid %7; }
QTabBar::tab:hover:!selected { background:%6; }
QToolButton#tabClose { color:%4; background:transparent; border:0; border-radius:5px; font-size:18px; padding:0; }
QToolButton#tabClose:hover { color:#d74949; background:%6; }
QDockWidget { color:%4; border:0; }
QDockWidget::title { background:%1; padding:10px 12px; font-weight:600; }
QListWidget, QTreeView { background:%2; color:%3; border:0; outline:0; padding:5px; }
QListWidget::item, QTreeView::item { padding:7px 8px; border-radius:5px; }
QListWidget::item:hover, QTreeView::item:hover { background:%6; }
QListWidget::item:selected, QTreeView::item:selected { background:%6; color:%7; }
QStatusBar { background:%1; color:%4; border-top:1px solid %5; padding:3px 8px; }
QStatusBar::item { border:0; }
QStatusBar QLabel { color:%4; padding:2px 5px; }
QPushButton { background:%2; color:%3; border:1px solid %5; border-radius:6px; padding:5px 10px; }
QPushButton:hover { background:%6; border-color:%7; }
QPushButton:disabled { color:%4; background:%1; }
QLineEdit, QSpinBox, QComboBox { background:%2; color:%3; selection-background-color:%6; selection-color:%3; border:1px solid %5; border-radius:5px; padding:4px 7px; }
QLineEdit:focus, QSpinBox:focus, QComboBox:focus { border-color:%7; }
QSplitter::handle, QMainWindow::separator { background:%5; width:1px; height:1px; }
QScrollBar:vertical { background:transparent; width:10px; margin:0; }
QScrollBar::handle:vertical { background:%5; border-radius:4px; min-height:32px; }
QScrollBar::handle:vertical:hover { background:%4; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height:0; }
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background:transparent; }
)").arg(surface, base, ink, muted, line, hover, accent));
    broadcastTheme();
    QString label;
    if (theme == QLatin1String("dark"))
        label = tr("主题:夜间");
    else if (theme == QLatin1String("paper"))
        label = tr("主题:纸白");
    else
        label = tr("主题:浅色");
    m_themeLabel->setText(label + QStringLiteral("  "));
    if (persist)
        m_files->setLastTheme(theme);
    // 主题/设置两处各有一套勾选项,必须一起刷新,否则一边勾着浅色一边勾着夜间
    for (QAction *a : menuBar()->findChildren<QAction *>()) {
        if (!a->isCheckable())
            continue;
        const QString tx = a->text();
        if (tx.contains(QStringLiteral("GitHub")))
            a->setChecked(theme == QLatin1String("light"));
        else if (tx.contains(QStringLiteral("Dark")))
            a->setChecked(theme == QLatin1String("dark"));
        else if (tx.contains(QStringLiteral("WeChat")))
            a->setChecked(theme == QLatin1String("paper"));
    }
    // 指示器自带底色,但刷新一次确保状态正确
    if (const Tab *t = currentTab(); t) {
        updateSaveIndicator(t->dirty);
        if (!t->pdf) updateZoomLabel(t->zoom);
    }
}

void MainWindow::broadcastTheme() const
{
    for (const Tab &t : m_tabs) {
        if (t.pdf) t.pdf->setTheme(m_theme);
        else if (t.host) t.host->runScript(Bridge::call(QStringLiteral("setTheme"), { m_theme }));
    }
}

void MainWindow::changeFontSize(int delta)
{
    m_fontSize = delta == 0 ? 16 : qBound(12, m_fontSize + delta, 28);
    QSettings().setValue(QStringLiteral("fontSize"), m_fontSize);
    for (const Tab &t : m_tabs) {
        if (!t.host) continue;
        t.host->runScript(Bridge::call(QStringLiteral("setFontSize"), { m_fontSize }));
        // 字号改完行高会变,行号/高亮列必须重排(与 applyZoom 同款)
        QTimer::singleShot(80, t.host, [host = t.host]() {
            host->runScript(QStringLiteral("window.msbridge.resyncDecor()"));
        });
    }
}

// 文档默认代码语言:更新标签、按路径持久化(未命名仅会话内)、下发页面
void MainWindow::applyDocLang(Tab &tab, const QString &lang)
{
    if (!tab.host) return;
    tab.docLang = lang;
    if (!tab.path.isEmpty()) {
        if (lang.isEmpty())
            QSettings().remove(QStringLiteral("doclang/") + tab.path);
        else
            QSettings().setValue(QStringLiteral("doclang/") + tab.path, lang);
    }
    tab.host->runScript(Bridge::call(QStringLiteral("setDocLang"), { lang }));
    statusBar()->showMessage(lang.isEmpty()
        ? tr("已取消本文档的默认代码语言")
        : tr("本文档新代码块将默认使用 %1").arg(lang), 3000);
}

// ---------------------------------------------------------------------------
// WebView 消息分发
// ---------------------------------------------------------------------------

void MainWindow::onWebMessage(WebViewHost *sender, const QJsonObject &obj)
{
    const QString t = obj.value(QStringLiteral("t")).toString();
    Tab *tab = tabForHost(sender);
    const int index = tab ? indexOfHost(sender) : -1;

    if (t == QLatin1String("contextMenu")) {
        qWarning() << "Mswrite: 右键菜单请求到达";
        // 延迟到消息回调返回后再弹:COM 回调内嵌套事件循环会与
        // WebView2 原生子窗口的鼠标捕获冲突,菜单弹不出来
        const bool math = obj.value(QStringLiteral("math")).toBool();
        const bool mathBold = obj.value(QStringLiteral("mathBold")).toBool();
        QTimer::singleShot(0, this, [this, sender, math, mathBold] {
            if (currentTab() && currentTab()->host == sender)
                showEditorContextMenu(math, mathBold);
        });
        return;
    }
    if (t == QLatin1String("keylog")) {
        qWarning() << "Mswrite 按键:" << obj.value(QStringLiteral("k")).toString()
                       << "焦点=" << obj.value(QStringLiteral("focus")).toString();
        return;
    }
    if (t == QLatin1String("jserror")) {
        qWarning() << "Mswrite JS 错误:" << obj.value(QStringLiteral("msg")).toString()
                   << "@" << obj.value(QStringLiteral("src")).toString()
                   << ":" << obj.value(QStringLiteral("line")).toInt();
        return;
    }
    if (!tab)
        return; // 消息来自不在标签里的 host(理论上不存在,防御)

    if (t == QLatin1String("ready")) {
        if (index == currentTabIndex())
            tab->host->runScript(QStringLiteral("window.msbridge.focus()"));
        // 启动校验:页面脚本版本(排查缓存送旧代码)
        tab->host->evalWithResult(QStringLiteral(
            "String(window.msbridgeVer || '未知')"),
            [this](const QString &ver) {
                qWarning() << "Mswrite: 页面脚本版本 =" << ver;
            });
        // Diagnostics must never overwrite an opened document just because
        // DevTools is enabled. The legacy destructive smoke test is opt-in.
        if (qEnvironmentVariableIsSet("MSWRITE_DEV")
            && qEnvironmentVariableIsSet("MSWRITE_SELF_TEST") && tab->path.isEmpty())
        QTimer::singleShot(3000, tab->host, [host = tab->host] {
            host->runScript(QStringLiteral(
                "(function(){"
                "var post=function(o){window.chrome.webview.postMessage(o)};"
                "var R=document.querySelector('.vditor-ir pre.vditor-reset')||document.body;"
                "function fire(code,key,ctrl,shift,alt){R.dispatchEvent(new KeyboardEvent('keydown',"
                "{key:key,code:code,bubbles:true,cancelable:true,ctrlKey:!!ctrl,shiftKey:!!shift,altKey:!!alt}));}"
                "var Q=[],RES=[];var LF=String.fromCharCode(8629)+String.fromCharCode(10);"
                "function step(n,f,c){Q.push({n:n,f:f,c:c});}"
                "function base(){var lc=R.querySelector('p')||R.lastChild;if(lc){var g=document.createRange();g.selectNodeContents(lc);g.collapse(true);"
                "window.getSelection().removeAllRanges();window.getSelection().addRange(g);}}"
                "function rst(md){window.msbridge.setContent(md);"
                "var lc=R.querySelector('p');if(!lc)lc=R.lastChild;"
                "var g=document.createRange();g.selectNodeContents(lc);"
                "window.getSelection().removeAllRanges();window.getSelection().addRange(g);}"
                "var Q=[],RES=[];function step(n,f,c){Q.push({n:n,f:f,c:c});}"
                "step('Ctrl+T 表格',function(){rst('alpha');fire('KeyT','t',1);},function(){return window.mswValue().indexOf('| 表头1')>=0;});"
                "step('Ctrl+Shift+K 代码',function(){rst('alpha');fire('KeyK','K',1,1);},function(){return window.mswValue().indexOf('cpp')>=0;});"
                "step('Ctrl+Shift+M 公式',function(){rst('alpha');fire('KeyM','M',1,1);},function(){return window.mswValue().indexOf('$$')>=0;});"
                "step('Ctrl+Shift+Q 引用',function(){rst('alpha');fire('KeyQ','Q',1,1);},function(){return window.mswValue().indexOf('> ')>=0;});"
                "step('Ctrl+Shift+[ 有序',function(){rst('alpha');fire('BracketLeft','{',1,1);},function(){return window.mswValue().indexOf('1. ')>=0;});"
                "step('Ctrl+Shift+] 无序',function(){rst('alpha');fire('BracketRight','}',1,1);},function(){return window.mswValue().indexOf('- ')>=0;});"
                "step('Alt+Shift+5 删除线',function(){rst('alpha');fire('Digit5','%',0,1,1);},function(){return window.mswValue().indexOf('~~')>=0;});"
                "step('Ctrl+B 加粗(选alpha)',function(){rst('alpha beta');"
                "var lc2=R.querySelector('p');var g2=document.createRange();g2.selectNodeContents(lc2.firstChild?lc2.firstChild:lc2);"
                "window.getSelection().removeAllRanges();window.getSelection().addRange(g2);"
                "fire('KeyB','b',1);},function(){var v=window.mswValue();return v.indexOf('**alpha beta**')>=0&&v.indexOf('**alpha beta**alpha')<0;});"
                "step('Ctrl+1 标题',function(){rst('alpha');fire('Digit1','1',1);},function(){return window.mswValue().indexOf('# alpha')>=0;});"
                "step('Ctrl+B 无选区整段',function(){rst('alpha beta');var lc3=R.querySelector('p');var tn=lc3.firstChild;var g3=document.createRange();g3.setStart(tn,5);g3.collapse(true);window.getSelection().removeAllRanges();window.getSelection().addRange(g3);fire('KeyB','b',1);},function(){var v=window.mswValue();return v.indexOf('**alpha beta**')>=0&&v.indexOf('**alpha beta**alpha')<0;});"
                "step('Ctrl+L 选行',function(){rst('alpha beta');fire('KeyL','l',1);},function(){return window.getSelection().toString().length>0;});"
                "step('颜色 红色包选区',function(){rst('alpha beta');var lc6=R.querySelector('p');var g6=document.createRange();g6.selectNodeContents(lc6);window.getSelection().removeAllRanges();window.getSelection().addRange(g6);window.msbridge.applyColor('#e74c3c');},function(){return window.mswValue().indexOf('<font color=\"#e74c3c\">alpha beta</font>')>=0;});"
                "step('DOM级span上色',function(){rst('alpha beta');var lc6=R.querySelector('p');"
                "var tn6=lc6.firstChild;var sp=document.createElement('span');"
                "sp.style.color='red';"
                "var t6=document.createTextNode('alpha');sp.appendChild(t6);"
                "tn6.replaceWith(sp,document.createTextNode(tn6.textContent.slice(5)));"
                "},function(){var v=window.mswValue();"
                "return v.indexOf('alpha')>=0;}),"
                "step('H1结构转储',function(){rst('alpha');fire('Digit1','1',1);},"
                "function(){var h=document.querySelector('.vditor-ir h1');"
                "window.chrome.webview.postMessage({t:'keylog',k:'H1HTML:'+(h?h.outerHTML.slice(0,180):'NONE')});"
                "return !!h;}),"
                "step('标题 H1->H2',function(){fire('Digit2','2',1);},"
                "function(){var v=window.mswValue();return v.indexOf('## alpha')>=0&&v.indexOf('## ##')<0;}),"
                "step('Enter x2 连续空行',function(){rst('alpha');"
                "var lc4=R.querySelector('p');var tn4=lc4.firstChild;var g4=document.createRange();g4.setStart(tn4,5);g4.collapse(true);"
                "window.getSelection().removeAllRanges();window.getSelection().addRange(g4);"
                "fire('Enter','Enter');"
                "setTimeout(function(){"
                "var s5=window.getSelection();if(s5.rangeCount){var r5=s5.getRangeAt(0);"
                "var g5=document.createRange();g5.setStart(r5.startContainer,r5.startOffset);g5.collapse(true);"
                "window.getSelection().removeAllRanges();window.getSelection().addRange(g5);}"
                "fire('Enter','Enter');},800);"
                "},function(){return window.mswValue().indexOf(String.fromCharCode(160))>=0;}),"
                "(function run(){"
                "if(!Q.length){post({t:'shortcutsTest',results:RES});return;}"
                "var s=Q.shift();"
                "try{s.f();}catch(e){RES.push(s.n+':FIRE-ERR');setTimeout(run,200);return;}"
                "setTimeout(function(){try{RES.push(s.n+':'+(s.c()?'PASS':'FAIL')+' ['+window.mswValue().slice(0,25).replace(String.fromCharCode(10),LF)+']')}catch(e){RES.push(s.n+':CHK-ERR')}run();},500);"
                "})();"
                "})()"));
        });
        return;
    }
    if (t == QLatin1String("changed")) {
        tab->rev = obj.value(QStringLiteral("rev")).toInt();
        tab->saveError.clear();
        markDirty(index, true);
        if(m_autoSave)
            tab->autoSave->start(2000);
        return;
    }
    if (t == QLatin1String("stats")) {
        tab->stats = tr("字数 %1 · 字符 %2 · 行 %3 · 段%4 列%5  ")
                         .arg(obj.value(QStringLiteral("words")).toInt())
                         .arg(obj.value(QStringLiteral("chars")).toInt())
                         .arg(obj.value(QStringLiteral("lines")).toInt())
                         .arg(obj.value(QStringLiteral("cl")).toInt())
                         .arg(obj.value(QStringLiteral("cc")).toInt());
        if (index == currentTabIndex())
            m_statsLabel->setText(tab->stats);
        return;
    }
    if (t == QLatin1String("outline")) {
        QVector<OutlineItem> items;
        const QJsonArray arr = obj.value(QStringLiteral("items")).toArray();
        for (const QJsonValue &v : arr) {
            OutlineItem it;
            it.level = v.toObject().value(QStringLiteral("level")).toInt(1);
            it.text = v.toObject().value(QStringLiteral("text")).toString();
            items.append(it);
        }
        if (index == currentTabIndex())
            m_outline->setItems(items);
        return;
    }
    if (t == QLatin1String("content")) {
        QString md = restoreImagePaths(*tab, obj.value(QStringLiteral("md")).toString());
        const int savedRev = obj.value(QStringLiteral("rev")).toInt();
        if (tab->awaitingContent && obj.value(QStringLiteral("request")).toInt()==tab->saveRequest) {
            tab->awaitingContent = false;
            const bool newFile=tab->path.isEmpty();
            if(newFile && md.trimmed().isEmpty()) {
                markDirty(index,false);
                return;
            }
            {
                // 未命名文档:默认文件名取文档开头正文(剥 Markdown 标记,
                // 由页面 firstLine 消息维护的 titleHint 同源);无标题回退时间戳
                const QString path = newFile ? uniqueUntitledPath(
                    FileService::titleFromMarkdown(md)) : tab->path;
                // 行尾保真:编辑器统一输出 \n,原文件是 CRLF 就还原成 CRLF,
                // 否则 Windows 老文档一保存整篇 diff
                md.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
                if (tab->crlf)
                    md.replace(QLatin1Char('\n'), QStringLiteral("\r\n"));
                if (FileService::writeFile(path, md, tab->enc)) {
                    tab->saveError.clear();
                    if(newFile) {
                        tab->path=path;
                        applyDocDir(*tab);updateTabText(index);
                        m_tabbar->setTabToolTip(index,QDir::toNativeSeparators(path));
                        m_files->pushRecentFile(path);
                        if(index==currentTabIndex())m_files->setLastOpenedFile(path);
                    }
                    statusBar()->showMessage(
                        newFile ? tr("新文档已自动保存到 %1").arg(QDir::toNativeSeparators(path))
                                : tr("已保存 %1").arg(QTime::currentTime().toString()), newFile ? 8000 : 2500);
                    if (savedRev < tab->rev) {
                        // 边打字边存:快照已过期。保留脏标记并再存一次,否则会出现
                        // "标签显示已保存、磁盘却是旧内容",此刻关窗就是静默丢字
                        markDirty(index, true);
                        if (tab->closeAfterSave || m_flushingSaves)
                            requestContent(*tab);   // 存完最新内容再关
                        else if(m_autoSave)
                            tab->autoSave->start(1500);
                    } else {
                        markDirty(index, false);
                        if (tab->closeAfterSave) {
                            tab->closeAfterSave = false;
                            closeTab(index);
                        }
                    }
                } else {
                    tab->closeAfterSave = false;
                    tab->saveError=tr("无法写入 %1。请检查权限或另存为，当前内容仍保留在编辑器中。").arg(QDir::toNativeSeparators(path));
                    markDirty(index,true);
                    statusBar()->showMessage(tab->saveError);
                }
            }
        }
        return;
    }
    if (t == QLatin1String("saveImage")) {
        saveImageFromWeb(*tab, obj.value(QStringLiteral("rid")).toInt(),
                         obj.value(QStringLiteral("mime")).toString(),
                         obj.value(QStringLiteral("base64")).toString());
        return;
    }
    if (t == QLatin1String("mode")) {
        tab->mode = obj.value(QStringLiteral("value")).toString();
        if (index == currentTabIndex())
            m_modeLabel->setText(tab->mode == QLatin1String("sv") ? tr("源码模式")
                                                                  : tr("所见即所得"));
        return;
    }
    if (t == QLatin1String("html")) {
        if (!m_exportBusy || sender != m_exportSource
            || obj.value(QStringLiteral("request")).toInt() != m_exportRequest) return;
        tab->titleHint = FileService::titleFromMarkdown(obj.value(QStringLiteral("titleSource")).toString());
        // 延后到 COM 回调之外:导出要开文件对话框(嵌套事件循环)
        const QString html = obj.value(QStringLiteral("html")).toString();
        QTimer::singleShot(0, this, [this, host = QPointer<WebViewHost>(sender), html, request = m_exportRequest] {
            if (!host || !m_exportBusy || m_exportSource != host || request != m_exportRequest) return;
            Tab *tb = tabForHost(host);   // 回传者才是导出对象,不一定是当前标签
            if (tb)
                finishExport(html, tb);
        });
        return;
    }
    if (t == QLatin1String("exportError")) {
        if (m_exportBusy && sender == m_exportSource
            && obj.value(QStringLiteral("request")).toInt() == m_exportRequest) {
            resetExport();
            statusBar()->showMessage(tr("导出渲染失败，请重试"), 5000);
        }
        return;
    }
    if (t == QLatin1String("pickImage")) {
        QTimer::singleShot(0, this, [this, host = sender] {
            const int idx = indexOfHost(host);
            if (idx >= 0)
                m_tabbar->setCurrentIndex(idx);
            handlePickImage();
        });
        return;
    }
    if (t == QLatin1String("replace")) {
        const QString prefill = obj.value(QStringLiteral("sel")).toString();
        QTimer::singleShot(0, this, [this, host = sender, prefill] {
            Tab *rt = tabForHost(host);
            if (!rt)
                return;
            const int idx = indexOfHost(host);
            if (idx >= 0)
                m_tabbar->setCurrentIndex(idx);
            handleReplaceDialog(prefill);
        });
        return;
    }
    if (t == QLatin1String("fullscreen")) {
        toggleFullscreen();
        return;
    }
    if (t == QLatin1String("aiPanel")) {
        // F9 来自编辑区(键盘事件在 Chromium 内,经页面转发)
        toggleAiDock();
        return;
    }
    if (t == QLatin1String("openUrl")) {
        // Alt+点击链接文字:系统浏览器打开
        const QString url = obj.value(QStringLiteral("url")).toString();
        if (!url.isEmpty())
            QDesktopServices::openUrl(QUrl(url));
        return;
    }
    if (t == QLatin1String("linkDialog")) {
        // Ctrl+K:弹链接地址输入(预填 https://),回页面插 [选中文字](链接)。
        // 延后到 COM 回调之外弹对话框(与右键菜单同一规避)
        const QString selText = obj.value(QStringLiteral("sel")).toString();
        QTimer::singleShot(0, this, [this, host = sender, selText] {
            Tab *lt = tabForHost(host);
            if (!lt)
                return;
            const QPointer<WebViewHost> editor(lt->host);
            const QString url = QInputDialog::getText(this, tr("插入链接"),
                tr("链接地址(选中文字将作为链接文字):"),
                QLineEdit::Normal, QStringLiteral("https://"));
            if (url.isEmpty() || !editor || !tabForHost(editor))
                return;
            editor->runScript(Bridge::call(QStringLiteral("insertLink"), { url, selText }));
        });
        return;
    }
    if (t == QLatin1String("zoom")) {
        // Ctrl+滚轮(页面内上报):整页缩放,当前标签独立
        const int dir = obj.value(QStringLiteral("dir")).toInt();
        if (tab)
            applyZoom(*tab, tab->zoom + (dir > 0 ? 0.05 : -0.05));
        return;
    }
    if (t == QLatin1String("devtools")) {
        openDevTools();
        return;
    }
    if (t == QLatin1String("openFind")) {
        // Ctrl+F:顶部查找栏(页面内闭环:输入即定位/高亮),预填当前选中文本
        const QString sel = obj.value(QStringLiteral("sel")).toString();
        if (tab)
            tab->host->runScript(Bridge::call(QStringLiteral("openFind"), { sel }));
        return;
    }
    if (t == QLatin1String("replaced")) {
        const int n = obj.value(QStringLiteral("count")).toInt();
        statusBar()->showMessage(n > 0 ? tr("已替换 %1 处").arg(n) : tr("未找到该内容"), 3000);
        return;
    }
    if (t == QLatin1String("notice")) {
        statusBar()->showMessage(obj.value(QStringLiteral("msg")).toString(), 2500);
        return;
    }
    if (t == QLatin1String("shortcutsTest")) {
        const QJsonArray arr = obj.value(QStringLiteral("results")).toArray();
        QStringList lines;
        for (const QJsonValue &v : arr)
            lines << v.toString();
        qWarning() << "Mswrite 快捷键矩阵:" << lines.join(QStringLiteral(" | "));
        return;
    }
    if (t == QLatin1String("docSearch")) {
        m_search->showDocResults(obj.value(QStringLiteral("items")).toArray());
        return;
    }
    if (t == QLatin1String("doclang")) {
        // 本文档默认代码语言(列表底部快捷入口回传)
        applyDocLang(*tab, obj.value(QStringLiteral("lang")).toString());
        return;
    }
    if (t == QLatin1String("firstLine")) {
        // 页面上报的文档开头正文(未命名文档的默认文件名候选);
        // 清洗与打开文件路径同源(FileService::titleFromMarkdown)
        tab->titleHint = FileService::titleFromMarkdown(
            obj.value(QStringLiteral("text")).toString());
        return;
    }
}

void MainWindow::saveImageFromWeb(Tab &tab, int rid, const QString &mime, const QString &base64)
{
    QString error;
    const QString rel = saveImageAsset(tab, base64, mime, &error);
    if (rel.isEmpty()) {
        statusBar()->showMessage(tr("图片未保存:%1").arg(error), 5000);
        tab.host->runScript(Bridge::call(QStringLiteral("imageRejected"), { rid, error }));
    } else {
        tab.host->runScript(Bridge::call(QStringLiteral("imageSaved"), { rid, rel }));
    }
}

// ---------------------------------------------------------------------------
// 快捷键扩展功能
// ---------------------------------------------------------------------------

void MainWindow::newWindow()
{
    QProcess::startDetached(QCoreApplication::applicationFilePath(), {});
}

void MainWindow::reopenClosedFile()
{
    while (!m_closedFiles.isEmpty()) {
        const QString p = m_closedFiles.takeLast();
        if (QFileInfo::exists(p)) {
            openPath(p);
            return;
        }
    }
    statusBar()->showMessage(tr("没有可重新打开的文件"), 2500);
}

void MainWindow::showPrefs()
{
    QMessageBox::information(this, tr("偏好设置"),
        tr("当前版本的设置项:\n"
           "· 主题:菜单 主题(浅色/夜间/纸白)\n"
           "· 页面缩放:Ctrl+= / Ctrl+- / Ctrl+0(Ctrl+滚轮同)\n"
           "· 字号:Ctrl+Alt+= 放大 / Ctrl+Alt+- 缩小 / Ctrl+Alt+0 复原\n"
           "· 图片:自动存入文档旁 assets 目录\n"
           "· 自动保存:停笔 2 秒\n"
           "更多设置界面开发中"));
}

void MainWindow::toggleOutlinePanel()
{
    if (auto *t = currentTab(); t && t->pdf) { t->pdf->toggleOutline(); return; }
    m_outline->setVisible(!m_outline->isVisible());
}

void MainWindow::globalSearch()
{
    m_search->focusInput();
}

void MainWindow::toggleSourceMode()
{
    Tab *t = currentTab();
    if (t && t->host)
        t->host->runScript(QStringLiteral("window.msbridge.toggleMode()"));
}

void MainWindow::toggleFullscreen()
{
    if (isFullScreen())
        showNormal();
    else
        showFullScreen();
}

void MainWindow::openDevTools()
{
    Tab *t = currentTab();
    if (t && t->host)
        t->host->openDevTools();
}

void MainWindow::handlePickImage()
{
    Tab *t = currentTab();
    if (!t || !t->host)
        return;
    const QPointer<WebViewHost> editor(t->host);
    const QString src = QFileDialog::getOpenFileName(this, tr("插入图片"),
        m_workspace, tr("图片 (*.png *.jpg *.jpeg *.gif *.webp *.bmp *.svg);;所有文件 (*.*)"));
    if (src.isEmpty())
        return;
    t = editor ? tabForHost(editor) : nullptr;
    if (!t) return;
    // 拷入文档 assets(与粘贴同款流程)
    QFile in(src);
    if (!in.open(QIODevice::ReadOnly)) {
        statusBar()->showMessage(tr("无法读取图片"), 3000);
        return;
    }
    const QByteArray bytes = in.readAll();
    in.close();
    const QString b64 = QString::fromLatin1(bytes.toBase64());
    QString error;
    const QString rel = saveImageAsset(*t, b64, QFileInfo(src).suffix(), &error);
    if (rel.isEmpty()) {
        statusBar()->showMessage(tr("图片未插入:%1").arg(error), 4000);
        return;
    }
    const QString base = QFileInfo(rel).completeBaseName();
    const QString md = QStringLiteral("\n![") + base + QStringLiteral("](")
                     + rel + QStringLiteral(")\n");
    t->host->runScript(Bridge::call(QStringLiteral("insertText"), { md }));
    statusBar()->showMessage(tr("已插入 %1").arg(rel), 3000);
}

// 菜单/右键"粘贴":剪贴板由 C++ 读 —— Qt 有完整权限,而页面侧
// document.execCommand('paste') 在 Chromium 下需要剪贴板读权限、
// WebView2 默认拒绝,旧实现等于空操作
void MainWindow::pasteFromClipboard()
{
    Tab *t = currentTab();
    if (!t || !t->host)
        return;
    const QMimeData *mime = QApplication::clipboard()->mimeData();
    if (!mime)
        return;

    const auto insertImage = [this, t](const QByteArray &bytes, const QString &fmtOrSuffix) {
        QString error;
        const QString rel = saveImageAsset(*t, QString::fromLatin1(bytes.toBase64()),
                                          fmtOrSuffix, &error);
        if (rel.isEmpty()) {
            statusBar()->showMessage(tr("图片未插入:%1").arg(error), 4000);
            return;
        }
        const QString base = QFileInfo(rel).completeBaseName();
        t->host->runScript(Bridge::call(QStringLiteral("insertText"),
            { QStringLiteral("\n![%1](%2)\n").arg(base, rel) }));
        statusBar()->showMessage(tr("已插入 %1").arg(rel), 3000);
    };

    // 1) 位图(截图/复制图片):统一转 PNG 落盘
    if (mime->hasImage()) {
        const QImage img = qvariant_cast<QImage>(mime->imageData());
        if (!img.isNull()) {
            QByteArray bytes;
            QBuffer buf(&bytes);
            buf.open(QIODevice::WriteOnly);
            if (img.save(&buf, "PNG")) {
                insertImage(bytes, QStringLiteral("image/png"));
                return;
            }
        }
    }
    // 2) 复制的是图片文件(资源管理器 Ctrl+C):按原格式落盘
    if (mime->hasUrls()) {
        static const QStringList kImgSuffix = { QStringLiteral("png"), QStringLiteral("jpg"),
            QStringLiteral("jpeg"), QStringLiteral("gif"), QStringLiteral("webp"),
            QStringLiteral("bmp"), QStringLiteral("svg"), QStringLiteral("avif") };
        for (const QUrl &u : mime->urls()) {
            const QString p = u.toLocalFile();
            if (p.isEmpty() || !QFileInfo(p).isFile())
                continue;
            const QString suffix = QFileInfo(p).suffix().toLower();
            if (!kImgSuffix.contains(suffix))
                continue;
            QFile in(p);
            if (!in.open(QIODevice::ReadOnly))
                continue;
            insertImage(in.readAll(), suffix);
            return;
        }
    }
    // 3) 文本
    if (mime->hasText() && !mime->text().isEmpty()) {
        t->host->runScript(Bridge::call(QStringLiteral("pasteText"), { mime->text() }));
        return;
    }
    statusBar()->showMessage(tr("剪贴板里没有可粘贴的内容"), 2500);
}

// 右键菜单:编辑命令 + 文字颜色(原生 QMenu,色块图标)
void MainWindow::showEditorContextMenu(bool math, bool mathBold)
{
    QMenu menu(this);
    // 菜单放大:默认字号与行距在高分屏上偏小
    const QString menuCss = QStringLiteral(
        "QMenu{font-size:15px;padding:6px 4px;}"
        "QMenu::item{padding:8px 34px 8px 12px;min-width:132px;}"
        "QMenu::separator{height:1px;margin:7px 10px;}");
    menu.setStyleSheet(menuCss);

    auto run = [this](const QString &js) {
        Tab *t = currentTab();
        if (t && t->host)
            t->host->runScript(js);
    };
    menu.addAction(tr("剪切(&T)"), QKeySequence::Cut, this, [run] {
        run(QStringLiteral("window.msbridge.editCmd('cut')")); });
    menu.addAction(tr("复制(&C)"), QKeySequence::Copy, this, [run] {
        run(QStringLiteral("window.msbridge.editCmd('copy')")); });
    menu.addAction(tr("粘贴(&P)"), QKeySequence::Paste, this, &MainWindow::pasteFromClipboard);
    menu.addAction(tr("全选(&A)"), QKeySequence::SelectAll, this, [run] {
        run(QStringLiteral("window.msbridge.editCmd('selectAll')")); });
    menu.addSeparator();

    if (math) {
        QAction *bold = menu.addAction(tr("公式加粗(&B)"));
        bold->setCheckable(true);
        bold->setChecked(mathBold);
        bold->setShortcut(QKeySequence::Bold);
        connect(bold, &QAction::triggered, this, [run] {
            run(QStringLiteral("window.msbridge.toggleMathBold()"));
        });
    }
    QMenu *colorMenu = menu.addMenu(math ? tr("公式颜色(&O)") : tr("文字颜色(&O)"));
    colorMenu->setStyleSheet(menuCss);
    const QList<QPair<QString, QString>> colors = {
        { QStringLiteral("#000000"), tr("黑色(默认,清除颜色)") },
        { QStringLiteral("#e74c3c"), tr("红色") },
        { QStringLiteral("#9b59b6"), tr("紫色") },
        { QStringLiteral("#3498db"), tr("蓝色") },
        { QStringLiteral("#2ecc71"), tr("绿色") },
        { QStringLiteral("#f1c40f"), tr("黄色") },
        { QStringLiteral("#e67e22"), tr("橙色") },
        { QStringLiteral("#95a5a6"), tr("灰色") },
    };
    for (const auto &c : colors) {
        // 色块图标:18px + 描边,浅色(黄/灰)也能看清边界
        QPixmap pm(18, 18);
        pm.fill(Qt::transparent);
        {
            QPainter p(&pm);
            p.setRenderHint(QPainter::Antialiasing);
            p.setPen(QPen(QColor(0, 0, 0, 70)));
            p.setBrush(QColor(c.first));
            p.drawRoundedRect(QRectF(1, 1, 16, 16), 3, 3);
        }
        QAction *act = colorMenu->addAction(QIcon(pm), c.second);
        const QString hex = c.first;
        connect(act, &QAction::triggered, this, [this, run, hex] {
            qWarning() << "Mswrite: 颜色选择" << hex;
            run(Bridge::call(QStringLiteral("applyColor"),
                             { hex == QLatin1String("#000000")
                                   ? QStringLiteral("clear") : hex }));
        });
    }
    menu.exec(QCursor::pos());
}

void MainWindow::handleReplaceDialog(const QString &prefill)
{
    Tab *t = currentTab();
    if (!t || !t->host)
        return;
    const QPointer<WebViewHost> editor(t->host);
    bool okFind = false;
    const QString find = QInputDialog::getText(this, tr("替换 — 查找"), tr("查找内容:"),
                                               QLineEdit::Normal, prefill, &okFind);
    if (!okFind || find.isEmpty())
        return;
    bool okRepl = false;
    const QString repl = QInputDialog::getText(this, tr("替换 — 替换为"),
                                               tr("将 \"%1\" 替换为:").arg(find),
                                               QLineEdit::Normal, QString(), &okRepl);
    if (!okRepl || !editor || !tabForHost(editor))
        return; // 取消不能当成"替换成空"(会把所有命中删掉)
    editor->runScript(Bridge::call(QStringLiteral("replaceAll"), { find, repl }));
}

void MainWindow::onOutlineGoto(int index)
{
    Tab *t = currentTab();
    if (t && t->host)
        t->host->runScript(Bridge::call(QStringLiteral("scrollToHeading"), { index }));
}

// ---------------------------------------------------------------------------
// 搜索
// ---------------------------------------------------------------------------

void MainWindow::onSearchRequested(const QString &scope, const QString &query)
{
    if (scope == QLatin1String("doc")) {
        Tab *t = currentTab();
        if (t && t->host) {
            t->host->runScript(Bridge::call(QStringLiteral("searchInDoc"), { query }));
        }
        return;
    }
    // 工作区递归搜索(后台线程,避免大工作区冻结 UI)
    const QString ws = m_workspace;
    if (ws.isEmpty() || !QFileInfo(ws).isDir()) {
        m_search->showStatus(tr("尚未打开工作区(文件 → 打开文件夹)"));
        return;
    }
    m_search->showStatus(tr("正在搜索..."));
    const QString lower = query.toLower();
    // 捕获值列表按值拷贝,线程内不碰任何 UI 成员
    const auto future = QtConcurrent::run([ws, lower]() -> QVector<QPair<QString, QString>> {
        QVector<QPair<QString, QString>> results;
        QDirIterator it(ws,
                        { QStringLiteral("*.md"), QStringLiteral("*.markdown"),
                          QStringLiteral("*.mdown"), QStringLiteral("*.txt") },
                        QDir::Files, QDirIterator::Subdirectories);
        constexpr int kMaxFiles = 2000, kMaxTotal = 200, kMaxPerFile = 10;
        int fileCount = 0;
        while (it.hasNext() && fileCount < kMaxFiles && results.size() < kMaxTotal) {
            const QString p = QDir::cleanPath(it.next());
            ++fileCount;
            bool ok = false;
            const QString content = FileService::readFile(p, &ok);
            if (!ok)
                continue;
            const QStringList lines = content.split(QLatin1Char('\n'));
            int hitsInFile = 0;
            for (int i = 0; i < lines.size()
                            && hitsInFile < kMaxPerFile
                            && results.size() < kMaxTotal; ++i) {
                const QString line = lines[i].trimmed();
                if (!line.isEmpty() && line.toLower().contains(lower)) {
                    results.append({ p,
                                     QStringLiteral("%1:%2").arg(i + 1).arg(line.left(80)) });
                    ++hitsInFile;
                }
            }
        }
        return results;
    });
    m_searchWatcher.setFuture(future);
}

void MainWindow::onSearchFileResult(const QString &path, const QString &lineText)
{
    // lineText 形如 "12:正文..."
    QString snippet = lineText;
    const int colon = lineText.indexOf(QLatin1Char(':'));
    if (colon > 0)
        snippet = lineText.mid(colon + 1);
    if (openPath(path)) {
        Tab *t = currentTab();
        if (t && t->host)
            t->host->runScript(Bridge::call(QStringLiteral("scrollToText"), { snippet }));
    }
}

void MainWindow::onSearchDocResult(int blockIndex)
{
    Tab *t = currentTab();
    if (t && t->host)
        t->host->runScript(Bridge::call(QStringLiteral("scrollToBlock"), { blockIndex }));
}

// ---------------------------------------------------------------------------
// 导出
// ---------------------------------------------------------------------------

QString MainWindow::composeExportHtml(const QString &bodyHtml, const Tab &tab)
{
    const QString webDir = resourcesWebDir();
    // 主题相关的四个 CSS 按 (webDir, 主题) 缓存:导出不应每次都做 4 次盘读
    static QHash<QString, QStringList> baseCssCache;
    const QString cssKey = webDir + QLatin1Char('\n') + m_theme;
    QStringList css;
    const auto cached = baseCssCache.find(cssKey);
    if (cached != baseCssCache.end()) {
        css = *cached;
    } else {
        const QString contentTheme = m_theme == QLatin1String("dark")
                                         ? QStringLiteral("dark")
                                         : (m_theme == QLatin1String("paper")
                                                ? QStringLiteral("wechat")
                                                : QStringLiteral("light"));
        css << readAll(webDir + QStringLiteral("/vditor/dist/index.css"))
            << readAll(webDir + QStringLiteral("/vditor/dist/css/content-theme/")
                       + contentTheme + QStringLiteral(".css"))
            << readAll(webDir + QStringLiteral("/vditor/dist/js/highlight.js/styles/")
                       + (m_theme == QLatin1String("dark") ? QStringLiteral("dark")
                                                           : QStringLiteral("github"))
                       + QStringLiteral(".min.css"))
            << readAll(webDir + QStringLiteral("/themes/") + m_theme + QStringLiteral(".css"));
        baseCssCache.insert(cssKey, css);
    }
    css << katexExportCss(webDir);

    // Vditor 的颜色变量定义在 .vditor 选择器上,导出页没有该类,
    // 不内联的话正文颜色全部丢失
    const QString vars = QStringLiteral(
        ":root{--border-color:#d1d5da;--second-color:rgba(88,96,105,.36);"
        "--panel-background-color:#fff;--toolbar-background-color:#f6f8fa;"
        "--toolbar-icon-color:#586069;--textarea-background-color:#fafbfc;"
        "--textarea-text-color:#24292e;--heading-border-color:#eaecef;"
        "--blockquote-color:#6a737d;--ir-heading-color:#660e7a;"
        "--ir-title-color:#4f4f4f;--ir-bi-color:#4f4f4f;--ir-link-color:#4285f4;"
        "--ir-bracket-color:rgba(88,96,105,.36);--ir-paren-color:rgba(88,96,105,.36);"
        "--code-background:#f6f8fa;}");
    const QString paperVars = QStringLiteral(
        ":root{--border-color:#dcd7c9;--second-color:rgba(122,112,92,.36);"
        "--panel-background-color:#fffdf7;--toolbar-background-color:#f2eee2;"
        "--toolbar-icon-color:#7a705c;--textarea-background-color:#f7f4ed;"
        "--textarea-text-color:#3f3f3f;--heading-border-color:#e8e3d5;"
        "--blockquote-color:#7a705c;--ir-heading-color:#8b5e3c;"
        "--ir-title-color:#504a3f;--ir-bi-color:#504a3f;--ir-link-color:#b8860b;"
        "--ir-bracket-color:rgba(122,112,92,.36);--ir-paren-color:rgba(122,112,92,.36);"
        "--code-background:#efe9d8;}body{background:#f7f4ed;color:#3f3f3f;}");
    const QString darkVars = QStringLiteral(
        ":root{--border-color:#3c3c3c;--second-color:rgba(158,158,158,.36);"
        "--panel-background-color:#252526;--toolbar-background-color:#252526;"
        "--toolbar-icon-color:#c5c5c5;--textarea-background-color:#1e1e1e;"
        "--textarea-text-color:#d4d4d4;--heading-border-color:#37373d;"
        "--blockquote-color:#9d9d9d;--ir-heading-color:#c586c0;"
        "--ir-title-color:#d4d4d4;--ir-bi-color:#d4d4d4;--ir-link-color:#569cd6;"
        "--ir-bracket-color:rgba(158,158,158,.45);--ir-paren-color:rgba(158,158,158,.45);"
        "--code-background:#252526;}body{background:#1e1e1e;color:#d4d4d4;}");

    const QString exportCss = QStringLiteral(R"CSS(
:root{--ms-font-size:%1px;}
body.ms-export{margin:0;}
body.ms-export .ms-export-article{max-width:856px;margin:0 auto;padding:40px 28px;box-sizing:border-box;}
body.ms-export .vditor-reset{font-size:var(--ms-font-size,16px);line-height:1.75;}
body.ms-export .vditor-reset pre{margin:8px 0;padding:12px 16px;border-radius:6px;overflow:auto;line-height:inherit;background:var(--code-background,#f6f8fa)!important;}
body.ms-export .vditor-reset pre code{display:block;padding:0!important;background:transparent!important;white-space:pre;max-height:none!important;}
body.ms-export .vditor-reset code{font-family:"Cascadia Code","JetBrains Mono",Consolas,"Courier New",monospace;}
body.ms-export .vditor-reset > .language-math,body.ms-export .vditor-reset div.language-math{margin:6px 0!important;padding:2px 0;text-align:center;background:transparent!important;min-height:1.25em;}
body.ms-export .vditor-reset > .language-math .katex-display,body.ms-export .vditor-reset div.language-math .katex-display{margin:0!important;display:flex!important;align-items:center;justify-content:center;}
body.ms-export .vditor-reset .katex-display{margin:0.2em 0!important;}
body.ms-export .vditor-reset img{max-width:100%;height:auto;}
body.ms-export .hljs-keyword,body.ms-export .hljs-selector-tag{color:#0057d9;}
body.ms-export .hljs-title,body.ms-export .hljs-title.function_,body.ms-export .hljs-title.class_,body.ms-export .hljs-built_in,body.ms-export .hljs-type{color:#16825d;}
body.ms-export .hljs-string,body.ms-export .hljs-attr{color:#b31d28;}
body.ms-export .hljs-number,body.ms-export .hljs-literal{color:#0550ae;}
body.ms-export .hljs-comment,body.ms-export .hljs-quote{color:#6a737d;font-style:italic;}
body.ms-export .hljs-meta{color:#6a737d;}
body.ms-export[data-ms-theme="dark"] .hljs-keyword,body.ms-export[data-ms-theme="dark"] .hljs-selector-tag{color:#569cd6;}
body.ms-export[data-ms-theme="dark"] .hljs-title,body.ms-export[data-ms-theme="dark"] .hljs-built_in,body.ms-export[data-ms-theme="dark"] .hljs-type{color:#4ec9b0;}
body.ms-export[data-ms-theme="dark"] .hljs-string{color:#ce9178;}
body.ms-export[data-ms-theme="dark"] .hljs-number,body.ms-export[data-ms-theme="dark"] .hljs-literal{color:#b5cea8;}
body.ms-export[data-ms-theme="dark"] .hljs-comment{color:#6a9955;}
@media print{
body.ms-export .ms-export-article{max-width:none;padding:0;}
/* Slice the original box at page breaks: only the first/last fragments have
   borders. Visible overflow lets long code blocks paginate normally. */
body.ms-export .vditor-reset pre:has(>code){border:0;border-top:0.75pt solid #000;border-bottom:0.75pt solid #000;border-radius:0;box-decoration-break:slice;-webkit-box-decoration-break:slice;overflow:visible;break-inside:auto;}
}
)CSS").arg(m_fontSize);
    css << exportCss;

    const QString title = !tab.path.isEmpty() ? QFileInfo(tab.path).completeBaseName()
                                                 : tr("未命名");
    const QString varBlock = (m_theme == QLatin1String("dark"))
                                 ? darkVars
                                 : (m_theme == QLatin1String("paper") ? paperVars : vars);
    return QStringLiteral("<!DOCTYPE html>\n<html lang=\"zh-CN\">\n<head>\n<meta charset=\"utf-8\">\n")
        + QStringLiteral("<title>") + title.toHtmlEscaped() + QStringLiteral("</title>\n")
        + QStringLiteral("<style>\n") + varBlock + QLatin1Char('\n')
        + css.join(QLatin1Char('\n')) + QStringLiteral("\n</style>\n</head>\n")
        + QStringLiteral("<body class=\"ms-export\" data-ms-theme=\"") + m_theme.toHtmlEscaped()
        + QStringLiteral("\">\n<article class=\"vditor-reset ms-export-article\">\n")
        + bodyHtml
        + QStringLiteral("\n</article>\n</body>\n</html>\n");
}

void MainWindow::exportHtml() { requestExport(kExportHtml); }
void MainWindow::exportPdf()  { requestExport(kExportPdf); }
void MainWindow::exportWord() { requestExport(kExportWord); }

void MainWindow::requestExport(int kind)
{
    Tab *t = currentTab();
    if (!t || !t->host)
        return;
    if (m_exportBusy) {
        statusBar()->showMessage(tr("正在导出，请等待当前导出完成"), 3000);
        return;
    }
    m_exportBusy = true;
    m_exportSource = t->host;
    m_exportKind = kind;
    const int request = ++m_exportRequest;
    t->host->runScript(QStringLiteral("window.msbridge.requestHtml(%1)").arg(request));
    QTimer::singleShot(30000, this, [this, request] {
        if (m_exportBusy && m_exportRequest == request && m_exportKind >= 0) {
            resetExport();
            statusBar()->showMessage(tr("导出等待超时，请重试"), 5000);
        }
    });
}

void MainWindow::resetExport()
{
    m_exportBusy = false;
    m_exportKind = -1;
    m_exportSource.clear();
    m_exportPdfTarget.clear();
    m_exportNavPending = false;
    if (m_exportHost) {
        m_exportHost->setAlwaysVisible(false);
        m_exportHost->hide();
    }
}

void MainWindow::finishExport(const QString &bodyHtml, Tab *tab)
{
    if (m_exportKind < 0 || !tab)
        return;
    const int kind = m_exportKind;
    m_exportKind = -1;

    // 用"回传 html 的那个标签",而不是 currentTab():请求与回传之间
    // 用户可能已切标签,否则导出文件名与图片映射会张冠李戴
    const Tab docTab = *tab; // Stable across modal dialogs and tab-vector moves.
    const QString base = docTab.path.isEmpty()
                             ? (docTab.titleHint.isEmpty() ? tr("未命名")
                                                           : docTab.titleHint)
                             : QFileInfo(docTab.path).completeBaseName();
    QString filter;
    if (kind == kExportPdf)
        filter = tr("PDF 文档 (*.pdf)");
    else if (kind == kExportWord)
        filter = tr("Word 文档 (*.doc)");
    else
        filter = tr("HTML 页面 (*.html *.htm)");

    QString target = QFileDialog::getSaveFileName(this, tr("导出"),
        (docTab.path.isEmpty() ? m_files->defaultSaveDir() : QFileInfo(docTab.path).absolutePath())
            + QStringLiteral("/")
            + base + (kind == kExportPdf ? QStringLiteral(".pdf")
                       : kind == kExportWord ? QStringLiteral(".doc")
                                             : QStringLiteral(".html")),
        filter);
    if (target.isEmpty()) {
        resetExport();
        return;
    }
    if (QFileInfo(target).suffix().isEmpty())
        target += kind == kExportPdf ? QStringLiteral(".pdf")
            : kind == kExportWord ? QStringLiteral(".doc") : QStringLiteral(".html");

    QString doc = composeExportHtml(bodyHtml, docTab);

    // 图片路径统一还原/转换:
    //   内部虚拟域 URL(imgpool*/doc*)与原始绝对路径都可能出现
    //   PDF      -> 虚拟域 URL 保留(导出页已映射),绝对路径转为虚拟域 URL
    //   HTML/Word-> 全部还原为 file:/// 本地路径
    {
        QList<ImgMap> all = m_imgMaps;
        if (!docTab.docHost.isEmpty()) all.append({docTab.docHost, docTab.docDir});
        for (const Tab &t : m_tabs) {
            if (!t.docHost.isEmpty() && !t.docDir.isEmpty())
                all.append({ t.docHost, t.docDir });
        }
        for (const ImgMap &m : all) {
            const QString hostUrl = QStringLiteral("https://") + m.host + QLatin1Char('/');
            const QString fileUrl = QUrl::fromLocalFile(QDir(m.dir).absolutePath()
                                    + QLatin1Char('/')).toString(QUrl::FullyEncoded);
            if (kind == kExportPdf) {
                doc.replace(fileUrl, hostUrl);
            } else {
                doc.replace(hostUrl, fileUrl);
            }
        }
    }

    if (kind == kExportPdf) {
        // 写临时页 → 导出页加载 → 打印 PDF
        const QString exeDir = QCoreApplication::applicationDirPath();
        QDir().mkpath(exeDir + QStringLiteral("/export-tmp"));
        m_exportAlternates = !m_exportAlternates;
        const QString name = QStringLiteral("/export-%1.html")
                                 .arg(m_exportAlternates ? 'a' : 'b');
        const QByteArray bytes = doc.toUtf8();
        QFile f(exeDir + QStringLiteral("/export-tmp") + name);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)
            || f.write(bytes) != bytes.size()) {
            QMessageBox::warning(this, tr("导出失败"), tr("无法写入临时导出页"));
            resetExport();
            return;
        }
        qWarning() << "Mswrite: 临时导出页已写" << (exeDir + "/export-tmp" + name)
                << doc.size() << "字符";
        m_exportPdfTarget = target;
        ensureExportHost();
        if (!docTab.docHost.isEmpty()) m_exportHost->addHostMapping(docTab.docHost, docTab.docDir);
        // 映射必须在每次导出时补:导出宿主只创建一次,映射是创建时登记的,
        // 换文档(或另存为换了目录)后 docHost 变了,不补就解析不到图片。
        // 所有标签一起映射:正文里可能引用其它已打开文档目录下的绝对路径
        for (const Tab &t : m_tabs) {
            if (!t.docHost.isEmpty() && !t.docDir.isEmpty())
                m_exportHost->addHostMapping(t.docHost, t.docDir);
        }
        const QString url = QStringLiteral("https://export.local/export-%1.html")
                                .arg(m_exportAlternates ? 'a' : 'b');
        m_exportHost->setAlwaysVisible(true);
        // 导出页必须在"显示"状态下渲染(隐藏会被 WebView2 挂起 → 空白 PDF),
        // 但不必铺满窗口:放右下角一个小窗,用户只瞥见一个小方块。
        // 打印版式由 A4 页面盒(PrintSettings)决定,与视口大小无关
        const int ew = 240, eh = 160;
        m_exportHost->setGeometry(qMax(0, width() - ew - 16),
                                  qMax(0, height() - eh - 16), ew, eh);
        m_exportHost->show();
        m_exportHost->raise();
        statusBar()->showMessage(tr("正在导出 PDF..."), 0);
        if (m_exportHost->isPageReady()) {
            m_exportHost->navigate(url);
        } else {
            m_exportNavPending = true; // 宿主就绪后由 pageReady 驱动
            m_exportNavUrl = url;
        }
        QTimer::singleShot(30000, this, [this, request = m_exportRequest] {
            if (m_exportBusy && m_exportRequest == request && !m_exportPdfTarget.isEmpty()) {
                resetExport();
                statusBar()->showMessage(tr("PDF 导出页加载超时，请重试"), 5000);
            }
        });
        return;
    }

    // HTML / Word:直接写盘
    QSaveFile f(target);
    if (!f.open(QIODevice::WriteOnly)) {
        resetExport();
        QMessageBox::warning(this, tr("导出失败"), tr("无法写入:\n%1").arg(target));
        return;
    }
    QString out = doc;
    if (kind == kExportWord) {
        // Word 可直接打开的 HTML(加 Office 命名空间)
        out.replace(QLatin1String("<html lang=\"zh-CN\">"),
                    QStringLiteral("<html xmlns:o=\"urn:schemas-microsoft-com:office:office\" "
                                   "xmlns:w=\"urn:schemas-microsoft-com:office:word\" "
                                   "xmlns=\"http://www.w3.org/TR/REC-html40\" lang=\"zh-CN\">"));
    }
    const QByteArray bytes = out.toUtf8();
    const bool ok = f.write(bytes) == bytes.size() && f.commit();
    resetExport();
    statusBar()->showMessage(ok ? tr("已导出 %1").arg(target) : tr("导出失败:无法写入 %1").arg(target), 5000);
}

void MainWindow::ensureExportHost()
{
    if (m_exportHost)
        return;
    const QString exeDir = QCoreApplication::applicationDirPath();
    const QString tmpDir = exeDir + QStringLiteral("/export-tmp");
    QDir().mkpath(tmpDir);

    m_exportHost = new WebViewHost(this);
    m_exportHost->setAlwaysVisible(true);  // 逻辑可见:防止隐藏态挂起渲染
    m_exportHost->resize(240, 160);        // 小视口即可(打印版式由 A4 页面盒决定)
    m_exportHost->addHostMapping(QStringLiteral("export.local"), tmpDir);
    for (const ImgMap &m : m_imgMaps)
        m_exportHost->addHostMapping(m.host, m.dir);
    // 当前文档目录:导出页加载 ./assets 图片(经 doc{n}.local)用。
    // 注意每次导出都会在 finishExport 里补映射(换文档后 docHost 会变)
    if (const Tab *ct = currentTab(); ct && !ct->docHost.isEmpty() && !ct->docDir.isEmpty())
        m_exportHost->addHostMapping(ct->docHost, ct->docDir);
    m_exportHost->hide(); // 待导出时再显示
    m_exportHost->start(exeDir + QStringLiteral("/webview-data"),
                        QString::fromLatin1(kVirtualHost), resourcesWebDir(),
                        QStringLiteral("about:blank"));

    connect(m_exportHost, &WebViewHost::navigated, this, [this](bool ok) {
        qWarning() << "Mswrite: 导出页 navigated ok=" << ok
                << "url=" << m_exportHost->currentUrl()
                << "待打印=" << !m_exportPdfTarget.isEmpty();
        if (!m_exportPdfTarget.isEmpty()
            && m_exportHost->currentUrl().contains(QLatin1String("export.local/export-"))) {
            if (!ok) {
                resetExport();
                statusBar()->showMessage(tr("PDF 导出页加载失败，请重试"), 5000);
                return;
            }
            // 硬闸:当前页确为本次导出页才打印(防止打印到旧页/空白页)
            const QString target = m_exportPdfTarget;
            m_exportPdfTarget.clear();
            qWarning() << "Mswrite: 进入渲染等待,目标" << target;
            // 先等页面真实渲染(字体/图片/布局),再打印,否则输出空白
            m_exportHost->waitRendered([this, target] {
                qWarning() << "Mswrite: 渲染等待完成,开始打印";
                m_exportHost->printToPdf(target, [this, target](bool okPdf, HRESULT hr) {
                    if (okPdf) {
                        qWarning() << "Mswrite: PDF 导出成功" << target;
                        statusBar()->showMessage(tr("已导出 %1").arg(target), 4000);
                    } else {
                        qWarning() << "Mswrite: PDF 导出失败,错误码"
                                   << QStringLiteral("0x%1").arg(uint(hr), 8, 16, QChar('0'))
                                   << "目标:" << target;
                        statusBar()->showMessage(tr("导出 PDF 失败(错误码见 mswrite.log)"), 4000);
                    }
                    // 降级为可隐藏:否则 Chromium 认为永远可见,导出后持续耗 CPU
                    resetExport();
                });
            });
        }
    });
    connect(m_exportHost, &WebViewHost::pageReady, this, [this] {
        if (m_exportNavPending) {
            m_exportNavPending = false;
            m_exportHost->navigate(m_exportNavUrl);
        }
    });
}

// ---------------------------------------------------------------------------
// 组合键
// ---------------------------------------------------------------------------

bool MainWindow::handleAccelerator(int vk, bool ctrl, bool shift, bool alt)
{
    if (!ctrl)
        return false; // 无 Ctrl 组合:放行(编辑器级在 bridge.js 处理)
    // Ctrl+Alt 组合:页面级 Ctrl+=/-/0 已归整页缩放(Typora 语义),
    // 字号调整退到 Ctrl+Alt
    if (alt) {
        switch (vk) {
        case 0xBB: return changeFontSize(+1), true;     // Ctrl+Alt+= 放大字号
        case 0xBD: return changeFontSize(-1), true;     // Ctrl+Alt+- 缩小字号
        case '0':  return changeFontSize(0), true;      // Ctrl+Alt+0 重置字号
        default:   return false;                        // 其余 Alt 组合放行
        }
    }

    // 注意:编辑区焦点在 WebView2 的 Chromium 子窗口上,Qt 收不到按键,
    // 菜单里的 QKeySequence 在编辑区不生效 —— 快捷键必须在这里(或
    // bridge.js)登记,否则就是"菜单写着、按下去没反应"
    if (shift) {
        switch (vk) {
        case 'N':  return newWindow(), true;            // 新建窗口
        case 'S':  return saveFileAs(), true;           // 另存为
        case 'O':  return openFolder(), true;           // 打开文件夹
        case 'T':  return reopenClosedFile(), true;     // 重新打开关闭的文件
        case 'F':  return globalSearch(), true;         // 全局搜索
        case 'E':  return exportHtml(), true;           // 导出 HTML
        case 'P':  return exportPdf(), true;            // 导出 PDF
        case 'W':  return exportWord(), true;           // 导出 Word
        case '1':  return toggleOutlinePanel(), true;   // 大纲
        case '2':  return quickOpen(), true;            // 文档列表(快速打开)
        default:   return false;
        }
    }
    // PDF 标签:只处理文件级快捷键(打开/关闭/退出),编辑/保存/模式切换全跳过
    {
        Tab *ct = currentTab();
        const bool pdfTab = ct && ct->pdf;
        if (pdfTab) {
            switch (vk) {
            case 'O': return openFile(), true;
            case 'P': return quickOpen(), true;
            case 'W': return closeTab(currentTabIndex()), true;
            case 'Q': return close(), true;
            case 0xBB: { // Ctrl+= PDF 放大
                ct->pdf->zoomIn();
                return true;
            }
            case 0xBD: { // Ctrl+- PDF 缩小
                ct->pdf->zoomOut();
                return true;
            }
            case 'C': return ct->pdf->copySelection(), true;
            case 'A': return ct->pdf->selectAll(), true;
            case 'F': return ct->pdf->startSearch(), true;
            case '0': { // Ctrl+0 适宽
                ct->pdf->zoomFitWidth();
                return true;
            }
            default: return false;
            }
        }
    }
    switch (vk) {
    case 'S':    return saveFile(), true;
    case 'O':    return openFile(), true;
    case 'P':    return quickOpen(), true;
    case 'N':    return newTab(), true;                 // 新建(标签)
    case 'W':    return closeTab(currentTabIndex()), true;
    case 'Q':    return close(), true;
    case 'F': {  // Ctrl+F:转发给页面顶部查找栏(当前文档,输入即定位)
        Tab *ft = currentTab();
        if (ft) ft->host->runScript(QStringLiteral("window.msbridge.openFind()"));
        return true;
    }
    case 0xBF:   return toggleSourceMode(), true;       // Ctrl+/ 源码模式
    case 0xBB: {  // Ctrl+= 整页放大(Typora 语义)
        Tab *zt = currentTab();
        if (zt) applyZoom(*zt, zt->zoom + 0.05);
        return true;
    }
    case 0xBD: {  // Ctrl+- 整页缩小
        Tab *zt = currentTab();
        if (zt) applyZoom(*zt, zt->zoom - 0.05);
        return true;
    }
    case '0': {   // Ctrl+0 重置整页缩放为 100%
        Tab *zt = currentTab();
        if (zt) applyZoom(*zt, 1.0);
        return true;
    }
    case 0xBC:   return showPrefs(), true;              // Ctrl+, 偏好设置
    default:     return false;                          // 其余放行给页面
    }
}

// ---------------------------------------------------------------------------
// AI 写作助手
// ---------------------------------------------------------------------------

// 懒创建 AI 独立窗口:启动零开销,首次打开才建线程。
// 窗口无父(与主窗口解绑):主窗口最小化/切后台都不影响 AI 使用
void MainWindow::ensureAiDock()
{
    if (m_aiDock)
        return;
    m_aiDock = new AiChatDock(); // 故意不传父窗口
    // 无已保存几何(首次/新机器)时,摆在主窗口右侧外侧;有则恢复用户上次的位置
    if (QSettings().value(QStringLiteral("aiGeometry")).toByteArray().isEmpty()) {
        const QRect mg = geometry();
        const int w = 883, gap = 12;
        QPoint pos(mg.right() + gap, mg.y() + 60);
        if (QScreen *scr = screen()) {
            const QRect avail = scr->availableGeometry();
            if (pos.x() + w > avail.right())
                pos.setX(qMax(avail.left(), mg.left() - w - gap));
        }
        m_aiDock->move(pos);
    }
    m_aiDock->hide();
    m_aiDock->setDocumentReader(
        [this](const QJsonObject &request,std::function<void(AiDocumentResult)> cb) { readAiDocument(request,std::move(cb)); });
    m_aiDock->setContextProvider([this]{return aiDocumentContext();});
    m_aiDock->setInsertHandler(
        [this](const QString &id,const QString &text) { return insertAiText(id,text); });
    connect(m_aiDock, &AiChatDock::configRequested, this, [this] { openAiConfig(); });
    connect(m_aiDock, &AiChatDock::skillsRequested, this, &MainWindow::openAiSkills);
    // 菜单勾选态跟随窗口可见性
    connect(m_aiDock, &AiChatDock::visibilityChanged, this, [this](bool visible) {
        if (m_aiToggleAction && m_aiToggleAction->isChecked() != visible)
            m_aiToggleAction->setChecked(visible);
    });
    applyCurrentAiProvider();
}

void MainWindow::toggleAiDock()
{
    ensureAiDock();
    m_aiDock->setVisible(!m_aiDock->isVisible());
    if (m_aiToggleAction && m_aiToggleAction->isChecked() != m_aiDock->isVisible())
        m_aiToggleAction->setChecked(m_aiDock->isVisible());
}

void MainWindow::openAiConfig()
{
    AiConfigDialog dlg(&m_aiStore, this);
    dlg.exec();
    applyCurrentAiProvider();
}

// 连接诊断:TLS + 网关连通,后台线程执行(最多 6 秒),弹窗给出结论
void MainWindow::runAiDoctor()
{
    const AiProvider p = m_aiStore.current() ? *m_aiStore.current() : AiProvider();
    if (p.apiKey.isEmpty()) {
        QMessageBox::information(this, tr("AI 连接诊断"),
                                  tr("还没有配置当前供应商,先在「AI 供应商」里添加或导入。"));
        return;
    }
    statusBar()->showMessage(tr("AI 连接诊断中…"), 0);
    auto *watcher = new QFutureWatcher<AiDoctor::Result>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher] {
        watcher->deleteLater();
        statusBar()->showMessage(tr("AI 诊断完成"), 2500);
        QMessageBox::information(this, tr("AI 连接诊断"),
                                 AiDoctor::format(watcher->result()));
    });
    watcher->setFuture(QtConcurrent::run([p] { return AiDoctor::run(p); }));
}

void MainWindow::applyCurrentAiProvider()
{
    if (!m_aiDock)
        return;
    const AiProvider *p = m_aiStore.current();
    if (p)
        m_aiDock->applyProvider(*p);
    else
        m_aiDock->setNoProvider();
}

// AI 写入当前文档:走页面桥的 insertText(光标处插入,Lute 即时渲染,
// 自动触发 changed → 脏标记 + 停笔 2 秒自动保存)
QString MainWindow::insertAiText(const QString &documentId,const QString &text)
{
    Tab *t = currentTab();
    if (!t)
        return QStringLiteral("错误:当前没有打开的文档");
    if(t->aiId!=documentId)
        return QStringLiteral("错误:当前文档已切换，请重新发送指令");
    if (t->pdf)
        return QStringLiteral("错误:当前标签是 PDF,只能编辑 Markdown 文档");
    t->host->runScript(Bridge::call(QStringLiteral("insertText"), { text }));
    statusBar()->showMessage(tr("AI 已插入 %1 字").arg(text.size()), 3000);
    return QStringLiteral("Inserted %1 characters at the caret.")
               .arg(text.size());
}

// 异步读当前文档全文(window.mswValue 由 bridge.js 暴露)
QJsonObject MainWindow::aiDocumentContext()
{
    Tab *t = currentTab();
    if(!t) return {};
    return {{"id",t->aiId},{"name",t->path.isEmpty() ? tr("未命名文档") : QFileInfo(t->path).fileName()},
            {"kind",t->pdf ? "pdf" : "markdown"},{"page_count",t->pdf ? t->pdf->pageCount() : 0},
            {"current_page",t->pdf ? t->pdf->currentPage()+1 : 0}};
}

void MainWindow::openAiSkills()
{
    UiDialogs::showSkills(this,m_theme);
}

void MainWindow::readAiDocument(const QJsonObject &request,std::function<void(AiDocumentResult)> cb)
{
    Tab *t=currentTab();
    const QString id=request.value(QStringLiteral("document_id")).toString();
    if(!t || t->aiId!=id) {
        cb({QStringLiteral("Error: the active document changed; start a new turn for the current file."),{}});
        return;
    }
    const QString name=aiDocumentContext().value(QStringLiteral("name")).toString();
    if(t->pdf) {
        auto *pdf=t->pdf;
        const int start=request.value("page").toInt(pdf->currentPage()+1)-1;
        if(start<0 || start>=pdf->pageCount()) {cb({QStringLiteral("Error: page out of range"),{}});return;}
        const int end=qMin(pdf->pageCount(),start+qBound(1,request.value("page_count").toInt(2),4));
        AiDocumentResult result;
        result.text=tr("Document: %1. Pages %2-%3 of %4.\n").arg(name).arg(start+1).arg(end).arg(pdf->pageCount());
        for(int page=start;page<end;++page) {
            const QString text=pdf->pageText(page);
            result.text+=QStringLiteral("\n[Page %1]\n%2\n").arg(page+1).arg(text.isEmpty() ? QStringLiteral("No extractable text; inspect the page image.") : text.left(50000));
            if(text.size()>50000) result.text+=QStringLiteral("[Page text truncated at 50000 characters]\n");
            if((request.value("include_images").toBool() || text.trimmed().isEmpty()) && result.images.size()<2) {
                const QImage image=pdf->pageImage(page);
                QByteArray bytes;QBuffer buffer(&bytes);buffer.open(QIODevice::WriteOnly);
                if(image.save(&buffer,"PNG")) result.images.append({QStringLiteral("%1 / page %2").arg(name).arg(page+1),"image/png",QString::fromLatin1(bytes.toBase64())});
            }
        }
        if(end<pdf->pageCount()) result.text+=QStringLiteral("\nMore pages remain. Next page: %1.\n").arg(end+1);
        result.text+=QStringLiteral("Images, when requested, cover at most two pages. Request further pages separately.");
        cb(std::move(result));return;
    }
    if(!t->host->isPageReady()) {cb({QStringLiteral("Error: editor is still loading; retry shortly."),{}});return;}
    t->host->evalWithResult(
        QStringLiteral("window.mswValue ? window.mswValue() : ''"),
        [guard=QPointer<MainWindow>(this),cb,id,request,name](const QString &v) {
            if(!guard || !guard->currentTab() || guard->currentTab()->aiId!=id) {cb({QStringLiteral("Error: the active document changed during reading."),{}});return;}
            const QStringList lines=unquoteJsonString(v).split('\n');
            const int start=request.value("start_line").toInt(1)-1;
            if(start<0 || start>=lines.size()) {cb({QStringLiteral("Error: line out of range"),{}});return;}
            const int end=qMin(int(lines.size()),start+qBound(1,request.value("line_count").toInt(1000),2000));
            QString text=QStringLiteral("Document: %1. Lines %2-%3 of %4.\n").arg(name).arg(start+1).arg(end).arg(lines.size());
            for(int i=start;i<end;++i) text+=QStringLiteral("%1: %2\n").arg(i+1).arg(lines[i]);
            if(end<lines.size()) text+=QStringLiteral("More lines remain. Next start_line: %1.\n").arg(end+1);
            cb({text,{}});
        });
}

// ---------------------------------------------------------------------------
// 退出
// ---------------------------------------------------------------------------

void MainWindow::closeEvent(QCloseEvent *event)
{
    if(m_flushingSaves) {event->ignore();return;}
    if(m_autoSave) {
        QVector<WebViewHost *> hosts;
        for(const Tab &tab:m_tabs)if(tab.host && (tab.dirty || tab.awaitingContent))hosts.append(tab.host);
        if(!hosts.isEmpty())flushSaves(hosts);
    }
    // 收集脏标签
    QVector<int> dirty;
    for (int i = 0; i < m_tabs.size(); ++i)
        if (m_tabs[i].dirty)
            dirty.append(i);

    if (!dirty.isEmpty()) {
        // 先问再动手:旧实现先给未命名文档弹"另存为"、再问保存/放弃,
        // 于是想"放弃"也得先过一遍另存为,顺序是反的
        QStringList names;
        for(int i:dirty) {
            names.append(m_tabs[i].path.isEmpty() ? tr("未命名文档") : QFileInfo(m_tabs[i].path).fileName());
            m_tabs[i].autoSave->stop();
        }
        const auto ret = UiDialogs::confirmSave(this,m_theme,names,true);
        if (ret == UiDialogs::SaveChoice::Cancel) {
            event->ignore();
            return;
        }
        if (ret == UiDialogs::SaveChoice::Save) {
            // 用 host 指针而不是下标:另存为若关掉了另一个标签,QVector 会挪位,
            // 预先记下的 dirty 下标会指到别的文档
            QVector<WebViewHost *> dirtyHosts;
            for (int i : dirty)
                dirtyHosts.append(m_tabs[i].host);
            for (WebViewHost *h : dirtyHosts) {
                Tab *tab = tabForHost(h);
                if (!tab)
                    continue;
                if (tab->path.isEmpty()) {
                    m_tabbar->setCurrentIndex(indexOfHost(h));
                    if (!saveFileAs()) {
                        event->ignore();
                        return;
                    }
                }
            }
            if (!flushSaves(dirtyHosts)) {
                statusBar()->showMessage(tr("保存未完成，已取消退出。请检查保存状态，或另存为后重试。"));
                event->ignore();
                return;
            }
        }
    }

    m_shuttingDown = true;
    for(Tab &tab:m_tabs)if(tab.autoSave)tab.autoSave->stop();

    // 回收独立 AI 窗口(停线程;它是无父窗口,不随主窗口自动销毁)
    if (m_aiDock) {
        delete m_aiDock;
        m_aiDock = nullptr;
    }

    // 记住布局与打开的文件(几何异常时不落盘,防止坏值反复复活)
    {
        const QRect g = normalGeometry();
        const QRect avail = this->screen()->availableGeometry();
        const bool sane = g.width() >= 600 && g.height() >= 400 && avail.intersects(g);
        QSettings geo;
        if (sane)
            geo.setValue(QStringLiteral("geometry"), saveGeometry());
        else
            geo.remove(QStringLiteral("geometry"));
        geo.setValue(QStringLiteral("windowState"), saveState());
        // 每文档一粒缩放键,不清理就永久累积;顺手删掉文件已不存在的
        const QStringList keys = geo.allKeys();
        for (const QString &key : keys) {
            const bool isZoom = key.startsWith(QStringLiteral("zoom/"));
            const bool isDocLang = key.startsWith(QStringLiteral("doclang/"));
            if (!isZoom && !isDocLang)
                continue;
            const QString file = key.mid(isZoom ? 5 : 8);
            if (!QFileInfo::exists(file))
                geo.remove(key);
        }
    }
    if (const Tab *t = currentTab(); t && !t->path.isEmpty())
        m_files->setLastOpenedFile(t->path);

    QMainWindow::closeEvent(event);
}
