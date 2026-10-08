// ai/AiChatDock.cpp — see ai/AiChatDock.h.

#include "ai/AiChatDock.h"
#include "ai/AiInputEdit.h"

#include <windows.h>

#include "ai/AiWorker.h"
#include "ai/ChatView.h"
#include "ai/ChatWebView.h"
#include "ai/AiModelPicker.h"
#include "ai/Http.h"
#include "ai/Llm.h"
#include "uiicons.h"

#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QComboBox>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QPointer>
#include <QLabel>
#include <QClipboard>
#include <QListView>
#include <QMenu>
#include <QMimeData>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QScrollArea>
#include <QPainter>
#include <QPainterPath>
#include <QResizeEvent>
#include <QRegularExpression>
#include <QSettings>
#include <QSaveFile>
#include <QStackedWidget>
#include <QStyle>
#include <QTextDocument>
#include <QThread>
#include <QTime>
#include <QTimer>
#include <QVBoxLayout>

namespace {

// A quiet folded-page motif, drawn as vectors in the space above the greeting.
// It scales with the welcome page and disappears when the window is too short.
class WelcomeCanvas final : public QWidget {
public:
    explicit WelcomeCanvas(QWidget *parent=nullptr) : QWidget(parent) {}
protected:
    void paintEvent(QPaintEvent *event) override {
        QWidget::paintEvent(event);
        auto *title=findChild<QLabel *>(QStringLiteral("aiWelcomeTitle"));
        if(!title || title->y()<105) return;
        const bool light=property("lightTheme").toBool();
        const qreal artHeight=qMin(180,title->y()-30);
        const qreal scale=qMin((width()-48)/480.0,artHeight/180.0);
        QPainter p(this); p.setRenderHint(QPainter::Antialiasing);
        p.translate(width()/2.0, title->y()-artHeight-16);
        p.scale(scale,scale); p.translate(-240,0);
        QLinearGradient fill(100,0,390,180);
        fill.setColorAt(0,QColor(light ? "#e9ebf7" : "#222630"));
        fill.setColorAt(1,QColor(light ? "#f3f4f7" : "#1a1d24"));
        QColor edge(light ? "#c8cfdf" : "#3e4657"); edge.setAlpha(125);
        p.setPen(QPen(edge,1.1)); p.setBrush(fill);
        QPainterPath left;
        left.moveTo(82,18); left.lineTo(172,18); left.lineTo(255,130);
        left.lineTo(166,130); left.closeSubpath(); p.drawPath(left);
        QPainterPath right;
        right.moveTo(278,18); right.lineTo(368,18); right.lineTo(240,180);
        right.lineTo(150,180); right.closeSubpath(); p.drawPath(right);
        p.setBrush(Qt::NoBrush);
        p.drawLine(QPointF(98,6),QPointF(181,6));
        p.drawLine(QPointF(286,6),QPointF(378,6));
        QColor accent(light ? "#7785bb" : "#8094c4"); accent.setAlpha(130);
        p.setPen(QPen(accent,1.5,Qt::SolidLine,Qt::RoundCap));
        p.drawLine(QPointF(375,108),QPointF(375,124));
        p.drawLine(QPointF(367,116),QPointF(383,116));
        p.setPen(QPen(edge,1)); p.drawLine(QPointF(94,151),QPointF(119,151));
        p.drawLine(QPointF(94,158),QPointF(109,158));
    }
    void resizeEvent(QResizeEvent *event) override {
        QWidget::resizeEvent(event);
        auto *grid=findChild<QGridLayout *>(QStringLiteral("aiWelcomeGrid"));
        if(!grid) return;
        const int columns=width()>=720 ? 3 : 2;
        if(property("columns").toInt()==columns) return;
        setProperty("columns",columns);
        QList<QWidget *> cards;
        while(auto *item=grid->takeAt(0)) { if(item->widget()) cards.append(item->widget()); delete item; }
        for(int i=0;i<cards.size();++i) grid->addWidget(cards[i],i/columns,i%columns);
        update();
    }
};

QString chatQss(bool light)
{
    if (light) {
    return QStringLiteral(R"(
AiChatDock { background: #f3f4f7; }
QFrame#aiSeparator { background:#e2e4ea; border:0; }
QLabel#aiModel { color: #5f6672; font-size: 12.5px; }
QListView#aiChat {
  background: transparent; border: none; outline: none;
}
QListView#aiChat::item { border: none; }
QLabel#aiHint {
  color: #565b66; font-size: 11.5px;
}
QWidget#aiInputBox {
  background: #ffffff; border: 1px solid #d9dbe3; border-radius: 14px;
}
QPlainTextEdit#aiInputInner {
  background: transparent; border: none; color: #1f2328;
  font-family: "Microsoft YaHei UI","Segoe UI"; font-size: 15px;
  selection-background-color: #2563eb;
}
QPushButton#aiSend {
  background: #2563eb; color: #ffffff; border: none;
  border-radius: 17px; min-width: 34px; max-width: 34px;
  min-height: 34px; max-height: 34px; font-size: 15px;
}
QPushButton#aiSend:hover { background: #1d4ed8; }
QPushButton#aiGhost {
  background: transparent; color: #6b7280;
  border: 1px solid #d9dbe3; border-radius: 15px;
  min-width: 30px; max-width: 30px; min-height: 30px; max-height: 30px;
  font-size: 14px;
}
QPushButton#aiGhost:hover { background: #eceef2; color: #1f2328; }
QPushButton#aiAttach {
  background: transparent; color: #6b7280;
  border: 1px solid #d9dbe3; border-radius: 15px;
  min-width: 32px; max-width: 32px; min-height: 32px; max-height: 32px;
  font-size: 14px;
}
QPushButton#aiAttach:hover { background: #eceef2; color: #1f2328; }
QPushButton#aiJump {
  color: #ffffff; border: none; border-radius: 15px;
  padding: 7px 18px; font-size: 12.5px; font-weight: 600;
  background: rgba(20, 22, 30, 200);
}
QPushButton#aiJump:hover { background: rgba(37, 99, 235, 230); }
QPushButton#aiModelBtn {
  background: #ffffff; color: #374151;
  border: 1px solid #d9dbe3; border-radius: 15px; min-height: 30px; max-height: 30px;
  font-size: 12.5px; padding: 0 12px; text-align: left;
}
QPushButton#aiModelBtn:hover { border-color: #3b82f6; }
QPushButton#aiWrite {
  border-radius: 15px; min-height: 30px; max-height: 30px;
  font-size: 12.5px; padding: 0 12px;
}
QComboBox#aiThink {
  background: #ffffff; color: #374151;
  border: 1px solid #d9dbe3; border-radius: 15px;
  min-height: 30px; max-height: 30px; padding: 0 8px 0 12px;
  font-size: 12.5px;
}
QComboBox#aiThink:hover { border: 1px solid #9aa3b2; }
QComboBox#aiThink::drop-down { border: none; width: 22px; }
QComboBox#aiThink::down-arrow {
  image: none; border-left: 4px solid transparent;
  border-right: 4px solid transparent; border-top: 5px solid #9ca3af;
  margin-right: 8px;
}
QComboBox QAbstractItemView {
  background: #ffffff; color: #1f2328;
  border: 1px solid #d9dbe3; selection-background-color: #2563eb;
}
QScrollBar:vertical { background: transparent; width: 9px; margin: 0; }
QScrollBar::handle:vertical {
  background: #d3d6de; border-radius: 4px; min-height: 36px;
}
QScrollBar::handle:vertical:hover { background: #b9bdc8; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }
QWidget#aiWelcome { background: transparent; }
QLabel#aiWelcomeTitle {
  color: #222b3d; font-size: 27px; font-weight: 600; background: transparent;
}
QLabel#aiWelcomeSub { color: #6b7280; font-size: 13.5px; background: transparent; }
QFrame#aiCard {
  background: #ffffff; border: 1px solid #e2e4ea; border-radius: 12px;
}
QFrame#aiCard:hover {
  background: #f7f9fc; border: 1px solid #3b82f6;
}
QLabel#aiCardHead { color: #1f2328; font-size: 14px; font-weight: 600; background: transparent; }
QLabel#aiCardDesc { color: #6b7280; font-size: 12px; background: transparent; }
)");
    }
    return QStringLiteral(R"(
AiChatDock { background: #1a1d24; }
QFrame#aiSeparator { background:#2b2f38; border:0; }
QLabel#aiModel { color: #9aa1ad; font-size: 12.5px; }
QListView#aiChat {
  background: transparent; border: none; outline: none;
}
QListView#aiChat::item { border: none; }
QLabel#aiHint {
  color: #99a0ad; font-size: 11.5px;
}
QWidget#aiInputBox {
  background: #232730; border: 1px solid #383d49; border-radius: 14px;
}
QPlainTextEdit#aiInputInner {
  background: transparent; border: none; color: #e4e4e7;
  font-family: "Microsoft YaHei UI","Segoe UI"; font-size: 15px;
  selection-background-color: #2563eb;
}
QPushButton#aiSend {
  background: #2563eb; color: #ffffff; border: none;
  border-radius: 17px; min-width: 34px; max-width: 34px;
  min-height: 34px; max-height: 34px; font-size: 15px;
}
QPushButton#aiSend:hover { background: #1d4ed8; }
QPushButton#aiGhost {
  background: transparent; color: #b3b9c6;
  border: 1px solid #383d49; border-radius: 15px;
  min-width: 30px; max-width: 30px; min-height: 30px; max-height: 30px;
  font-size: 14px;
}
QPushButton#aiGhost:hover { background: #2c313c; color: #f0f2f7; }
QPushButton#aiAttach {
  background: transparent; color: #b3b9c6;
  border: 1px solid #383d49; border-radius: 15px;
  min-width: 32px; max-width: 32px; min-height: 32px; max-height: 32px;
  font-size: 14px;
}
QPushButton#aiAttach:hover { background: #2c313c; color: #f0f2f7; }
QPushButton#aiJump {
  color: #ffffff; border: none; border-radius: 15px;
  padding: 7px 18px; font-size: 12.5px; font-weight: 600;
  background: rgba(45, 50, 62, 235);
}
QPushButton#aiJump:hover { background: rgba(37, 99, 235, 230); }
QPushButton#aiModelBtn {
  background: #232730; color: #d8dde6;
  border: 1px solid #383d49; border-radius: 15px; min-height: 30px; max-height: 30px;
  font-size: 12.5px; padding: 0 12px; text-align: left;
}
QPushButton#aiModelBtn:hover { border-color: #3b82f6; }
QPushButton#aiWrite {
  border-radius: 15px; min-height: 30px; max-height: 30px;
  font-size: 12.5px; padding: 0 12px;
}
QComboBox#aiThink {
  background: #232730; color: #d8dde6;
  border: 1px solid #383d49; border-radius: 15px;
  min-height: 30px; max-height: 30px; padding: 0 8px 0 12px;
  font-size: 12.5px;
}
QComboBox#aiThink:hover { border: 1px solid #4a5060; }
QComboBox#aiThink::drop-down { border: none; width: 22px; }
QComboBox#aiThink::down-arrow {
  image: none; border-left: 4px solid transparent;
  border-right: 4px solid transparent; border-top: 5px solid #9ca3af;
  margin-right: 8px;
}
QComboBox QAbstractItemView {
  background: #232730; color: #f0f2f7;
  border: 1px solid #383d49; selection-background-color: #2563eb;
}
QScrollBar:vertical { background: transparent; width: 9px; margin: 0; }
QScrollBar::handle:vertical {
  background: #3d424e; border-radius: 4px; min-height: 36px;
}
QScrollBar::handle:vertical:hover { background: #515868; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }
QWidget#aiWelcome { background: transparent; }
QLabel#aiWelcomeTitle {
  color: #eef0fb; font-size: 27px; font-weight: 600; background: transparent;
}
QLabel#aiWelcomeSub { color: #a4abb8; font-size: 13.5px; background: transparent; }
QFrame#aiCard {
  background: #232730; border: 1px solid #383d49; border-radius: 12px;
}
QFrame#aiCard:hover {
  background: #2a2f3a; border: 1px solid #3b82f6;
}
QLabel#aiCardHead { color: #eef0f7; font-size: 14px; font-weight: 600; background: transparent; }
QLabel#aiCardDesc { color: #a4abb8; font-size: 12px; background: transparent; }
)");
}

