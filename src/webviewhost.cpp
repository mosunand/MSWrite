#include "webviewhost.h"

#include <QResizeEvent>
#include <QShowEvent>
#include <QHideEvent>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QList>
#include <QPair>
#include <QMouseEvent>
#include <QTimer>
#include <QUrl>
#include <QPointer>
#include <QTextBrowser>
#include <QLabel>
#include <QVBoxLayout>

#include <functional>
#include <atomic>

#include <windows.h>
#include <objbase.h>
#include "WebView2.h"

// ---------------------------------------------------------------------------
// MinGW 的 __uuidof 只声明不定义,需按官方 __CRT_UUID_DECL 模式手动实例化
// (GUID 值逐字提取自本仓库内的 WebView2.h)
// ---------------------------------------------------------------------------
__CRT_UUID_DECL(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler, 0x4e8a3389, 0xc9d8, 0x4bd2, 0xb6, 0xb5, 0x12, 0x4f, 0xee, 0x6c, 0xc1, 0x4d)
__CRT_UUID_DECL(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler, 0x6c4819f3, 0xc9b7, 0x4260, 0x81, 0x27, 0xc9, 0xf5, 0xbd, 0xe7, 0xf6, 0x8c)
__CRT_UUID_DECL(ICoreWebView2WebMessageReceivedEventHandler, 0x57213f19, 0x00e6, 0x49fa, 0x8e, 0x07, 0x89, 0x8e, 0xa0, 0x1e, 0xcb, 0xd2)
__CRT_UUID_DECL(ICoreWebView2NavigationCompletedEventHandler, 0xd33a35bf, 0x1c49, 0x4f98, 0x93, 0xab, 0x00, 0x6e, 0x05, 0x33, 0xfe, 0x1c)
__CRT_UUID_DECL(ICoreWebView2ExecuteScriptCompletedHandler, 0x49511172, 0xcc67, 0x4bca, 0x99, 0x23, 0x13, 0x71, 0x12, 0xf4, 0xc4, 0xcc)
__CRT_UUID_DECL(ICoreWebView2AcceleratorKeyPressedEventHandler, 0xb29c7e28, 0xfa79, 0x41a8, 0x8e, 0x44, 0x65, 0x81, 0x1c, 0x76, 0xdc, 0xb2)
__CRT_UUID_DECL(ICoreWebView2_3, 0xA0D6DF20, 0x3B92, 0x416D, 0xAA, 0x0C, 0x43, 0x7A, 0x9C, 0x72, 0x78, 0x57)
__CRT_UUID_DECL(ICoreWebView2_2, 0x9E8F0CF8, 0xE670, 0x4B5E, 0xB2, 0xBC, 0x73, 0xE0, 0x61, 0xE3, 0x18, 0x4C)
__CRT_UUID_DECL(ICoreWebView2Settings3, 0xfdb5ab74, 0xaf33, 0x4854, 0x84, 0xf0, 0x0a, 0x63, 0x1d, 0xeb, 0x5e, 0xba)
__CRT_UUID_DECL(ICoreWebView2WebResourceRequestedEventHandler, 0xab00b74c, 0x15f1, 0x4646, 0x80, 0xe8, 0xe7, 0x63, 0x41, 0xd2, 0x5d, 0x71)
__CRT_UUID_DECL(ICoreWebView2_7, 0x79c24d83, 0x09a3, 0x45ae, 0x94, 0x18, 0x48, 0x7f, 0x32, 0xa5, 0x87, 0x40)
__CRT_UUID_DECL(ICoreWebView2Environment6, 0x7cecdbf4, 0xd694, 0x4e2f, 0xa0, 0x82, 0x24, 0x4e, 0x99, 0xd4, 0x80, 0x7f)
__CRT_UUID_DECL(ICoreWebView2PrintToPdfCompletedHandler, 0xccf1ef04, 0xfd8e, 0x4d5f, 0xb2, 0xde, 0x09, 0x83, 0xe4, 0x1b, 0x8c, 0x36)

namespace {

// waitRendered 的兜底上限:超过则认为渲染已无意义,放行导出
constexpr int kRenderTimeoutMs = 8000;

// -----------------------------------------------------------------------
// COM 事件处理器:纯手写 COM,不依赖 WRL
// -----------------------------------------------------------------------

// JS postMessage 到达
class MessageHandler final : public ICoreWebView2WebMessageReceivedEventHandler {
public:
    std::function<void(const QString &)> onMessage;

    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_ref; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = --m_ref;
        if (!n) delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, __uuidof(IUnknown)) ||
            IsEqualIID(riid, __uuidof(ICoreWebView2WebMessageReceivedEventHandler))) {
            *ppv = static_cast<ICoreWebView2WebMessageReceivedEventHandler *>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2 *,
                                    ICoreWebView2WebMessageReceivedEventArgs *args) override {
        LPWSTR raw = nullptr;
        if (SUCCEEDED(args->get_WebMessageAsJson(&raw)) && raw) {
            if (onMessage) onMessage(QString::fromWCharArray(raw));
            CoTaskMemFree(raw);
        }
        return S_OK;
    }
