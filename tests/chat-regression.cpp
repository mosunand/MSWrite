// Exercises the real chat document/delegate and math renderer without providers,
// saved settings, network requests, or the running editor.
#include "ai/AiMathPainter.h"
#include "ai/ChatView.h"
#include "fileservice.h"

#include <QApplication>
#include <QImage>
#include <QPainter>
#include <QStyleOptionViewItem>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextFragment>
#include <QTextStream>
#include <functional>
#include <stdexcept>

namespace {
QStringList warnings;

void check(bool ok, const QString &message)
{
    if (!ok)
        throw std::runtime_error(message.toStdString());
}

int imageCount(const QTextDocument &doc)
{
    int result = 0;
    for (auto block = doc.begin(); block.isValid(); block = block.next())
        for (auto it = block.begin(); !it.atEnd(); ++it)
            if (it.fragment().charFormat().isImageFormat())
                result += it.fragment().length();
    return result;
}

struct ChatFixture {
    ChatModel model;
    ChatBubbleDelegate delegate;
    QStyleOptionViewItem option;
    ChatMsg *message;

    ChatFixture()
    {
        option.rect = QRect(0, 0, 640, 100);
        model.append(ChatMsg::Assistant, QString());
        message = model.msgAt(0);
    }

    void render(const QString &text, bool finalized, bool light)
    {
        message->text = text;
        message->finalized = finalized;
        model.touch(0);
        delegate.setLightTheme(light);
        option.rect.setHeight(delegate.sizeHint(option, model.index(0)).height());
        check(message->doc != nullptr, QStringLiteral("Missing document"));
        QImage image(option.rect.size(), QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        painter.translate(2, 3);
        const auto transform = painter.transform();
        const auto pen = painter.pen();
        const auto brush = painter.brush();
        delegate.paint(&painter, option, model.index(0));
        check(painter.transform() == transform && painter.pen() == pen
                  && painter.brush() == brush,
              QStringLiteral("Delegate leaked painter state"));
    }
};
} // namespace

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    qInstallMessageHandler([](QtMsgType type, const QMessageLogContext &, const QString &message) {
        if (type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg)
            warnings.append(message);
    });
    QTextStream out(stdout);
    int passed = 0, failed = 0;
    auto test = [&](const char *name, const std::function<void()> &body) {
        warnings.clear();
        try {
            body();
            check(warnings.isEmpty(), warnings.join(QLatin1Char('\n')));
            ++passed;
            out << "PASS " << name << '\n';
        } catch (const std::exception &e) {
            ++failed;
            out << "FAIL " << name << ": " << e.what() << '\n';
        }
        out.flush();
    };

    struct MarkdownCase { const char *name; QString text; int images; QString literal; };
    const QList<MarkdownCase> markdownCases = {
        { "inline math", QStringLiteral("Before $x^2$ after"), 1, "after" },
        { "display math", QStringLiteral("Before\n\n$$\n\\frac{x^2}{y_1}\n$$\n\nAfter"), 1, "After" },
        { "backtick fence followed by math", QStringLiteral("```cpp\n$x$\n```\n\n$y$"), 1, "$x$" },
        { "tilde fence followed by math", QStringLiteral("~~~text\n$$x$$\n~~~\n\n$y$"), 1, "$$x$$" },
        { "long fence containing shorter fence", QStringLiteral("````\n```\n$x$\n````\n\n$y$"), 1, "$x$" },
        { "indented code", QStringLiteral("    $x$\n\n$y$"), 1, "$x$" },
        { "tab indented code", QStringLiteral("\t$x$\n\n$y$"), 1, "$x$" },
        { "inline code", QStringLiteral("`$x$` then $y$"), 1, "$x$" },
        { "double backtick code", QStringLiteral("``a`$x$`` then $y$"), 1, "a`$x$" },
        { "escaped dollars", QStringLiteral("\\$x\\$ then $y$"), 1, "$x$" },
        { "escaped dollar inside math", QStringLiteral("$x+\\$+y$"), 1, {} },
        { "incomplete inline math", QStringLiteral("Before $x^2"), 0, "$x^2" },
        { "incomplete display math", QStringLiteral("Before $$x^2$"), 0, "$$x^2$" },
        { "inline math cannot cross newline", QStringLiteral("Before $x\ny$ after"), 0, "$x" },
        { "unclosed code fence", QStringLiteral("```cpp\n$x$\n"), 0, "$x$" },
        { "CRLF fenced code", QStringLiteral("```cpp\r\n$x$\r\n```\r\n\r\n$y$"), 1, "$x$" },
    };
    for (const auto &c : markdownCases) {
        test(c.name, [&] {
            ChatFixture fixture;
            for (bool finalized : {false, true}) {
                for (bool light : {false, true}) {
                    fixture.render(c.text, finalized, light);
                    check(imageCount(*fixture.message->doc) == c.images,
                          QStringLiteral("Expected %1 images, got %2 (final=%3, light=%4): %5")
                              .arg(c.images).arg(imageCount(*fixture.message->doc))
                              .arg(finalized).arg(light).arg(fixture.message->doc->toPlainText()));
                    check(c.literal.isEmpty() || fixture.message->doc->toPlainText().contains(c.literal),
                          QStringLiteral("Literal code/text was changed"));
                }
            }
        });
    }
    test("every streaming prefix completes", [&] {
        ChatFixture fixture;
        const QString text = QStringLiteral("Before\n\n```cpp\nconst char *s = \"$x$\";\n```\n\n"
                                            "`$literal$` then $$\\sum_{i=1}^{n} i^2$$ after.");
        for (int i = 0; i <= text.size(); ++i) {
            fixture.delegate.setAnimationFrame(i % 4);
            fixture.render(text.left(i), false, false);
        }
    });
    test("user, notice and thinking preserve painter state", [&] {
        ChatFixture fixture;
        for (const auto kind : {ChatMsg::User, ChatMsg::Notice, ChatMsg::Thinking}) {
            fixture.message->kind = kind;
            fixture.render(QStringLiteral("Test message"), true, false);
        }
    });

