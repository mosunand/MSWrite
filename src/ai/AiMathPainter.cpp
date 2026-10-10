// ai/AiMathPainter.cpp — see ai/AiMathPainter.h.
//
// 设计:递归下降把 LaTeX 解析成盒树(水平拼接/分数/根号/上下标/大运算符),
// 两遍布局(量尺寸 → 画),基线对齐,透明底 2x 抗锯齿。
// 与 MathRender 字符画同源的符号表,但这里产出的是真图形。

#include "ai/AiMathPainter.h"

#include <QFontMetricsF>
#include <QMap>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <memory>
#include <vector>
#include <cmath>

namespace AiMathPainter {

namespace {

// ───────────────────────── 符号表 ─────────────────────────

const QMap<QString, QString> &commands()
{
    static const QMap<QString, QString> m = {
        // greek
        { "alpha", "α" }, { "beta", "β" }, { "gamma", "γ" }, { "delta", "δ" },
        { "epsilon", "ε" }, { "zeta", "ζ" }, { "eta", "η" }, { "theta", "θ" },
        { "iota", "ι" }, { "kappa", "κ" }, { "lambda", "λ" }, { "mu", "μ" },
        { "nu", "ν" }, { "xi", "ξ" }, { "pi", "π" }, { "rho", "ρ" },
        { "sigma", "σ" }, { "tau", "τ" }, { "upsilon", "υ" }, { "phi", "φ" },
        { "chi", "χ" }, { "psi", "ψ" }, { "omega", "ω" },
        { "Gamma", "Γ" }, { "Delta", "Δ" }, { "Theta", "Θ" }, { "Lambda", "Λ" },
        { "Xi", "Ξ" }, { "Pi", "Π" }, { "Sigma", "Σ" }, { "Phi", "Φ" },
        { "Psi", "Ψ" }, { "Omega", "Ω" },
        // relations / operators
        { "times", "×" }, { "div", "÷" }, { "pm", "±" }, { "mp", "∓" },
        { "cdot", "⋅" }, { "leq", "≤" }, { "geq", "≥" }, { "neq", "≠" },
        { "approx", "≈" }, { "equiv", "≡" }, { "sim", "∼" }, { "propto", "∝" },
        { "infty", "∞" }, { "partial", "∂" }, { "nabla", "∇" },
        { "forall", "∀" }, { "exists", "∃" }, { "in", "∈" }, { "notin", "∉" },
        { "subset", "⊂" }, { "cup", "∪" }, { "cap", "∩" }, { "emptyset", "∅" },
        { "to", "→" }, { "rightarrow", "→" }, { "leftarrow", "←" },
        { "Rightarrow", "⇒" }, { "leftrightarrow", "↔" }, { "mapsto", "↦" },
        { "ldots", "…" }, { "cdots", "⋯" }, { "vdots", "⋮" }, { "ddots", "⋱" },
        { "perp", "⊥" }, { "parallel", "∥" }, { "angle", "∠" },
        { "degree", "°" }, { "circ", "∘" }, { "prime", "′" },
        { "hbar", "ℏ" }, { "ell", "ℓ" },
        // big operators(带 limits)
        { "sum", "∑" }, { "prod", "∏" }, { "int", "∫" },
        { "iint", "∬" }, { "oint", "∮" },
        { "lim", "lim" }, { "max", "max" }, { "min", "min" },
        // 常量
        { "pi2", "π" },
    };
    return m;
}

const QMap<QString, QString> &functions()
{
    static const QMap<QString, QString> m = {
        { "sin", "sin" }, { "cos", "cos" }, { "tan", "tan" },
        { "log", "log" }, { "ln", "ln" }, { "exp", "exp" },
        { "sqrt", {} }, { "frac", {} }, { "text", {} },
        { "mathrm", {} }, { "left", {} }, { "right", {} },
        { "operatorname", {} }, { "displaystyle", {} }, { "limits", {} },
        { "begin", {} }, { "end", {} }, { "textcolor", {} },
        { "quad", {} }, { "qquad", {} },
        { "vec", {} },  // 矢量箭头(上方画 →)
        { "hat", {} },  // 上方尖帽
        { "bar", {} },  // 上方横线
        { "dfrac", {} }, { "tfrac", {} },
    };
    return m;
}

// 数学函数名:保持直立(斜体 sin 看着像 s·i·n)
const QStringList &funcNames()
{
    static const QStringList m = {
        QStringLiteral("sin"), QStringLiteral("cos"), QStringLiteral("tan"),
        QStringLiteral("log"), QStringLiteral("ln"), QStringLiteral("exp"),
        QStringLiteral("sinh"), QStringLiteral("cosh"), QStringLiteral("tanh"),
        QStringLiteral("det"), QStringLiteral("dim"), QStringLiteral("gcd"),
        QStringLiteral("deg"), QStringLiteral("arg"),
    };
    return m;
}

bool isSpacingCmd(const QString &n)
{
    return n == QLatin1String("quad") || n == QLatin1String("qquad");
}

// ───────────────────────── 盒模型 ─────────────────────────
//
// 每个 Box:width/height/ascend(基线上的高度)。绘制时给定左上原点(基线锚定)。

struct Box {
    virtual ~Box() = default;
    virtual void layout(const QFont &font) = 0;
    virtual void draw(QPainter &p, qreal x, qreal baseline, const QColor &fg) = 0;
    qreal w = 0, h = 0, asc = 0; // h = asc + desc
};

using BoxPtr = std::unique_ptr<Box>;
using BoxVec = std::vector<BoxPtr>;
constexpr qreal kScriptScale = 0.62;

struct TextBox : Box {
    QString text;
    bool italic = false;
    bool upright = false; // \mathrm/\text
    QFont drawFont;
    qreal leftPad = 0;
    void layout(const QFont &font) override {
        drawFont = font;
        drawFont.setItalic(italic && !upright);
        const QFontMetricsF fm(drawFont);
        const QRectF ink = fm.tightBoundingRect(text);
        leftPad = std::max(qreal(0), -ink.left());
        w = leftPad + std::max(fm.horizontalAdvance(text), ink.right());
        asc = fm.ascent();
        h = fm.height();
    }
    void draw(QPainter &p, qreal x, qreal baseline, const QColor &fg) override {
        // 测量与绘制必须使用同一字体,并计入斜体字形伸出的部分。
        const QFont original = p.font();
        p.setFont(drawFont);
        p.setPen(fg);
        p.drawText(QPointF(x + leftPad, baseline), text);
        p.setFont(original);
    }
};

// 上/下标组合:base + sup + sub
struct ScriptBox : Box {
    BoxPtr base, sup, sub;
    qreal supShift = 0, subShift = 0;
    void layout(const QFont &font) override;
    void draw(QPainter &p, qreal x, qreal baseline, const QColor &fg) override;
};

// 分数
struct FracBox : Box {
    BoxPtr num, den;
    qreal lineY = 0, numY = 0, denY = 0;
    void layout(const QFont &font) override;
    void draw(QPainter &p, qreal x, qreal baseline, const QColor &fg) override;
};

// 根号
struct SqrtBox : Box {
    BoxPtr body;
    void layout(const QFont &font) override;
    void draw(QPainter &p, qreal x, qreal baseline, const QColor &fg) override;
};

// 大运算符(∑/∫/lim 带上下限)
struct BigOpBox : Box {
    QString glyph;      // ∑ 等(或 lim 文字)
    bool textual = false; // lim/max/min 按文字画
    BoxPtr under, over;
    TextBox symbol;
    qreal overY = 0, underY = 0;
    void layout(const QFont &font) override;
    void draw(QPainter &p, qreal x, qreal baseline, const QColor &fg) override;
};

// 水平序列
struct RowBox : Box {
    BoxVec items;
    void layout(const QFont &font) override {
        w = 0; asc = 0; h = 0;
        qreal desc = 0;
        for (BoxPtr &b : items) {
            b->layout(font);
            w += b->w;
            asc = std::max(asc, b->asc);
            desc = std::max(desc, b->h - b->asc);
        }
        h = asc + desc;
    }
    void draw(QPainter &p, qreal x, qreal baseline, const QColor &fg) override {
        qreal cx = x;
        for (BoxPtr &b : items) {
            b->draw(p, cx, baseline, fg);
            cx += b->w;
        }
    }
};

// 矢量箭头(\vec{F}):内容上方画小箭头
struct VecBox : Box {
    BoxPtr body;
    bool hat = false; // true = 尖帽(\hat)
    bool bar = false; // true = 横线(\bar)
    void layout(const QFont &font) override {
        const QFontMetricsF fm(font);
        if (body)
            body->layout(font);
        w = body ? body->w : fm.horizontalAdvance(QStringLiteral("x"));
        asc = (body ? body->asc : fm.ascent()) + 5; // 上方留 5px 给箭头
        h = asc + (body ? (body->h - body->asc) : fm.descent());
    }
    void draw(QPainter &p, qreal x, qreal baseline, const QColor &fg) override {
        if (body)
            body->draw(p, x, baseline, fg);
        const qreal arrowY = baseline - asc + 2;
        const qreal cx = x + w / 2;
        QPen pen(fg);
        pen.setWidthF(1.3);
        p.setPen(pen);
        if (hat) {
            // 尖帽 ^ 形
            QPainterPath path;
            path.moveTo(x + 1, arrowY + 2);
            path.lineTo(cx, arrowY - 1);
            path.lineTo(x + w - 1, arrowY + 2);
            p.drawPath(path);
        } else if (bar) {
            // 横线
            p.drawLine(QPointF(x, arrowY + 1), QPointF(x + w, arrowY + 1));
        } else {
            // 右箭头 →:横线 + 尖头
            p.drawLine(QPointF(x + 1, arrowY), QPointF(x + w - 1, arrowY));
            QPainterPath tip;
            tip.moveTo(x + w - 5, arrowY - 2.5);
            tip.lineTo(x + w - 1, arrowY);
            tip.lineTo(x + w - 5, arrowY + 2.5);
            p.drawPath(tip);
        }
    }
};

// ───────────────────────── 布局实现 ─────────────────────────

void ScriptBox::layout(const QFont &font)
{
    const QFontMetricsF fm(font);
    if (base)
        base->layout(font);
    if (sup)
        sup->layout(font);
    if (sub)
        sub->layout(font);
    const qreal baseAsc = base ? base->asc : fm.ascent() * 0.6;
    const qreal baseDesc = base ? (base->h - base->asc) : fm.descent();
    const qreal supAsc = sup ? sup->asc * kScriptScale : 0;
    const qreal subDesc = sub ? (sub->h - sub->asc) * kScriptScale : 0;
    const qreal supW = sup ? sup->w * kScriptScale : 0;
    const qreal subW = sub ? sub->w * kScriptScale : 0;
    w = (base ? base->w : 0) + std::max(supW, subW);
    supShift = fm.ascent() * 0.45;
    subShift = fm.height() * 0.25;
    asc = std::max({baseAsc, sup ? supAsc + supShift : qreal(0),
                    sub ? sub->asc * kScriptScale - subShift : qreal(0)});
    h = asc + std::max({baseDesc, sub ? subDesc + subShift : qreal(0),
                       sup ? (sup->h - sup->asc) * kScriptScale - supShift : qreal(0)});
}

void ScriptBox::draw(QPainter &p, qreal x, qreal baseline, const QColor &fg)
{
    if (base)
        base->draw(p, x, baseline, fg);
    const qreal bx = x + (base ? base->w : 0);
    if (sup) {
        p.save();
        p.translate(bx, baseline - supShift);
        p.scale(kScriptScale, kScriptScale);
        sup->draw(p, 0, 0, fg);
        p.restore();
    }
    if (sub) {
        p.save();
        p.translate(bx, baseline + subShift);
        p.scale(kScriptScale, kScriptScale);
        sub->draw(p, 0, 0, fg);
        p.restore();
    }
}

void FracBox::layout(const QFont &font)
{
    const QFontMetricsF fm(font);
    if (num)
        num->layout(font);
    if (den)
        den->layout(font);
    w = std::max(num ? num->w : 0, den ? den->w : 0) + 8;
    const qreal nw = num ? num->h : fm.height() * 0.5;
    const qreal dw = den ? den->h : fm.height() * 0.5;
    lineY = -fm.xHeight() * 0.4;
    const qreal gap = 3;
    numY = lineY - gap - (num ? num->h - num->asc : qreal(0));
    denY = lineY + gap + (den ? den->asc : qreal(0));
    asc = nw + gap - lineY;
    h = asc + lineY + gap + dw;
}

void FracBox::draw(QPainter &p, qreal x, qreal baseline, const QColor &fg)
{
    if (num)
        num->draw(p, x + (w - num->w) / 2, baseline + numY, fg);
    if (den)
        den->draw(p, x + (w - den->w) / 2, baseline + denY, fg);
    QPen pen(fg);
    pen.setWidthF(1.4);
    p.setPen(pen);
    p.drawLine(QPointF(x, baseline + lineY), QPointF(x + w, baseline + lineY));
}

void SqrtBox::layout(const QFont &font)
{
    const QFontMetricsF fm(font);
    if (body)
        body->layout(font);
    w = (body ? body->w : 0) + 14;
    asc = (body ? body->asc : fm.ascent()) + 5;
    h = asc + (body ? (body->h - body->asc) : fm.descent());
}

void SqrtBox::draw(QPainter &p, qreal x, qreal baseline, const QColor &fg)
{
    const qreal bx = x + 12, bw = body ? body->w : 0;
    const qreal top = baseline - asc + 1;
    // √ 符号 + 顶横线
    QPen pen(fg);
    pen.setWidthF(1.6);
    p.setPen(pen);
    QPainterPath path;
    path.moveTo(x + 1, baseline - asc * 0.45);
    path.lineTo(x + 5, baseline - asc * 0.45 + 3);
    path.lineTo(x + 9, baseline);
    path.lineTo(x + 11, top);
    path.lineTo(x + 12 + bw, top);
    p.drawPath(path);
    if (body)
        body->draw(p, bx, baseline, fg);
}

void BigOpBox::layout(const QFont &font)
{
    if (under)
        under->layout(font);
    if (over)
        over->layout(font);
    QFont operatorFont = font;
    if (!textual) {
        if (font.pixelSize() > 0)
            operatorFont.setPixelSize(qMax(1, qRound(font.pixelSize() * 1.25)));
        else if (font.pointSizeF() > 0)
            operatorFont.setPointSizeF(font.pointSizeF() * 1.25);
    }
    symbol.text = glyph;
    symbol.layout(operatorFont);
    const qreal desc = symbol.h - symbol.asc;
    w = std::max({symbol.w, under ? under->w * kScriptScale : 0,
                  over ? over->w * kScriptScale : 0});
    asc = symbol.asc + (over ? over->h * kScriptScale + 2 : 0);
    h = asc + desc + (under ? under->h * kScriptScale + 2 : 0);
    overY = -symbol.asc - 2 - (over ? (over->h - over->asc) * kScriptScale : 0);
    underY = desc + 2 + (under ? under->asc * kScriptScale : 0);
}

void BigOpBox::draw(QPainter &p, qreal x, qreal baseline, const QColor &fg)
{
    symbol.draw(p, x + (w - symbol.w) / 2, baseline, fg);
    const auto drawLimit = [&](Box *box, qreal y) {
        if (!box)
            return;
        p.save();
        p.translate(x + (w - box->w * kScriptScale) / 2, baseline + y);
        p.scale(kScriptScale, kScriptScale);
        box->draw(p, 0, 0, fg);
        p.restore();
    };
    drawLimit(over.get(), overY);
    drawLimit(under.get(), underY);
}

// ───────────────────────── 解析器 ─────────────────────────

struct Parser {
    const QString &s;
    int i = 0;
    bool upright = false;
    // {...} 嵌套深度:readGroup ↔ readAtoms ↔ readAtom 互递归,深度 = 花括号层数。
    // 无上限则畸形公式可把栈冲爆,故硬性截断。
    static constexpr int kMaxDepth = 64;
    int depth = 0;