private:
    std::atomic<ULONG> m_ref{1};
};

// 环境创建完成(共享环境,回调分发到所有等待实例)
class InitialScriptHandler final : public ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler {
public:
    std::function<void(HRESULT)> onDone;
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_ref; }
    ULONG STDMETHODCALLTYPE Release() override { const auto n = --m_ref; if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override {
        if (!out) return E_POINTER;
        if (IsEqualIID(id, IID_IUnknown) || IsEqualIID(id, IID_ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler)) {
            *out = static_cast<ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler *>(this);
            AddRef(); return S_OK;
        }
        *out = nullptr; return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT result, LPCWSTR) override { if (onDone) onDone(result); return S_OK; }
private:
    std::atomic<ULONG> m_ref{1};
};

class EnvCreatedHandler final : public ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler {
public:
    std::function<void(ICoreWebView2Environment *)> onDone;

    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_ref; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = --m_ref;
        if (!n) delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, __uuidof(IUnknown)) ||
            IsEqualIID(riid, __uuidof(ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler))) {
            *ppv = static_cast<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT, ICoreWebView2Environment *env) override {
        if (onDone) {
            if (env) env->AddRef();
            onDone(env); // 失败也要回调,否则 s_creating 卡死、waiters 永不重试
        }
        return S_OK;
    }
private:
    std::atomic<ULONG> m_ref{1};
};

// 控制器创建完成
class ControllerCreatedHandler final : public ICoreWebView2CreateCoreWebView2ControllerCompletedHandler {
public:
    std::function<void(HRESULT, ICoreWebView2Controller *)> onDone;

    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_ref; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = --m_ref;
        if (!n) delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, __uuidof(IUnknown)) ||
            IsEqualIID(riid, __uuidof(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler))) {
            *ppv = static_cast<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT result, ICoreWebView2Controller *ctrl) override {
        if (onDone) { if (ctrl) ctrl->AddRef(); onDone(result,ctrl); }
        return S_OK;
    }
private:
    std::atomic<ULONG> m_ref{1};
};

// 导航完成(带错误码)
class NavCompletedHandler final : public ICoreWebView2NavigationCompletedEventHandler {
public:
    std::function<void(bool, HRESULT)> onDone;

    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_ref; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = --m_ref;
        if (!n) delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, __uuidof(IUnknown)) ||
            IsEqualIID(riid, __uuidof(ICoreWebView2NavigationCompletedEventHandler))) {
            *ppv = static_cast<ICoreWebView2NavigationCompletedEventHandler *>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2 *,
                                    ICoreWebView2NavigationCompletedEventArgs *args) override {
        BOOL ok = FALSE;
        HRESULT err = S_OK;
        args->get_IsSuccess(&ok);
        args->get_WebErrorStatus(reinterpret_cast<COREWEBVIEW2_WEB_ERROR_STATUS *>(&err));
        if (onDone) onDone(ok != FALSE, err);
        return S_OK;
    }
private:
    std::atomic<ULONG> m_ref{1};
};

// ExecuteScript 完成回调(结果忽略)
class ScriptDoneHandler final : public ICoreWebView2ExecuteScriptCompletedHandler {
public:
    std::function<void(const QString &)> onResult;

    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_ref; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = --m_ref;
        if (!n) delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, __uuidof(IUnknown)) ||
            IsEqualIID(riid, __uuidof(ICoreWebView2ExecuteScriptCompletedHandler))) {
            *ppv = static_cast<ICoreWebView2ExecuteScriptCompletedHandler *>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT, LPCWSTR result) override {
        if (onResult)
            onResult(result ? QString::fromWCharArray(result) : QString());
        return S_OK;
    }
private:
    std::atomic<ULONG> m_ref{1};
};

