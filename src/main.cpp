#include "mainwindow.h"

#include "ai/AiChatDock.h"
#include "ai/AiDoctor.h"
#include "ai/AiProviders.h"
#include "ai/Http.h"
#include "ai/Llm.h"
#include "ai/LlmCodec.h"
#include "ai/MswriteSkill.h"
#include "ai/Types.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDateTime>
#include <QFile>
#include <QRect>
#include <QIcon>
#include <QScreen>
#include <QTimer>
#include <QTextStream>

#include <windows.h>
#include <objbase.h>

namespace {
// 文件日志:排障用(WebView2 打印失败/JS 错误等在 GUI 下无处可见)
void fileMessageHandler(QtMsgType type, const QMessageLogContext &, const QString &msg)
{
    static QFile log(QCoreApplication::applicationDirPath()
                     + QStringLiteral("/mswrite.log"));
    if (!log.isOpen())
        log.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);
    if (!log.isOpen())
        return;
    static const char *tags[] = { "DEBUG", "WARN ", "CRIT ", "FATAL", "INFO " };
    QTextStream stream(&log);
    stream << QDateTime::currentDateTime().toString(QStringLiteral("MM-dd HH:mm:ss.zzz"))
           << ' ' << tags[type] << ' ' << msg << '\n';
    stream.flush();
}
} // namespace

namespace {

// 无头 AI 连接诊断:TLS + GET /models + 最小真实对话,结果写 exe 旁 ai-doctor.txt
int runAiDoctor()
{
    QString out = QStringLiteral("Mswrite AI 连接诊断 %1\n\n")
                      .arg(QDateTime::currentDateTime().toString(Qt::ISODate));
    AiProviderStore store = AiProviderStore::load();
    const AiProvider *p = store.current();
    if (!p) {
        out += QStringLiteral("没有当前供应商(先在 AI 设置里配置或导入 MS-Agent)");
    } else {
        out += QStringLiteral("当前供应商: %1 · %2 · %3\n\n")
                   .arg(p->name, protocolName(p->protocol), p->baseUrl);
        out += AiDoctor::format(AiDoctor::run(*p));

        // 最小真实对话(max_tokens=16,约 10 token 消耗):验证与 SSE 不同的 POST 通路
        out += QStringLiteral("\n对话测试(非流式,max_tokens=16):\n");
        QVector<ChatMessage> hist;
        ChatMessage u;
        u.role = QStringLiteral("user");
        u.text = QStringLiteral("hi");
        hist.push_back(u);
        const QByteArray body = LlmCodec::requestBody(
            p->protocol, p->model, QStringLiteral("You are a connection test."),
            hist, {}, false, 16);
        const HttpResult hr = Http::postJson(LlmCodec::endpoint(p->protocol, p->baseUrl),
                                              body, LlmCodec::headers(p->protocol, p->apiKey),
                                              20000);
        const ChatResponse resp = LlmCodec::parse(p->protocol, hr.status, hr.body);
        if (hr.error.isEmpty() && resp.error.isEmpty() && !resp.text.isEmpty()) {
            out += QStringLiteral("成功: HTTP %1 · 回复 %2\n")
                       .arg(hr.status)
                       .arg(resp.text.left(60).simplified());
        } else if (!resp.error.isEmpty()) {
            out += QStringLiteral("失败: %1\n原始响应: %2\n")
                       .arg(resp.error, QString::fromUtf8(hr.body.left(400)));
        } else {
            out += QStringLiteral("异常: HTTP %1 · %2 · %3\n")
                       .arg(QString::number(hr.status), hr.error,
                            QString::fromUtf8(hr.body.left(400)));
        }
    }
    QFile f(QCoreApplication::applicationDirPath() + QStringLiteral("/ai-doctor.txt"));
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        f.write(out.toUtf8());
        f.close();
    }
    qWarning() << "Mswrite: AI 诊断完成,报告在" << QCoreApplication::applicationDirPath()
               << "/ai-doctor.txt";
    return 0;
}

} // namespace