QString fmtSeconds(qint64 ms)
{
    if (ms < 1000)
        return QStringLiteral("%1ms").arg(ms);
    if (ms < 10000)
        return QStringLiteral("%1s").arg(ms / 1000.0, 0, 'f', 1);
    return QStringLiteral("%1s").arg(qRound(ms / 1000.0));
}

// 盲文转圈帧(连接/思考动效);按码点索引,不能按字节(UTF-8 盲文是多字节)
const QString kSpinner = QStringLiteral("⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏");

} // namespace

AiChatDock::AiChatDock(QWidget *parent)
    : QWidget(parent, Qt::Window) // 独立窗口(通常无父:主窗口最小化不影响本窗口)
{
    setWindowTitle(tr("我的AI助手"));
    setObjectName(QStringLiteral("aiChatDock"));
    setWindowIcon(QIcon(QStringLiteral(":/logo.ico")));
    setMinimumSize(QSize(460, 520));

    // 记住上次的大小与位置(用户拖到哪就是哪);首次用默认 883x1096
    const QByteArray geo = QSettings().value(QStringLiteral("aiGeometry")).toByteArray();
    if (!geo.isEmpty() && restoreGeometry(geo)) {
        // 恢复成功
    } else {
        resize(883, 1096);
    }
    // 主题跟随主窗口(读主主题,不再读 aiTheme 旧覆盖值),主窗口切主题时
    // 由 MainWindow::broadcastTheme → followHostTheme 实时同步
    m_lightTheme = QSettings().value(QStringLiteral("theme"), QStringLiteral("light"))
                       .toString() != QLatin1String("dark");
    setStyleSheet(chatQss(m_lightTheme));

    // 写入模式 / 思考程度持久化
    QSettings s;
    m_writeMode = qBound(0, s.value(QStringLiteral("aiWriteMode"), 0).toInt(), 2);
    // 思考档位持久化为档位名(字符串);旧版整数 0关1低2中3高 迁移:中并入高
    m_thinkEffort = s.value(QStringLiteral("aiThinkEffort")).toString();
    if (!s.contains(QStringLiteral("aiThinkEffort"))) {
        const int old = s.value(QStringLiteral("aiThinkLevel"), 3).toInt();
        m_thinkEffort = old <= 0 ? QString() : (old == 1 ? QStringLiteral("low") : QStringLiteral("high"));
    }

    buildUi();
    // 网页启动前也保存主题,就绪后与会话一起同步。
    m_chat->setLightTheme(m_lightTheme);
    wireWorker();
    loadSession(); // 有上次对话则恢复;没有则显示欢迎卡片页

    // 动效计时器:连接转圈 + 思考转圈 + 流式光标闪烁 + 等待秒数
    m_animTimer = new QTimer(this);
    m_animTimer->setInterval(120);
    connect(m_animTimer, &QTimer::timeout, this, [this] { onAnimTick(); });
}

AiChatDock::~AiChatDock()
{
    // 关窗路径可能不走 hideEvent(delete 不触发):析构兜底保存几何
    QSettings().setValue(QStringLiteral("aiGeometry"), saveGeometry());
    // 先请求中断在途请求,再停线程:避免线程卡在 HTTP 里拖住退出
    HttpAbort::request();
    if (m_worker) {
        // 原 runInsert 用 BlockingQueuedConnection 回调本对象:析构开始后
        // GUI 不再处理事件,工作线程会永久卡住,wait 超时销毁 QThread 即 qFatal。
        // 置位 stop 后 worker 侧 GUI 回调立即走超时放行,线程得以正常退出。
        m_worker->requestStop();
    }
    m_thread->quit();
    if (!m_thread->wait(3000)) {
        // 线程没在 3s 内退出。m_thread 是本对象的子对象,析构后续会被 Qt 直接
        // delete —— 那时若线程仍在运行,QThread 会 qFatal("Destroyed while thread
        // is still running"),直接闪退。这里置 NULL 断掉父子关系,让线程由自身
        // 的 deleteLater 路径回收,而不是被析构途中强行销毁。
        qWarning("AI 工作线程未在超时内退出,放弃回收以避免崩溃");
        m_thread->setParent(nullptr);
        m_thread = nullptr;
    }
}

