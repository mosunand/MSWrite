#pragma once

#include <QWidget>
#include <QJsonObject>
#include <QStringList>
#include <QTimer>
#include <functional>

// 承载 WebView2(系统自带 Chromium)的 Qt 原生控件。
// COM 细节全部封装在 .cpp 的 Impl 中,头文件不暴露 WebView2 类型。
// 多个实例共享同一个 WebView2 环境(同一用户数据目录),支撑多标签页。
class WebViewHost : public QWidget
{
    Q_OBJECT
public:
    explicit WebViewHost(QWidget *parent = nullptr);
    ~WebViewHost() override;

    // 加速键过滤器:编辑区内按下 Ctrl/Alt 组合键时同步调用,
    // 返回 true 表示已消费(阻止 Chromium 默认行为)。
    using AcceleratorFilter = std::function<bool(int vk, bool ctrl, bool shift, bool alt)>;
    void setAcceleratorFilter(AcceleratorFilter filter);
    // Opaque native loading surface; no partially rendered document is exposed.
    void showLoading(const QString &theme, const QString &text);
    void setLoadingTheme(const QString &theme);
    void showLoadingError(const QString &text);
    void finishLoading();
    bool isLoading() const { return m_loading != nullptr; }

    // 异步启动:首次调用创建共享环境,后续实例直接复用
    void start(const QString &userDataFolder,
               const QString &virtualHost,
               const QString &virtualFolder,
               const QString &startUrl, const QString &initialScript = QString());

    // 追加文档/图片目录映射,当前页面立即生效,无需重新导航。
    void addHostMapping(const QString &virtualHost, const QString &folder);
    // Latest editor state stays in the native process when its renderer restarts.
    void setRecoverySnapshot(const QString &content, int revision);
    QString recoveryContent() const;
    int recoveryRevision() const;

    // 置 true 后 WebView2 视为始终可见(隐藏导出页用:不可见时渲染会挂起)
    void setAlwaysVisible(bool on);

    // 页面就绪前调用会被排队,就绪后立即下发
    void runScript(const QString &js);

    // 执行脚本并回传结果(诊断用;结果为 JSON 字符串)
    void evalWithResult(const QString &js, std::function<void(const QString &)> result);

    // 打开开发者工具(Shift+F12)
    void openDevTools();

    // 整链重建:关闭现有 COM 控制器与页面,用原参数完全重新创建。
    // 用于页面进入无法自愈的死态(消息管道断/渲染永挂)时的终极自愈,
    // 等效于重启整个 WebView2 实例,但不影响同进程的其他宿主。
    void recreateBrowser();

    // 整页缩放(WebView2 原生 ZoomFactor,Chrome 同款行为):
    // factor 1.0 = 100%;范围钳制 0.5~2.0
    void setZoomFactor(double factor);
    double zoomFactor() const;
    void zoomIn();    // +5%
    void zoomOut();   // -5%

    // 显式导航(如导出 PDF 用的隐藏页)
    void navigate(const QString &url);

    // 当前页面 URL(排障/导出前校验用)
    QString currentUrl() const;

    // 当前页面打印为 PDF(A4 纵向);完成后回调 done(成功与否, HRESULT 错误码)
    void printToPdf(const QString &outputPath, std::function<void(bool, HRESULT)> done);

    // 等待页面渲染真正完成(字体/图片就绪 + 双帧)后回调
    void waitRendered(std::function<void()> done);

    // 环境/运行时加载失败原因(空 = 正常);用于给用户明确提示
    static QString environmentError();

    bool isPageReady() const { return m_pageReady; }
    bool isClosing() const { return m_closing; }

signals:
    void pageReady();                 // 首次导航完成
    void navigated(bool ok);          // 每次导航完成
    void loadingRetry();
    void message(const QJsonObject &obj);               // JS -> C++

protected:
    bool event(QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void retryLoading();   // 子类自愈用(对话页转圈卡死时自动重载)

private:
    friend struct Impl;
    void raiseLoading();
    struct Impl;
    Impl *d = nullptr;

    AcceleratorFilter m_accelerator;
    bool m_pageReady = false;
    bool m_closing = false;
    QStringList m_pendingScripts;
    QWidget *m_loading = nullptr;
    // WebView2 控制器创建的确定性兜底:CreateCoreWebView2Controller 的完成
    // 回调在父窗口隐藏等场景下会被静默吞掉(无成功也无失败回调),页面永久
    // 转圈。创建后 12s 内没拿到控制器就自动重试;窗口不可见时先挂起,等
    // showEvent 再建(WebView2 要求创建时父窗口可见)。
    QTimer m_ctrlWatchdog;
    bool m_createOnShow = false;
    qint64 m_lastRecreateAt = 0;   // 上次整链重建时刻:600ms 内的重复触发合并
};