namespace {

// 完全复刻 AiWorker::run 的真实链路(流式 + Insert 工具 + 完整系统提示),
// 结果写 exe 旁 ai-smoke.txt —— 用于定位面板里"链接失败"的真实原因。
// 可选参数指定供应商:Mswrite.exe --ai-smoke Deepseek
int runAiSmoke(const QStringList &args)
{
    QString out = QStringLiteral("Mswrite AI 冒烟测试(与面板同链路) %1\n\n")
                      .arg(QDateTime::currentDateTime().toString(Qt::ISODate));
    AiProviderStore store = AiProviderStore::load();
    const int flagIdx = args.indexOf(QStringLiteral("--ai-smoke"));
    const bool hasName = flagIdx >= 0 && flagIdx + 1 < args.size()
                         && !args.at(flagIdx + 1).startsWith(QLatin1Char('-'));
    const QString wanted = hasName ? args.at(flagIdx + 1) : QString();
    const AiProvider *p = wanted.isEmpty() ? store.current() : store.find(wanted);
    if (!p || p->apiKey.isEmpty()) {
        out += wanted.isEmpty() ? QStringLiteral("没有可用的当前供应商")
                                : QStringLiteral("找不到供应商: %1").arg(wanted);
    } else {
        out += QStringLiteral("供应商: %1 · %2 · %3 · 模型 %4\n\n")
                   .arg(p->name, protocolName(p->protocol), p->baseUrl, p->model);

        AiLlmConfig cfg;
        cfg.apiKey = p->apiKey;
        cfg.baseUrl = p->baseUrl;
        cfg.model = p->model;
        cfg.protocol = p->protocol;
        cfg.maxTokens = p->maxTokens > 0 ? p->maxTokens : 4096;
        Llm llm;
        llm.setConfig(cfg);

        // 与 AiWorker 相同的 Insert 工具 schema
        ToolSchema s;
        s.name = QStringLiteral("Insert");
        s.description = QStringLiteral("Insert Markdown text at the user's caret.");
        s.parameters = QJsonObject{
            { QStringLiteral("type"), QStringLiteral("object") },
            { QStringLiteral("properties"),
              QJsonObject{ { QStringLiteral("text"),
                             QJsonObject{ { QStringLiteral("type"), QStringLiteral("string") } } } } },
            { QStringLiteral("required"), QJsonArray{ QStringLiteral("text") } },
        };

        QVector<ChatMessage> hist;
        ChatMessage u;
        u.role = QStringLiteral("user");
        u.text = QStringLiteral("请用 Insert 工具在文档末尾插入一句:连接冒烟测试完成。");
        hist.push_back(u);

        int textChunks = 0, thinkChunks = 0;
        StreamSink sink;
        sink.onText = [&](const QString &d) {
            ++textChunks;
            out += QStringLiteral("[text] %1\n").arg(d.simplified().left(80));
        };
        sink.onThinking = [&](const QString &d) {
            ++thinkChunks;
            if (thinkChunks <= 3)
                out += QStringLiteral("[think] %1\n").arg(d.simplified().left(80));
        };

        QElapsedTimer t;
        t.start();
        const ChatResponse r = llm.complete(
            MswriteSkill::withDocument(QStringLiteral("# 冒烟测试文档\n\n第一段正文。")),
            hist, { s }, sink);
        out += QStringLiteral("\n---- 结果 ----\n耗时 %1 ms\nHTTP %2 · SSE 事件 %3\n")
                   .arg(t.elapsed())
                   .arg(r.httpStatus)
                   .arg(r.sseEvents);
        out += QStringLiteral("error: %1\n").arg(r.error.isEmpty() ? QStringLiteral("(无)") : r.error);
        out += QStringLiteral("原始响应: %1\n")
                   .arg(QString::fromUtf8(r.raw.left(600)).simplified());
        out += QStringLiteral("text(%1 段): %2\n").arg(textChunks).arg(r.text.left(200));
        out += QStringLiteral("thinking 段数: %1\ntoolCalls: %2\n")
                   .arg(thinkChunks)
                   .arg(r.toolCalls.size());
        for (const ToolCall &c : r.toolCalls)
            out += QStringLiteral("  - %1: %2 字\n")
                       .arg(c.name, QString::number(c.input.value(QStringLiteral("text")).toString().size()));
        out += QStringLiteral("tokens: in=%1 out=%2\n")
                   .arg(r.inputTokens)
                   .arg(r.outputTokens);
    }
    QFile f(QCoreApplication::applicationDirPath() + QStringLiteral("/ai-smoke.txt"));
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        f.write(out.toUtf8());
        f.close();
    }
    qWarning() << "Mswrite: AI 冒烟完成,报告在" << QCoreApplication::applicationDirPath()
               << "/ai-smoke.txt";
    return 0;
}

} // namespace

int main(int argc, char *argv[])
{
    // WebView2 使用 STA,原生剪贴板/拖放还需要 OLE 初始化。
    const HRESULT comInit = OleInitialize(nullptr);

    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Mswrite"));
    QApplication::setOrganizationName(QStringLiteral("Mswrite"));
    QApplication::setApplicationVersion(QStringLiteral("2.0.0"));
    qInstallMessageHandler(fileMessageHandler);

    const QStringList args = QCoreApplication::arguments();
    if (args.contains(QStringLiteral("--ai-doctor")))
        return runAiDoctor();
    if (args.contains(QStringLiteral("--ai-smoke")))
        return runAiSmoke(args);

    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addPositionalArgument(QStringLiteral("file"), QStringLiteral("要打开的 Markdown 文件"));
    parser.process(app);

    MainWindow win(nullptr, parser.positionalArguments().value(0));
    win.setWindowIcon(QIcon(QStringLiteral(":/logo.ico")));
    win.show();
    // 后台/最小化方式启动时,OS 会在事件循环开始后才应用最小化状态,
    // 同步检查拦不住 —— 用事件循环内的延迟检查兜底
    QTimer::singleShot(120, &win, [&win]() {
        const QRect avail = win.screen()->availableGeometry();
        // 只处理"启动被最小化"的情况。旧条件把 Maximized/FullScreen
        // 也当成异常打回普通窗口,用户上次最大化退出后再开就被还原。
        if (win.isMinimized()) {
            win.showNormal();
            win.activateWindow();
        }
        const QRect g = win.normalGeometry();
        const bool sane = g.width() >= 600 && g.height() >= 400 && avail.intersects(g);
        if (!sane) {
            // 默认尺寸 = 用户当前使用习惯(1252x1099);小屏放不下时退近全屏
            const QRect def(535, 157, 1252, 1099);
            if (avail.intersects(def))
                win.setGeometry(def);
            else
                win.setGeometry(avail.adjusted(16, 16, -16, -16));
            qWarning() << "Mswrite: 启动几何异常已重置";
        }
    });
    const int code = app.exec();
    if (SUCCEEDED(comInit))
        OleUninitialize();
    return code;
}