// 加速键:交给 Qt 侧过滤器裁决
class AcceleratorHandler final : public ICoreWebView2AcceleratorKeyPressedEventHandler {
public:
    std::function<bool(UINT vk, bool ctrl, bool shift, bool alt)> onKey;

    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_ref; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = --m_ref;
        if (!n) delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, __uuidof(IUnknown)) ||
            IsEqualIID(riid, __uuidof(ICoreWebView2AcceleratorKeyPressedEventHandler))) {
            *ppv = static_cast<ICoreWebView2AcceleratorKeyPressedEventHandler *>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2Controller *,
                                    ICoreWebView2AcceleratorKeyPressedEventArgs *args) override {
        COREWEBVIEW2_KEY_EVENT_KIND kind = COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN;
        args->get_KeyEventKind(&kind);
        if (kind != COREWEBVIEW2_KEY_EVENT_KIND_KEY_DOWN &&
            kind != COREWEBVIEW2_KEY_EVENT_KIND_SYSTEM_KEY_DOWN)
            return S_OK;

        UINT vk = 0;
        args->get_VirtualKey(&vk);
        const bool ctrl  = GetKeyState(VK_CONTROL) < 0;
        const bool shift = GetKeyState(VK_SHIFT) < 0;
        const bool alt   = GetKeyState(VK_MENU) < 0;

        if (!ctrl && !alt)
            return S_OK; // 普通输入直接放行

        if (onKey && onKey(vk, ctrl, shift, alt))
            args->put_Handled(TRUE); // 仅消费 Qt 确认的组合键
        return S_OK;
    }
private:
    std::atomic<ULONG> m_ref{1};
};

// PDF 打印完成(带错误码,便于排障)
class PrintToPdfHandler final : public ICoreWebView2PrintToPdfCompletedHandler {
public:
    std::function<void(bool, HRESULT)> onDone;

    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_ref; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = --m_ref;
        if (!n) delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
        if (!ppv) return E_POINTER;
        if (IsEqualIID(riid, __uuidof(IUnknown)) ||
            IsEqualIID(riid, __uuidof(ICoreWebView2PrintToPdfCompletedHandler))) {
            *ppv = static_cast<ICoreWebView2PrintToPdfCompletedHandler *>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE Invoke(HRESULT errorCode, BOOL isSuccessful) override {
        if (onDone) onDone(isSuccessful != FALSE, errorCode);
        return S_OK;
    }
private:
    std::atomic<ULONG> m_ref{1};
};


} // namespace

// ---------------------------------------------------------------------------
// Impl:每个 WebViewHost 实例的 COM 指针;共享环境用静态成员
// ---------------------------------------------------------------------------

struct WebViewHost::Impl {
    WebViewHost *q = nullptr;

    ICoreWebView2Controller *ctrl = nullptr;
    ICoreWebView2           *web = nullptr;

    MessageHandler           *hMsg = nullptr;
    NavCompletedHandler      *hNav = nullptr;
    AcceleratorHandler       *hAcc = nullptr;

    EventRegistrationToken tokMsg{};
    EventRegistrationToken tokNav{};
    EventRegistrationToken tokAcc{};

    QString virtualHost;
    QString virtualFolder;
    QString startUrl;
    QString initialScript;
    QString curUrl;                      // 最近一次导航的 URL
    bool alwaysVisible = false;         // 导出页等隐藏场景:渲染不挂起
    double zoom = 1.0;                  // 页面缩放(控制器就绪后恢复)
    QList<QPair<QString, QString>> extraMappings; // 启动后补加的虚拟主机
    std::function<void()> renderedCb;   // waitRendered 的在途回调
    int renderRequest = 0;

    // ---- 共享环境 ----
    static ICoreWebView2Environment *s_env;
    static QList<Impl *> s_waiters;
    static QString s_envError;          // 环境/运行时加载失败原因(供 UI 提示)
    static bool s_creating;             // 创建在途:失败后允许重试,成功前禁止重入
    static HMODULE s_loaderLib;         // Loader 句柄缓存:重试不重复加载(类外初始化)

