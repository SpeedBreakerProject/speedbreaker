#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace gpu {
    class Wakeup {
    public:
        uint32_t Snapshot() const { return epoch_.load(std::memory_order_acquire); }
        void Wake() {
            { std::lock_guard<std::mutex> l(m_); epoch_.fetch_add(1, std::memory_order_release); }
            cv_.notify_all();
        }
        bool Wait(uint32_t seen, std::chrono::nanoseconds timeout) {   // true: woken; false: timed out
            std::unique_lock<std::mutex> l(m_);
            return cv_.wait_for(l, timeout, [&] { return epoch_.load(std::memory_order_acquire) != seen; });
        }
    private:
        std::mutex m_; std::condition_variable cv_; std::atomic<uint32_t> epoch_{ 0 };
    };
    inline Wakeup& VblankWakeup()   { static Wakeup w; return w; }   // Sleep until Vblank
    inline Wakeup& D3DWakeup()      { static Wakeup w; return w; }   // Synchronize between CPU and CP (D3DWaits and PM4_EVENT_WRITE_SHD)
    inline Wakeup& ProgressWakeup() { static Wakeup w; return w; }   // Guest thread Yield (KeDelayExecution 0 timeout)
    inline Wakeup& WriteWatchWakeup() { static Wakeup w; return w; } // Write Watch wakeup
}