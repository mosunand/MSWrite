// Exercises the real chat document/delegate and math renderer without providers,
// saved settings, network requests, or the running editor.
#include "ai/AiMathPainter.h"
#include "ai/ChatView.h"

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
