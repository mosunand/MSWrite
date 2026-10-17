#include <QtWidgets>
#include <QJsonDocument>
#include <QJsonArray>
#include <QTemporaryDir>
#include <windows.h>
#include <functional>
#include <iostream>
#include <stdexcept>

// Exercise the real chat coordinator without loading WebView2 or touching the
// user's settings. Only the browser transport below is replaced.
#define private public
#include "ai/ChatWebView.h"
#undef private
#include "ai/ChatView.h"

namespace {
int checks = 0;
int failures = 0;
int retries = 0;
int recreations = 0;
QList<QJsonObject> updates;
QList<std::function<void(const QString &)>> delayedReplies;
QString nextReply = QStringLiteral("true");
bool delayReply = false;

void check(bool value, const char *name)
{
    ++checks;
    if (!value) ++failures;
    std::cout << (value ? "PASS " : "FAIL ") << name << '\n';
}

void pump(int milliseconds)
{
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}

void acknowledge(ChatWebView &view, int sequence)
{
    emit view.message(QJsonObject{{QStringLiteral("t"), QStringLiteral("chatRendered")},
                                 {QStringLiteral("sequence"), sequence}});
    pump(10);
}

int latestSequence() { return updates.constLast().value(QStringLiteral("sequence")).toInt(); }
}

