#pragma once

#include <thread>
#include <map>
#include <chrono>
#include <functional>
#include <mutex>
#include <queue>
#include <future>
#include <vector>
#include <atomic>
#include <condition_variable>
#include <type_traits>

/*
 * PriorityTaskManager
 * ----------
 * A lightweight priority-based task manager with priority and name support.
 *
 * Unlike a traditional thread pool with fixed worker threads,
 * this implementation spawns a new thread per submitted task and
 * automatically cleans it up when the task finishes.
 *
 * Features:
 * - Submit tasks with a priority (lower value = higher priority).
 * - Returns std::future to retrieve results asynchronously.
 * - Provides job control methods: join(name), joinAll().
 * - getJobList() for monitoring currently running tasks.
 * - Automatic cleanup: finished jobs are removed immediately.
 *
 * Design notes:
 * - Thread-per-task model (not a worker pool).
 * - join(name): waits until no task with the given name remains.
 *   (New tasks submitted under the same name during waiting are also included.)
 * - joinAll(): waits until all submitted tasks finish.
 *
 * Compatibility:
 * - Supports C++11 (e.g. MFC environments) via std::result_of.
 * - Uses std::invoke_result automatically for C++17 and later.
 *
 * Limitations:
 * - Each task runs on its own thread. For very large numbers of tasks,
 *   consider a worker-based thread pool to reduce thread overhead.
 */
class PriorityTaskManager
{
    using pool_size_type = unsigned int;
    template <typename F, typename... Args>
    using job_result_of =
#if defined(_CXX17_DEPRECATE_RESULT_OF) || defined(__cpp_lib_is_invocable)
        typename std::invoke_result<F, Args...>::type;
#else
        typename std::result_of<F(Args...)>::type;
#endif


public:
    PriorityTaskManager(bool closeTaskWait = true, pool_size_type threadMaxCnt = 30, int waitInterval_ms = 1000)
        : max_thread_cnt(threadMaxCnt), wait_interval(waitInterval_ms), task_wait(closeTaskWait)
    {
        job_processing();
    }

    ~PriorityTaskManager()
    {
        shutdown();
    }

public:
    template <typename F, typename... Args>
    auto submit(int priority, const std::string& jobName, F&& f, Args&&... args)
        -> std::future<job_result_of<F, Args...>>
    {
        if (!run.load()) throw std::runtime_error("PriorityTaskManager stopped");
        using R = job_result_of<F, Args...>;

        auto task = std::make_shared<std::packaged_task<R()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...));

        auto fut = task->get_future();

        {
            std::unique_lock<std::mutex> lock(job_mtx);

            Job job;
            job.priority = priority;
            job.job_name = jobName;
            job.job = [task]() { (*task)(); };

            wait_job.push(std::move(job));
        }

        cv.notify_one();
        return fut;
    }

    void join(const std::string& jobName)
    {
        std::unique_lock<std::mutex> lock(job_mtx);
        close_notif.wait(lock, [&]
            {
                return work_result.find(jobName) == work_result.end();
            });
    }

    void joinAll()
    {
        std::unique_lock<std::mutex> lock(job_mtx);
        close_notif.wait(lock, [&] { return work_result.empty(); });
    }

public:
    void start()
    {
        if (!run && pool_process.empty())
        {
            run = true;
            job_processing();
        }
    }

    void shutdown()
    {
        if (!run) return;

        std::vector<std::thread> close;
        close.reserve(task_wait ? 2 : 1);
        run = false;

        cv.notify_all();
        if (task_wait)
        {
            close.emplace_back(&PriorityTaskManager::joinAll, this);
        }
        
        close.emplace_back(
        [this](){
            for (auto& t : pool_process)
            {
                if (t.joinable())
                    t.join();
            }

            pool_process.clear();
        });

        for (auto& t : close) if (t.joinable()) t.join();
    }

    pool_size_type getCurJobCount()
    {
        std::lock_guard<std::mutex> lock(job_mtx);
        return static_cast<pool_size_type>(work_result.size());
    }

    std::vector<std::string> getJobList()
    {
        std::lock_guard<std::mutex> lock(job_mtx);
        std::vector<std::string> job_list;
        job_list.reserve(work_result.size());

        for (auto& kv : work_result)
        {
            job_list.push_back(kv.first);
        }

        return job_list;
    }

    void setMaxThreadCnt(unsigned int threadMaxCnt)
    {
        this->max_thread_cnt = threadMaxCnt;
    }

    void setInterval(unsigned int interval)
    {
        this->wait_interval = interval;
    }

private:
    struct Job
    {
        std::function<void(void)> job;
        std::string job_name;
        int priority;

        bool operator<(const Job& other) const
        {
            return priority > other.priority; // If the priority is low, first
        }
    };

    std::vector<std::thread> pool_process;
    std::priority_queue<Job> wait_job;
    std::multimap<std::string, std::future<void>> work_result;

    std::mutex job_mtx;
    std::condition_variable cv;
    std::condition_variable close_notif;

    std::atomic<bool> run = true;
    std::atomic<unsigned int> max_thread_cnt;
    std::atomic<unsigned int> wait_interval;
    const bool task_wait;

    void self_clean(std::multimap<std::string, std::future<void>>::iterator it)
    {
        std::future<void> fut;
        {
            std::lock_guard<std::mutex> lock(job_mtx);
            fut = std::move(it->second);
            work_result.erase(it);

            close_notif.notify_all();
        }
        fut.wait();
    }


    /* add PriorityTaskManager jobs */
	void job_processing()
	{
		std::thread insert([&]() {
			while (run)
			{
				std::unique_lock<std::mutex> lock(job_mtx);
				cv.wait(lock, [&] { return !run || !wait_job.empty(); });

				if (!run) break;

                if (work_result.size() >= max_thread_cnt)
                {
                    cv.wait_for(lock,
                        std::chrono::milliseconds(wait_interval.load()),
                        [&] { return !run || work_result.size() < max_thread_cnt || !wait_job.empty(); });
                    continue;
                }

				Job cur = wait_job.top();
				wait_job.pop();
				lock.unlock();

				auto task = std::make_shared<std::packaged_task<void()>>(cur.job);
				auto fut = task->get_future();

				std::multimap<std::string, std::future<void>>::iterator it;
				{
					std::lock_guard<std::mutex> g(job_mtx);
					it = work_result.emplace(cur.job_name, std::move(fut));
				}

				std::thread([this, task, it]()
					{
                        (*task)();
                        this->self_clean(it); // task Clean up after performing task
					}).detach();
			}
			});

        pool_process.emplace_back(std::move(insert));
    }

    PriorityTaskManager(const PriorityTaskManager&) = delete;
    PriorityTaskManager& operator=(const PriorityTaskManager&) = delete;
};