    static void ensureEnvironment(const QString &userDataFolder)
    {
        // 已有环境或正在创建:直接继续(调用方自行处理)
        if (s_env || s_creating)
            return;
        HMODULE lib = s_loaderLib ? s_loaderLib : LoadLibraryW(L"WebView2Loader.dll");
        if (!lib) {
            s_envError = QStringLiteral("无法加载 WebView2Loader.dll(错误码 %1)")
                             .arg(GetLastError());
            qCritical() << "Mswrite:" << s_envError;
            return;
        }
        using PFN_CreateEnv = HRESULT(STDMETHODCALLTYPE *)(
            PCWSTR, PCWSTR, ICoreWebView2EnvironmentOptions *,
            ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *);
        const auto createEnv = reinterpret_cast<PFN_CreateEnv>(
            GetProcAddress(lib, "CreateCoreWebView2EnvironmentWithOptions"));
        if (!createEnv) {
            s_envError = QStringLiteral("WebView2Loader.dll 缺少导出函数");
            qCritical() << "Mswrite:" << s_envError;
            return;
        }
        s_loaderLib = lib;   // 记住句柄:下次重试直接复用,不重复 LoadLibrary
        auto *h = new EnvCreatedHandler;
        h->onDone = [](ICoreWebView2Environment *env) {
            s_creating = false;
            if (!env) {
                if (s_envError.isEmpty())
                    s_envError = QStringLiteral("WebView2 环境创建返回空");
                return; // waiters 留着,下次 start() 会重试
            }
            s_env = env; // 持全局引用
            const QList<Impl *> waiters = s_waiters;
            s_waiters.clear();
            for (Impl *impl : waiters)
                impl->onEnvironment(env);
        };
        s_creating = true;
        const std::wstring dataFolder = userDataFolder.toStdWString();
        const HRESULT hr = createEnv(nullptr, dataFolder.c_str(), nullptr, h);
        // 我们的那份引用在此释放:之后生命周期由运行时的
        // AddRef/Release 对管理(Invoke 后自行析构),避免泄漏
        h->Release();
        if (FAILED(hr)) {
            s_creating = false;
            s_envError = QStringLiteral("创建 WebView2 环境失败(错误码 0x%1)")
                             .arg(uint(hr), 8, 16, QChar('0'));
            qCritical() << "Mswrite:" << s_envError;
        }
    }

    void applyBounds()
    {
        if (!ctrl || !q)
            return;
        const qreal dpr = q->devicePixelRatioF();
        RECT r{ 0, 0,
                static_cast<LONG>(qRound(q->width() * dpr)),
                static_cast<LONG>(qRound(q->height() * dpr)) };
        ctrl->put_Bounds(r);
    }
    void bindWindow()
    {
        if(!ctrl || !q->internalWinId()) return;
        const HWND current=reinterpret_cast<HWND>(q->internalWinId());
        HWND parent=nullptr;ctrl->get_ParentWindow(&parent);
        if(parent!=current) ctrl->put_ParentWindow(current);
        applyBounds();
    }

