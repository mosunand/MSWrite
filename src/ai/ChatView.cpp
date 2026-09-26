// ai/ChatView.cpp — see ai/ChatView.h.

#include "ai/ChatView.h"

#include "ai/AiMathPainter.h"

#include <QFont>
#include <QFontMetrics>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QDateTime>
#include <QPainter>
#include <QRegularExpression>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>

// ---------------------------------------------------------------------------
// 调色板
// ---------------------------------------------------------------------------

ChatPalette chatPalette(bool light)
{
    ChatPalette p;
    p.light = light;
    if (!light) { // 深色(默认)
        p.bubbleBg = QColor(QStringLiteral("#17181f"));
        p.bubbleBorder = QColor(QStringLiteral("#262838"));
        p.text = QColor(QStringLiteral("#e6e6ea"));
        p.streamCursor = QColor(QStringLiteral("#60a5fa"));
        p.userGrad1 = QColor(QStringLiteral("#3b82f6"));
        p.userGrad2 = QColor(QStringLiteral("#2563eb"));
        p.aiGrad1 = QColor(QStringLiteral("#a78bfa"));
        p.aiGrad2 = QColor(QStringLiteral("#6d5ce6"));
        p.meta = QColor(QStringLiteral("#565b66"));
        p.thinking = QColor(QStringLiteral("#6f7480"));
        p.code = QColor(QStringLiteral("#a5d6ff"));
        p.codeBg = QColor(QStringLiteral("#0f1014"));
        p.link = QColor(QStringLiteral("#60a5fa"));
        p.muted = QColor(QStringLiteral("#71717a"));
        p.error = QColor(QStringLiteral("#f87171"));
        p.ok = QColor(QStringLiteral("#34d399"));
        p.accent = QColor(QStringLiteral("#60a5fa"));
        p.lavender = QColor(QStringLiteral("#a5b4fc"));
        p.hero = QColor(QStringLiteral("#eef0fb"));
        p.heroSub = QColor(QStringLiteral("#aeb6c2"));
    } else { // 浅色
        p.bubbleBg = QColor(QStringLiteral("#ffffff"));
        p.bubbleBorder = QColor(QStringLiteral("#e2e4ea"));
        p.text = QColor(QStringLiteral("#1f2328"));
        p.streamCursor = QColor(QStringLiteral("#2563eb"));
        p.userGrad1 = QColor(QStringLiteral("#3b82f6"));
        p.userGrad2 = QColor(QStringLiteral("#2563eb"));
        p.aiGrad1 = QColor(QStringLiteral("#a78bfa"));
        p.aiGrad2 = QColor(QStringLiteral("#6d5ce6"));
        p.meta = QColor(QStringLiteral("#8a8f9a"));
        p.thinking = QColor(QStringLiteral("#6b7280"));
        p.code = QColor(QStringLiteral("#b31d28"));
        p.codeBg = QColor(QStringLiteral("#f0f1f4"));
        p.link = QColor(QStringLiteral("#2563eb"));
        p.muted = QColor(QStringLiteral("#8a8f9a"));
        p.error = QColor(QStringLiteral("#dc2626"));
        p.ok = QColor(QStringLiteral("#059669"));
        p.accent = QColor(QStringLiteral("#2563eb"));
        p.lavender = QColor(QStringLiteral("#6d5ce6"));
        p.hero = QColor(QStringLiteral("#111827"));
        p.heroSub = QColor(QStringLiteral("#4b5563"));
    }
    return p;
}

// 语义角色 → 颜色
QColor roleColor(const ChatPalette &p, const QString &role)
{
    if (role == QLatin1String("error"))
        return p.error;
    if (role == QLatin1String("ok"))
        return p.ok;
    if (role == QLatin1String("accent"))
        return p.accent;
    if (role == QLatin1String("lavender"))
        return p.lavender;
    if (role == QLatin1String("hero"))
        return p.hero;
    if (role == QLatin1String("herosub"))
        return p.heroSub;
    return p.muted;
}

ChatMsg::~ChatMsg()
{
    delete doc;
}

// ---------------------------------------------------------------------------
// ChatModel
// ---------------------------------------------------------------------------