    struct TitleCase { const char *name; QString markdown; QString expected; };
    const QList<TitleCase> titleCases = {
        { "bold first line", QStringLiteral("**AI生成多媒体内容安全与可信溯源**"),
          QStringLiteral("AI生成多媒体内容安全与可信溯源") },
        { "atx heading", QStringLiteral("# 项目周报\n\n正文段落"), QStringLiteral("项目周报") },
        { "deep heading", QStringLiteral("#### 四级标题\n正文"), QStringLiteral("四级标题") },
        { "quote and bullet", QStringLiteral("> - 带引用的列表\n\n正文"), QStringLiteral("带引用的列表") },
        { "ordered task", QStringLiteral("1. [x] 已完成任务"), QStringLiteral("已完成任务") },
        { "inline code prefix", QStringLiteral("`代码开头`的文档"), QStringLiteral("代码开头的文档") },
        { "inline math prefix", QStringLiteral("$E=mc^2$ 的推导"), QStringLiteral("E=mc^2 的推导") },
        { "display math delimiters ignored", QStringLiteral("$$\nE=mc^2\n$$\n\n公式后面的标题"),
          QStringLiteral("E=mc^2") },
        { "code fence skipped", QStringLiteral("```python\nprint('hi')\n```\n\n围栏后面的标题"),
          QStringLiteral("围栏后面的标题") },
        { "link text kept", QStringLiteral("[链接文字](https://example.com) 开头"),
          QStringLiteral("链接文字 开头") },
        { "image alt kept", QStringLiteral("![封面图](./assets/cover.png)\n\n封面说明"),
          QStringLiteral("封面图") },
        { "font tag stripped", QStringLiteral("<font color=\"#e74c3c\">红色开头的文档</font>"),
          QStringLiteral("红色开头的文档") },
        { "underline emphasis", QStringLiteral("__粗体下划线__开头"), QStringLiteral("粗体下划线开头") },
        { "illegal filename chars", QStringLiteral("报告: 2026/09/28 (第一版)"),
          QStringLiteral("报告 20260928 (第一版)") },
        { "blank lines skipped", QStringLiteral("\n\n**第二行才有效**"), QStringLiteral("第二行才有效") },
        { "empty document", QString(), QString() },
        { "closing heading markers", QStringLiteral("## **周报** ###"), QStringLiteral("周报") },
        { "marker-only lines", QStringLiteral("---\n###\n***\n\n**正文标题**"), QStringLiteral("正文标题") },
        { "display delimiters stripped", QStringLiteral("$$AI生成多媒体内容安全与可信溯源$$"),
          QStringLiteral("AI生成多媒体内容安全与可信溯源") },
        { "front matter and comment", QStringLiteral("---\ntitle: metadata\n---\n<!-- hidden\ncomment -->\n# 正文标题"), QStringLiteral("正文标题") },
        { "reserved device name", QStringLiteral("**CON.txt**"), QStringLiteral("_CON.txt") },
        { "bare carriage return", QStringLiteral("\r\r# 首行\r正文"), QStringLiteral("首行") },
        { "reference link", QStringLiteral("[项目文档][ref]\n\n[ref]: https://example.com"), QStringLiteral("项目文档") },
    };
    for (const auto &c : titleCases) {
        test(c.name, [&] {
            const QString got = FileService::titleFromMarkdown(c.markdown);
            check(got == c.expected, QStringLiteral("expected \"%1\" got \"%2\"")
                                         .arg(c.expected, got));
        });
    }
    test("sanitize keeps unique-name safety", [&] {
        check(FileService::sanitizeFileName(QStringLiteral("a/b\\c:d*e?f\"g<h>i|j"))
                  == QStringLiteral("abcdefghij"), QStringLiteral("illegal characters removed"));
        const QString collapsed = FileService::sanitizeFileName(
            QStringLiteral("  多  个 空白\t制表  "));
        check(collapsed == QStringLiteral("多 个 空白 制表"),
              QStringLiteral("whitespace collapsed: got \"%1\"").arg(collapsed));
        check(FileService::sanitizeFileName(QStringLiteral("尾部点..."))
                  == QStringLiteral("尾部点"), QStringLiteral("trailing dots removed"));
        const QString emoji = QString(59, QLatin1Char('a')) + QString::fromUtf8("😀");
        const QString shortName = FileService::sanitizeFileName(emoji);
        check(!shortName.back().isHighSurrogate(), QStringLiteral("emoji is not split on truncation"));
        check(FileService::sanitizeFileName(QStringLiteral("NUL")) == QStringLiteral("_NUL"),
              QStringLiteral("Windows device name is safe"));
    });

