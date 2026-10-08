#pragma once
// ai/ChatView.h — 聊天气泡视图:极简列表模型 + 自绘代理。
// 代理负责:圆角气泡(用户蓝/AI 卡片)、气泡内 Markdown+公式 渁染、
// 思考行/通知行/统计小字;支持浅色/深色主题。

#include <QAbstractItemDelegate>
#include <QAbstractListModel>
#include <QColor>
#include <QHash>
#include <QVector>

#include "ai/Types.h"

class QTextDocument;

// 主题调色板(所有颜色集中于此,浅深两套)
struct ChatPalette {
    QColor windowBg;      // (QSS 用,此处仅占位)
    QColor bubbleBg, bubbleBorder;  // AI 气泡
    QColor text;          // AI 气泡正文
    QColor streamCursor;  // 流式光标
    QColor userGrad1, userGrad2;
    QColor aiGrad1, aiGrad2;        // AI 头像渐变
    QColor meta;          // 统计小字
    QColor thinking;      // 思考行
    QColor code, codeBg, link;
    // 通知角色色
    QColor muted, error, ok, accent, lavender, hero, heroSub;
    bool light = false;
};

ChatPalette chatPalette(bool light);

struct ChatMsg {
    enum Kind { User, Assistant, Thinking, Notice };
    Kind kind = Notice;
    QString text;    // User:纯文本; Assistant:Markdown 源(流式时为原文)
    QString meta;    // Assistant:统计行; Thinking:"思考中 12s"
    QString role;    // Notice 语义色角色: muted|error|ok|accent|lavender|hero|herosub
    bool finalized = false; // Assistant:流式结束,按 Markdown 重建
    bool preferLatex = false; // 本条回复创建时的偏好,不会随之后的开关回退
    bool hero = false;      // Notice:大号标题(欢迎页)
    QString fullText;       // Thinking:完整思维链(展开用)
    bool expandable = false; // Thinking:可展开
    bool expanded = false;  // Thinking:当前展开
    QVector<AiAttach> images; // User:图片附件(面板内显示原图)
    int rev = 0;      // 内容修订号(流式每次 +1)
    int builtRev = -1;
    int builtFrame = -1;   // 动画帧(流式光标闪烁用)
    int builtTheme = -1;   // 主题(浅/深切换时重建)
    QTextDocument *doc = nullptr; // 代理惰性构建,析构归 ChatMsg
    ~ChatMsg();
};

class ChatModel : public QAbstractListModel {
    Q_OBJECT
public:
    explicit ChatModel(QObject *parent = nullptr);
    ~ChatModel() override;

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;

    ChatMsg *msgAt(int row) const;
    int append(ChatMsg::Kind kind, const QString &text, const QString &role = QString());
    void touch(int row);   // 行内容变更(流式更新/改 meta)
    void removeFrom(int row); // 删除 row 及之后所有行(重新生成用)
    void clearAll();

private:
    QVector<ChatMsg *> m_rows;
};

class ChatBubbleDelegate : public QAbstractItemDelegate {
    Q_OBJECT
public:
    explicit ChatBubbleDelegate(QObject *parent = nullptr);

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &index) const override;
    bool editorEvent(QEvent *event, QAbstractItemModel *model,
                     const QStyleOptionViewItem &option,
                     const QModelIndex &index) override; // 复制按钮点击

    // 动画帧(120ms 一帧):流式气泡的闪烁光标随帧重画
    void setAnimationFrame(int frame) { m_frame = frame; }
    void setLightTheme(bool light) { m_light = light; }

    // 某行的复制按钮矩形(视口坐标;无按钮返回空)。供视口鼠标追踪换光标
    QRect copyButtonRect(const QStyleOptionViewItem &option,
                         const QModelIndex &index) const;

    static constexpr int kCopyBtn = 22; // 复制按钮边长

signals:
    void copyRequested(const QString &text); // 点击复制按钮
    void regenerateRequested(int row); // 点击"重新生成"

private:
    ChatMsg *msgAt(const QModelIndex &index) const;
    QTextDocument *ensureDoc(ChatMsg *m) const; // rev/帧/主题变化则重建
    void applyLineHeight(QTextDocument *doc) const; // 行距放宽(155%)

    static constexpr int kPadX = 13;   // 气泡内边距
    static constexpr int kPadY = 10;
    static constexpr int kGap = 16;    // 行间距(消息之间要透气)
    static constexpr int kMetaH = 18;  // 统计小字行高
    static constexpr int kSide = 14;   // 距视口左右
    static constexpr int kAvatar = 32; // 头像直径
    static constexpr int kAvGap = 10;  // 头像与气泡间距
    static constexpr int kThinkH = 40; // 思考行高(加高防裁剪)

    int m_frame = 0;
    bool m_light = false;
    // 复制反馈:行号 → ✓ 截止时间戳(只闪被点的那一行,不是全局)
    QHash<int, qint64> m_copyFlash;
};
