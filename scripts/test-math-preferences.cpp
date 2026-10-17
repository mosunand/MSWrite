#include "uidialogs.h"
#include "ai/MswriteSkill.h"
#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QProcess>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <exception>
#include <functional>
#include <iostream>
#include <stdexcept>

namespace {
int checks = 0;
void check(bool value, const char *name)
{
    if (!value) throw std::runtime_error(name);
    ++checks;
    std::cout << "PASS " << name << '\n';
}

bool preferences(const QString &theme, const std::function<void(QDialog *, QCheckBox *)> &edit)
{
    std::exception_ptr failure;
    QTimer::singleShot(0, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!dialog) {
            failure = std::make_exception_ptr(std::runtime_error("preferences modal missing"));
            QApplication::exit(1);
            return;
        }
        try {
            auto *mode = dialog->findChild<QCheckBox *>(QStringLiteral("mathModeCheck"));
            check(mode != nullptr, "actual preferences dialog contains MathJax control");
            edit(dialog, mode);
        } catch (...) {
            failure = std::current_exception();
            dialog->reject();
        }
    });
    const bool accepted = UiDialogs::showPrefs(nullptr, theme);
    if (failure) std::rethrow_exception(failure);
    return accepted;
}

QString prompt(const QJsonValue &mode)
{
    const QJsonObject context{{QStringLiteral("id"), QStringLiteral("test-document")},
                              {QStringLiteral("mathMode"), mode}};
    return MswriteSkill::withContext(
        QString::fromUtf8(QJsonDocument(context).toJson(QJsonDocument::Compact)), 0, {});
}
}

int main(int argc, char **argv)
{
    // The test never reads/writes the application's registry settings.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    if (app.arguments().size() == 4 && app.arguments().at(1) == QStringLiteral("--verify-settings")) {
        QSettings saved(app.arguments().at(2), QSettings::IniFormat);
        return saved.status() == QSettings::NoError
            && saved.value(QStringLiteral("mathMode")).toString() == app.arguments().at(3)
            && saved.value(QStringLiteral("preferLatex")).toBool() ? 0 : 1;
    }
    QTemporaryDir isolated;
    if (!isolated.isValid()) return 1;
    QCoreApplication::setOrganizationName(QStringLiteral("MswriteMathPreferencesTest"));
    QCoreApplication::setApplicationName(QStringLiteral("isolated"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, isolated.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, isolated.path());
    try {
        QSettings settings;
        settings.setValue(QStringLiteral("lineHeight"), 1.25);
        settings.setValue(QStringLiteral("motto"), QStringLiteral("原座右铭"));
        settings.setValue(QStringLiteral("preferLatex"), true);
        check(!settings.contains(QStringLiteral("mathMode")), "fresh settings have no mode override");
        check(!preferences(QStringLiteral("light"), [&](QDialog *dialog, QCheckBox *mode) {
            check(!mode->isChecked(), "missing mode defaults to traditional formulas");
            mode->setChecked(true);
            dialog->findChild<QDoubleSpinBox *>()->setValue(1.8);
            dialog->findChild<QLineEdit *>(QStringLiteral("mottoEdit"))->setText(QStringLiteral("不应保存"));
            check(!QSettings().contains(QStringLiteral("mathMode")), "checkbox edit does not save before confirmation");
            dialog->reject();
        }), "cancel returns false");
        check(!QSettings().contains(QStringLiteral("mathMode")), "cancel preserves missing mode setting");
        check(QSettings().value(QStringLiteral("lineHeight")).toDouble() == 1.25
              && QSettings().value(QStringLiteral("motto")).toString() == QStringLiteral("原座右铭"),
              "cancel preserves other preferences");

        check(preferences(QStringLiteral("dark"), [&](QDialog *dialog, QCheckBox *mode) {
            mode->setChecked(true);
            dialog->accept();
        }), "confirm accepts MathJax mode");
        settings.sync();
        check(QSettings().value(QStringLiteral("mathMode")).toString() == QStringLiteral("mathjax"),
              "MathJax confirmation persists mode for new settings readers");
        check(QSettings().value(QStringLiteral("preferLatex")).toBool(),
              "MathJax mode preserves inactive legacy preference");
        QProcess restarted;
        restarted.start(QCoreApplication::applicationFilePath(), {
            QStringLiteral("--verify-settings"), settings.fileName(), QStringLiteral("mathjax")});
        check(restarted.waitForFinished(5000) && restarted.exitStatus() == QProcess::NormalExit
              && restarted.exitCode() == 0, "new process restores persisted MathJax and legacy preference");
        check(!preferences(QStringLiteral("light"), [&](QDialog *dialog, QCheckBox *mode) {
            check(mode->isChecked(), "reopened preferences restore MathJax selection");
            mode->setChecked(false);
            dialog->reject();
        }), "cancel mode switch returns false");
        check(QSettings().value(QStringLiteral("mathMode")).toString() == QStringLiteral("mathjax"),
              "canceled mode switch retains MathJax");
        check(preferences(QStringLiteral("light"), [&](QDialog *dialog, QCheckBox *mode) {
            mode->setChecked(false);
            dialog->accept();
        }), "confirm accepts traditional mode");
        check(QSettings().value(QStringLiteral("mathMode")).toString() == QStringLiteral("markdown"),
              "traditional confirmation persists normalized mode");

        settings.setValue(QStringLiteral("mathMode"), QStringLiteral("unsupported"));
        check(preferences(QStringLiteral("paper"), [&](QDialog *dialog, QCheckBox *mode) {
            check(!mode->isChecked(), "invalid saved mode falls back to traditional formulas");
            dialog->accept();
        }), "confirm accepts invalid-mode fallback");
        check(QSettings().value(QStringLiteral("mathMode")).toString() == QStringLiteral("markdown"),
              "confirmed invalid setting is normalized");
        settings.sync();
        check(settings.status() == QSettings::NoError, "isolated preferences persisted without errors");

        const auto mathjax = prompt(QStringLiteral("mathjax"));
        check(mathjax.contains(QStringLiteral("# Active math mode: MathJax"))
              && mathjax.contains(QStringLiteral("Use only \\(...\\) for inline formulas and \\[...\\]")),
              "MathJax AI instructions use only active backslash delimiters");
        check(mathjax.contains(QStringLiteral("dollar delimiters $...$ and $$...$$ are disabled"))
              && !mathjax.contains(QStringLiteral("Use only $...$ for inline formulas")),
              "MathJax AI instructions disable traditional delimiters without conflicting examples");
        const auto traditional = prompt(QStringLiteral("markdown"));
        check(traditional.contains(QStringLiteral("Use only $...$ for inline formulas"))
              && traditional.contains(QStringLiteral("MathJax delimiters \\(...\\) and \\[...\\] are disabled")),
              "traditional AI instructions explicitly disable MathJax delimiters");
        for (const QJsonValue &invalid : {QJsonValue(), QJsonValue(true), QJsonValue(123),
                                          QJsonValue(QStringLiteral("unsupported"))}) {
            check(prompt(invalid).contains(QStringLiteral("# Active math mode: traditional Markdown / LaTeX")),
                  "invalid AI context mode safely falls back to traditional formulas");
        }
        check(MswriteSkill::withContext(QStringLiteral("not JSON"), 0, {})
              .contains(QStringLiteral("# Active math mode: traditional Markdown / LaTeX")),
              "malformed AI context safely falls back");
        std::cout << checks << " native math preference checks passed.\n";
    } catch (const std::exception &error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
    return 0;
}
