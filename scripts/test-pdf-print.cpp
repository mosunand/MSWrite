#include <QtWidgets>
#include <windows.h>
#include <functional>
#include <iostream>

// Exercise the real request coordinator without starting WebView2, printing
// files, or using the user's settings. COM replies and lost replies are driven
// through the same private completion path used by PrintToPdfHandler.
#define private public
#include "webviewhost.h"
#undef private

namespace {
int checks = 0, failures = 0;
void check(bool ok, const char *name)
{
    ++checks;
    if (!ok) ++failures;
    std::cout << (ok ? "PASS " : "FAIL ") << name << '\n';
}
void pump(int milliseconds = 10)
{
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}
}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    WebViewHost host;
    int callbacks = 0;
    bool lastOk = false;
    HRESULT lastError = S_OK;
    auto done = [&](bool ok, HRESULT error) { ++callbacks; lastOk = ok; lastError = error; };

    const auto first = host.beginPdfRequest(done);
    check(first != 0 && host.m_printPending && host.m_printWatchdog.isActive(),
          "a print dispatch starts its completion watchdog");
    host.finishPdfRequest(first, true, S_OK);
    check(callbacks == 0 && !host.m_printPending && !host.m_printWatchdog.isActive(),
          "completion consumes the request before leaving the COM callback");
    host.finishPdfRequest(first, false, E_FAIL);
    pump();
    check(callbacks == 1 && lastOk && lastError == S_OK,
          "duplicate print acknowledgments cannot deliver twice");

    const auto second = host.beginPdfRequest(done);
    const auto duplicate = host.beginPdfRequest(done);
    check(duplicate == 0 && host.m_printRequest == second && host.m_printPending,
          "a concurrent print cannot replace the active request");
    pump();
    check(callbacks == 2 && !lastOk && lastError == HRESULT_FROM_WIN32(ERROR_BUSY),
          "a concurrent print receives a queued busy failure");
    host.finishPdfRequest(first, true, S_OK);
    check(host.m_printPending, "a previous request cannot complete the next print");
    host.finishPdfRequest(second, false, E_FAIL);
    pump();
    check(callbacks == 3 && !lastOk && lastError == E_FAIL,
          "synchronous COM dispatch errors release the print request");

    const auto lost = host.beginPdfRequest(done);
    const auto oldGeneration = host.m_printGeneration;
    host.m_printWatchdog.start(20);
    pump(60);
    check(callbacks == 4 && !lastOk && lastError == HRESULT_FROM_WIN32(ERROR_TIMEOUT),
          "a missing COM acknowledgment eventually delivers a failure");
    check(!host.m_printPending && !host.m_printWatchdog.isActive(),
          "a timed out print no longer blocks future exports");
    const auto recovered = host.beginPdfRequest(done);
    check(host.m_printGeneration != oldGeneration,
          "timeout closes the old controller before accepting another print");
    host.finishPdfRequest(lost, true, S_OK);
    check(host.m_printPending && callbacks == 4,
          "late acknowledgment from a timed out controller is ignored");
    host.finishPdfRequest(recovered, true, S_OK);
    pump();
    check(callbacks == 5 && lastOk, "printing can complete after timeout recovery");

    const auto cancelled = host.beginPdfRequest(done);
    host.cancelPdfRequest(E_ABORT);
    host.finishPdfRequest(cancelled, true, S_OK);
    pump();
    check(callbacks == 6 && !lastOk && lastError == E_ABORT && !host.m_printPending,
          "controller shutdown cancels a pending request once");

    host.printToPdf(QStringLiteral("unused.pdf"), done);
    check(callbacks == 6 && !host.m_printPending,
          "a missing runtime fails without executing user callbacks inline");
    pump();
    check(callbacks == 7 && !lastOk && lastError == E_FAIL,
          "a missing runtime reports failure and keeps export available");

    host.beginPdfRequest({});
    host.m_printWatchdog.start(20);
    pump(60);
    check(!host.m_printPending && !host.m_printWatchdog.isActive(),
          "a print without a user callback still times out safely");

    {
        auto dying = new WebViewHost;
        dying->beginPdfRequest(done);
        delete dying;
    }
    pump();
    check(callbacks == 7, "destruction never calls back into a closing owner");
    host.m_closing = true;
    check(host.beginPdfRequest(done) == 0, "closing hosts reject new print requests");
    pump();
    check(callbacks == 7, "closing hosts cannot receive deferred print callbacks");

    std::cout << checks << " PDF print checks; " << failures << " failures.\n";
    return failures ? 1 : 0;
}