    void setMapping(const QString &host, const QString &folder)
    {
        ICoreWebView2_3 *web3 = nullptr;
        if (SUCCEEDED(web->QueryInterface(__uuidof(ICoreWebView2_3),
                                          reinterpret_cast<void **>(&web3))) && web3) {
            const std::wstring h = host.toStdWString();
            const std::wstring d = folder.toStdWString();
            web3->SetVirtualHostNameToFolderMapping(h.c_str(), d.c_str(),
                                                     COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
            web3->Release();
        }
    }

    // 页面资源拦截保留于头文件之外——图片经专用虚拟主机(doc{n}.local)提供

    void onEnvironment(ICoreWebView2Environment *environment)
    {
        environment->AddRef(); // 本实例自持引用(回调路径与直调路径统一在此加)
        env = environment;     // 全局 s_env 由 ensureEnvironment 持有,不归本实例

        createController();
    }

    void createController()
    {
        if (hCtrl) { hCtrl->Release(); hCtrl=nullptr; }
        ++controllerAttempts;

        auto *h = new ControllerCreatedHandler;
        h->onDone = [guard = QPointer<WebViewHost>(q)](HRESULT result, ICoreWebView2Controller *c) {
            if (guard) {
                if (c) guard->d->onController(c);
                else guard->d->controllerFailed(result);
            } else if (c) { c->Close(); c->Release(); }
        };
        hCtrl = h;

        const HWND hwnd = reinterpret_cast<HWND>(q->winId());
        const HRESULT result=env->CreateCoreWebView2Controller(hwnd, hCtrl);
        if (FAILED(result)) controllerFailed(result);
    }

    void controllerFailed(HRESULT result)
    {
        qWarning() << "Mswrite: WebView controller creation failed" << Qt::hex << result;
        // A new tab can arrive while WebView2 is stopping its last browser
        // process. Retry after that transition; permanent errors stay bounded.
        const bool transient=result==CO_E_SERVER_EXEC_FAILURE || result==E_ABORT
            || result==HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
        if (transient && controllerAttempts<3) {
            QTimer::singleShot(250*controllerAttempts,q,[this]{createController();});
        }
    }

    void onController(ICoreWebView2Controller *controller)
    {
        ctrl = controller; // Invoke 中已 AddRef
        bindWindow();
        ctrl->get_CoreWebView2(&web);

        // JS -> C++ 消息
        auto *m = new MessageHandler;
        m->onMessage = [this](const QString &json) {
            QJsonParseError err{};
            const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &err);
            if (err.error != QJsonParseError::NoError || !doc.isObject())
                return;
            const QJsonObject obj = doc.object();
            const QString type=obj.value(QStringLiteral("t")).toString();
            if ((type==QLatin1String("ready") || type==QLatin1String("chatRendered")) && q->m_preview) {
                QTimer::singleShot(0, q, [guard = QPointer<WebViewHost>(q)] {
                    if (!guard || !guard->m_preview) return;
                    guard->m_preview->hide();
                    guard->m_preview->deleteLater();
                    guard->m_preview = nullptr;
                    if (guard->d->ctrl) guard->d->ctrl->put_IsVisible(guard->d->alwaysVisible || guard->isVisible());
                });
            }
            // 渲染完成信号(waitRendered 注入的脚本回传),内部消费不上抛
            if (obj.value(QStringLiteral("t")).toString() == QLatin1String("rendered")) {
                if (renderedCb && obj.value(QStringLiteral("request")).toInt() == renderRequest) {
                    auto cb = std::move(renderedCb);
                    renderedCb = nullptr;
                    cb();
                }
                return;
            }
            emit q->message(obj);
        };
        hMsg = m;
        web->add_WebMessageReceived(hMsg, &tokMsg);

        // 每次导航完成上报(首次额外触发 pageReady)
        auto *n = new NavCompletedHandler;
        n->onDone = [this](bool ok, HRESULT err) {
            Q_UNUSED(err);
            emit q->navigated(ok);
            // 首次导航失败不能标成就绪:否则排队的 setContent 会打到错误页上,
            // 之后即使用户刷新也已经把"已就绪"门关上了
            if (!ok || q->m_pageReady)
                return;
            q->m_pageReady = true;
            emit q->pageReady();
            const QStringList pending = q->m_pendingScripts;
            q->m_pendingScripts.clear();
            if (!pending.isEmpty()) runScriptNow(pending.join(QStringLiteral(";\n")));
        };
        hNav = n;
        web->add_NavigationCompleted(hNav, &tokNav);

        // 编辑区 Ctrl 组合键 -> Qt 侧过滤器
        auto *a = new AcceleratorHandler;
        a->onKey = [this](UINT vk, bool ctrl, bool shift, bool alt) -> bool {
            return q->m_accelerator && q->m_accelerator(int(vk), ctrl, shift, alt);
        };
        hAcc = a;
        ctrl->add_AcceleratorKeyPressed(hAcc, &tokAcc);

        // 基础设置:禁默认右键菜单;开发态保留 DevTools 便于排查
        ICoreWebView2Settings *settings = nullptr;
        if (SUCCEEDED(web->get_Settings(&settings)) && settings) {
            settings->put_AreDefaultContextMenusEnabled(FALSE);
            settings->put_AreDevToolsEnabled(TRUE); // Shift+F12 开发者工具
            // 关键:禁用 Chromium 浏览器级快捷键(Ctrl+T/D/H/L/J/=/- 等),
            // 否则被浏览器层吃掉,页面 keydown 永远收不到(编辑器快捷键大面积失效的根因)
            ICoreWebView2Settings3 *s3 = nullptr;
            if (SUCCEEDED(settings->QueryInterface(__uuidof(ICoreWebView2Settings3),
                                                   reinterpret_cast<void **>(&s3))) && s3) {
                s3->put_AreBrowserAcceleratorKeysEnabled(FALSE);
                s3->Release();
                qWarning() << "Mswrite: 浏览器快捷键已禁用,编辑器键位直达页面";
            }
            settings->Release();
        }

        // 虚拟主机映射:主映射 + 追加映射(文档目录经 doc{n}.local 提供)
        setMapping(virtualHost, virtualFolder);
        for (const auto &pair : extraMappings)
            setMapping(pair.first, pair.second);

        applyBounds();
        ctrl->put_IsVisible(alwaysVisible || (q->isVisible() && !q->m_preview));
        if (!qFuzzyCompare(zoom, 1.0))
            ctrl->put_ZoomFactor(zoom);   // 控制器就绪后恢复缩放

        if (!startUrl.isEmpty()) {
            curUrl = startUrl;
            if (initialScript.isEmpty()) q->navigate(startUrl);
            else {
                auto *ready = new InitialScriptHandler;
                ready->onDone = [guard = QPointer<WebViewHost>(q)](HRESULT result) {
                    if (!guard) return;
                    if (FAILED(result)) qWarning() << "Mswrite: initial script registration failed" << result;
                    guard->navigate(guard->d->startUrl);
                };
                const auto script = initialScript.toStdWString();
                const HRESULT hr = web->AddScriptToExecuteOnDocumentCreated(script.c_str(), ready);
                ready->Release();
                if (FAILED(hr)) q->navigate(startUrl);
            }
        }
    }