    Parser(const QString &src) : s(src) {}

    bool atEnd() const { return i >= s.size(); }
    QChar peek() const { return i < s.size() ? s.at(i) : QChar(); }

    void skipSpace()
    {
        while (!atEnd() && peek().isSpace())
            ++i;
    }

    QString readCmdName()
    {
        int k = i + 1;
        while (k < s.size() && s.at(k).isLetter())
            ++k;
        if (k == i + 1) // \, \; 等单字符命令
            return s.mid(i + 1, 1);
        return s.mid(i + 1, k - i - 1);
    }

    // 读一个 {...} 组(已位于 '{')
    BoxVec readGroup()
    {
        if (depth >= Parser::kMaxDepth) {
            // 超深嵌套:跳到匹配的 '}'(或串尾)并返回空组,防止无限递归
            while (i < s.size() && s.at(i) != QLatin1Char('}'))
                ++i;
            if (i < s.size())
                ++i;
            return {};
        }
        ++i; // '{'
        // 组内也要合并上下标,否则分子/根号中的 x^2 会退化为 x2。
        ++depth;
        BoxVec out = readAtoms();
        --depth;
        if (peek() == QLatin1Char('}'))
            ++i;
        return out;
    }

    BoxPtr rowFrom(BoxVec &&v)
    {
        if (v.size() == 1)
            return std::move(v.front());
        auto r = std::make_unique<RowBox>();
        r->items = std::move(v);
        return r;
    }

