#pragma once

#include <QWidget>
#include <QJsonObject>
#include <QStringList>
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
    // Readable first content while the asynchronous browser runtime starts.
    void showStartupPreview(const QString &markdown, const QString &theme, int fontSize);

    // 异步启动:首次调用创建共享环境,后续实例直接复用
    void start(const QString &userDataFolder,
               const QString &virtualHost,
               const QString &virtualFolder,
               const QString &startUrl, const QString &initialScript = QString());

    // 追加虚拟主机映射(须在 navigate 之前设置)
    void addHostMapping(const QString &virtualHost, const QString &folder);

    // 置 true 后 WebView2 视为始终可见(隐藏导出页用:不可见时渲染会挂起)
    void setAlwaysVisible(bool on);

    // 页面就绪前调用会被排队,就绪后立即下发
    void runScript(const QString &js);

    // 执行脚本并回传结果(诊断用;结果为 JSON 字符串)
    void evalWithResult(const QString &js, std::function<void(const QString &)> result);

    // 打开开发者工具(Shift+F12)
    void openDevTools();

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

signals:
    void pageReady();                 // 首次导航完成
    void navigated(bool ok);          // 每次导航完成
    void message(const QJsonObject &obj);               // JS -> C++

protected:
    bool event(QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;

private:
    friend struct Impl;
    struct Impl;
    Impl *d = nullptr;

    AcceleratorFilter m_accelerator;
    bool m_pageReady = false;
    QStringList m_pendingScripts;
    QWidget *m_preview = nullptr;
};
