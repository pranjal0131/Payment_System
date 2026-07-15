#pragma once
//
// ThreadPool.hpp — a fixed-size pool of worker threads that execute queued tasks.
//
// LLD lesson: spawning a std::thread per request is expensive (kernel thread
// creation + context-switch overhead) and unbounded (a spike creates thousands
// of threads and thrashes). A thread pool creates N workers ONCE and feeds them
// tasks from a shared queue. This caps concurrency and amortizes thread cost —
// the standard way to handle parallel work in a service.
//
// HLD note: in a payment system the pool would run things like asynchronous
// settlement, webhook delivery, and fraud-scoring — work that can happen off the
// caller's critical path. The synchronous debit/credit stays on the request
// thread; slow I/O is handed to the pool.
//
// submit() returns a std::future<R> so the caller can retrieve the result (or a
// propagated exception) later — this is the Active Object / task-based
// concurrency model, not raw thread management.
//
#include <functional>
#include <future>
#include <memory>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "payment/infra/BlockingQueue.hpp"

namespace pay::infra {

class ThreadPool {
public:
    // Default to hardware concurrency; never fewer than one worker.
    explicit ThreadPool(unsigned workers = std::thread::hardware_concurrency())
        : tasks_(/*capacity*/ 1024) {
        if (workers == 0) workers = 1;
        workers_.reserve(workers);
        for (unsigned i = 0; i < workers; ++i)
            workers_.emplace_back([this] { workerLoop(); });
    }

    // Non-copyable: owns threads and a queue.
    ThreadPool(const ThreadPool&)            = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // Graceful shutdown: stop accepting work, drain the queue, join all workers.
    ~ThreadPool() { shutdown(); }

    // Submit a callable with any signature; get back a future for its result.
    //
    //   auto fut = pool.submit([](int a, int b){ return a + b; }, 2, 3);
    //   int sum = fut.get();   // 5
    //
    // We package the call into a std::packaged_task so exceptions thrown inside
    // the task are captured and re-thrown at future.get() — no crash on a
    // worker thread, which would otherwise call std::terminate.
    template <typename F, typename... Args>
    auto submit(F&& f, Args&&... args)
        -> std::future<std::invoke_result_t<F, Args...>> {
        using R = std::invoke_result_t<F, Args...>;

        // Bind args now; the task becomes a zero-arg callable the worker can run.
        auto bound = std::bind(std::forward<F>(f), std::forward<Args>(args)...);
        auto task  = std::make_shared<std::packaged_task<R()>>(std::move(bound));
        std::future<R> fut = task->get_future();

        // Type-erase into std::function<void()> so the queue holds one type.
        tasks_.push([task] { (*task)(); });
        return fut;
    }

    // Stop the pool: close the queue (workers drain remaining tasks) and join.
    // Idempotent and called automatically by the destructor.
    void shutdown() {
        if (stopped_.exchange(true)) return;
        tasks_.close();
        for (auto& t : workers_)
            if (t.joinable()) t.join();
    }

    std::size_t pending() const { return tasks_.size(); }
    std::size_t size()    const { return workers_.size(); }

private:
    void workerLoop() {
        // pop() returns nullopt once the queue is closed AND empty -> exit.
        while (auto task = tasks_.pop()) {
            (*task)();   // packaged_task swallows exceptions into the future
        }
    }

    BlockingQueue<std::function<void()>> tasks_;
    std::vector<std::thread>             workers_;
    std::atomic<bool>                    stopped_{false};
};

} // namespace pay::infra