void AiChatDock::buildUi()
{
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(12, 10, 12, 10);
    lay->setSpacing(8);

    // ── 顶栏:状态点 + 模型 + 新对话/设置 ──
    auto *top = new QHBoxLayout;
    top->setSpacing(6);
    m_model = new QLabel(tr("未配置供应商"), this);
    m_model->setObjectName(QStringLiteral("aiModel"));
    m_model->setCursor(Qt::PointingHandCursor);
    m_model->setToolTip(tr("点击切换供应商/模型"));
    m_model->installEventFilter(this); // 点击 = 模型切换菜单
    top->addWidget(m_model, 1);
    m_themeBtn = new QPushButton(m_lightTheme ? tr("🌙") : tr("☀"), this);
    m_themeBtn->setObjectName(QStringLiteral("aiGhost"));
    m_themeBtn->setCursor(Qt::PointingHandCursor);
    m_themeBtn->setToolTip(tr("切换浅色/深色主题"));
    connect(m_themeBtn, &QPushButton::clicked, this, [this] { toggleTheme(); });
    m_newBtn = new QPushButton(tr("⟳"), this);
    m_newBtn->setObjectName(QStringLiteral("aiGhost"));
    m_newBtn->setCursor(Qt::PointingHandCursor);
    m_newBtn->setToolTip(tr("新对话(清空上下文)"));
    m_setBtn = new QPushButton(tr("⚙"), this);
    m_setBtn->setObjectName(QStringLiteral("aiGhost"));
    m_setBtn->setCursor(Qt::PointingHandCursor);
    m_setBtn->setToolTip(tr("AI 设置"));
    top->addWidget(m_themeBtn);
    top->addWidget(m_newBtn);
    top->addWidget(m_setBtn);
    lay->addLayout(top);

    auto *separator = new QFrame(this);
    separator->setObjectName(QStringLiteral("aiSeparator"));
    separator->setFixedHeight(1);
    lay->addWidget(separator);

    // ── 中央区:欢迎卡片页与可选择文字的网页对话 ──
    m_msgs = new ChatModel(this);
    m_stack = new QStackedWidget(this);
    m_chat = new ChatWebView(m_msgs, m_stack);
    m_stack->addWidget(buildWelcomePage());
    m_stack->addWidget(m_chat);
    m_stack->setCurrentIndex(0);
    lay->addWidget(m_stack, 1);
    connect(m_chat, &ChatWebView::exportRequested, this, [this] {
        QString all;
        for (int i = 0; i < m_msgs->rowCount(); ++i) {
            const auto *m = m_msgs->msgAt(i);
            if (m->kind != ChatMsg::User && m->kind != ChatMsg::Assistant) continue;
            all += (m->kind == ChatMsg::User ? QStringLiteral("## 我\n") : QStringLiteral("## AI\n"))
                 + m->text + QStringLiteral("\n\n");
        }
        const QString path = QFileDialog::getSaveFileName(this, tr("导出对话"),
                            QStringLiteral("对话.md"), tr("Markdown (*.md)"));
        if (path.isEmpty()) return;
        QSaveFile file(path);
        const QByteArray data = all.trimmed().toUtf8();
        if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
            m_msgs->append(ChatMsg::Notice, tr("导出失败: %1").arg(file.errorString()), QStringLiteral("error"));
            return;
        }
        m_msgs->append(ChatMsg::Notice, tr("已导出 %1").arg(path), QStringLiteral("ok"));
    });

    // 用户消息"复制进询问框":原文回填输入框(可改后再发,适合追问)
    connect(m_chat, &ChatWebView::askEditRequested, this, [this](const QString &text) {
        m_input->setPlainText(text);
        QTextCursor c = m_input->textCursor();
        c.movePosition(QTextCursor::End);
        m_input->setTextCursor(c);
        m_input->setFocus();
    });

    // 消息出现 → 切到聊天页;清空 → 回欢迎页
    connect(m_msgs, &QAbstractItemModel::rowsInserted, this, [this] { updateStackPage(); });
    connect(m_msgs, &QAbstractItemModel::modelReset, this, [this] { updateStackPage(); });

    // ── 附件缩略图条(有附件才显示) ──
    m_attachStrip = new QWidget(this);
    m_attachStrip->setObjectName(QStringLiteral("aiAttachStrip"));
    m_attachLay = new QHBoxLayout(m_attachStrip);
    m_attachLay->setContentsMargins(0, 0, 0, 0);
    m_attachLay->setSpacing(6);
    m_attachStrip->hide();
    lay->addWidget(m_attachStrip);

    // ── 控制行:模型切换 / 写入模式 / 思考程度 / 附件按钮 ──
    auto *controls = new QHBoxLayout;
    controls->setSpacing(6);
    // 模型切换按钮:"供应商/模型 ▾",点开双栏弹层(左供应商右模型)
    m_modelBtn = new QPushButton(this);
    m_modelBtn->setObjectName(QStringLiteral("aiModelBtn"));
    m_modelBtn->setCursor(Qt::PointingHandCursor);
    m_modelBtn->setToolTip(tr("切换供应商/模型"));
    m_modelBtn->setText(tr("未配置供应商 ▾"));
    connect(m_modelBtn, &QPushButton::clicked, this, [this] { showModelMenu(); });
    controls->addWidget(m_modelBtn);
    m_writeBtn = new QPushButton(this);
    m_writeBtn->setObjectName(QStringLiteral("aiWrite"));
    m_writeBtn->setCursor(Qt::PointingHandCursor);
    m_writeBtn->setToolTip(tr("写入模式:AI 是否/如何把内容写进当前文档"));
    applyWriteModeStyle();
    connect(m_writeBtn, &QPushButton::clicked, this, [this] {
        // 下拉菜单(不再循环切换)
        QMenu menu(this);
        menu.setStyleSheet(chatQss(m_lightTheme));
        QAction *act0 = menu.addAction(tr("⊘ 不写入 — AI 只在面板回答"));
        QAction *act1 = menu.addAction(tr("✎ AI 决定 — 该写的自动写"));
        QAction *act2 = menu.addAction(tr("✎✎ 强制全写 — 全部回答写进文档"));
        act0->setCheckable(true);
        act1->setCheckable(true);
        act2->setCheckable(true);
        act0->setChecked(m_writeMode == 0);
        act1->setChecked(m_writeMode == 1);
        act2->setChecked(m_writeMode == 2);
        const QAction *picked = menu.exec(m_writeBtn->mapToGlobal(
            QPoint(0, m_writeBtn->height() + 4)));
        if (picked == act0)
            m_writeMode = 0;
        else if (picked == act1)
            m_writeMode = 1;
        else if (picked == act2)
            m_writeMode = 2;
        else
            return; // 没选
        QSettings().setValue(QStringLiteral("aiWriteMode"), m_writeMode);
        applyWriteModeStyle();
    });

    m_thinkBox = new QComboBox(this);
    m_thinkBox->setObjectName(QStringLiteral("aiThink"));
    m_thinkBox->setToolTip(tr("思考程度(档位按当前模型配置;模型不支持时自动忽略)"));
    rebuildThinkBox({QStringLiteral("low"), QStringLiteral("high")});
    connect(m_thinkBox, &QComboBox::currentIndexChanged, this,
            [this](int) { onThinkLevelChanged(); });

    auto *attachBtn = new QPushButton(this);
    attachBtn->setObjectName(QStringLiteral("aiAttach"));
    attachBtn->setProperty("iconName",QStringLiteral("attach"));
    attachBtn->setIcon(UiIcons::icon("attach",QColor(m_lightTheme ? "#536277" : "#c2c8d0")));
    attachBtn->setIconSize(QSize(20,20));
    attachBtn->setAccessibleName(tr("添加附件"));
    attachBtn->setCursor(Qt::PointingHandCursor);
    attachBtn->setToolTip(tr("上传图片或文本文件(也可以直接 Ctrl+V 粘贴截图)"));
    connect(attachBtn, &QPushButton::clicked, this, [this] {
        const QStringList files = QFileDialog::getOpenFileNames(
            this, tr("添加附件"), QString(),
            tr("图片 (*.png *.jpg *.jpeg *.gif *.webp *.bmp);;文本文件 (*.md *.txt *.json *.csv *.log *.cpp *.h *.py *.js *.ts);;所有文件 (*.*)"));
        if (files.isEmpty())
            return;
        QList<QPair<QString, QByteArray>> images;
        for (const QString &f : files) {
            const QString suffix = QFileInfo(f).suffix().toLower();
            static const QStringList kImg = { QStringLiteral("png"), QStringLiteral("jpg"),
                                               QStringLiteral("jpeg"), QStringLiteral("gif"),
                                               QStringLiteral("webp"), QStringLiteral("bmp") };
            if (kImg.contains(suffix)) {
                QFile in(f);
                if (in.open(QIODevice::ReadOnly))
                    images.append({ QFileInfo(f).fileName(), in.readAll() });
                continue;
            }
            QFile in(f);
            if (!in.open(QIODevice::ReadOnly)) {
                m_msgs->append(ChatMsg::Notice, tr("无法读取 %1").arg(QFileInfo(f).fileName()),
                               QStringLiteral("error"));
                continue;
            }
            const QByteArray data = in.read(256 * 1024);
            if (data.contains('\0')) {
                m_msgs->append(ChatMsg::Notice,
                               tr("跳过二进制文件 %1").arg(QFileInfo(f).fileName()),
                               QStringLiteral("muted"));
                continue;
            }
            attachTextFile(QFileInfo(f).fileName(), QString::fromUtf8(data));
        }
        if (!images.isEmpty())
            attachImages(images);
        scrollBottom();
    });

    controls->addWidget(m_writeBtn);
    controls->addWidget(m_thinkBox);
    controls->addStretch();
    lay->addLayout(controls);

    // ── 输入区:圆角容器 + 内嵌圆形发送按钮 ──
    auto *inputBox = new QWidget(this);
    inputBox->setObjectName(QStringLiteral("aiInputBox"));
    auto *inputLay = new QHBoxLayout(inputBox);
    inputLay->setContentsMargins(12, 8, 8, 8);
    inputLay->setSpacing(8);
    m_input = new AiInputEdit(inputBox);
    m_input->setObjectName(QStringLiteral("aiInputInner"));
    m_input->setFixedHeight(44);
    // Enter 发送 + 截图粘贴:事件过滤器;视口也要装(点击落在 viewport 上)
    m_input->installEventFilter(this);
    m_input->viewport()->installEventFilter(this);
    m_send = new QPushButton(tr("➤"), inputBox);
    m_send->setObjectName(QStringLiteral("aiSend"));
    m_send->setCursor(Qt::PointingHandCursor);
    m_send->setToolTip(tr("发送"));
    inputLay->addWidget(attachBtn,0,Qt::AlignBottom);
    inputLay->addWidget(m_input, 1);
    inputLay->addWidget(m_send, 0, Qt::AlignBottom);
    lay->addWidget(inputBox);

    // 底部提示

    // 发送/停止二合一:忙时点击 = 停止
    connect(m_send, &QPushButton::clicked, this, [this] {
        if (m_busy)
            stop();
        else
            send();
    });
    // 输入随内容自适应高度
    connect(m_input, &QPlainTextEdit::textChanged, this, [this] { autoGrowInput(); });
    connect(m_newBtn, &QPushButton::clicked, this, [this] { clearConversation(); });
    connect(m_setBtn, &QPushButton::clicked, this, [this] {
        QMenu menu(this);
        menu.addAction(tr("AI 供应商…"),this,[this]{emit configRequested();});
        menu.addAction(tr("AI 技能目录…"),this,[this]{emit skillsRequested();});
        menu.exec(m_setBtn->mapToGlobal(QPoint(0,m_setBtn->height())));
    });
}

