#pragma once
#include <mutex>
#include <condition_variable>

namespace d3dwaits
{
    inline std::atomic<uint32_t> fence_page{0};
    class BinarySemaphore {
    private:
        std::mutex mtx;
        std::condition_variable cv;
        bool signaled = false;

    public:
        void release() {
            {
                std::lock_guard<std::mutex> lock(mtx);
                signaled = true;
            }
            cv.notify_one();
        }

        // Returns true if signaled, false if timed out
        bool acquire_with_timeout(std::chrono::milliseconds timeout) {
            std::unique_lock<std::mutex> lock(mtx);
            
            // Wait for the flag to be true OR for the timeout to expire
            bool success = cv.wait_for(lock, timeout, [this] { return signaled; });
            
            if (success) {
                signaled = false; // Reset ONLY if we actually got a real signal
            }
            return success;
        }
    };

    // Declare the semaphore as extern so other files can see it
    extern BinarySemaphore waiting_semaphore;
    
    void Block();
}
