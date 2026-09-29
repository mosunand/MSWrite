#pragma once
// ai/AiChatDock.h — 独立 AI 对话窗口(与主窗口解绑:主窗口最小化不影响它)。
// 深色气泡 UI + Markdown 渲染 + 连接/思考动效 + 附件(截图粘贴/文件上传)
// + 三档写入开关 + 思考程度选择。

#include <QElapsedTimer>
#include <QHash>
#include <QString>
#include <QTextCursor>
#include <QTimer>
#include <QWidget>
#include <functional>

#include "ai/AiProviders.h"
#include "ai/Types.h"

class QComboBox;
class QFrame;
class QGridLayout;
class QLabel;
class ChatWebView;
class QPlainTextEdit;
class QPushButton;
class QStackedWidget;
class QThread;
class AiWorker;
class ChatModel;

class AiChatDock : public QWidget {
    Q_OBJECT
public:
    using ContextProvider = std::function<QJsonObject()>;
    using DocumentReader = std::function<void(const QJsonObject &, std::function<void(AiDocumentResult)>)>;
    // 把 text 插入当前文档光标处,返回给模型的工具结果文本
    using InsertHandler = std::function<QString(const QString &, const QString &)>;

    // 无父窗口:独立于主窗口(主窗口最小化/切后台都不影响 AI 使用)
    explicit AiChatDock(QWidget *parent = nullptr);
    ~AiChatDock() override;

    void setDocumentReader(DocumentReader r) { m_docReader = std::move(r); }
    void setContextProvider(ContextProvider p) { m_contextProvider=std::move(p); }
    void setInsertHandler(InsertHandler h) { m_insert = std::move(h); }
    void readDocument(const QJsonObject &request, std::function<void(AiDocumentResult)> done);
    void requestSelection(const QString &text, const QString &name, int page, bool translate);

    void applyProvider(const AiProvider &p); // 更新标签 + 下发工作线程
    void setNoProvider();                    // 未配置时的提示标签
    void clearConversation();                // 清历史 + 清屏
    bool isLightTheme() const { return m_lightTheme; }

public slots:
    // 工作线程经 BlockingQueuedConnection 调用(在 GUI 线程执行)
    QString insertAtCursor(const QString &text);

signals:
    void runRequested(const QString &userText, const QString &docMarkdown,
                       int writeMode, int thinkLevel, const QVector<AiAttach> &images);
    void configRequested(); // 面板请求打开 AI 设置
    void skillsRequested();
    void visibilityChanged(bool visible); // QWidget 没有,自发自收供菜单同步

protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    bool eventFilter(QObject *obj, QEvent *event) override; // Enter 发送 + 图片粘贴

private:
    void buildUi();
    QWidget *buildWelcomePage();     // 欢迎卡片页(发消息即消失)
    void wireWorker();
    void updateStackPage();          // 无消息→欢迎页;有消息→聊天页
    void saveSession();              // 对话持久化(轮末/清理时)
    bool loadSession();              // 启动恢复上次对话
    static QString sessionFilePath();
    void autoGrowInput();
    void send();
    void stop();
    void dispatch(const QString &userText); // 固定文档标识,正文由工具按需读取
    void scrollBottom();

    // 附件
    void attachImages(const QList<QPair<QString, QByteArray>> &files); // (名字, 数据)
    void attachTextFile(const QString &name, const QString &content);
    void rebuildAttachStrip();
    void clearAttachments();

    // 写入模式 / 思考程度
    void cycleWriteMode();
    void applyWriteModeStyle();
    void onThinkLevelChanged();

    // 主题
    void toggleTheme();
    void repaintAll(); // 主题切换后全量重绘(气泡/列表/欢迎页)
    void restyleDynamicWidgets(); // 写按钮/思考下拉随主题换色

    // 智能滚动

    // 动效
    void onAnimTick();

    QLabel *m_model = nullptr;       // 顶栏:状态点 + 供应商/模型
    QPushButton *m_newBtn = nullptr; // 顶栏:新对话
    QPushButton *m_setBtn = nullptr; // 顶栏:设置
    QPushButton *m_themeBtn = nullptr; // 顶栏:浅色/深色切换
    QStackedWidget *m_stack = nullptr; // 页0:欢迎卡片页;页1:聊天列表
    ChatWebView *m_chat = nullptr;
    QPlainTextEdit *m_input = nullptr;
    QPushButton *m_send = nullptr;  // 圆形按钮:发送/停止二合一

    QWidget *m_attachStrip = nullptr;    // 附件缩略图条
    class QHBoxLayout *m_attachLay = nullptr;
    QPushButton *m_writeBtn = nullptr;    // 写入模式:不写入/AI 决定/强制全写
    QComboBox *m_thinkBox = nullptr;      // 思考程度:关/低/中/高

    ChatModel *m_msgs = nullptr;
    QThread *m_thread = nullptr;
    AiWorker *m_worker = nullptr;

    QVector<AiAttach> m_images;                       // 待发送图片
    QVector<QPair<QString, QString>> m_textFiles;      // 待发送文本附件 (名, 内容)

    int m_writeMode = 0;   // 0不写入 1AI 决定 2强制全写(持久化)
    int m_thinkLevel = 3;  // 0关 1低 2中 3高(持久化)
    bool m_lightTheme = false; // 浅色主题(持久化 aiTheme=1)

    int m_assistRow = -1;      // 本轮流式回答的气泡行
    int m_thinkRow = -1;       // 本轮思考行(正文出现后冻结)
    QElapsedTimer m_thinkElapsed;
    bool m_busy = false;
    bool m_anyText = false;
    bool m_turnPreferLatex = false;
    bool m_hasProvider = false; // GUI 线程自持(不跨线程读 worker 状态)
    QString m_modelText;

    QTimer *m_animTimer = nullptr;  // 120ms 动效:转圈 + 闪烁光标 + 等待秒数
    int m_animFrame = 0;
    int m_waitSecs = 0;
    QHash<QWidget *, QString> m_cardPrompts; // 欢迎卡片 → 预置指令
    QStringList m_inputHistory;  // ↑↓ 翻阅的输入历史(最近 50 条)
    int m_histIdx = -1;          // -1 = 未浏览;0 = 最新一条
    QString m_histDraft;           // 浏览历史前的草稿(↓ 到底恢复)


    QString m_activeDocumentId;
    // 本轮参数随 runRequested 一起发
    QVector<AiAttach> m_dispatchImages;
    int m_dispatchMode = 0;
    int m_dispatchLevel = 3;

    DocumentReader m_docReader;
    ContextProvider m_contextProvider;
    InsertHandler m_insert;
};
