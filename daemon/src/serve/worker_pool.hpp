// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// serve/worker_pool.hpp -- the threads that do the work the loop must not block on.
//
// A lock acquire can wait seconds for a peer on another node to release. The main loop must keep
// answering meanwhile, so it hands that wait to a worker here and reads the next request. C++ has no
// GIL, so these run in parallel: several acquires and several policy evals proceed at once.

#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace fsdaemon::serve
{

class WorkerPool
{
public:
    explicit WorkerPool(std::uint32_t count);

    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    // Stops taking work and joins. A task already running finishes, which is what cme's own acquire
    // deadline bounds.
    ~WorkerPool();

    // Hands @task to a worker. Runs on some pool thread, in no guaranteed order against other tasks.
    void submit(std::function<void()> task);

private:
    void runWorker();

    std::vector<std::thread> threads_;
    std::queue<std::function<void()>> tasks_;
    std::mutex mutex_;
    std::condition_variable ready_;
    bool stopping_{false};
};

}  // namespace fsdaemon::serve
