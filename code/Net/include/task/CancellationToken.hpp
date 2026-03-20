#pragma once

#include <atomic>

namespace net {

class CancellationToken {
public:
    void Cancel() { cancelled_.store(true, std::memory_order_release); }
    bool IsCancelled() const { return cancelled_.load(std::memory_order_acquire); }
    void Reset() { cancelled_.store(false, std::memory_order_release); }

private:
    std::atomic<bool> cancelled_{false};
};

} // namespace net
