// MathRender.cpp — see MathRender.h.
//
// Pipeline: lex LaTeX → flat token list (with group trees for {} args) →
// recursive descent over a small grammar (frac, sqrt, scripts, big operators,
// matrix) → multi-line canvas merge for vertical constructs.

#include "ai/AiMath.h"

#include "ai/AiText.h"

#include <QChar>
#include <QHash>
#include <QMap>
#include <QVector>

#include <algorithm>

namespace MathRender {

namespace {

// ───────────────────────────── symbol tables ─────────────────────────────

struct Sym {
    QString uni;   // preferred unicode
    QString ascii; // fallback when uni is unavailable
};

const QHash<QString, Sym>& commands()
{
    static const QHash<QString, Sym> m = {
        // greek
        { QStringLiteral("alpha"), { QStringLiteral("α"), QStringLiteral("a") } },
        { QStringLiteral("beta"), { QStringLiteral("β"), QStringLiteral("b") } },
        { QStringLiteral("gamma"), { QStringLiteral("γ"), QStringLiteral("Y") } },
        { QStringLiteral("delta"), { QStringLiteral("δ"), QStringLiteral("d") } },
        { QStringLiteral("epsilon"), { QStringLiteral("ε"), QStringLiteral("e") } },
        { QStringLiteral("zeta"), { QStringLiteral("ζ"), QStringLiteral("z") } },
        { QStringLiteral("eta"), { QStringLiteral("η"), QStringLiteral("n") } },
        { QStringLiteral("theta"), { QStringLiteral("θ"), QStringLiteral("0") } },
        { QStringLiteral("iota"), { QStringLiteral("ι"), QStringLiteral("i") } },
        { QStringLiteral("kappa"), { QStringLiteral("κ"), QStringLiteral("k") } },
        { QStringLiteral("lambda"), { QStringLiteral("λ"), QStringLiteral("L") } },
        { QStringLiteral("mu"), { QStringLiteral("μ"), QStringLiteral("u") } },
        { QStringLiteral("nu"), { QStringLiteral("ν"), QStringLiteral("v") } },
        { QStringLiteral("xi"), { QStringLiteral("ξ"), QStringLiteral("X") } },
        { QStringLiteral("pi"), { QStringLiteral("π"), QStringLiteral("p") } },
        { QStringLiteral("rho"), { QStringLiteral("ρ"), QStringLiteral("r") } },
        { QStringLiteral("sigma"), { QStringLiteral("σ"), QStringLiteral("S") } },
        { QStringLiteral("tau"), { QStringLiteral("τ"), QStringLiteral("t") } },
        { QStringLiteral("upsilon"), { QStringLiteral("υ"), QStringLiteral("u") } },
        { QStringLiteral("phi"), { QStringLiteral("φ"), QStringLiteral("f") } },
        { QStringLiteral("chi"), { QStringLiteral("χ"), QStringLiteral("x") } },
        { QStringLiteral("psi"), { QStringLiteral("ψ"), QStringLiteral("P") } },
        { QStringLiteral("omega"), { QStringLiteral("ω"), QStringLiteral("w") } },
        { QStringLiteral("Gamma"), { QStringLiteral("Γ"), QStringLiteral("r") } },
        { QStringLiteral("Delta"), { QStringLiteral("Δ"), QStringLiteral("D") } },
        { QStringLiteral("Theta"), { QStringLiteral("Θ"), QStringLiteral("O") } },
        { QStringLiteral("Lambda"), { QStringLiteral("Λ"), QStringLiteral("L") } },
        { QStringLiteral("Xi"), { QStringLiteral("Ξ"), QStringLiteral("X") } },
        { QStringLiteral("Pi"), { QStringLiteral("Π"), QStringLiteral("TT") } },
        { QStringLiteral("Sigma"), { QStringLiteral("Σ"), QStringLiteral("E") } },
        { QStringLiteral("Phi"), { QStringLiteral("Φ"), QStringLiteral("F") } },
        { QStringLiteral("Psi"), { QStringLiteral("Ψ"), QStringLiteral("P") } },
        { QStringLiteral("Omega"), { QStringLiteral("Ω"), QStringLiteral("W") } },
        // operators / relations
        { QStringLiteral("times"), { QStringLiteral("×"), QStringLiteral("x") } },
        { QStringLiteral("div"), { QStringLiteral("÷"), QStringLiteral("/") } },
        { QStringLiteral("pm"), { QStringLiteral("±"), QStringLiteral("+/-") } },
        { QStringLiteral("mp"), { QStringLiteral("∓"), QStringLiteral("-/+") } },
        { QStringLiteral("cdot"), { QStringLiteral("·"), QStringLiteral("*") } },
        { QStringLiteral("leq"), { QStringLiteral("≤"), QStringLiteral("<=") } },
        { QStringLiteral("geq"), { QStringLiteral("≥"), QStringLiteral(">=") } },
        { QStringLiteral("neq"), { QStringLiteral("≠"), QStringLiteral("!=") } },
        { QStringLiteral("approx"), { QStringLiteral("≈"), QStringLiteral("~") } },
        { QStringLiteral("equiv"), { QStringLiteral("≡"), QStringLiteral("=") } },
        { QStringLiteral("sim"), { QStringLiteral("∼"), QStringLiteral("~") } },
        { QStringLiteral("propto"), { QStringLiteral("∝"), QStringLiteral("~") } },
        { QStringLiteral("infty"), { QStringLiteral("∞"), QStringLiteral("8") } },
        { QStringLiteral("partial"), { QStringLiteral("∂"), QStringLiteral("d") } },
        { QStringLiteral("nabla"), { QStringLiteral("∇"), QStringLiteral("V") } },
        { QStringLiteral("forall"), { QStringLiteral("∀"), QStringLiteral("V") } },
        { QStringLiteral("exists"), { QStringLiteral("∃"), QStringLiteral("E") } },
        { QStringLiteral("in"), { QStringLiteral("∈"), QStringLiteral("in") } },
        { QStringLiteral("notin"), { QStringLiteral("∉"), QStringLiteral("!in") } },
        { QStringLiteral("subset"), { QStringLiteral("⊂"), QStringLiteral("C") } },
        { QStringLiteral("cup"), { QStringLiteral("∪"), QStringLiteral("U") } },
        { QStringLiteral("cap"), { QStringLiteral("∩"), QStringLiteral("^") } },
        { QStringLiteral("emptyset"), { QStringLiteral("∅"), QStringLiteral("{}") } },
        { QStringLiteral("to"), { QStringLiteral("→"), QStringLiteral("->") } },
        { QStringLiteral("rightarrow"), { QStringLiteral("→"), QStringLiteral("->") } },
        { QStringLiteral("leftarrow"), { QStringLiteral("←"), QStringLiteral("<-") } },
        { QStringLiteral("Rightarrow"), { QStringLiteral("⇒"), QStringLiteral("=>") } },
        { QStringLiteral("leftrightarrow"), { QStringLiteral("↔"), QStringLiteral("<->") } },
        { QStringLiteral("mapsto"), { QStringLiteral("↦"), QStringLiteral("|->") } },
        { QStringLiteral("ldots"), { QStringLiteral("…"), QStringLiteral("...") } },
        { QStringLiteral("cdots"), { QStringLiteral("⋯"), QStringLiteral("...") } },
        { QStringLiteral("vdots"), { QStringLiteral("⋮"), QStringLiteral(":") } },
        { QStringLiteral("ddots"), { QStringLiteral("⋱"), QStringLiteral(":.") } },
        { QStringLiteral("perp"), { QStringLiteral("⊥"), QStringLiteral("_|_") } },
        { QStringLiteral("parallel"), { QStringLiteral("∥"), QStringLiteral("||") } },
        { QStringLiteral("angle"), { QStringLiteral("∠"), QStringLiteral("<") } },
        { QStringLiteral("degree"), { QStringLiteral("°"), QStringLiteral("deg") } },
        { QStringLiteral("circ"), { QStringLiteral("∘"), QStringLiteral("o") } },
        { QStringLiteral("prime"), { QStringLiteral("′"), QStringLiteral("'") } },
        { QStringLiteral("hbar"), { QStringLiteral("ℏ"), QStringLiteral("h") } },
        { QStringLiteral("ell"), { QStringLiteral("ℓ"), QStringLiteral("l") } },
        { QStringLiteral("Re"), { QStringLiteral("ℜ"), QStringLiteral("Re") } },
        { QStringLiteral("Im"), { QStringLiteral("ℑ"), QStringLiteral("Im") } },
        // spacing
        { QStringLiteral(","), { QStringLiteral(" "), QStringLiteral(" ") } },
        { QStringLiteral(";"), { QStringLiteral("  "), QStringLiteral("  ") } },
        { QStringLiteral("quad"), { QStringLiteral("    "), QStringLiteral("    ") } },
        { QStringLiteral("qquad"), { QStringLiteral("        "), QStringLiteral("        ") } },
        { QStringLiteral("left"), { QString(), QString() } },
        { QStringLiteral("right"), { QString(), QString() } },
        { QStringLiteral("mathrm"), { QString(), QString() } },
        { QStringLiteral("text"), { QString(), QString() } },
        { QStringLiteral("operatorname"), { QString(), QString() } },
        { QStringLiteral("displaystyle"), { QString(), QString() } },
        { QStringLiteral("limits"), { QString(), QString() } },
        { QStringLiteral("big"), { QString(), QString() } },
        { QStringLiteral("Big"), { QString(), QString() } },
        { QStringLiteral("bigg"), { QString(), QString() } },
    };
    return m;
}

// big operators with limits get a "vertical" rendering
const QHash<QString, QString>& bigOps()
{
    static const QHash<QString, QString> m = {
        { QStringLiteral("sum"), QStringLiteral("∑") },
        { QStringLiteral("prod"), QStringLiteral("∏") },
        { QStringLiteral("int"), QStringLiteral("∫") },
        { QStringLiteral("iint"), QStringLiteral("∬") },
        { QStringLiteral("oint"), QStringLiteral("∮") },
        { QStringLiteral("lim"), QStringLiteral("lim") },
        { QStringLiteral("max"), QStringLiteral("max") },
        { QStringLiteral("min"), QStringLiteral("min") },
    };
    return m;
}

const QHash<QChar, QString>& bracketPairs()
{
    static const QHash<QChar, QString> m = {
        { QLatin1Char('('), QStringLiteral("(") },
        { QLatin1Char(')'), QStringLiteral(")") },
        { QLatin1Char('['), QStringLiteral("[") },
        { QLatin1Char(']'), QStringLiteral("]") },
        { QLatin1Char('{'), QStringLiteral("{") },
        { QLatin1Char('}'), QStringLiteral("}") },
        { QLatin1Char('|'), QStringLiteral("|") },
        { QLatin1Char('.'), QString() },
    };
    return m;
}

// unicode superscript / subscript availability
QString superscript(const QString& s)
{
    static const QString sup = QStringLiteral("⁰¹²³⁴⁵⁶⁷⁸⁹");
    static const QString lettersUp = QStringLiteral("ᴬᴮᴰᴱᴳᴴᴵᴶᴷᴸᴹᴹᴺᴼᴾᴿᵀᵁⱽᵂ");
    QString out;
    for (const QChar c : s) {
        const int d = c.digitValue();
        if (d >= 0 && d <= 9) {
            out += sup.at(d);
            continue;
        }
        if (c == QLatin1Char('+'))
            out += QStringLiteral("⁺");
        else if (c == QLatin1Char('-'))
            out += QStringLiteral("⁻");
        else if (c == QLatin1Char('='))
            out += QStringLiteral("⁼");
        else if (c == QLatin1Char('('))
            out += QStringLiteral("⁽");
        else if (c == QLatin1Char(')'))
            out += QStringLiteral("⁾");
        else if (c == QLatin1Char('n'))
            out += QStringLiteral("ⁿ");
        else if (c == QLatin1Char('i'))
            out += QStringLiteral("ⁱ");
        else
            return QString(); // no unicode superscript → caller falls back
    }
    return out;
}

QString subscript(const QString& s)
{
    static const QString sub = QStringLiteral("₀₁₂₃₄₅₆₇₈₉");
    QString out;
    for (const QChar c : s) {
        const int d = c.digitValue();
        if (d >= 0 && d <= 9) {
            out += sub.at(d);
            continue;
        }
        if (c == QLatin1Char('+'))
            out += QStringLiteral("₊");
        else if (c == QLatin1Char('-'))
            out += QStringLiteral("₋");
        else if (c == QLatin1Char('='))
            out += QStringLiteral("₌");
        else if (c == QLatin1Char('('))
            out += QStringLiteral("₍");
        else if (c == QLatin1Char(')'))
            out += QStringLiteral("₎");
        else if (c == QLatin1Char('a'))
            out += QStringLiteral("ₐ");
        else if (c == QLatin1Char('e'))
            out += QStringLiteral("ₑ");
        else if (c == QLatin1Char('i'))
            out += QStringLiteral("ᵢ");
        else if (c == QLatin1Char('j'))
            out += QStringLiteral("ⱼ");
        else if (c == QLatin1Char('n'))
            out += QStringLiteral("ₙ");
        else if (c == QLatin1Char('x'))
            out += QStringLiteral("ₓ");
        else
            return QString();
    }
    return out;
}

// ───────────────────────────── lexer ─────────────────────────────

struct Node {
    enum Kind { Text, Cmd, Group, Sup, Sub, SupSub };
    Kind kind = Text;
    QString text;      // Text literal / Cmd name
    QVector<Node> children; // Group contents; Sup/Sub [arg]; SupSub [sup, sub]
};

// result: node list + how far we got
void lex(const QString& s, int from, const QString& stopCmds,
         QVector<Node>* out, int* endPos, QString* hitCmd)
{
    int i = from;
    while (i < s.size()) {
        const QChar c = s.at(i);
        if (c == QLatin1Char('\\')) {
            const int j = i + 1;
            int k = j;
            while (k < s.size() && s.at(k).isLetter())
                ++k;
            QString name;
            if (k > j) {
                name = s.mid(j, k - j);
                i = k;
            } else if (k < s.size()) { // \, \; \{ …
                name = s.at(k);
                i = k + 1;
            } else {
                i = k;
            }
            if (!stopCmds.isEmpty() && stopCmds.contains(name)) {
                if (hitCmd)
                    *hitCmd = name;
                if (endPos)
                    *endPos = i - 1 - name.size() + 0; // position of the backslash
                *endPos = i - name.size() - 1;
                return;
            }
            Node n;
            n.kind = Node::Cmd;
            n.text = name;
            out->push_back(n);
            continue;
        }
        if (c == QLatin1Char('{')) {
            QVector<Node> inner;
            int end = i;
            QString hc;
            lex(s, i + 1, stopCmds, &inner, &end, &hc);
            if (hc.isEmpty()) {
                // stopped at a bare '}' (lex returns when it sees it below)
            }
            // find the matching brace: lex stops at '}' handled here
            // We handle '}' explicitly:
            i = end;
            if (i < s.size() && s.at(i) == QLatin1Char('}'))
                ++i;
            Node n;
            n.kind = Node::Group;
            n.children = inner;
            out->push_back(n);
            continue;
        }
        if (c == QLatin1Char('}')) {
            if (endPos)
                *endPos = i;
            if (hitCmd)
                hitCmd->clear();
            return;
        }
        if (c == QLatin1Char('^') || c == QLatin1Char('_')) {
            // script: single char or group
            QVector<Node> arg;
            int k = i + 1;
            if (k < s.size() && s.at(k) == QLatin1Char('{')) {
                int e = k;
                QString hc;
                lex(s, k + 1, QString(), &arg, &e, &hc);
                i = e;
                if (i < s.size() && s.at(i) == QLatin1Char('}'))
                    ++i;
            } else if (k < s.size() && s.at(k) == QLatin1Char('\\')) {
                // single \command as the script argument — parse ONE command,
                // never recurse (a bare lex() would swallow the rest of the line)
                int j = k + 1;
                int e2 = j;
                while (e2 < s.size() && s.at(e2).isLetter())
                    ++e2;
                QString name = e2 > j ? s.mid(j, e2 - j)
                                      : (e2 < s.size() ? QString(s.at(e2)) : QString());
                i = e2 > j ? e2 : (e2 < s.size() ? e2 + 1 : e2);
                Node t;
                const QHash<QString, Sym>& syms = commands();
                if (syms.contains(name)) {
                    const Sym& sym = syms.value(name);
                    t.kind = Node::Text;
                    t.text = sym.uni.isEmpty() ? sym.ascii : sym.uni;
                } else {
                    t.kind = Node::Cmd;
                    t.text = name;
                }
                arg.push_back(t);
            } else if (k < s.size()) {
                Node t;
                t.kind = Node::Text;
                t.text = QString(s.at(k));
                arg.push_back(t);
                i = k + 1;
            } else {
                i = k;
            }
            // merge ^a_b into SupSub when adjacent
            if (!out->isEmpty() && out->last().kind == Node::Sup && c == QLatin1Char('_')) {
                Node& prev = out->last();
                Node n;
                n.kind = Node::SupSub;
                n.children = prev.children;
                n.children.push_back(arg.first());
                prev = n;
                continue;
            }
            if (!out->isEmpty() && out->last().kind == Node::Sub && c == QLatin1Char('^')) {
                Node& prev = out->last();
                Node n;
                n.kind = Node::SupSub;
                n.children.push_back(arg.first());
                n.children.push_back(prev.children.first());
                prev = n;
                continue;
            }
            Node n;
            n.kind = c == QLatin1Char('^') ? Node::Sup : Node::Sub;
            n.children = arg;
            out->push_back(n);
            continue;
        }
        if (c == QLatin1Char('$')) {
            if (endPos)
                *endPos = i;
            if (hitCmd)
                hitCmd = hitCmd; // unchanged
            return;
        }
        // plain text run
        int k = i;
        while (k < s.size() && s.at(k) != QLatin1Char('\\') && s.at(k) != QLatin1Char('{')
               && s.at(k) != QLatin1Char('}') && s.at(k) != QLatin1Char('^')
               && s.at(k) != QLatin1Char('_') && s.at(k) != QLatin1Char('$'))
            ++k;
        Node n;
        n.kind = Node::Text;
        n.text = s.mid(i, k - i);
        out->push_back(n);
        i = k;
    }
    if (endPos)
        *endPos = s.size();
}

// ───────────────────────────── canvas ─────────────────────────────

// A small multi-line canvas: lines[row] padded to the same width.
struct Canvas {
    QStringList rows;
    int baseRow = 0; // the row that sits on the text baseline