WebViewHost::WebViewHost(QWidget *parent) : QWidget(parent) {}
WebViewHost::~WebViewHost() { m_closing = true; }
void WebViewHost::setAlwaysVisible(bool) {}
void WebViewHost::setLoadingTheme(const QString &) {}
void WebViewHost::showLoading(const QString &, const QString &)
{
    if (!m_loading) m_loading = new QWidget(this);
}
void WebViewHost::showLoadingError(const QString &) { showLoading({}, {}); }
void WebViewHost::finishLoading()
{
    delete m_loading;
    m_loading = nullptr;
}
void WebViewHost::start(const QString &, const QString &, const QString &, const QString &, const QString &)
{
    m_pageReady = true;
}
void WebViewHost::runScript(const QString &) {}
void WebViewHost::evalWithResult(const QString &script, std::function<void(const QString &)> done)
{
    const QString begin = QStringLiteral("window.chatView.update(");
    const qsizetype start = script.indexOf(begin);
    if (start < 0) { done(QStringLiteral("true")); return; }
    const qsizetype end = script.lastIndexOf(QStringLiteral(");return true;"));
    const QByteArray json = script.mid(start + begin.size(), end - start - begin.size()).toUtf8();
    updates.append(QJsonDocument::fromJson(json).object());
    const QString reply = nextReply;
    nextReply = QStringLiteral("true");
    if (delayReply) {
        delayedReplies.append(std::move(done));
        delayReply = false;
    } else done(reply);
}
QString WebViewHost::environmentError() { return {}; }
void WebViewHost::retryLoading()
{
    ++retries;
    m_pageReady = false;
    emit loadingRetry();
}
void WebViewHost::recreateBrowser()
{
    ++recreations;
    m_pageReady = false;
    emit loadingRetry();
}
bool WebViewHost::event(QEvent *event) { return QWidget::event(event); }
void WebViewHost::resizeEvent(QResizeEvent *event) { QWidget::resizeEvent(event); }
void WebViewHost::showEvent(QShowEvent *event) { QWidget::showEvent(event); }
void WebViewHost::hideEvent(QHideEvent *event) { QWidget::hideEvent(event); }
void WebViewHost::mousePressEvent(QMouseEvent *event) { QWidget::mousePressEvent(event); }

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QTemporaryDir isolated;
    if (!isolated.isValid()) return 1;
    QCoreApplication::setOrganizationName(QStringLiteral("MswriteChatSyncTest"));
    QCoreApplication::setApplicationName(QStringLiteral("isolated"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, isolated.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, isolated.path());

    ChatModel model;
    model.append(ChatMsg::Assistant, QStringLiteral("first"));
    ChatWebView view(&model);
    view.show();
    emit view.message(QJsonObject{{QStringLiteral("t"), QStringLiteral("chatReady")}});
    pump(110);
    check(updates.size() == 1, "first ready event sends one complete update");
    check(view.m_renderPending && view.m_watchdog.isActive(),
          "first update arms the render watchdog even without further messages");
    check(updates.constLast().value(QStringLiteral("reset")).toBool(),
          "first update contains the complete model");
    acknowledge(view, latestSequence());
    check(view.m_rendered && !view.m_renderPending && !view.m_watchdog.isActive() && !view.isLoading(),
          "matching render acknowledgment removes loading and disarms watchdog");

    const int beforeFailure = updates.size();
    nextReply = QStringLiteral("false");
    model.append(ChatMsg::Assistant, QStringLiteral("new message after stable render"));
    pump(550);
    check(updates.size() >= beforeFailure + 2,
          "failed update on a previously rendered page is automatically retried");
    check(updates.constLast().value(QStringLiteral("reset")).toBool()
          && updates.constLast().value(QStringLiteral("rows")).toArray().size() == 2,
          "retry resends all model rows instead of losing the new message");
    acknowledge(view, latestSequence());

    const int beforeModeFailure = updates.size();
    nextReply = QString();
    view.setMathMode(QStringLiteral("mathjax"));
    pump(550);
    check(updates.size() >= beforeModeFailure + 2,
          "mode switch retries a transient empty browser execution result");
    check(updates.constLast().value(QStringLiteral("mathMode")).toString() == QStringLiteral("mathjax"),
          "mode retry carries the most recent selected mode");
    acknowledge(view, latestSequence());

    delayReply = true;
    model.append(ChatMsg::Assistant, QStringLiteral("older delayed update"));
    pump(100);
    const int oldSequence = latestSequence();
    model.append(ChatMsg::Assistant, QStringLiteral("latest update"));
    pump(100);
    const int currentSequence = latestSequence();
    check(currentSequence > oldSequence, "a newer model update receives a new sequence");
    acknowledge(view, oldSequence);
    check(view.m_renderPending && view.m_watchdog.isActive(),
          "stale acknowledgment cannot finish the latest render");
    acknowledge(view, currentSequence);
    delayedReplies.takeFirst()(QStringLiteral("false"));
    pump(400);
    check(view.m_rendered && !view.m_renderPending && !view.m_reset,
          "late failed execution reply cannot reset a newer successful render");

    const int beforeHidden = updates.size();
    view.hide();
    view.setMathMode(QStringLiteral("markdown"));
    model.append(ChatMsg::Assistant, QStringLiteral("while hidden"));
    pump(120);
    check(updates.size() == beforeHidden, "hidden page preserves pending changes without dispatching");
    view.show();
    pump(120);
    check(updates.size() == beforeHidden + 1
          && updates.constLast().value(QStringLiteral("reset")).toBool()
          && updates.constLast().value(QStringLiteral("rows")).toArray().size() == model.rowCount()
          && updates.constLast().value(QStringLiteral("mathMode")).toString() == QStringLiteral("markdown"),
          "showing the page resends complete current state and the latest mode");
    acknowledge(view, latestSequence());

    model.append(ChatMsg::Assistant, QStringLiteral("render acknowledgment never arrives"));
    pump(100);
    check(view.m_watchdog.isActive(), "a later update also arms the watchdog");
    view.m_watchdog.setInterval(15);
    pump(90);
    check(retries == 1 && !view.m_ready && !view.m_renderPending,
          "missing render acknowledgment triggers recovery rather than permanent loading");
    check(recreations == 0, "first missed acknowledgment uses bounded reload before recreation");

    std::cout << checks << " chat synchronization checks, " << failures << " failures.\n";
    return failures == 0 ? 0 : 1;
}