ChatModel::ChatModel(QObject *parent)
    : QAbstractListModel(parent)
{
}

ChatModel::~ChatModel()
{
    clearAll();
}

int ChatModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_rows.size();
}

QVariant ChatModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_rows.size())
        return {};
    if (role == Qt::DisplayRole)
        return m_rows.at(index.row())->text;
    return {};
}

ChatMsg *ChatModel::msgAt(int row) const
{
    return (row >= 0 && row < m_rows.size()) ? m_rows[row] : nullptr;
}

int ChatModel::append(ChatMsg::Kind kind, const QString &text, const QString &role)
{
    beginInsertRows(QModelIndex(), m_rows.size(), m_rows.size());
    auto *m = new ChatMsg;
    m->kind = kind;
    m->text = text;
    m->role = role;
    m_rows.push_back(m);
    endInsertRows();
    return m_rows.size() - 1;
}

void ChatModel::touch(int row)
{
    if (row < 0 || row >= m_rows.size())
        return;
    ++m_rows[row]->rev;
    const QModelIndex i = index(row, 0);
    emit dataChanged(i, i);
}

void ChatModel::removeFrom(int row)
{
    if (row < 0 || row >= m_rows.size())
        return;
    beginRemoveRows(QModelIndex(), row, m_rows.size() - 1);
    for (int i = m_rows.size() - 1; i >= row; --i)
        delete m_rows[i];
    m_rows.resize(row);
    endRemoveRows();
}

void ChatModel::clearAll()
{
    if (m_rows.isEmpty())
        return;
    beginResetModel();
    qDeleteAll(m_rows);
    m_rows.clear();
    endResetModel();
}

// ---------------------------------------------------------------------------
// Markdown + LaTeX 公式渲染(图片方案)
// ---------------------------------------------------------------------------

