// SPDX-License-Identifier: Apache-2.0
// Copyright XCENA Inc.
//
// serve/worker_pool.cpp -- see worker_pool.hpp.

#include "serve/worker_pool.hpp"

#include <cstdint>
#include <functional>
#include <mutex>
#include <utility>

namespace fsdaemon::serve
{

WorkerPool::WorkerPool(std::uint32_t count)
{
    threads_.reserve(count);
    for (std::uint32_t started = 0; started < count; ++started)
    {
        threads_.emplace_back([this]
                              {
                                  runWorker();
                              });
    }
}

WorkerPool::~WorkerPool()
{
    {
        const std::lock_guard guard{mutex_};
        stopping_ = true;
    }
    ready_.notify_all();
    for (auto& worker : threads_)
    {
        worker.join();
    }
}

void WorkerPool::submit(std::function<void()> task)
{
    {
        const std::lock_guard guard{mutex_};
        tasks_.push(std::move(task));
    }
    ready_.notify_one();
}

void WorkerPool::runWorker()
{
    for (;;)
    {
        std::function<void()> task;
        {
            std::unique_lock guard{mutex_};
            ready_.wait(guard, [this]
                        {
                            return stopping_ || !tasks_.empty();
                        });
            if (stopping_ && tasks_.empty())
            {
                return;
            }
            task = std::move(tasks_.front());
            tasks_.pop();
        }
        task();
    }
}

}  // namespace fsdaemon::serve
