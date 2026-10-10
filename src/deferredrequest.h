#pragma once

#include <QDeadlineTimer>
#include <atomic>

// A queued GUI action must expire with the caller's wait, rather than perform
// a write after the caller has already reported that the operation failed.
class DeferredRequest {
public:
    explicit DeferredRequest(int timeoutMs) : deadline_(timeoutMs, Qt::PreciseTimer) {}
    bool active() const { return !cancelled_.load() && !deadline_.hasExpired(); }
    void cancel() { cancelled_.store(true); }
private:
    const QDeadlineTimer deadline_;
    std::atomic_bool cancelled_{false};
};