namespace {

// 私有区占位符:公式 → 单个对象替换符(0xE000 + 序号十进制 + 0xE001)。
// Markdown 解析原样保留;渲染后用 QTextCursor 把它换成嵌入图片。
const QChar kMathStart(0xE000);
const QChar kMathEnd(0xE001);

void setBaseFont(QTextDocument &doc)
{
    QFont base;
    base.setFamilies({ QStringLiteral("Microsoft YaHei UI"), QStringLiteral("Segoe UI") });
    base.setPixelSize(15);
    doc.setDefaultFont(base);
    doc.setDocumentMargin(0);
}

struct MathSlot {
    QString latex;
    bool display = false;
};

// 扫描 Markdown:公式段 → 占位符,登记到 slots(跳过代码围栏里的 $)
QString extractMathSlots(const QString &md, QVector<MathSlot> *mathSlots)
{
    QString out;
    out.reserve(md.size() + 16);
    QChar fence;
    int fenceLength = 0;
    auto escaped = [&md](int pos) {
        int slashes = 0;
        while (pos > 0 && md.at(--pos) == QLatin1Char('\\'))
            ++slashes;
        return slashes % 2 != 0;
    };
    int i = 0;
    while (i < md.size()) {
        // 按整行消费围栏及其内容,包括换行符。停在 '\n' 会使下一轮永不前进。
        if (i == 0 || md.at(i - 1) == QLatin1Char('\n')) {
            const int nl = md.indexOf(QLatin1Char('\n'), i);
            const int end = nl < 0 ? md.size() : nl;
            int marker = i;
            while (marker < end && md.at(marker) == QLatin1Char(' '))
                ++marker;
            const QChar ch = marker < end ? md.at(marker) : QChar();
            int runEnd = marker;
            if (ch == QLatin1Char('`') || ch == QLatin1Char('~'))
                while (runEnd < end && md.at(runEnd) == ch)
                    ++runEnd;
            const int count = runEnd - marker;
            const QString suffix = md.mid(runEnd, end - runEnd);
            const bool wasInFence = !fence.isNull();
            if (marker - i <= 3 && count >= 3) {
                if (!wasInFence && (ch != QLatin1Char('`') || !suffix.contains(ch))) {
                    fence = ch;
                    fenceLength = count;
                } else if (wasInFence && ch == fence && count >= fenceLength
                           && suffix.trimmed().isEmpty()) {
                    fence = QChar();
                }
            }
            // 缩进代码同样保留美元符号。
            if (wasInFence || !fence.isNull() || marker - i >= 4
                || ch == QLatin1Char('\t')) {
                const int next = nl < 0 ? end : nl + 1;
                out += md.mid(i, next - i);
                i = next;
                continue;
            }
        }
        // 行内代码内的 $ 是源码,匹配相同长度的反引号才结束代码跨度。
        if (md.at(i) == QLatin1Char('`') && !escaped(i)) {
            int runEnd = i + 1;
            while (runEnd < md.size() && md.at(runEnd) == QLatin1Char('`'))
                ++runEnd;
            int next = runEnd;
            for (int pos = runEnd; pos < md.size();) {
                pos = md.indexOf(QLatin1Char('`'), pos);
                if (pos < 0)
                    break;
                int closeEnd = pos + 1;
                while (closeEnd < md.size() && md.at(closeEnd) == QLatin1Char('`'))
                    ++closeEnd;
                if (closeEnd - pos == runEnd - i) {
                    next = closeEnd;
                    break;
                }
                pos = closeEnd;
            }
            out += md.mid(i, next - i);
            i = next;
            continue;
        }
        if (md.at(i) == QLatin1Char('$') && !escaped(i)) {
            // $$...$$(块级,可跨行)或 $...$(行内,同行配对)
            const bool display = (i + 1 < md.size() && md.at(i + 1) == QLatin1Char('$'));
            const int bodyStart = i + (display ? 2 : 1);
            const QString delimiter = display ? QStringLiteral("$$") : QStringLiteral("$");
            int end = md.indexOf(delimiter, bodyStart);
            while (end >= 0 && escaped(end))
                end = md.indexOf(delimiter, end + delimiter.size());
            const int nl = md.indexOf(QLatin1Char('\n'), i);
            if (!display && nl >= 0 && end >= nl)
                end = -1;
            if (end > bodyStart) {
                MathSlot slot;
                slot.latex = md.mid(bodyStart, end - bodyStart);
                slot.display = display;
                mathSlots->push_back(slot);
                // 占位 = start + 序号 + end
                out += kMathStart;
                out += QString::number(mathSlots->size() - 1);
                out += kMathEnd;
                i = end + (display ? 2 : 1);
                continue;
            }
            // 流式中还没闭合的 $$ 必须整对保留,不能把第二个 $ 当作行内公式。
            out += delimiter;
            i = bodyStart;
            continue;
        }
        out += md.at(i);
        ++i;
    }
    return out;
}

// 占位符 → 图片:先 addResource,再 setHtml(内联 img 引用资源名)
// 资源名 "math:<idx>";失败(空图)的公式退回源码文本
QString injectMathImages(QTextDocument *doc, const QString &html,
                         const QVector<MathSlot> &mathSlots, const QColor &fg)
{
    QString out;
    out.reserve(html.size() + 256);
    for (int i = 0; i < html.size(); ++i) {
        if (html.at(i) == kMathStart) {
            const int close = html.indexOf(kMathEnd, i);
            if (close < 0) {
                out += html.at(i);
                continue;
            }
            const int idx = html.mid(i + 1, close - i - 1).toInt();
            if (idx >= 0 && idx < mathSlots.size()) {
                const QPixmap pm = AiMathPainter::render(mathSlots.at(idx).latex, fg);
                if (!pm.isNull()) {
                    const QString name = QStringLiteral("math:%1").arg(idx);
                    doc->addResource(QTextDocument::ImageResource, QUrl(name), QVariant(pm));
                    // HTML 单位是逻辑像素:除以 dpr
                    const int w = qRound(pm.width() / pm.devicePixelRatio());
                    const int h = qRound(pm.height() / pm.devicePixelRatio());
                    if (mathSlots.at(idx).display) {
                        // 块级:独占一行,居中,上下留呼吸
                        out += QStringLiteral(
                                   "<br><p align=\"center\" style=\"margin-top:4px;"
                                   "margin-bottom:4px\"><img src=\"%1\" width=\"%2\" "
                                   "height=\"%3\"/></p><br>")
                                   .arg(name, QString::number(w), QString::number(h));
                    } else {
                        // 行内:middle 对齐,不撑行高
                        out += QStringLiteral(
                                   "<img src=\"%1\" width=\"%2\" height=\"%3\" "
                                   "style=\"vertical-align:middle\"/>")
                                   .arg(name, QString::number(w), QString::number(h));
                    }
                } else {
                    // 退回源码
                    out += mathSlots.at(idx).latex.toHtmlEscaped();
                }
            }
            i = close;
            continue;
        }
        out += html.at(i);
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// ChatBubbleDelegate
// ---------------------------------------------------------------------------

ChatBubbleDelegate::ChatBubbleDelegate(QObject *parent)
    : QAbstractItemDelegate(parent)
{
}

ChatMsg *ChatBubbleDelegate::msgAt(const QModelIndex &index) const
{
    const auto *model = qobject_cast<const ChatModel *>(index.model());
    return model ? model->msgAt(index.row()) : nullptr;
}

// 行距放宽:块级 155% 行高,阅读舒适(DeepSeek 风格的松感)
void ChatBubbleDelegate::applyLineHeight(QTextDocument *doc) const
{
    QTextCursor cur(doc);
    cur.movePosition(QTextCursor::Start);
    QTextBlockFormat fmt;
    fmt.setLineHeight(155, QTextBlockFormat::ProportionalHeight);
    for (QTextBlock b = doc->firstBlock(); b.isValid(); b = b.next()) {
        cur.setPosition(b.position());
        if (!cur.block().blockFormat().lineHeight())
            cur.setBlockFormat(fmt);
    }
}

QTextDocument *ChatBubbleDelegate::ensureDoc(ChatMsg *m) const
{
    const ChatPalette p = chatPalette(m_light);
    // 流式行需要随动画帧重建(闪烁光);主题切换全量重建;其余只看 rev
    const bool needsFrame = (m->kind == ChatMsg::Assistant && !m->finalized);
    if (m->doc && m->builtRev == m->rev && m->builtTheme == int(m_light)
        && (!needsFrame || m->builtFrame == m_frame))
        return m->doc;
    if (!m->doc)
        m->doc = new QTextDocument();
    m->doc->clear();
    setBaseFont(*m->doc);
    if (m->kind == ChatMsg::User) {
        // 蓝底白字(HTML 内联,纯文本渲染不吃 CSS)
        QString esc = m->text.toHtmlEscaped();
        esc.replace(QLatin1Char('\n'), QStringLiteral("<br>"));
        m->doc->setHtml(QStringLiteral(
            "<div style=\"color:#ffffff;line-height:155%\">%1</div>").arg(esc));
    } else if (m->kind == ChatMsg::Assistant) {
        if (m->finalized) {
            // 轮末:Markdown 正式渲染 + 公式(占位符两段式)。
            // 文字颜色不靠脆弱的字符串替换:正文整段包进显式颜色的 div,
            // 文档 palette 通过 drawContents 前的 QPainter pen 兜底
            m->doc->setDefaultStyleSheet(QStringLiteral(
                "body { color:%4; }"
                "code { font-family:'Cascadia Mono',Consolas,monospace; color:%1; }"
                "pre { background-color:%2; border-radius:6px; }"
                "a { color:%3; }"
                "h1 { font-size:22px; font-weight:700; }"
                "h2 { font-size:19px; font-weight:700; }"
                "h3 { font-size:17px; font-weight:600; }"
                "blockquote { color:%5; border-left:3px solid %6; "
                "padding-left:10px; margin-left:2px; }"
                "table { border-collapse:collapse; }"
                "th { background-color:%2; font-weight:600; padding:4px 10px; }"
                "td { padding:4px 10px; }")
                .arg(p.code.name(), p.codeBg.name(), p.link.name(),
                     p.text.name(), p.muted.name(), p.accent.name()));
            // 公式两段式:①抽走公式留占位符 ②Markdown ③占位符→嵌入图片
            QVector<MathSlot> mathSlots;
            const QString withMath = extractMathSlots(m->text, &mathSlots);
            QTextDocument tmp;
            setBaseFont(tmp);
            tmp.setMarkdown(withMath);
            QString html = tmp.toHtml();
            html.replace(QLatin1String("background-color:#ffffff"),
                         QStringLiteral("background-color:transparent"));
            html.replace(QLatin1String("background-color: #ffffff"),
                         QStringLiteral("background-color:transparent"));
            const QString injected = injectMathImages(m->doc, html, mathSlots, p.text);
            // 找到 body 内容区,套一层显式颜色 div(浅深主题通用)
            const int bodyOpen = injected.indexOf(QLatin1String("<body"));
            const int bodyTagEnd = bodyOpen < 0 ? -1
                : injected.indexOf(QLatin1Char('>'), bodyOpen);
            if (bodyTagEnd > 0) {
                const int bodyClose = injected.lastIndexOf(QLatin1String("</body>"));
                const QString inner = injected.mid(bodyTagEnd + 1, bodyClose - bodyTagEnd - 1);
                m->doc->setHtml(QStringLiteral("<div style=\"color:%1;line-height:155%\">%2</div>")
                                    .arg(p.text.name(), inner));
            } else {
                m->doc->setHtml(QStringLiteral("<div style=\"color:%1;line-height:155%\">%2</div>")
                                    .arg(p.text.name(), injected));
            }
            applyLineHeight(m->doc);
        } else {
            // 流式期间实时 Markdown 渲染(与完成态同管线,消跳变)
            // 240ms 重建一次(帧%2),闪烁光标保持
            QVector<MathSlot> mathSlots;
            const QString withMath = extractMathSlots(m->text, &mathSlots);
            QTextDocument tmp;
            setBaseFont(tmp);
            tmp.setMarkdown(withMath);
            QString html = tmp.toHtml();
            html.replace(QLatin1String("background-color:#ffffff"),
                         QStringLiteral("background-color:transparent"));
            const QString injected = injectMathImages(m->doc, html, mathSlots, p.text);
            const int bodyOpen = injected.indexOf(QLatin1String("<body"));
            const int bodyTagEnd = bodyOpen < 0 ? -1
                : injected.indexOf(QLatin1Char('>'), bodyOpen);
            QString inner;
            if (bodyTagEnd > 0) {
                const int bodyClose = injected.lastIndexOf(QLatin1String("</body>"));
                inner = injected.mid(bodyTagEnd + 1, bodyClose - bodyTagEnd - 1);
            } else {
                inner = injected;
            }
            const QString cursor = (m_frame % 4 == 0)
                ? QStringLiteral("<span style=\"color:%1\">▍</span>").arg(p.streamCursor.name())
                : QString();
            m->doc->setHtml(QStringLiteral("<div style=\"color:%1;line-height:155%\">%2%3</div>")
                                .arg(p.text.name(), inner, cursor));
        }
    } else if (m->kind == ChatMsg::Notice) {
        // 通知:居中、可换行(长错误信息完整可见);hero = 大号欢迎标题
        const QColor c = roleColor(p, m->role);
        QString esc = m->text.toHtmlEscaped();
        esc.replace(QLatin1Char('\n'), QStringLiteral("<br>"));
        if (m->hero) {
            m->doc->setHtml(QStringLiteral(
                "<p align=\"center\" style=\"color:%1;font-size:23px;"
                "font-weight:600;line-height:150%\">%2</p>")
                    .arg(c.name(), esc));
        } else {
            m->doc->setHtml(QStringLiteral(
                "<p align=\"center\" style=\"color:%1;font-size:14px;"
                "line-height:150%\">%2</p>")
                                .arg(c.name(), esc));
        }
    } else {
        m->doc->setPlainText(m->text);
    }
    m->builtRev = m->rev;
    m->builtFrame = m_frame;
    m->builtTheme = int(m_light);
    return m->doc;
}

void ChatBubbleDelegate::paint(QPainter *p, const QStyleOptionViewItem &option,
                               const QModelIndex &index) const
{
    ChatMsg *m = msgAt(index);
    if (!m)
        return;
    const ChatPalette pal = chatPalette(m_light);
    const int w = qMax(option.rect.width(), 120);
    const int y0 = option.rect.top();

    p->save();
    p->setRenderHint(QPainter::Antialiasing, true);

    // 思考行:折叠 = 单行尾句(点击展开);展开 = 完整思维链多行
    if (m->kind == ChatMsg::Thinking) {
        QFont f = option.font;
        f.setPixelSize(13);
        p->setFont(f);
        if (!m->expanded) {
            p->setPen(pal.thinking);
            QFontMetrics fm(f);
            QString line = m->meta + QLatin1Char(' ') + m->text;
            if (m->expandable)
                line += tr("  (点击展开)");
            line = fm.elidedText(line, Qt::ElideRight, w - 28);
            p->drawText(QRect(14, y0 + 8, w - 28, 24),
                        Qt::AlignVCenter | Qt::AlignLeft, line);
        } else {
            // 展开态:完整思维链 + 折叠提示
            QFontMetrics fm(f);
            const QString head = m->meta + tr("  (点击折叠)");
            p->setPen(pal.thinking);
            p->drawText(QRect(14, y0 + 8, w - 28, 20),
                        Qt::AlignVCenter | Qt::AlignLeft, head);
            // 正文多行绘制
            p->setPen(pal.muted);
            const int lineH = fm.height() + 1;
            int y = y0 + 32;
            const QStringList lines = m->fullText.split(QLatin1Char('\n'));
            int drawn = 0;
            for (const QString &ln : lines) {
                if (y + lineH > option.rect.bottom() - 6 || drawn > 400) {
                    p->drawText(QRect(14, y, w - 28, lineH),
                                Qt::AlignVCenter | Qt::AlignLeft, tr("…"));
                    break;
                }
                const QString elided = fm.elidedText(ln, Qt::ElideRight, w - 34);
                p->drawText(QRect(18, y, w - 34, lineH),
                            Qt::AlignVCenter | Qt::AlignLeft, elided);
                y += lineH;
                ++drawn;
            }
        }
        p->restore();
        return;
    }

    // 通知行:居中文档(无气泡)
    if (m->kind == ChatMsg::Notice) {
        QTextDocument *doc = ensureDoc(m);
        doc->setTextWidth(w - 36);
        p->translate(18, y0 + 6);
        doc->drawContents(p);
        p->restore();
        return;
    }

    // ---- 气泡(User / Assistant):头像 + 圆角卡片 ----
    QTextDocument *doc = ensureDoc(m);
    const bool user = m->kind == ChatMsg::User;
    const int usable = w - 2 * (kSide + kAvatar + kAvGap);
    doc->setTextWidth(-1);
    const int ideal = int(doc->idealWidth()) + 2 * kPadX;
    const int maxW = qMax(90, usable * 92 / 100);
    const int bw = qMin(ideal, maxW);
    doc->setTextWidth(bw - 2 * kPadX);
    const int dh = int(doc->size().height());
    const int bh = dh + 2 * kPadY;

    const QRect avatar(user ? w - kSide - kAvatar : kSide, y0 + 2, kAvatar, kAvatar);
    const int bx = user ? w - kSide - kAvatar - kAvGap - bw
                        : kSide + kAvatar + kAvGap;
    const QRectF bubble(bx, y0 + 2, bw, bh);

    // 头像:圆形渐变 + 白色标识
    {
        QLinearGradient g(avatar.topLeft(), avatar.bottomRight());
        if (user) {
            g.setColorAt(0, pal.userGrad1);
            g.setColorAt(1, pal.userGrad2);
        } else {
            g.setColorAt(0, pal.aiGrad1);
            g.setColorAt(1, pal.aiGrad2);
        }
        p->setPen(Qt::NoPen);
        p->setBrush(g);
        p->drawEllipse(avatar);
        QFont af = option.font;
        af.setBold(true);
        af.setPixelSize(user ? 13 : 11);
        p->setFont(af);
        p->setPen(Qt::white);
        p->drawText(avatar, Qt::AlignCenter,
                    user ? tr("我") : QStringLiteral("AI"));
    }

    // 气泡体
    if (user) {
        QLinearGradient g(bubble.topLeft(), bubble.bottomLeft());
        g.setColorAt(0, pal.userGrad1);
        g.setColorAt(1, pal.userGrad2);
        p->setPen(Qt::NoPen);
        p->setBrush(g);
    } else {
        p->setPen(QPen(pal.bubbleBorder, 1));
        p->setBrush(pal.bubbleBg);
    }
    p->drawRoundedRect(bubble, 14, 14);

    p->save();
    p->translate(bubble.left() + kPadX, bubble.top() + kPadY);
    doc->drawContents(p);
    p->restore();

    // 复制按钮(AI 完成态/User 气泡):小圆钮,点击后短暂画 ✓
    const bool showCopy = (m->kind == ChatMsg::Assistant && m->finalized)
                          || m->kind == ChatMsg::User;
    if (showCopy) {
        const QRect btn = copyButtonRect(option, index);
        p->setPen(QPen(pal.bubbleBorder, 1));
        p->setBrush(m_light ? QColor(255, 255, 255, 245) : QColor(30, 32, 42, 245));
        p->drawEllipse(btn);
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        const qint64 flashUntil = m_copyFlash.value(index.row(), 0);
        if (flashUntil > 0 && now < flashUntil) {
            // 已复制反馈:绿色 ✓
            p->setPen(QPen(QColor("#22c55e"), 2.2));
            p->drawLine(QPointF(btn.left() + 6, btn.center().y()),
                        QPointF(btn.left() + 10, btn.bottom() - 6));
            p->drawLine(QPointF(btn.left() + 10, btn.bottom() - 6),
                        QPointF(btn.right() - 5, btn.top() + 6));
        } else {
            // 复制图形:后卡(大) + 前卡(小,带折角)
            p->setPen(QPen(pal.muted, 1.5));
            const qreal b = btn.left(), t = btn.top();
            p->drawRoundedRect(QRectF(b + 5.5, t + 5.5, 9, 9), 1.8, 1.8);
            p->drawRoundedRect(QRectF(b + 8.5, t + 8.5, 8, 8), 1.8, 1.8);
            p->drawLine(QPointF(b + 8.5, t + 16.5), QPointF(b + 8.5, t + 8.5));
            p->drawLine(QPointF(b + 8.5, t + 8.5), QPointF(b + 16.5, t + 8.5));
        }
    }
    p->restore();

    // 统计小字 + 重新生成按钮(AI 完成态气泡下方,同侧)
    if (m->kind == ChatMsg::Assistant && m->finalized) {
        p->save();
        QFont f = option.font;
        f.setPixelSize(12);
        p->setFont(f);
        p->setPen(pal.meta);
        int textW = 0;
        if (!m->meta.isEmpty()) {
            QFontMetrics fm(f);
            textW = fm.horizontalAdvance(m->meta);
            p->drawText(QRect(bx, int(bubble.bottom()) + 4, textW + 8, kMetaH),
                        Qt::AlignVCenter | Qt::AlignLeft, m->meta);
        }
        // ↻ 重新生成(紧跟统计行)
        QFont rf = f;
        rf.setUnderline(false);
        p->setFont(rf);
        p->setPen(pal.accent);
        p->drawText(QRect(bx + textW + 12, int(bubble.bottom()) + 4,
                          80, kMetaH),
                    Qt::AlignVCenter | Qt::AlignLeft, tr("↻ 重新生成"));
        p->restore();
    }
}

// 复制按钮矩形:AI 气泡(完成态)内部右上角 —— 不和头像/正文打架
QRect ChatBubbleDelegate::copyButtonRect(const QStyleOptionViewItem &option,
                                         const QModelIndex &index) const
{
    ChatMsg *m = msgAt(index);
    if (!m || (m->kind != ChatMsg::Assistant && m->kind != ChatMsg::User))
        return {};
    if (m->kind == ChatMsg::Assistant && !m->finalized)
        return {}; // 流式中的 AI 气泡没有按钮
    // 需要气泡宽度才能定位;sizeHint/paint 共用此函数,宽度由 option.rect 给
    QTextDocument *doc = ensureDoc(m);
    const int w = qMax(option.rect.width(), 120);
    const int usable = w - 2 * (kSide + kAvatar + kAvGap);
    doc->setTextWidth(-1);
    const int ideal = int(doc->idealWidth()) + 2 * kPadX;
    const int maxW = qMax(90, usable * 92 / 100);
    const int bw = qMin(ideal, maxW);
    doc->setTextWidth(bw - 2 * kPadX);
    // 统一:气泡外部右下角(紧贴气泡,不占内容区)
    const int bx = (m->kind == ChatMsg::User)
                       ? w - kSide - kAvatar - kAvGap - bw
                       : kSide + kAvatar + kAvGap;
    const int by = option.rect.top() + 2; // 气泡顶
    const int bh = int(ensureDoc(m)->size().height()) + 2 * kPadY; // 气泡高
    return QRect(bx + bw - kCopyBtn + 4, by + bh + 2,
                 kCopyBtn, kCopyBtn);
}

bool ChatBubbleDelegate::editorEvent(QEvent *event, QAbstractItemModel *model,
                                      const QStyleOptionViewItem &option,
                                      const QModelIndex &index)
{
    Q_UNUSED(model)
    if (event->type() != QEvent::MouseButtonRelease)
        return false;
    ChatMsg *m = msgAt(index);
    if (!m || (m->kind != ChatMsg::Assistant && m->kind != ChatMsg::User))
        return false;
    if (m->kind == ChatMsg::Assistant && !m->finalized)
        return false;
    const QRect btn = copyButtonRect(option, index);
    auto *me = static_cast<QMouseEvent *>(event);
    if (btn.contains(me->pos())) {
        m_copyFlash[index.row()] = QDateTime::currentMSecsSinceEpoch() + 1200;
        emit copyRequested(m->text);
        return true;
    }
    // ↻ 重新生成:AI 完成态气泡下方,统计行旁
    if (m->kind == ChatMsg::Assistant && m->finalized) {
        QTextDocument *doc = ensureDoc(m);
        const int w = qMax(option.rect.width(), 120);
        const int usable = w - 2 * (kSide + kAvatar + kAvGap);
        doc->setTextWidth(-1);
        const int ideal = int(doc->idealWidth()) + 2 * kPadX;
        const int maxW = qMax(90, usable * 92 / 100);
        const int bw = qMin(ideal, maxW);
        doc->setTextWidth(bw - 2 * kPadX);
        const int bx = kSide + kAvatar + kAvGap;
        const int bh = int(doc->size().height()) + 2 * kPadY;
        // 重新生成区域 = 统计行右侧 80px
        QFont f;
        f.setPixelSize(12);
        const QFontMetrics fm(f);
        const int metaW = m->meta.isEmpty() ? 0 : fm.horizontalAdvance(m->meta);
        const QRect regenRect(bx + metaW + 12, option.rect.top() + 2 + bh + 4,
                               80, kMetaH);
        if (regenRect.contains(me->pos())) {
            emit regenerateRequested(index.row());
            return true;
        }
    }
    return false;
}

QSize ChatBubbleDelegate::sizeHint(const QStyleOptionViewItem &option,
                                  const QModelIndex &index) const
{
    ChatMsg *m = msgAt(index);
    if (!m)
        return QSize(120, 24);
    const int w = qMax(option.rect.width(), 120);
    if (m->kind == ChatMsg::Thinking) {
        if (m->expanded) {
            const int lines = qMin(m->fullText.count(QLatin1Char('\n')) + 1, 400);
            return QSize(w, 36 + lines * 19 + kGap);
        }
        return QSize(w, kThinkH + kGap);
    }
    if (m->kind == ChatMsg::Notice) {
        QTextDocument *doc = ensureDoc(m);
        doc->setTextWidth(w - 36);
        return QSize(w, int(doc->size().height()) + 12 + kGap);
    }

    QTextDocument *doc = ensureDoc(m);
    const int usable = w - 2 * (kSide + kAvatar + kAvGap);
    doc->setTextWidth(-1);
    const int ideal = int(doc->idealWidth()) + 2 * kPadX;
    const int maxW = qMax(90, usable * 92 / 100);
    const int bw = qMin(ideal, maxW);
    doc->setTextWidth(bw - 2 * kPadX);
    int h = int(doc->size().height()) + 2 * kPadY + 4;
    if (m->kind == ChatMsg::Assistant && m->finalized)
        h += kMetaH + 4; // 完成态始终有统计/操作行(即使 meta 为空)
    return QSize(w, h + kGap);
}