void AiChatDock::wireWorker()
{
    // 跨线程队列信号需要元类型注册
    qRegisterMetaType<QVector<AiAttach>>("QVector<AiAttach>");

    m_thread = new QThread(this);
    m_worker = new AiWorker();
    m_worker->setDock(this);
    m_worker->moveToThread(m_thread);
    connect(m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(this, &AiChatDock::runRequested, m_worker,
            &AiWorker::run);

    // 重新生成:删除该 AI 行及后续,回溯用户指令重发
    connect(m_chat, &ChatWebView::regenerateRequested, this,
            [this](int row) {
        if (m_busy || !m_hasProvider)
            return;
        // 找该行之前最近的 User 行
        int userRow = -1;
        for (int i = row - 1; i >= 0; --i) {
            const ChatMsg *m = m_msgs->msgAt(i);
            if (m && m->kind == ChatMsg::User) {
                userRow = i;
                break;
            }
        }
        if (userRow < 0)
            return;
        const QString userText = m_msgs->msgAt(userRow)->text;
        // 从 worker 历史里截断:保留 userRow 之前的(含该 user 消息)
        const int keepCount = userRow + 1;
        // 【防御】lambda 在 worker 线程执行,不能捕获 this 再解引用 m_worker:
        // 析构期间 wait() 阻塞 GUI 时,worker 仍可能跑这条已投递的调用。
        // 直接捕获 worker 指针(QPointer 在其销毁后自动置空)。
        QPointer<AiWorker> worker = m_worker;
        QMetaObject::invokeMethod(m_worker, [worker, keepCount]() {
            if (worker)
                worker->truncateHistory(keepCount);
        }, Qt::QueuedConnection);
        // 模型里删掉 userRow 之后的行(含 AI 回复)
        m_msgs->removeFrom(userRow + 1);
        scrollBottom();
        // 重发(不重新显示用户气泡,直接发原始 text)
        QString full = userText;
        // 如果原始 text 有 📎 附件行,去掉(附件已不存)
        const int clipMark = full.indexOf(QStringLiteral("\n📎 "));
        if (clipMark > 0)
            full = full.left(clipMark);
        m_dispatchImages.clear();
        m_dispatchMode = m_writeMode;
        m_dispatchEffort = m_thinkEffort;
        dispatch(full);
    });

    // 思考滚动行:正文出现后冻结为"思考 Ns"
    connect(m_worker, &AiWorker::thinkingDelta, this, [this](const QString &text) {
        if (m_assistRow >= 0)
            return; // 正文已在输出,思考行冻结
        if (m_thinkRow < 0)
            m_thinkRow = m_msgs->append(ChatMsg::Thinking, QString());
        ChatMsg *m = m_msgs->msgAt(m_thinkRow);
        if (!m)
            return;
        if (!m_thinkElapsed.isValid())
            m_thinkElapsed.start();
        // 完整思维链留档(fullText),text 只保留尾句用于单行显示
        m->fullText += text;
        m->text = m->fullText.size() > 160 ? m->fullText.right(140) : m->fullText;
        m->expandable = !m->fullText.trimmed().isEmpty();
        m_msgs->touch(m_thinkRow); // meta(思考中 Ns ⠹)由动效计时器渲染
        scrollBottom();
    });

    // 流式正文:气泡按原文追加,轮末 finalize 成 Markdown
    connect(m_worker, &AiWorker::textDelta, this, [this](const QString &text) {
        if (m_assistRow < 0) {
            if (m_thinkRow >= 0) {
                if (ChatMsg *t = m_msgs->msgAt(m_thinkRow)) {
                    t->meta = tr("💭 思考 %1").arg(
                        fmtSeconds(m_thinkElapsed.isValid()
                                       ? m_thinkElapsed.elapsed() : 0));
                    m_msgs->touch(m_thinkRow);
                }
                m_thinkRow = -1;
            }
            m_assistRow = m_msgs->append(ChatMsg::Assistant, QString());
            m_msgs->msgAt(m_assistRow)->preferLatex=m_turnPreferLatex;
        }
        m_anyText = true;
        if (ChatMsg *m = m_msgs->msgAt(m_assistRow)) {
            m->text += text;
            m_msgs->touch(m_assistRow);
        }
        scrollBottom();
    });

    // 本轮统计:思考用时/速度/token(气泡下的小字)
    connect(m_worker, &AiWorker::turnStats, this,
            [this](qint64 thinkMs, double tps, qint64 outTok, qint64 inTok) {
        QString meta;
        if (thinkMs > 0)
            meta += tr("思考 %1").arg(fmtSeconds(thinkMs));
        if (tps > 0)
            meta += (meta.isEmpty() ? QString() : tr(" · ")) + tr("%1 tok/s").arg(tps, 0, 'f', 1);
        if (outTok > 0)
            meta += (meta.isEmpty() ? QString() : tr(" · ")) + tr("输出 %1").arg(outTok);
        if (inTok > 0)
            meta += (meta.isEmpty() ? QString() : tr(" · ")) + tr("上下文 %1").arg(inTok);
        if (meta.isEmpty())
            return;
        if (m_assistRow >= 0) {
            if (ChatMsg *m = m_msgs->msgAt(m_assistRow)) {
                m->meta = meta;
                m_msgs->touch(m_assistRow);
            }
        } else if (m_thinkRow >= 0) {
            if (ChatMsg *m = m_msgs->msgAt(m_thinkRow)) {
                m->meta = meta;
                m_msgs->touch(m_thinkRow);
            }
        }
    });

    // 轮末:气泡重渲染为 Markdown
    connect(m_worker, &AiWorker::turnRendered, this, [this](const QString &full) {
        if(m_assistRow<0 && !full.isEmpty()) {
            m_assistRow=m_msgs->append(ChatMsg::Assistant,QString());
            m_msgs->msgAt(m_assistRow)->preferLatex=m_turnPreferLatex;
        }
        if (m_assistRow >= 0) {
            if (ChatMsg *m = m_msgs->msgAt(m_assistRow)) {
                m->text = full;
                m->finalized = true;
                m_anyText=!full.isEmpty();
                m_msgs->touch(m_assistRow);
            }
        }
        scrollBottom();
    });

    connect(m_worker, &AiWorker::turnFinished, this, [this](const QString &error) {
        // 没等到任何输出就把等待行收尾(错误详情由下方通知行显示)
if (m_thinkRow >= 0) {
                if (ChatMsg *t = m_msgs->msgAt(m_thinkRow)) {
                    t->meta = tr("·");
                    t->role = error.isEmpty() ? QString() : QStringLiteral("error");
                    m_msgs->touch(m_thinkRow);
                }
                m_thinkRow = -1;
            }
        if (error == QLatin1String("已中断")) {
            m_msgs->append(ChatMsg::Notice, tr("⏹ 已停止(可继续输入新指令)"),
                           QStringLiteral("muted"));
        } else if (!error.isEmpty()) {
            m_msgs->append(ChatMsg::Notice, error, QStringLiteral("error"));
        } else if (!m_anyText && m_assistRow < 0) {
            m_msgs->append(ChatMsg::Notice, tr("(本轮没有文字回复)"),
                           QStringLiteral("muted"));
        }
        scrollBottom();
    });

    connect(m_worker, &AiWorker::busyChanged, this, [this](bool busy) {
        m_busy = busy;
        m_chat->setBusy(busy);
        setWindowTitle(busy ? tr("我的AI助手 · 生成中…") : tr("我的AI助手"));
        if (busy) {
            m_send->setText(tr("■"));
            m_send->setStyleSheet(QStringLiteral(
                "border-radius:17px;min-width:34px;max-width:34px;"
                "min-height:34px;max-height:34px;font-size:13px;"
                "background:#2a1418;color:#f87171;border:1px solid #7f1d1d;"));
            m_send->setToolTip(tr("停止"));
            m_anyText = false;
            m_assistRow = -1;
            m_thinkRow = -1;
            m_waitSecs = 0;
            m_animFrame = 0;
            m_thinkElapsed.invalidate();
            // 立刻给出等待反馈,动效计时器接管后续渲染
            m_thinkRow = m_msgs->append(ChatMsg::Thinking, QString());
            if (ChatMsg *m = m_msgs->msgAt(m_thinkRow))
                m->meta = QStringLiteral("⠋ 连接中");
            m_animTimer->start();
        } else {
            m_animTimer->stop();
            m_send->setText(tr("➤"));
            m_send->setStyleSheet(QString());
            m_send->setToolTip(tr("发送"));
            saveSession(); // 轮末落盘:重启可恢复本次对话
            m_input->setFocus();
        }
    });

    // ★ 关键:启动工作线程的事件循环 —— 丢掉它 run() 永远不会执行(黑盒根因,勿删!)
    m_thread->start();
}

void AiChatDock::onAnimTick()
{
    ++m_animFrame;


    // 等待/思考行:转圈 + 秒数(每 8 帧约 1s 刷新一次秒数)
    if (m_busy && m_thinkRow >= 0 && m_assistRow < 0) {
        if (ChatMsg *m = m_msgs->msgAt(m_thinkRow)) {
            const QChar spin = kSpinner.at(m_animFrame % kSpinner.size());
            if (!m_thinkElapsed.isValid()) {
                m_waitSecs = m_animFrame * 120 / 1000;
                m->meta = QStringLiteral("%1 连接中 %2s").arg(spin).arg(m_waitSecs);
            } else {
                const qint64 sec = m_thinkElapsed.elapsed() / 1000;
                m->meta = QStringLiteral("💭 思考中 %1s %2 ·").arg(sec).arg(spin);
            }
            m_msgs->touch(m_thinkRow);
        }
    }


}

void AiChatDock::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    emit visibilityChanged(true);
    m_input->setFocus();
    scrollBottom(); // 会话恢复后再打开:直接滚到最新消息
}