    BoxPtr readAtom()
    {
        if (depth >= Parser::kMaxDepth) {
            // 超深：跳过单个字符/命令，避免死循环
            if (!atEnd()) {
                if (peek() == QLatin1Char('\\')) {
                    ++i; // 跳过反斜杠
                    if (i < s.size() && s.at(i).isLetter()) {
                        while (i < s.size() && s.at(i).isLetter())
                            ++i;
                    }
                } else {
                    ++i;
                }
            }
            return nullptr;
        }
        if (atEnd())
            return nullptr;
        const QChar c = peek();
        if (c == QLatin1Char('\\')) {
            const QString name = readCmdName();
            i += name.size() + 1;
            skipSpace(); // TeX 命令与参数之间可以有空格/换行。
            if (name == QLatin1String("frac") || name == QLatin1String("dfrac")
                || name == QLatin1String("tfrac")) {
                auto f = std::make_unique<FracBox>();
                if (peek() == QLatin1Char('{'))
                    f->num = rowFrom(readGroup());
                skipSpace();
                if (peek() == QLatin1Char('{'))
                    f->den = rowFrom(readGroup());
                return f;
            }
            if (name == QLatin1String("sqrt")) {
                auto sq = std::make_unique<SqrtBox>();
                if (peek() == QLatin1Char('{'))
                    sq->body = rowFrom(readGroup());
                return sq;
            }
            // 矢量箭头/尖帽/横线(\vec{F} → F 上方画 →)
            if (name == QLatin1String("vec") || name == QLatin1String("hat")
                || name == QLatin1String("bar") || name == QLatin1String("overline")) {
                auto vb = std::make_unique<VecBox>();
                vb->hat = (name == QLatin1String("hat"));
                vb->bar = (name == QLatin1String("bar") || name == QLatin1String("overline"));
                if (peek() == QLatin1Char('{')) {
                    vb->body = rowFrom(readGroup());
                } else if (!atEnd() && peek().isLetter()) {
                    // \vec F → 单字母(无括号)
                    const QChar ch = peek();
                    ++i;
                    auto t = std::make_unique<TextBox>();
                    t->text = ch;
                    t->italic = true;
                    vb->body = std::move(t);
                }
                return vb;
            }
            // 数学函数名:直立(\sin → "sin" 不斜体)
            if (funcNames().contains(name)) {
                auto t = std::make_unique<TextBox>();
                t->text = name;
                t->upright = true; // 不斜体
                t->italic = false;
                return t;
            }
            if (name == QLatin1String("sum") || name == QLatin1String("prod")
                || name == QLatin1String("int") || name == QLatin1String("iint")
                || name == QLatin1String("oint") || name == QLatin1String("lim")
                || name == QLatin1String("max") || name == QLatin1String("min")) {
                auto op = std::make_unique<BigOpBox>();
                op->glyph = commands().value(name);
                op->textual = (name == QLatin1String("lim") || name == QLatin1String("max")
                               || name == QLatin1String("min"));
                // limits: ^over _under 紧随
                return op;
            }
            if (name == QLatin1String("text") || name == QLatin1String("mathrm")
                || name == QLatin1String("operatorname")) {
                if (peek() == QLatin1Char('{')) {
                    const bool previous = upright;
                    upright = true;
                    BoxVec v = readGroup();
                    upright = previous;
                    return rowFrom(std::move(v));
                }
                return nullptr;
            }
            if (isSpacingCmd(name)) {
                auto t = std::make_unique<TextBox>();
                t->text = name == QLatin1String("quad") ? QStringLiteral("    ")
                                                        : QStringLiteral("        ");
                return t;
            }
            // 符号命令
            auto t = std::make_unique<TextBox>();
            t->text = commands().value(name, QStringLiteral("\\") + name);
            return t;
        }
        if (c == QLatin1Char('{')) {
            return rowFrom(readGroup());
        }
        if (c == QLatin1Char('}')) {
            ++i;
            return nullptr;
        }
        if (c == QLatin1Char('^') || c == QLatin1Char('_')) {
            // 修饰前一个原子:由调用方(readAtoms)处理;这里不该走到
            ++i;
            return nullptr;
        }
        // 普通字符:变量斜体;关系/运算符两侧加薄空(数学排版惯例)
        ++i;
        auto t = std::make_unique<TextBox>();
        t->italic = c.isLetter() && !upright;
        // = + - < > 两侧 2px 间距(靠在 text 前后加窄空格,等宽不可见)
        if (c.isSpace()) {
            // 文本组保留空白,但不能用换行符做单行字形测量。
            t->text = QLatin1Char(' ');
        } else if (c == QLatin1Char('=') || c == QLatin1Char('+')
            || c == QLatin1Char('<') || c == QLatin1Char('>')) {
            t->text = QStringLiteral(" ") + c + QStringLiteral(" ");
        } else {
            t->text = c;
        }
        return t;
    }