    void runScriptNow(const QString &js,
                      const std::function<void(const QString &)> &resultCb = {})
    {
        if (!web) {
            if (resultCb) resultCb(QString());
            return;
        }
        auto *done = new ScriptDoneHandler;
        if (resultCb)
            done->onResult = resultCb;
        const std::wstring code = js.toStdWString();
        const HRESULT result=web->ExecuteScript(code.c_str(), done);
        if(FAILED(result) && resultCb) resultCb(QString());
        done->Release(); // 运行时在调用期间自持引用
    }

    void shutdown()
    {
        // 关键:环境创建是异步的,若本实例在回调前销毁,s_waiters 里
        // 会留下悬垂指针 —— 回调触发即 use-after-free(启动瞬间关标签/关窗)
        s_waiters.removeAll(this);
        if (web) {
            if (hMsg) web->remove_WebMessageReceived(tokMsg);
            if (hNav) web->remove_NavigationCompleted(tokNav);
        }
        if (ctrl) {
            if (hAcc) ctrl->remove_AcceleratorKeyPressed(tokAcc);
            ctrl->Close();
        }
        renderedCb = nullptr;
        auto rel = [](void **p) { if (*p) { static_cast<IUnknown *>(*p)->Release(); *p = nullptr; } };
        rel(reinterpret_cast<void **>(&web));
        rel(reinterpret_cast<void **>(&ctrl));
        rel(reinterpret_cast<void **>(&env));
        rel(reinterpret_cast<void **>(&hMsg));
        rel(reinterpret_cast<void **>(&hNav));
        rel(reinterpret_cast<void **>(&hAcc));
        rel(reinterpret_cast<void **>(&hCtrl));
    }

    void doPrintToPdf(const QString &outPath, std::function<void(bool, HRESULT)> done)
    {
        if (!web || !env) {
            if (done) done(false, E_FAIL);
            return;
        }
        ICoreWebView2_7 *web7 = nullptr;
        ICoreWebView2Environment6 *env6 = nullptr;
        if (FAILED(web->QueryInterface(__uuidof(ICoreWebView2_7),
                                       reinterpret_cast<void **>(&web7))) || !web7) {
            qWarning() << "Mswrite: 运行时过旧,不支持 PrintToPdf";
            if (done) done(false, E_NOINTERFACE);
            return;
        }
        ICoreWebView2PrintSettings *ps = nullptr;
        if (SUCCEEDED(env->QueryInterface(__uuidof(ICoreWebView2Environment6),
                                          reinterpret_cast<void **>(&env6))) && env6) {
            if (SUCCEEDED(env6->CreatePrintSettings(&ps)) && ps) {
                ps->put_Orientation(COREWEBVIEW2_PRINT_ORIENTATION_PORTRAIT);
                ps->put_PageWidth(8.27);   // A4
                ps->put_PageHeight(11.69);
                ps->put_ShouldPrintBackgrounds(TRUE);
                ps->put_ShouldPrintHeaderAndFooter(FALSE);
                ps->put_MarginTop(0.4);
                ps->put_MarginBottom(0.4);
                ps->put_MarginLeft(0.4);
                ps->put_MarginRight(0.4);
            }
            env6->Release();
        }
        auto *h = new PrintToPdfHandler;
        h->onDone = std::move(done);
        const std::wstring out = QDir::toNativeSeparators(outPath).toStdWString();
        const HRESULT hr = web7->PrintToPdf(out.c_str(), ps, h);
        if (FAILED(hr) && h->onDone) h->onDone(false, hr);
        if (ps) ps->Release();
        web7->Release();
        h->Release(); // 运行时在调用期间自持引用,与 runScriptNow 一致
    }

private:
    ICoreWebView2Environment  *env = nullptr;
    ControllerCreatedHandler *hCtrl = nullptr;
    int controllerAttempts = 0;
};

ICoreWebView2Environment *WebViewHost::Impl::s_env = nullptr;
QList<WebViewHost::Impl *> WebViewHost::Impl::s_waiters;
QString WebViewHost::Impl::s_envError;
bool WebViewHost::Impl::s_creating = false;
HMODULE WebViewHost::Impl::s_loaderLib = nullptr;

QString WebViewHost::environmentError()
{
    return Impl::s_envError;
}

// ---------------------------------------------------------------------------
// WebViewHost
// ---------------------------------------------------------------------------

WebViewHost::WebViewHost(QWidget *parent)
    : QWidget(parent)
    , d(new Impl)
{
    d->q = this;
    setAttribute(Qt::WA_NativeWindow);
    // Native ancestors keep the browser inside the stacked page's clipping and
    // visibility hierarchy. Alien ancestors can leave a live but covered HWND.
    setMinimumSize(200, 200);
}