void AiChatDock::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);

}

void AiChatDock::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    QSettings().setValue(QStringLiteral("aiGeometry"), saveGeometry());
    emit visibilityChanged(false);
}

// Enter 发送(Shift+Enter 放行换行);Ctrl+V 图片进附件;欢迎卡片点击即发送
bool AiChatDock::eventFilter(QObject *obj, QEvent *event)
{
    // 欢迎卡片:点击 = 填入预置指令并直接发送
    if (event->type() == QEvent::MouseButtonRelease) {
        if (obj == m_model) {   // 顶栏模型名:点击 = 切换供应商/模型
            showModelMenu();
            return true;
        }
        if (QWidget *card = qobject_cast<QWidget *>(obj)) {
            if (m_cardPrompts.contains(card)) {
                m_input->setPlainText(m_cardPrompts.value(card));
                send();
                return true;
            }
        }
    }
    // 点输入框时把 Win32 焦点强制还给顶层窗口:点过聊天区后,Win32 焦点
    // 停在聊天 WebView2 的 Chrome 子窗口,而 Qt 焦点仍在输入框(Qt 认为
    // 焦点没变就不再抢)→ 键盘消息全进了网页,表现为"输入不了,点一下
    // 思考强度下拉框(会触发 Qt 焦点变化)就好了"
    if (m_input && (obj == m_input || obj == m_input->viewport())
        && event->type() == QEvent::MouseButtonPress) {
        QPointer<QWidget> win = window();
        QTimer::singleShot(0, this, [win] {
            if (win)
                ::SetFocus(reinterpret_cast<HWND>(win->winId()));
        });
        return false;
    }
    if (obj == m_input && event->type() == QEvent::KeyPress) {
        auto *ke = static_cast<QKeyEvent *>(event);
        // 铁律:Enter=发送,Shift+Enter=换行。IME 组合中的 Enter 会被输入法
        // 自己消费(提交用),根本到不了这里;能到这里的都是用户明确按键。
        // 旧版在这里查 isComposing()/QInputMethod::isVisible(),中文输入法
        // 下会把 Enter 拦成换行 —— 就是"写入后消息发不出去"的元凶
        if ((ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter)
            && !(ke->modifiers() & Qt::ShiftModifier)) {
            if (m_busy)
                stop(); // 生成中回车 = 停止(与红色按钮同语义,不再无响应)
            else
                send();
            return true;
        }
        // 拼音组合未上屏时,↑↓ 属于 IME 候选键,别抢(其余按键不受影响)
        if (static_cast<AiInputEdit *>(m_input)->isComposing())
            return false;
        // ↑↓ 翻输入历史(终端式):空框或光标在第一行开头时 ↑ = 上一条
        if ((ke->key() == Qt::Key_Up || ke->key() == Qt::Key_Down)
            && !m_inputHistory.isEmpty()) {
            QTextCursor cur = m_input->textCursor();
            const bool atStart = cur.atStart() || m_input->toPlainText().isEmpty();
            const bool atEnd = cur.atEnd() || m_input->toPlainText().isEmpty();
            if (ke->key() == Qt::Key_Up && atStart) {
                if (m_histIdx < 0)
                    m_histDraft = m_input->toPlainText(); // 记草稿
                if (m_histIdx + 1 < m_inputHistory.size()) {
                    ++m_histIdx;
                    m_input->setPlainText(m_inputHistory.at(
                        m_inputHistory.size() - 1 - m_histIdx));
                    m_input->moveCursor(QTextCursor::End);
                    return true;
                }
            } else if (ke->key() == Qt::Key_Down && m_histIdx >= 0) {
                // ↓ 往回走;到底部恢复草稿
                --m_histIdx;
                if (m_histIdx < 0) {
                    m_input->setPlainText(m_histDraft);
                    m_histDraft.clear();
                } else {
                    m_input->setPlainText(m_inputHistory.at(
                        m_inputHistory.size() - 1 - m_histIdx));
                }
                m_input->moveCursor(QTextCursor::End);
                return true;
            }
        }
        // Ctrl+V:剪贴板里是图片/图片文件 → 进附件栏
        if (ke->key() == Qt::Key_V && (ke->modifiers() & Qt::ControlModifier)) {
            const QMimeData *mime = QApplication::clipboard()->mimeData();
            if (mime && mime->hasImage()) {
                const QImage img = qvariant_cast<QImage>(mime->imageData());
                if (!img.isNull()) {
                    QByteArray bytes;
                    QBuffer buf(&bytes);
                    buf.open(QIODevice::WriteOnly);
                    if (img.save(&buf, "PNG")) {
                        attachImages({ { tr("截图 %1")
                                             .arg(QTime::currentTime().toString(
                                                 QStringLiteral("HH:mm"))),
                                         bytes } });
                        return true; // 图片不落文本框
                    }
                }
            }
            if (mime && mime->hasUrls()) {
                QList<QPair<QString, QByteArray>> images;
                for (const QUrl &u : mime->urls()) {
                    const QString p = u.toLocalFile();
                    if (p.isEmpty() || !QFileInfo(p).isFile())
                        continue;
                    const QString suffix = QFileInfo(p).suffix().toLower();
                    static const QStringList kImg = { QStringLiteral("png"),
                                                      QStringLiteral("jpg"), QStringLiteral("jpeg"),
                                                      QStringLiteral("gif"), QStringLiteral("webp"),
                                                      QStringLiteral("bmp") };
                    if (!kImg.contains(suffix))
                        continue;
                    QFile in(p);
                    if (in.open(QIODevice::ReadOnly))
                        images.append({ QFileInfo(p).fileName(), in.readAll() });
                }
                if (!images.isEmpty()) {
                    attachImages(images);
                    return true;
                }
            }
        }
    }
    return QWidget::eventFilter(obj, event);
}

// ---------------------------------------------------------------------------
// 写入模式 / 思考程度
// ---------------------------------------------------------------------------

void AiChatDock::cycleWriteMode()
{
    m_writeMode = (m_writeMode + 1) % 3;
    QSettings().setValue(QStringLiteral("aiWriteMode"), m_writeMode);
    applyWriteModeStyle();
    const QString msg = m_writeMode == 0 ? tr("写入已关闭:AI 只在面板回答")
                      : m_writeMode == 1 ? tr("写入模式:AI 自己决定何时写进文档")
                                         : tr("写入模式:强制,AI 的全部回答写进文档");
    m_msgs->append(ChatMsg::Notice, msg,
                   m_writeMode == 0 ? QStringLiteral("muted")
                    : m_writeMode == 1 ? QStringLiteral("accent")
                                       : QStringLiteral("ok"));
    scrollBottom();
}

void AiChatDock::applyWriteModeStyle()
{
    // 状态色固定(语义不变),底色随主题取对比值
    const char *bg = m_lightTheme ? "#ffffff" : "#232730";
    const char *bd = m_lightTheme ? "#d9dbe3" : "#383d49";
    const char *mut = m_lightTheme ? "#6b7280" : "#8a8f9a";
    switch (m_writeMode) {
    case 0:
        m_writeBtn->setText(tr("✎ 不写入"));
        m_writeBtn->setStyleSheet(QStringLiteral(
            "background:%1;color:%3;border:1px solid %2;").arg(bg, bd, mut));
        break;
    case 2:
        m_writeBtn->setText(tr("✎✎ 强制全写"));
        m_writeBtn->setStyleSheet(QStringLiteral(
            "background:%1;color:%3;border:1px solid %2;")
                .arg(bg, m_lightTheme ? "#9adbbf" : "#1d5c3c", m_lightTheme ? "#059669" : "#34d399"));
        break;
    default:
        m_writeBtn->setText(tr("✎ AI 决定"));
        m_writeBtn->setStyleSheet(QStringLiteral(
            "background:%1;color:%3;border:1px solid %2;")
                .arg(bg, m_lightTheme ? "#9db8e8" : "#1e3a8a", m_lightTheme ? "#2563eb" : "#60a5fa"));
        break;
    }
}

void AiChatDock::onThinkLevelChanged()
{
    m_thinkEffort = m_thinkBox->currentData().toString();
    QSettings().setValue(QStringLiteral("aiThinkEffort"), m_thinkEffort);
}

// 浅色/深色切换:QSS + 调色板全量换,持久化,立即重绘
void AiChatDock::toggleTheme()
{
    // 会话内临时切换(不持久化):面板始终以主窗口主题为准,
    // 主窗口下次切主题时由 followHostTheme 重新对齐
    applyLightTheme(!m_lightTheme);
}