    void normalize()
    {
        if (rows.isEmpty())
            rows << QString();
        const int w = maxWidth();
        for (QString& r : rows) {
            while (Text::visibleWidth(r) < w)
                r += QLatin1Char(' ');
        }
    }

    int maxWidth() const
    {
        int w = 0;
        for (const QString& r : rows)
            w = std::max(w, Text::visibleWidth(r));
        return w;
    }

    QString join() const
    {
        QStringList trimmed = rows;
        for (QString& r : trimmed) {
            while (r.endsWith(QLatin1Char(' ')))
                r.chop(1);
        }
        return trimmed.join(QLatin1Char('\n'));
    }
};

Canvas hcat(Canvas a, Canvas b, int gap = 0)
{
    a.normalize();
    b.normalize();
    // align baselines: pad rows above/below so both baseRows sit on one line
    const int topA = a.baseRow;
    const int topB = b.baseRow;
    const int top = std::max(topA, topB);
    const int botA = int(a.rows.size()) - 1 - a.baseRow;
    const int botB = int(b.rows.size()) - 1 - b.baseRow;
    const int bot = std::max(botA, botB);
    const int rows = top + 1 + bot;

    Canvas out;
    out.rows.reserve(rows);
    for (int i = 0; i < rows; ++i)
        out.rows << QString();
    out.baseRow = top;

    const int wA = a.maxWidth();
    for (int i = 0; i < int(a.rows.size()); ++i)
        out.rows[top - topA + i] = a.rows.at(i);
    for (int i = 0; i < int(b.rows.size()); ++i) {
        const int row = top - topB + i;
        const int cur = Text::visibleWidth(out.rows.at(row));
        QString pad;
        if (cur < wA + gap)
            pad = QString(wA + gap - cur, QLatin1Char(' '));
        out.rows[row] = out.rows.at(row) + pad + b.rows.at(i); // append, not replace
    }
    return out;
}

// put `over` above and `under` below, centered on the operator glyph
Canvas withLimits(const QString& op, const QString& over, const QString& under)
{
    Canvas c;
    const int opW = Text::visibleWidth(op);
    const int overW = Text::visibleWidth(over);
    const int underW = Text::visibleWidth(under);
    const int w = std::max({ opW, overW, underW });
    auto center = [&](const QString& s, int width) {
        const int sw = Text::visibleWidth(s);
        const int left = (width - sw) / 2;
        return QString(left, QLatin1Char(' ')) + s;
    };
    if (!over.isEmpty())
        c.rows << center(over, w);
    c.rows << center(op, w);
    c.baseRow = c.rows.size() - 1;
    if (!under.isEmpty())
        c.rows << center(under, w);
    return c;
}

// ───────────────────────────── render nodes ─────────────────────────────

struct R {
    Canvas canvas;
    QString flat; // single-line equivalent for width measuring
};

QString renderNodesFlat(const QVector<Node>& nodes);

R renderFrac(const QVector<Node>& num, const QVector<Node>& den)
{
    const QString n = renderNodesFlat(num);
    const QString d = renderNodesFlat(den);
    Canvas c;
    const int w = std::max(Text::visibleWidth(n), Text::visibleWidth(d));
    const int nl = (w - Text::visibleWidth(n)) / 2;
    const int dl = (w - Text::visibleWidth(d)) / 2;
    c.rows << QString(nl, QLatin1Char(' ')) + n
           << QString(w, QChar(0x2500))
           << QString(dl, QLatin1Char(' ')) + d;
    c.baseRow = 1;
    R r;
    r.canvas = c;
    r.flat = QStringLiteral("(%1)/(%2)").arg(n, d);
    return r;
}

R renderSqrt(const QVector<Node>& body)
{
    const QString b = renderNodesFlat(body);
    Canvas c;
    c.rows << QStringLiteral("   ") + QString(Text::visibleWidth(b), QChar(0x0305))
           << QStringLiteral("√ ") + b;
    c.baseRow = 1;
    R r;
    r.canvas = c;
    r.flat = QStringLiteral("√(%1)").arg(b);
    return r;
}

R renderScripts(const QString& base, const QVector<Node>& sup, const QVector<Node>& sub)
{
    // flat form only; the multi-line stacked fallback lives in renderNodes.
    const QString s = renderNodesFlat(sup);
    const QString b = renderNodesFlat(sub);
    const QString us = s.isEmpty() ? QString() : superscript(s);
    const QString bs = b.isEmpty() ? QString() : subscript(b);
    const QString flatS = s.isEmpty() ? QString()
                                      : (us.isEmpty() ? QStringLiteral("^") + s : us);
    const QString flatB = b.isEmpty() ? QString()
                                      : (bs.isEmpty() ? QStringLiteral("_") + b : bs);
    R r;
    Canvas c;
    c.rows << base + flatS + flatB;
    c.baseRow = 0;
    r.canvas = c;
    r.flat = base + flatS + flatB;
    return r;
}

// big operators with limits are rendered via withLimits() directly

// forward
R renderNode(const Node& n, const QVector<Node>* prev, const QVector<Node>* next, int idx);

// matrix via \begin{matrix}...\end{matrix} is handled at the flat-string level
QString renderMatrix(const QString& body)
{
    // split rows by \\, cells by &
    const QStringList rows = body.split(QStringLiteral("\\\\"));
    QStringList out;
    int maxCols = 0;
    QVector<QStringList> cells;
    for (const QString& row : rows) {
        cells << row.split(QLatin1Char('&'));
        maxCols = std::max(maxCols, int(cells.last().size()));
    }
    QVector<int> widths(maxCols, 0);
    for (const QStringList& row : cells) {
        for (int i = 0; i < row.size(); ++i)
            widths[i] = std::max(widths[i], Text::visibleWidth(row.at(i).simplified()));
    }
    const int n = cells.size();
    for (int r = 0; r < n; ++r) {
        QString line = QStringLiteral("│ ");
        for (int i = 0; i < cells.at(r).size(); ++i) {
            const QString cell = cells.at(r).at(i).simplified();
            line += cell + QString(widths.at(i) - Text::visibleWidth(cell) + 1, QLatin1Char(' '));
        }
        line += QStringLiteral("│");
        out << line;
    }
    if (!out.isEmpty()) {
        const int w = Text::visibleWidth(out.first());
        out.prepend(QStringLiteral("┌") + QString(w - 2, QChar(0x2500)) + QStringLiteral("┐"));
        out.append(QStringLiteral("└") + QString(w - 2, QChar(0x2500)) + QStringLiteral("┘"));
    }
    return out.join(QLatin1Char('\n'));
}

QString renderNodesFlat(const QVector<Node>& nodes)
{
    QString out;
    const QHash<QString, Sym>& syms = commands();
    for (int i = 0; i < nodes.size(); ++i) {
        const Node& n = nodes.at(i);
        switch (n.kind) {
        case Node::Text:
            out += n.text;
            break;
        case Node::Cmd: {
            if (n.text == QStringLiteral("frac") || n.text == QStringLiteral("dfrac")
                || n.text == QStringLiteral("tfrac")) {
                // grab two following groups
                QVector<Node> a, b;
                if (i + 1 < nodes.size() && nodes.at(i + 1).kind == Node::Group) {
                    a = nodes.at(i + 1).children;
                    ++i;
                }
                if (i + 1 < nodes.size() && nodes.at(i + 1).kind == Node::Group) {
                    b = nodes.at(i + 1).children;
                    ++i;
                }
                const R r = renderFrac(a, b);
                out += r.flat;
                break;
            }
            if (n.text == QStringLiteral("sqrt")) {
                QVector<Node> a;
                if (i + 1 < nodes.size() && nodes.at(i + 1).kind == Node::Group) {
                    a = nodes.at(i + 1).children;
                    ++i;
                }
                out += renderSqrt(a).flat;
                break;
            }
            if (bigOps().contains(n.text)) {
                QString under, over;
                if (i + 1 < nodes.size()) {
                    const Node& nx = nodes.at(i + 1);
                    if (nx.kind == Node::Sub) {
                        under = renderNodesFlat(nx.children);
                        ++i;
                    } else if (nx.kind == Node::Sup) {
                        over = renderNodesFlat(nx.children);
                        ++i;
                    } else if (nx.kind == Node::SupSub) {
                        over = renderNodesFlat(nx.children.mid(0, 1));
                        under = nx.children.size() > 1 ? renderNodesFlat(nx.children.mid(1, 1))
                                                       : QString();
                        ++i;
                    }
                }
                if (i + 1 < nodes.size()) {
                    const Node& nx = nodes.at(i + 1);
                    if (over.isEmpty() && nx.kind == Node::Sup) {
                        over = renderNodesFlat(nx.children);
                        ++i;
                    } else if (under.isEmpty() && nx.kind == Node::Sub) {
                        under = renderNodesFlat(nx.children);
                        ++i;
                    }
                }
                const QString op = bigOps().value(n.text);
                out += op + (under.isEmpty() ? QString() : under)
                    + (over.isEmpty() ? QString() : over);
                break;
            }
            if (n.text == QStringLiteral("begin")) {
                // flat path: swallow until \end — handled by caller (matrix)
                break;
            }
            if (syms.contains(n.text)) {
                const Sym& s = syms.value(n.text);
                out += s.uni.isEmpty() ? s.ascii : s.uni;
                break;
            }
            out += QLatin1Char('\\') + n.text; // unknown → keep source
            break;
        }
        case Node::Group:
            out += renderNodesFlat(n.children);
            break;
        case Node::Sup:
            out += renderScripts(QString(), n.children, QVector<Node>()).flat;
            break;
        case Node::Sub:
            out += renderScripts(QString(), QVector<Node>(), n.children).flat;
            break;
        case Node::SupSub:
            out += renderScripts(QString(),
                                 n.children.isEmpty() ? QVector<Node>() : n.children.mid(0, 1),
                                 n.children.size() > 1 ? n.children.mid(1, 1) : QVector<Node>())
                       .flat;
            break;
        }
    }
    return out;
}

// full (multi-line) rendering
R renderNodes(const QVector<Node>& nodes)
{
    Canvas acc;
    acc.rows << QString();
    acc.baseRow = 0;
    QString flat;
    const QHash<QString, Sym>& syms = commands();

    auto appendFlat = [&](const QString& s) {
        Canvas c;
        c.rows << s;
        c.baseRow = 0;
        acc = hcat(acc, c);
        flat += s;
    };

    for (int i = 0; i < nodes.size(); ++i) {
        const Node& n = nodes.at(i);
        switch (n.kind) {
        case Node::Text:
            appendFlat(n.text);
            break;
        case Node::Cmd: {
            if (n.text == QStringLiteral("frac") || n.text == QStringLiteral("dfrac")
                || n.text == QStringLiteral("tfrac")) {
                QVector<Node> a, b;
                if (i + 1 < nodes.size() && nodes.at(i + 1).kind == Node::Group) {
                    a = nodes.at(i + 1).children;
                    ++i;
                }
                if (i + 1 < nodes.size() && nodes.at(i + 1).kind == Node::Group) {
                    b = nodes.at(i + 1).children;
                    ++i;
                }
                const R r = renderFrac(a, b);
                acc = hcat(acc, r.canvas, 1);
                flat += r.flat;
                break;
            }
            if (n.text == QStringLiteral("sqrt")) {
                QVector<Node> a;
                if (i + 1 < nodes.size() && nodes.at(i + 1).kind == Node::Group) {
                    a = nodes.at(i + 1).children;
                    ++i;
                }
                const R r = renderSqrt(a);
                acc = hcat(acc, r.canvas, 1);
                flat += r.flat;
                break;
            }
            if (bigOps().contains(n.text)) {
                QString under, over;
                if (i + 1 < nodes.size()) {
                    const Node& nx = nodes.at(i + 1);
                    if (nx.kind == Node::Sub) {
                        under = renderNodesFlat(nx.children);
                        ++i;
                    } else if (nx.kind == Node::Sup) {
                        over = renderNodesFlat(nx.children);
                        ++i;
                    } else if (nx.kind == Node::SupSub) {
                        over = renderNodesFlat(nx.children.mid(0, 1));
                        under = nx.children.size() > 1 ? renderNodesFlat(nx.children.mid(1, 1))
                                                       : QString();
                        ++i;
                    }
                }
                if (i + 1 < nodes.size()) {
                    const Node& nx = nodes.at(i + 1);
                    if (over.isEmpty() && nx.kind == Node::Sup) {
                        over = renderNodesFlat(nx.children);
                        ++i;
                    } else if (under.isEmpty() && nx.kind == Node::Sub) {
                        under = renderNodesFlat(nx.children);
                        ++i;
                    }
                }
                const QString op = bigOps().value(n.text);
                Canvas c = withLimits(op, over, under);
                acc = hcat(acc, c, 1);
                flat += op + (under.isEmpty() ? QString() : under)
                    + (over.isEmpty() ? QString() : over);
                break;
            }
            if (syms.contains(n.text)) {
                const Sym& s = syms.value(n.text);
                appendFlat(s.uni.isEmpty() ? s.ascii : s.uni);
                break;
            }
            appendFlat(QLatin1Char('\\') + n.text);
            break;
        }
        case Node::Group:
            appendFlat(renderNodesFlat(n.children));
            break;
        case Node::Sup:
        case Node::Sub: {
            // the accumulated canvas is the base; stack the script beside it
            const QString s = renderNodesFlat(n.children);
            QString u = n.kind == Node::Sup ? superscript(s) : subscript(s);
            if (u.isEmpty()) {
                Canvas c;
                if (n.kind == Node::Sup) {
                    c.rows << s << QString(); // sup above, baseline row empty
                    c.baseRow = 1;
                } else {
                    c.rows << QString() << s; // baseline row empty, sub below
                    c.baseRow = 0;
                }
                acc = hcat(acc, c, 0);
                flat += (n.kind == Node::Sup ? QStringLiteral("^") : QStringLiteral("_")) + s;
            } else {
                appendFlat(u);
            }
            break;
        }
        case Node::SupSub: {
            const QString sup = n.children.isEmpty() ? QString() : renderNodesFlat(n.children.mid(0, 1));
            const QString sub = n.children.size() > 1 ? renderNodesFlat(n.children.mid(1, 1)) : QString();
            const QString us = superscript(sup);
            const QString bs = subscript(sub);
            if (us.isEmpty() || bs.isEmpty()) {
                // stacked fallback: sup line over sub line beside the base
                Canvas c;
                c.rows << (us.isEmpty() ? sup : us) << (bs.isEmpty() ? sub : bs);
                c.baseRow = 1;
                acc = hcat(acc, c, 0);
                flat += QStringLiteral("^") + sup + QStringLiteral("_") + sub;
            } else {
                appendFlat(us + bs);
            }
            break;
        }
        }
    }
    R r;
    acc.normalize();
    r.canvas = acc;
    r.flat = flat;
    return r;
}

// extract the body of \begin{env}...\end{env} from raw latex
bool extractEnv(const QString& latex, const QString& env, QString* body, QString* rest)
{
    const QString beginTok = QStringLiteral("\\begin{") + env + QStringLiteral("}");
    const QString endTok = QStringLiteral("\\end{") + env + QStringLiteral("}");
    const int b = latex.indexOf(beginTok);
    if (b < 0)
        return false;
    const int e = latex.indexOf(endTok, b + beginTok.size());
    if (e < 0)
        return false;
    if (body)
        *body = latex.mid(b + beginTok.size(), e - b - beginTok.size());
    if (rest)
        *rest = latex.left(b) + latex.mid(e + endTok.size());
    return true;
}

} // namespace

QString render(const QString& latexIn, bool block)
{
    QString latex = latexIn.trimmed();
    if (latex.isEmpty())
        return QString();

    // matrix environments: special-cased on the raw string
    for (const char* env : { "matrix", "pmatrix", "bmatrix", "vmatrix", "cases" }) {
        QString body, rest;
        if (extractEnv(latex, QString::fromLatin1(env), &body, &rest)) {
            const QString rendered = renderMatrix(body);
            QString before = render(rest, block); // recursive for surrounding text
            if (block) {
                QStringList out;
                if (!before.trimmed().isEmpty())
                    out << before;
                out << rendered;
                return out.join(QLatin1Char('\n'));
            }
            return (before.trimmed().isEmpty() ? QString() : before + QLatin1Char(' ')) + rendered;
        }
    }

    QVector<Node> nodes;
    int end = 0;
    QString hit;
    lex(latex, 0, QString(), &nodes, &end, &hit);

    const R r = renderNodes(nodes);
    if (block) {
        // display style: keep multi-line canvas, no surrounding quotes
        return r.canvas.join();
    }
    // inline: collapse to the flat form if it fits one line
    if (r.canvas.rows.size() == 1)
        return r.flat.isEmpty() ? r.canvas.join() : r.flat;
    // inline but inherently multi-line (frac etc.): compact fraction form
    return r.flat;
}

} // namespace MathRender