    const QStringList formulas = {
        QStringLiteral("x^2+1"), QStringLiteral("\\frac{x^2}{y_1}"),
        QStringLiteral("\\sqrt{a^2+b^2}"), QStringLiteral("\\sum_{i=1}^{n} i^2"),
        QStringLiteral("\\lim_{x\\to0} \\frac{\\sin x}{x}"),
        QStringLiteral("\\text{upright} + \\mathrm{sin}(x)"),
        QStringLiteral("x_{a_i}^{b^2} + \\frac{1}{\\sqrt{1+x^2}}"),
        QStringLiteral("\\int_{0}^{1} x^2 \\mathrm{d}x"),
        QStringLiteral("\\vec{F}=m\\vec{a}")
    };
    QImage sheet(1000, formulas.size() * 150, QImage::Format_RGB32);
    sheet.fill(Qt::white);
    QPainter sheetPainter(&sheet);
    int row = 0;
    for (const auto &latex : formulas) {
        const QByteArray name = (QStringLiteral("render ") + latex).toUtf8();
        test(name.constData(), [&] {
            const QPixmap pm = AiMathPainter::render(latex, Qt::black);
            check(!pm.isNull(), QStringLiteral("Null formula image"));
            const QImage image = pm.toImage();
            QRect ink;
            for (int y = 0; y < image.height(); ++y)
                for (int x = 0; x < image.width(); ++x)
                    if (qAlpha(image.pixel(x, y)) > 0)
                        ink |= QRect(x, y, 1, 1);
            check(!ink.isEmpty(), QStringLiteral("Empty formula"));
            check(ink.left() > 0 && ink.top() > 0 && ink.right() < image.width() - 1
                      && ink.bottom() < image.height() - 1,
                  QStringLiteral("Formula ink reaches image edge"));
            sheetPainter.setPen(Qt::darkGray);
            sheetPainter.drawText(20, row * 150 + 22, latex);
            sheetPainter.drawPixmap(20, row * 150 + 40, pm);
        });
        ++row;
    }
    sheetPainter.end();
    test("braces preserve nested script rendering", [&] {
        for (const QString &latex : {QStringLiteral("x^2"), QStringLiteral("x_a"),
                                      QStringLiteral("x_{a_i}^{b^2}")}) {
            check(AiMathPainter::render(latex, Qt::black).toImage()
                      == AiMathPainter::render(QLatin1Char('{') + latex + QLatin1Char('}'), Qt::black).toImage(),
                  QStringLiteral("Braced scripts differ from unbraced scripts"));
        }
    });
    test("math whitespace preserves formula layout", [&] {
        const QList<QPair<QString, QString>> cases = {
            { QStringLiteral("x^2"), QStringLiteral("\n x ^ 2 \r\n") },
            { QStringLiteral("\\frac{x^2}{y_1}"), QStringLiteral("\n\\frac {x ^2}\n {y_1}\n") },
            { QStringLiteral("\\sum_{i=1}^{n}i^2"), QStringLiteral("\\sum _{i=1} ^ {n} i^2") },
            { QStringLiteral("\\vec{F}"), QStringLiteral("\\vec F") }
        };
        for (const auto &pair : cases)
            check(AiMathPainter::render(pair.first, Qt::black).toImage()
                      == AiMathPainter::render(pair.second, Qt::black).toImage(),
                  QStringLiteral("Whitespace changes layout: ") + pair.second);
    });
    test("command scripts preserve the command argument", [&] {
        check(AiMathPainter::render(QStringLiteral("x^\\mathrm{T}"), Qt::black).toImage()
                  == AiMathPainter::render(QStringLiteral("x^{\\mathrm{T}}"), Qt::black).toImage(),
              QStringLiteral("Unbraced command script loses its argument"));
    });
    if (app.arguments().size() > 1)
        check(sheet.save(app.arguments().at(1)), QStringLiteral("Failed to save render sheet"));
    out << passed << " chat regression checks passed; " << failed << " failed.\n";
    return failed == 0 ? 0 : 1;
}