// 主窗口主题切换时让 AI 面板跟随(此前只在面板构造时读一次主题,
// 之后主窗口切夜间/浅色,AI 面板永远停在旧主题)
void AiChatDock::followHostTheme(bool light)
{
    if (m_lightTheme == light)
        return;
    applyLightTheme(light);
}

void AiChatDock::applyLightTheme(bool light)
{
    m_lightTheme = light;
    m_themeBtn->setText(m_lightTheme ? tr("🌙") : tr("☀"));
    setStyleSheet(chatQss(m_lightTheme));
    m_chat->setLightTheme(m_lightTheme);
    restyleDynamicWidgets();
    repaintAll();
}

void AiChatDock::restyleDynamicWidgets()
{
    // 写入按钮的动态样式随主题重算
    applyWriteModeStyle();
    if(auto *button=findChild<QPushButton *>(QStringLiteral("aiAttach")))
        button->setIcon(UiIcons::icon("attach",QColor(m_lightTheme ? "#536277" : "#c2c8d0")));
}

void AiChatDock::repaintAll()
{
    // 欢迎页子控件由 QSS 驱动,polish 一遍强制换肤
    if (QWidget *welcome = m_stack->widget(0)) {
        if(auto *canvas=welcome->findChild<QWidget *>(QStringLiteral("aiWelcome"))) {
            canvas->setProperty("lightTheme",m_lightTheme);
            canvas->update();
        }
        for (QWidget *c : welcome->findChildren<QWidget *>()) {
            c->style()->unpolish(c);
            c->style()->polish(c);
            if(auto *label=qobject_cast<QLabel *>(c); label && label->property("welcomeIcon").isValid())
                label->setPixmap(UiIcons::icon(label->property("welcomeIcon").toString(),
                    QColor(m_lightTheme ? "#69799a" : "#a5b4d2")).pixmap(QSize(19,19),devicePixelRatioF()));
        }
    }
}

// ---------------------------------------------------------------------------
// 附件
// ---------------------------------------------------------------------------

void AiChatDock::attachImages(const QList<QPair<QString, QByteArray>> &files)
{
    for (const auto &f : files) {
        AiAttach a;
        a.name = f.first;
        a.mime = QStringLiteral("image/png");
        // 按魔数识别真实类型(jpg/webp/gif 上传时 MIME 不能标错)
        const QByteArray &d = f.second;
        if (d.startsWith("\x89PNG"))
            a.mime = QStringLiteral("image/png");
        else if (d.startsWith("\xff\xd8\xff"))
            a.mime = QStringLiteral("image/jpeg");
        else if (d.startsWith("GIF8"))
            a.mime = QStringLiteral("image/gif");
        else if (d.startsWith("RIFF") && d.size() > 12 && d.mid(8, 4) == "WEBP")
            a.mime = QStringLiteral("image/webp");
        else if (d.startsWith("BM"))
            a.mime = QStringLiteral("image/bmp");
        a.base64 = QString::fromLatin1(d.toBase64());
        m_images.append(a);
    }
    rebuildAttachStrip();
    scrollBottom();
}

void AiChatDock::attachTextFile(const QString &name, const QString &content)
{
    m_textFiles.append({ name, content });
    rebuildAttachStrip();
}