WebViewHost::~WebViewHost()
{
    // Controller::Close can synchronously complete pending COM operations,
    // before QObject's destructor has invalidated QPointers.
    m_closing = true;
    d->shutdown();
    delete d;
}

void WebViewHost::setAcceleratorFilter(AcceleratorFilter filter)
{
    m_accelerator = std::move(filter);
}

void WebViewHost::showStartupPreview(const QString &markdown, const QString &theme, int fontSize)
{
    if(m_preview) {
        if(auto *text=m_preview->findChild<QTextBrowser *>(QStringLiteral("startupText")))
            text->setMarkdown(markdown.left(32000));
        return;
    }
    // Never load embedded image URLs in this short-lived, read-only preview.
    class PreviewText final : public QTextBrowser {
    public:
        using QTextBrowser::QTextBrowser;
        QVariant loadResource(int, const QUrl &) override { return {}; }
    };
    m_preview = new QWidget(this);
    m_preview->setObjectName(QStringLiteral("startupPreview"));
    auto *layout = new QVBoxLayout(m_preview);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    auto *hint = new QLabel(tr("内容已打开 · 编辑器准备中…"), m_preview);
    hint->setObjectName(QStringLiteral("startupHint"));
    hint->setStyleSheet(QStringLiteral("font-size:12px; padding:8px 28px; color:%1;")
        .arg(theme == QLatin1String("dark") ? "#a3adbd" : "#66748a"));
    auto *text = new PreviewText(m_preview);
    text->setObjectName(QStringLiteral("startupText"));
    text->setFrameShape(QFrame::NoFrame);
    text->setOpenLinks(false);
    text->setOpenExternalLinks(false);
    const bool dark = theme == QLatin1String("dark");
    text->setStyleSheet(QStringLiteral("QTextBrowser { background:%1; color:%2; padding:20px 40px; font-family:'Segoe UI','Microsoft YaHei UI'; font-size:%3px; }")
        .arg(dark ? "#181b20" : theme == QLatin1String("paper") ? "#fffdf8" : "#ffffff",
             dark ? "#e5e7eb" : "#253041").arg(fontSize));
    // Bound work for very large documents; the browser receives the full content.
    QFont previewFont(QStringLiteral("Segoe UI"));
    previewFont.setPixelSize(fontSize);
    text->document()->setDefaultFont(previewFont);
    text->setMarkdown(markdown.left(32000));
    layout->addWidget(hint);
    layout->addWidget(text, 1);
    m_preview->setGeometry(rect());
    m_preview->show();
    if(d->ctrl) d->ctrl->put_IsVisible(FALSE);
}

void WebViewHost::start(const QString &userDataFolder,
                        const QString &virtualHost,
                        const QString &virtualFolder,
                        const QString &startUrl, const QString &initialScript)
{
    d->virtualHost = virtualHost;
    d->virtualFolder = virtualFolder;
    d->startUrl = startUrl;
    d->initialScript = initialScript;

    if (Impl::s_env) {
        d->onEnvironment(Impl::s_env); // 环境已就绪,直接建控制器
        return;
    }
    if (!Impl::s_waiters.contains(d))
        Impl::s_waiters.append(d);
    // 每次都尝试:上次 LoadLibrary/CreateEnv 失败后,新标签必须能重试,
    // 否则 s_waiters 非空却再也不调 ensureEnvironment,所有标签永久空白
    Impl::ensureEnvironment(userDataFolder);
}

void WebViewHost::addHostMapping(const QString &virtualHost, const QString &folder)
{
    if (d->web)
        d->setMapping(virtualHost, folder);
    else
        d->extraMappings.append({ virtualHost, folder });
}

void WebViewHost::setAlwaysVisible(bool on)
{
    d->alwaysVisible = on;
    if (d->ctrl && on)
        d->ctrl->put_IsVisible(TRUE);
}

void WebViewHost::runScript(const QString &js)
{
    if (m_pageReady)
        d->runScriptNow(js);
    else
        m_pendingScripts.append(js);
}

void WebViewHost::evalWithResult(const QString &js,
                                 std::function<void(const QString &)> result)
{
    // 不等 pageReady:能收到 JS 消息时 web 必然存在,
    // NavigationCompleted 可能晚于 JS ready(资源仍在加载)
    d->runScriptNow(js, [guard = QPointer<WebViewHost>(this), result = std::move(result)](const QString &value) {
        if (guard && !guard->m_closing && result) result(value);
    });
}

void WebViewHost::openDevTools()
{
    if (d->web)
        d->web->OpenDevToolsWindow();
}

void WebViewHost::setZoomFactor(double factor)
{
    d->zoom = qBound(0.5, factor, 2.0);
    if (d->ctrl)
        d->ctrl->put_ZoomFactor(d->zoom);
}