    // 读一串原子,处理 ^ _ 修饰与前一个盒子的合并(大运算符的上下限也在此)
    BoxVec readAtoms()
    {
        BoxVec out;
        while (!atEnd()) {
            const QChar c = peek();
            if (c == QLatin1Char('}') )
                break;
            if (c.isSpace() && !upright) {
                ++i; // 数学模式忽略空白,避免脚标误挂到空格上。
                continue;
            }
            if (c == QLatin1Char('^') || c == QLatin1Char('_')) {
                ++i;
                skipSpace();
                BoxPtr arg;
                if (peek() == QLatin1Char('{'))
                    arg = rowFrom(readGroup());
                else if (!atEnd() && peek() != QLatin1Char('}'))
                    arg = readAtom(); // 单字符/命令共用解析,保留外层组的闭括号。
                if (!arg)
                    continue;
                // 并入前一个 Box:
                //  - BigOpBox → under/over
                //  - 已有 ScriptBox → 补另一侧
                //  - 普通 → 新建 ScriptBox
                if (!out.empty()) {
                    Box *prev = out.back().get();
                    if (auto *op = dynamic_cast<BigOpBox *>(prev)) {
                        if (c == QLatin1Char('^'))
                            op->over = std::move(arg);
                        else
                            op->under = std::move(arg);
                        continue;
                    }
                    if (auto *sc = dynamic_cast<ScriptBox *>(prev)) {
                        if (c == QLatin1Char('^') && !sc->sup)
                            sc->sup = std::move(arg);
                        else if (c == QLatin1Char('_') && !sc->sub)
                            sc->sub = std::move(arg);
                        continue;
                    }
                    auto script = std::make_unique<ScriptBox>();
                    if (c == QLatin1Char('^'))
                        script->sup = std::move(arg);
                    else
                        script->sub = std::move(arg);
                    script->base = std::move(out.back());
                    out.back() = std::move(script);
                    continue;
                }
                // 行首直接 ^_:孤立脚本,做无 base 脚本
                auto script = std::make_unique<ScriptBox>();
                if (c == QLatin1Char('^'))
                    script->sup = std::move(arg);
                else
                    script->sub = std::move(arg);
                out.push_back(std::move(script));
                continue;
            }
            BoxPtr b = readAtom();
            if (b)
                out.push_back(std::move(b));
        }
        return out;
    }
};

} // namespace

// ───────────────────────── 公开渲染入口 ─────────────────────────

QPixmap render(const QString &latex, const QColor &fg, double dpr)
{
    if (latex.size() > 64 * 1024 || !std::isfinite(dpr) || dpr <= 0 || dpr > 8 || latex.trimmed().isEmpty())
        return {};

    Parser parser(latex);
    BoxVec boxes = parser.readAtoms();
    if (boxes.empty())
        return {};
    BoxPtr root = parser.rowFrom(std::move(boxes));

    // 字体:数学斜体风格(Cambria Math 有斜体希腊;回退 Consolas/雅黑)
    QFont font;
    font.setFamilies({ QStringLiteral("Cambria Math"), QStringLiteral("Segoe UI"),
                       QStringLiteral("Microsoft YaHei UI") });
    font.setPixelSize(19);
    font.setStyleStrategy(QFont::ForceOutline);

    root->layout(font);

    const qreal pad = 4;
    const qreal width = (root->w + 2 * pad) * dpr, height = (root->h + 2 * pad) * dpr;
    if (!std::isfinite(width) || !std::isfinite(height) || width <= 0 || height <= 0 || width > 4000 || height > 2000)
        return {}; // Validate before converting floating-point sizes to integers.
    const int wPix = qCeil(width);
    const int hPix = qCeil(height);
    if (wPix <= 0 || hPix <= 0 || wPix > 4000 || hPix > 2000)
        return {};

    QPixmap pm(wPix, hPix);
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    {
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.setRenderHint(QPainter::TextAntialiasing);
        p.setFont(font);
        root->draw(p, pad, pad + root->asc, fg);
    }
    return pm;
}

} // namespace AiMathPainter