void AiChatDock::rebuildAttachStrip()
{
    // 清空重建
    while (QLayoutItem *it = m_attachLay->takeAt(0)) {
        if (it->widget())
            it->widget()->deleteLater();
        delete it;
    }
    const int total = m_images.size() + m_textFiles.size();
    m_attachStrip->setVisible(total > 0);
    if (total == 0)
        return;

    for (int i = 0; i < m_images.size(); ++i) {
        QImage img;
        img.loadFromData(QByteArray::fromBase64(m_images[i].base64.toLatin1()));
        auto *thumb = new QLabel(m_attachStrip);
        thumb->setFixedSize(46, 46);
        thumb->setAlignment(Qt::AlignCenter);
        // 附件缩略图底色跟随主题(此前写死深色,浅色主题下是一排黑块)
        thumb->setStyleSheet(m_lightTheme
            ? QStringLiteral("background:#eef0f5;border:1px solid #d9dbe3;border-radius:8px;")
            : QStringLiteral("background:#2c313c;border:1px solid #3d434f;border-radius:8px;"));
        if (!img.isNull())
            thumb->setPixmap(QPixmap::fromImage(img.scaled(
                40, 40, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
        else
            thumb->setText(tr("🖼"));
        thumb->setToolTip(m_images[i].name);
        m_attachLay->addWidget(thumb);
        const int idx = i;
        auto *rm = new QPushButton(tr("✕"), m_attachStrip);
        rm->setFixedSize(20, 20);
        rm->setCursor(Qt::PointingHandCursor);
        rm->setStyleSheet(m_lightTheme
            ? QStringLiteral("background:#dfe2ea;color:#3a4150;border:none;border-radius:10px;font-size:12px;")
            : QStringLiteral("background:#3d434f;color:#e8ebf2;border:none;border-radius:10px;font-size:12px;"));
        connect(rm, &QPushButton::clicked, this, [this, idx] {
            m_images.removeAt(idx);
            rebuildAttachStrip();
        });
        m_attachLay->addWidget(rm);
    }
    for (int i = 0; i < m_textFiles.size(); ++i) {
        auto *chip = new QLabel(QStringLiteral("📄 ") + m_textFiles[i].first,
                                m_attachStrip);
        chip->setStyleSheet(m_lightTheme
            ? QStringLiteral("background:#eef0f5;color:#3a4150;border:1px solid #d9dbe3;"
                            "border-radius:14px;padding:4px 12px;font-size:12px;")
            : QStringLiteral("background:#2c313c;color:#e8ebf2;border:1px solid #3d434f;"
                            "border-radius:14px;padding:4px 12px;font-size:12px;"));
        chip->setToolTip(tr("文本文件,内容将随消息发给 AI"));
        m_attachLay->addWidget(chip);
        const int idx = i;
        auto *rm = new QPushButton(tr("✕"), m_attachStrip);
        rm->setFixedSize(20, 20);
        rm->setCursor(Qt::PointingHandCursor);
        rm->setStyleSheet(m_lightTheme
            ? QStringLiteral("background:#dfe2ea;color:#3a4150;border:none;border-radius:10px;font-size:12px;")
            : QStringLiteral("background:#3d434f;color:#e8ebf2;border:none;border-radius:10px;font-size:12px;"));
        connect(rm, &QPushButton::clicked, this, [this, idx] {
            m_textFiles.removeAt(idx);
            rebuildAttachStrip();
        });
        m_attachLay->addWidget(rm);
    }
    m_attachLay->addStretch();
}

void AiChatDock::clearAttachments()
{
    m_images.clear();
    m_textFiles.clear();
    rebuildAttachStrip();
}

// ---------------------------------------------------------------------------
// 基础流程
// ---------------------------------------------------------------------------

void AiChatDock::autoGrowInput()
{
    const int content = int(m_input->document()->size().height());
    const int h = qBound(44, content + 8, 150);
    if (h != m_input->height())
        m_input->setFixedHeight(h);
}

// 欢迎卡片页:基于 Mswrite 主题的常用指令;用户一发消息(或点卡片)即消失
QWidget *AiChatDock::buildWelcomePage()
{
    auto *scroll = new QScrollArea;
    scroll->setObjectName(QStringLiteral("aiWelcomeScroll"));
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setStyleSheet(QStringLiteral("QScrollArea#aiWelcomeScroll { background:transparent; border:0; }"));
    auto *page = new WelcomeCanvas;
    page->setObjectName(QStringLiteral("aiWelcome"));
    page->setProperty("lightTheme",m_lightTheme);
    scroll->setWidget(page);
    auto *lay = new QVBoxLayout(page);
    lay->setContentsMargins(24, 30, 24, 24);
    lay->addStretch(6);

    const int hour=QTime::currentTime().hour();
    const QString greeting=hour<6 ? tr("夜深了") : hour<12 ? tr("上午好") : hour<18 ? tr("下午好") : tr("晚上好");
    auto *title = new QLabel(tr("%1，今天想写点什么？").arg(greeting), page);
    title->setObjectName(QStringLiteral("aiWelcomeTitle"));
    title->setAlignment(Qt::AlignHCenter);
    title->setWordWrap(true);
    auto *sub = new QLabel(tr("MSWRITE  ·  从一个想法，到一篇好文章"), page);
    sub->setWordWrap(true);
    sub->setObjectName(QStringLiteral("aiWelcomeSub"));
    sub->setAlignment(Qt::AlignHCenter);
    lay->addWidget(title);
    lay->addWidget(sub);
    lay->addSpacing(28);

    struct Card {
        const char *icon;
        QString title;
        QString desc;
        QString prompt;
    };
    const QVector<Card> cards = {
        { "edit", tr("续写正文"), tr("从光标处接着往下写"),
          tr("请从当前光标处接着文档内容往下写两段,保持文风一致。") },
        { "summary", tr("全文总结"), tr("提炼全文的关键要点"),
          tr("请在文档末尾写一段全文总结。") },
        { "sparkle", tr("润色文字"), tr("让选中的文字更流畅"),
          tr("请润色我选中的文字:保持原意,表达更流畅、更精炼。") },
        { "formula", tr("插入公式"), tr("写下清晰的数学表达"),
          tr("请在光标处插入一段演示用的 LaTeX 公式(包含分数与求和),并配一句说明。") },
        { "book", tr("文档问答"), tr("围绕当前文档一起探索"),
          tr("阅读当前文档,在面板里直接告诉我它的核心要点(不要写入文档)。") },
        { "image", tr("看图写说明"), tr("粘贴图片，补上文字"),
          tr("看一下我粘贴的截图,在光标处为它写一段说明。") },
    };

    auto *gridWrap = new QWidget(page);
    auto *grid = new QGridLayout(gridWrap);
    grid->setObjectName(QStringLiteral("aiWelcomeGrid"));
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(10);
    for (int i = 0; i < cards.size(); ++i) {
        auto *card = new QFrame(gridWrap);
        card->setObjectName(QStringLiteral("aiCard"));
        card->setMinimumHeight(82);
        card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        card->setCursor(Qt::PointingHandCursor);
        auto *cl = new QVBoxLayout(card);
        cl->setContentsMargins(14, 12, 14, 10);
        cl->setSpacing(2);
        auto *heading=new QHBoxLayout;
        heading->setSpacing(8);
        auto *icon=new QLabel(card);
        icon->setFixedSize(20,20);
        icon->setProperty("welcomeIcon",QString::fromUtf8(cards[i].icon));
        icon->setPixmap(UiIcons::icon(QString::fromUtf8(cards[i].icon),QColor(m_lightTheme ? "#69799a" : "#a5b4d2"))
            .pixmap(QSize(19,19),devicePixelRatioF()));
        auto *head = new QLabel(cards[i].title,card);
        head->setObjectName(QStringLiteral("aiCardHead"));
        auto *desc = new QLabel(cards[i].desc, card);
        desc->setObjectName(QStringLiteral("aiCardDesc"));
        desc->setWordWrap(true);
        heading->addWidget(icon); heading->addWidget(head); heading->addStretch();
        cl->addLayout(heading);
        cl->addWidget(desc);
        // 点击卡片 = 填入指令并直接发送
        card->installEventFilter(this);
        m_cardPrompts.insert(card, cards[i].prompt);
        grid->addWidget(card, i / 3, i % 3);
    }
    lay->addWidget(gridWrap);
    lay->addStretch(5);
    return scroll;
}

void AiChatDock::updateStackPage()
{
    m_stack->setCurrentIndex(m_msgs->rowCount() == 0 ? 0 : 1);
}

// ---------------------------------------------------------------------------
// 会话持久化(%APPDATA%/Mswrite/ai-session.json):重启恢复对话与模型上下文
// ---------------------------------------------------------------------------

QString AiChatDock::sessionFilePath()
{
    return qEnvironmentVariable("APPDATA") + QStringLiteral("/Mswrite/ai-session.json");
}

void AiChatDock::saveSession()
{
    QJsonArray rows;
    for (int i = 0; i < m_msgs->rowCount(); ++i) {
        const ChatMsg *m = m_msgs->msgAt(i);
        if (!m || m->kind == ChatMsg::Thinking) // 思考行是过程态,不存
            continue;
        QJsonArray imgs;
        for (const AiAttach &a : m->images)
            imgs.append(QJsonObject{{QStringLiteral("n"), a.name},
                                    {QStringLiteral("mime"), a.mime},
                                    {QStringLiteral("b64"), a.base64}});
        rows.append(QJsonObject{
            { QStringLiteral("k"), int(m->kind) },
            { QStringLiteral("t"), m->text },
            { QStringLiteral("c"), m->role },
            { QStringLiteral("m"), m->meta },
            { QStringLiteral("hero"), m->hero },
            { QStringLiteral("fin"), m->finalized },
            { QStringLiteral("preferLatex"), m->preferLatex },
            { QStringLiteral("imgs"), imgs },
        });
    }
    QJsonArray hist;
    const QVector<ChatMessage> h = m_worker->historySnapshot();
    for (const ChatMessage &mm : h) {
        QJsonObject o{
            { QStringLiteral("role"), mm.role },
            { QStringLiteral("text"), mm.text },
            { QStringLiteral("cid"), mm.toolCallId },
            { QStringLiteral("tn"), mm.toolName },
        };
        QJsonArray tcs;
        for (const ToolCall &tc : mm.toolCalls) {
            tcs.append(QJsonObject{
                { QStringLiteral("id"), tc.id },
                { QStringLiteral("name"), tc.name },
                { QStringLiteral("input"), tc.input },
                { QStringLiteral("thoughtSignature"), tc.thoughtSignature },
            });
        }
        o.insert(QStringLiteral("toolCalls"), tcs);
        QJsonArray himgs;
        for (const AiAttach &a : mm.images)
            himgs.append(QJsonObject{{QStringLiteral("n"), a.name},
                                     {QStringLiteral("mime"), a.mime},
                                     {QStringLiteral("b64"), a.base64}});
        o.insert(QStringLiteral("imgs"), himgs);
        hist.append(o);
    }
    const QString path = sessionFilePath();
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        qWarning() << "Mswrite: cannot create AI session directory";
        return;
    }
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        qWarning() << "Mswrite: cannot open AI session" << f.errorString();
        return;
    }
    const QByteArray data = QJsonDocument(QJsonObject{
        { QStringLiteral("version"), 1 },
        { QStringLiteral("rows"), rows },
        { QStringLiteral("history"), hist },
    }).toJson(QJsonDocument::Compact);
    if (f.write(data) != data.size() || !f.commit())
        qWarning() << "Mswrite: cannot save AI session" << f.errorString();
}

bool AiChatDock::loadSession()
{
    QFile f(sessionFilePath());
    if (!f.exists() || !f.open(QIODevice::ReadOnly))
        return false;
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    const QJsonArray rows = root.value(QStringLiteral("rows")).toArray();
    if (rows.isEmpty())
        return false;
    for (const QJsonValue &v : rows) {
        const QJsonObject o = v.toObject();
        const int k = o.value(QStringLiteral("k")).toInt();
        if (k < int(ChatMsg::User) || k > int(ChatMsg::Notice))
            continue;
        const int row = m_msgs->append(static_cast<ChatMsg::Kind>(k),
                                       o.value(QStringLiteral("t")).toString(),
                                       o.value(QStringLiteral("c")).toString());
        if (ChatMsg *m = m_msgs->msgAt(row)) {
            m->meta = o.value(QStringLiteral("m")).toString();
            m->hero = o.value(QStringLiteral("hero")).toBool();
            m->finalized = o.value(QStringLiteral("fin")).toBool();
            m->preferLatex = o.value(QStringLiteral("preferLatex")).toBool();
            const QJsonArray imgs = o.value(QStringLiteral("imgs")).toArray();
            for (const QJsonValue &iv : imgs) {
                const QJsonObject io = iv.toObject();
                m->images.append({io.value(QStringLiteral("n")).toString(),
                                  io.value(QStringLiteral("mime")).toString(),
                                  io.value(QStringLiteral("b64")).toString()});
            }
        }
    }
    QVector<ChatMessage> hist;
    const QJsonArray hs = root.value(QStringLiteral("history")).toArray();
    for (const QJsonValue &v : hs) {
        const QJsonObject o = v.toObject();
        ChatMessage m;
        m.role = o.value(QStringLiteral("role")).toString();
        m.text = o.value(QStringLiteral("text")).toString();
        m.toolCallId = o.value(QStringLiteral("cid")).toString();
        m.toolName = o.value(QStringLiteral("tn")).toString();
        const QJsonArray tcs = o.value(QStringLiteral("toolCalls")).toArray();
        for (const QJsonValue &tv : tcs) {
            const QJsonObject to = tv.toObject();
            ToolCall tc;
            tc.id = to.value(QStringLiteral("id")).toString();
            tc.name = to.value(QStringLiteral("name")).toString();
            tc.input = to.value(QStringLiteral("input")).toObject();
            tc.thoughtSignature = to.value(QStringLiteral("thoughtSignature")).toString();
            m.toolCalls.push_back(tc);
        }
        const QJsonArray himgs = o.value(QStringLiteral("imgs")).toArray();
        for (const QJsonValue &iv : himgs) {
            const QJsonObject io = iv.toObject();
            m.images.append({io.value(QStringLiteral("n")).toString(),
                             io.value(QStringLiteral("mime")).toString(),
                             io.value(QStringLiteral("b64")).toString()});
        }
        hist.push_back(m);
    }
    // 模型上下文回填到工作线程(排队执行,天然串行)
    // 【防御】lambda 在 worker 线程执行:捕获 worker 本身而非 this,
    // 析构期间 wait() 阻塞 GUI 时不会解引用已析构的 this(UAF)。
    QPointer<AiWorker> worker = m_worker;
    QMetaObject::invokeMethod(m_worker, [worker, hist]() {
        if (worker)
            worker->restoreHistory(hist);
    }, Qt::QueuedConnection);
    return true;
}

void AiChatDock::applyProvider(const AiProvider &p)
{
    const QString modelId = p.effectiveModelId();
    m_hasProvider = p.protocol != Protocol::Unsupported && !p.apiKey.trimmed().isEmpty()
                    && !p.baseUrl.trimmed().isEmpty() && !modelId.trimmed().isEmpty();
    m_modelText = QStringLiteral("%1 · %2").arg(p.name, modelId);
    m_model->setText(m_busy ? m_modelText + tr(" · 生成中…") : m_modelText);
    m_curProvider = p.name;
    m_curModelId = modelId;
    if (m_modelBtn) {
        // 供应商/模型名太长会把控制行撑变形:超长就省略模型名尾部
        QString full = p.name + QLatin1Char('/') + modelId;
        if (full.size() > 34)
            full = p.name + QLatin1Char('/') + modelId.left(qMax(8, 30 - p.name.size())) + QStringLiteral("…");
        m_modelBtn->setText(full + QStringLiteral(" ▾"));
    }
    rebuildThinkBox(p.effectiveThinkLevels());

    AiLlmConfig cfg;
    cfg.apiKey = p.apiKey;
    cfg.baseUrl = p.baseUrl;
    cfg.model = modelId;
cfg.protocol = p.protocol;
    cfg.maxTokens = p.effectiveMaxTokens(); // 0 = 默认 (LlmCodec 落 1000448)
    // 【防御】同上：捕获 worker 本身，避免析构时 UAF。
    QPointer<AiWorker> worker = m_worker;
    QMetaObject::invokeMethod(m_worker, [worker, cfg]() {
        if (worker)
            worker->applyConfig(cfg);
    }, Qt::QueuedConnection);
}

// 思考档位下拉 = 关 + 当前模型配置的档位列表(每模型独立,可自定义)
void AiChatDock::rebuildThinkBox(const QStringList &levels)
{
    const QString keep = m_thinkEffort;
    const QSignalBlocker blocker(m_thinkBox);
    m_thinkBox->clear();
    m_thinkBox->addItem(tr("思考 关"), QString());
    for (const QString &lv : levels)
        m_thinkBox->addItem(tr("思考 %1").arg(lv), lv);
    int idx = m_thinkBox->findData(keep);
    if (idx < 0)
        idx = m_thinkBox->count() - 1; // 档位名不在本模型列表:回退到最高档
    if (idx < 0)
        idx = 0;
    m_thinkBox->setCurrentIndex(idx);
    m_thinkEffort = m_thinkBox->currentData().toString();
}

// 点顶栏模型名/控制行按钮:双栏弹层切换(左供应商、右该网址下的模型)
void AiChatDock::showModelMenu()
{
    if (!m_listProviders || !m_modelSwitch)
        return;
    QWidget *anchor = m_modelBtn ? m_modelBtn : static_cast<QWidget *>(m_model);
    AiModelPicker::showFor(anchor, m_listProviders(), m_curProvider,
        [this](const QString &provider, const QString &model) {
            if (m_modelSwitch)
                m_modelSwitch(provider, model);
        },
        [this] { emit configRequested(); },
        !m_lightTheme);   // dark = 深色主题
}

void AiChatDock::setNoProvider()
{
    m_hasProvider = false;
    m_modelText.clear();
    m_model->setText(tr("● 未配置供应商 — 点右上 ⚙ 设置"));
}

void AiChatDock::clearConversation()
{
    QMetaObject::invokeMethod(m_worker, [this]() {
        m_worker->clearHistory();
    }, Qt::QueuedConnection);
    m_msgs->clearAll();
    m_assistRow = -1;
    m_thinkRow = -1;
    m_anyText = false;
    QFile::remove(sessionFilePath()); // 新对话:上次会话作废
    // 模型清空 → modelReset → 自动回到欢迎卡片页
    scrollBottom();
}

QString AiChatDock::insertAtCursor(const QString &text)
{
    QString result;
    if (m_insert)
        result = m_insert(m_activeDocumentId,text);
    else
        result = QStringLiteral("错误:当前没有打开的文档");
    if (result.startsWith(QLatin1String("错误"))) {
        m_msgs->append(ChatMsg::Notice, result, QStringLiteral("error"));
    } else {
        // 从结果里取字数("Inserted N characters at the caret.")
        static const QRegularExpression re(QStringLiteral("([0-9]+)"));
        const auto m_ = re.match(result);
        const QString n = m_.hasMatch() ? m_.captured(1) : QString();
        m_msgs->append(ChatMsg::Notice,
                       n.isEmpty() ? tr("✍ 已写入文档")
                                   : tr("✍ 已写入文档(%1 字)").arg(n),
                       QStringLiteral("ok"));
    }
    scrollBottom();
    return result;
}

void AiChatDock::send()
{
    if (m_busy)
        return;
    // 防呆护栏:工作线程没起来绝不允许静默吞掉消息
    if (!m_thread->isRunning()) {
        qWarning() << "Mswrite: AI 工作线程未运行,消息被拒";
        m_msgs->append(ChatMsg::Notice,
                       tr("内部错误:AI 工作线程未运行。关闭本窗口再按 F9 重开即可恢复。"),
                       QStringLiteral("error"));
        scrollBottom();
        return;
    }
    const QString text = m_input->toPlainText().trimmed();
    const bool hasAttach = !m_images.isEmpty() || !m_textFiles.isEmpty();
    if (text.isEmpty() && !hasAttach)
        return;
    if (!m_hasProvider) {
        m_msgs->append(ChatMsg::Notice, tr("没有连接服务器：请先在 AI 供应商中配置 API Key、地址和模型。"),
                       QStringLiteral("error"));
        scrollBottom();
        return;
    }
    m_input->clear();

    // 发给模型:正文 + 文本附件内容
    QString full = text;
    QStringList names;
    for (const auto &tf : m_textFiles) {
        full += QStringLiteral("\n\n[file: %1]\n%2").arg(tf.first, tf.second);
        names << tf.first;
    }
    for (const AiAttach &a : m_images)
        names << a.name;

    // 面板显示:正文 + 附件清单;图片本体也存进气泡(用户要看到截图原图)
    QString display = text;
    if (!names.isEmpty())
        display += QStringLiteral("\n📎 ") + names.join(QStringLiteral("、"));

    const QVector<AiAttach> images = m_images;
    const int urow = m_msgs->append(ChatMsg::User, display);
    if (ChatMsg *um = m_msgs->msgAt(urow))
        um->images = images;
    m_chat->scrollToBottom(true);
    // 输入历史入栈(↑↓ 翻阅用;去重)
    if (!text.isEmpty()
        && (m_inputHistory.isEmpty() || m_inputHistory.last() != text)) {
        m_inputHistory.append(text);
        if (m_inputHistory.size() > 50)
            m_inputHistory.removeFirst();
    }
    m_histIdx = -1; // 发出后回到"无浏览"态

    const int mode = m_writeMode;
    const QString effort = m_thinkEffort;
    clearAttachments();

    // 捕获进 dispatch 链
    m_dispatchImages = images;
    m_dispatchMode = mode;
    m_dispatchEffort = effort;
    dispatch(full);
}

void AiChatDock::stop()
{
    HttpAbort::request();
    // 不再预发"正在停止"行:worker 会很快回 turnFinished("已中断"),
    // 两行提示是噪音;发送按钮变红 ■ 本身已是状态反馈
}

void AiChatDock::dispatch(const QString &userText)
{
    m_turnPreferLatex=QSettings().value(QStringLiteral("preferLatex"),false).toBool();
    const QJsonObject context=m_contextProvider ? m_contextProvider() : QJsonObject();
    m_activeDocumentId=context.value(QStringLiteral("id")).toString();
    const int mode=context.value(QStringLiteral("kind")).toString()==QLatin1String("pdf") ? 0 : m_dispatchMode;
    // Set the guard before queuing the worker; rapid clicks cannot start two turns.
    m_busy=true;
    emit runRequested(userText,QString::fromUtf8(QJsonDocument(context).toJson(QJsonDocument::Compact)),
                      mode,m_dispatchEffort,m_dispatchImages);
    m_dispatchImages.clear();
}

void AiChatDock::readDocument(const QJsonObject &request, std::function<void(AiDocumentResult)> done)
{
    if(m_docReader) m_docReader(request,std::move(done));
    else done({QStringLiteral("Error: no active document"),{}});
}

void AiChatDock::requestSelection(const QString &text,const QString &name,int page,bool translate)
{
    show(); raise(); activateWindow();
    if(text.trimmed().isEmpty()) return;
    const QString prompt=(translate ? tr("请将以下选区完整翻译成中文，保留公式、引用和段落结构。")
                                    : tr("请分析以下选区的核心观点、术语、推理依据及局限，必要时读取上下文。"))
        +tr("\n来源：%1，第 %2 页\n\n<selection>\n%3\n</selection>").arg(name).arg(page+1).arg(text);
    if(!m_hasProvider) {
        m_msgs->append(ChatMsg::Notice,tr("没有连接服务器：请先在 AI 供应商中配置 API Key、地址和模型。"),QStringLiteral("error"));
        return;
    }
    if(m_busy) {
        m_msgs->append(ChatMsg::Notice,tr("当前回答仍在生成，请结束后再执行选区操作。"),QStringLiteral("muted"));
        return;
    }
    m_msgs->append(ChatMsg::User,prompt);
    m_dispatchMode=0; m_dispatchEffort=m_thinkEffort; m_dispatchImages.clear();
    dispatch(prompt);
    m_chat->scrollToBottom(true);
}

void AiChatDock::scrollBottom()
{
    m_chat->scrollToBottom();
}