double WebViewHost::zoomFactor() const
{
    return d->zoom;
}

void WebViewHost::zoomIn()
{
    setZoomFactor(d->zoom + 0.05);
}

void WebViewHost::zoomOut()
{
    setZoomFactor(d->zoom - 0.05);
}

void WebViewHost::navigate(const QString &url)
{
    if (!d->web)
        return;
    d->startUrl = url;
    d->curUrl = url;
    const std::wstring u = url.toStdWString();
    d->web->Navigate(u.c_str());
}

QString WebViewHost::currentUrl() const
{
    return d->curUrl;
}

void WebViewHost::printToPdf(const QString &outputPath, std::function<void(bool, HRESULT)> done)
{
    d->doPrintToPdf(outputPath, [guard = QPointer<WebViewHost>(this), done = std::move(done)](bool ok, HRESULT hr) {
        if (guard && !guard->m_closing && done) done(ok, hr);
    });
}

void WebViewHost::waitRendered(std::function<void()> done)
{
    if (!done)
        return;
    if (!d->web) {
        done();
        return;
    }
    // JS 完成后回传 {t:'rendered'};ExecuteScript 无法 await Promise,
    // 因此不能像旧实现那样"发出去 + 猜 500ms"
    d->renderedCb = std::move(done);
    const int request = ++d->renderRequest;
    // 兜底:页面异常/图片永不 decode 时也要放行,否则导出永久卡住
    QTimer::singleShot(kRenderTimeoutMs, this, [this, request] {
        if (!d->renderedCb || d->renderRequest != request)
            return;
        qWarning() << "Mswrite: 渲染等待超时,继续导出";
        auto cb = std::move(d->renderedCb);
        d->renderedCb = nullptr;
        cb();
    });
    // 等两帧 + 字体就绪,并主动等所有图片 decode 完成
    const std::wstring js =
        L"(function(){function imgs(){var a=document.images;"
        L"return Promise.all(Array.prototype.map.call(a,function(i){"
        L"return (i.decode?i.decode():Promise.resolve()).catch(function(){0;})}));}"
        L"var fonts=(document.fonts&&document.fonts.ready)?document.fonts.ready:Promise.resolve();"
        L"function post(){try{window.chrome.webview.postMessage({t:'rendered',request:%1});}catch(e){}}"
        L"new Promise(function(r){requestAnimationFrame(function(){"
        L"requestAnimationFrame(function(){r(0);});});})"
        L".then(function(){return Promise.all([fonts,imgs()]);}).then(post,post);})()";
    d->runScriptNow(QString::fromWCharArray(js.c_str()).arg(request));
}

void WebViewHost::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (m_preview) m_preview->setGeometry(rect());
    d->applyBounds();
}

bool WebViewHost::event(QEvent *event)
{
    const bool result=QWidget::event(event);
    if(d && (event->type()==QEvent::WinIdChange || event->type()==QEvent::ParentChange))
        d->bindWindow();
    return result;
}

void WebViewHost::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    d->bindWindow();
    if (d->ctrl)
        d->ctrl->put_IsVisible(d->alwaysVisible || !m_preview);
}

void WebViewHost::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    if (d->ctrl && !d->alwaysVisible)
        d->ctrl->put_IsVisible(FALSE);
}

// ---------------------------------------------------------------------------
// 焦点战争根治:Qt 与内嵌 Chromium 互相抢键盘焦点。
// 点击本控件时,把 Win32 焦点直接交给 Chromium 的子 HWND,
// 并在 WM_MOUSEACTIVATE 里拒绝 Qt 的激活抢占。
// ---------------------------------------------------------------------------

namespace {
// 枚举查找 Chrome_WidgetWin_1(Chromium 的输入窗口)
struct FindChromeCtx { HWND found = nullptr; };
BOOL CALLBACK findChromeChild(HWND h, LPARAM lp)
{
    wchar_t cls[64] = {};
    if (GetClassNameW(h, cls, 64) && wcscmp(cls, L"Chrome_WidgetWin_1") == 0) {
        reinterpret_cast<FindChromeCtx *>(lp)->found = h;
        return FALSE;
    }
    return TRUE;
}
}

void WebViewHost::mousePressEvent(QMouseEvent *event)
{
    QWidget::mousePressEvent(event);
    if (!d->web)
        return;
    HWND mine = reinterpret_cast<HWND>(winId());
    FindChromeCtx ctx;
    EnumChildWindows(mine, findChromeChild, reinterpret_cast<LPARAM>(&ctx));
    if (ctx.found)
        SetFocus(ctx.found);
}
