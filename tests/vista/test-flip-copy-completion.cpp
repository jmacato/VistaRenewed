#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstdio>
#include <initializer_list>
#include <mutex>
#include <thread>
using BOOLEAN = bool;
using NTSTATUS = int;
static std::mutex state;
static std::condition_variable cv;
static int attempts;
static bool copying, release_copy;
static void ExAcquireFastMutex(std::mutex *m) {
    {
        std::lock_guard<std::mutex> l(state);
        attempts++;
        cv.notify_all();
    }
    m->lock();
}
static void ExReleaseFastMutex(std::mutex *m) { m->unlock(); }
class VioGpuVidPN {
  public:
    std::mutex m_flipSubmitMutex;
    NTSTATUS m_lastFlipStatus = 0;
    bool armed = true;
    int outcome = 0;
    BOOLEAN TryPromoteFlip();
    NTSTATUS CompletePendingFlip();
    BOOLEAN TryPromoteFlipLocked() {
        if (!armed)
            return false;
        armed = false;
        std::unique_lock<std::mutex> l(state);
        copying = true;
        cv.notify_all();
        cv.wait(l, [] { return release_copy; });
        m_lastFlipStatus = outcome;
        return outcome == 0;
    }
};
/* SOURCE_UNDER_TEST */
int main() {
    for (int failure : {0, -5}) {
        VioGpuVidPN v;
        v.outcome = failure;
        attempts = 0;
        copying = false;
        release_copy = false;
        std::atomic<bool> returned = false;
        int result = 99;
        std::thread timer([&] { v.TryPromoteFlip(); });
        {
            std::unique_lock<std::mutex> l(state);
            cv.wait(l, [] { return copying; });
        }
        std::thread dma([&] {
            result = v.CompletePendingFlip();
            returned = true;
        });
        {
            std::unique_lock<std::mutex> l(state);
            cv.wait(l, [] { return attempts == 2; });
            assert(!returned);
            release_copy = true;
            cv.notify_all();
        }
        timer.join();
        dma.join();
        assert(returned && result == failure);
    }
    VioGpuVidPN v;
    release_copy = true;
    assert(v.CompletePendingFlip() == 0 && !v.armed);
    puts("PASS DMA waits for timer-owned copy; success/failure propagated; unclaimed "
         "flip copied before completion");
}
