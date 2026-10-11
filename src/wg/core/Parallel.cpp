#include "wg/core/Parallel.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace wg {

namespace {

// Workers sleep until a job is posted, then take chunks from a shared counter.
// One job at a time: concurrent callers (e.g. the viewer's generation thread
// and the app) are serialized by jobMutex_.
class Pool {
public:
    Pool() {
        const int workers = std::max(1, static_cast<int>(std::thread::hardware_concurrency()) - 1);
        for (int t = 0; t < workers; ++t)
            threads_.emplace_back([this] { work(); });
    }
    ~Pool() {
        {
            std::lock_guard lock(mutex_);
            stop_ = true;
        }
        wake_.notify_all();
        for (auto& thread : threads_)
            thread.join();
    }

    int size() const { return static_cast<int>(threads_.size()) + 1; }

    void run(int chunks, const std::function<void(int)>& chunk) {
        std::lock_guard job(jobMutex_);
        {
            // No worker may still be inside the previous job: it would take
            // indices of this one while they are being reset.
            std::unique_lock lock(mutex_);
            done_.wait(lock, [&] { return busy_ == 0; });
            chunk_ = &chunk;
            chunks_ = chunks;
            next_ = 0;
            pending_ = chunks;
            ++generation_;
        }
        wake_.notify_all();
        inPool = true;
        drain(); // the caller works too
        inPool = false;
        std::unique_lock lock(mutex_);
        done_.wait(lock, [&] { return pending_ == 0 && busy_ == 0; });
        chunk_ = nullptr;
    }

    static thread_local bool inPool;

private:
    void drain() {
        while (true) {
            const int k = next_.fetch_add(1);
            if (k >= chunks_)
                return;
            (*chunk_)(k);
            if (pending_.fetch_sub(1) == 1) {
                std::lock_guard lock(mutex_);
                done_.notify_all();
            }
        }
    }

    void work() {
        inPool = true;
        unsigned seen = 0;
        while (true) {
            {
                std::unique_lock lock(mutex_);
                wake_.wait(lock, [&] { return stop_ || generation_ != seen; });
                if (stop_)
                    return;
                seen = generation_;
                ++busy_;
            }
            drain();
            {
                std::lock_guard lock(mutex_);
                if (--busy_ == 0)
                    done_.notify_all();
            }
        }
    }

    std::vector<std::thread> threads_;
    std::mutex jobMutex_, mutex_;
    std::condition_variable wake_, done_;
    const std::function<void(int)>* chunk_ = nullptr;
    std::atomic<int> chunks_{0};
    std::atomic<int> next_{0}, pending_{0};
    unsigned generation_ = 0;
    int busy_ = 0; // workers inside drain()
    bool stop_ = false;
};

thread_local bool Pool::inPool = false;

Pool& pool() {
    static Pool instance;
    return instance;
}

} // namespace

void parallelFor(int count, const std::function<void(int, int)>& fn, int minChunk) {
    if (count <= 0)
        return;
    if (count < 2 * minChunk || Pool::inPool) {
        fn(0, count);
        return;
    }
    Pool& p = pool();
    // A few chunks per thread, for balance.
    const int chunks = std::clamp(count / minChunk, 1, 4 * p.size());
    const int size = (count + chunks - 1) / chunks;
    p.run(chunks, [&](int k) {
        const int begin = k * size, end = std::min(count, begin + size);
        if (begin < end)
            fn(begin, end);
    });
}

} // namespace wg
