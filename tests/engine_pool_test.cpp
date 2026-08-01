#include "engine_pool.hpp"

#include <chrono>
#include <future>
#include <string>
#include <thread>

using namespace std::chrono_literals;

int main(int argc, char** argv) {
    if (argc != 2) return 1;
    lw::ppocr::http::EngineSettings settings;
    settings.model_manifest = argv[1];
    settings.engine_instances = 1;
    lw::ppocr::http::EnginePool pool(settings);

    auto held = pool.AcquireFor(1s, 1);
    if (held.status != lw::ppocr::http::EnginePool::AcquireStatus::Acquired) {
        return 2;
    }
    const auto timed_out = pool.AcquireFor(1ms, 1);
    if (timed_out.status !=
        lw::ppocr::http::EnginePool::AcquireStatus::TimedOut) {
        return 3;
    }

    auto waiter = std::async(std::launch::async, [&pool] {
        auto acquired = pool.AcquireFor(5s, 1);
        return acquired.status;
    });
    std::this_thread::sleep_for(25ms);
    const auto full = pool.AcquireFor(1ms, 1);
    if (full.status !=
        lw::ppocr::http::EnginePool::AcquireStatus::QueueFull) {
        return 4;
    }
    held.lease = lw::ppocr::http::EnginePool::Lease{};
    if (waiter.get() !=
        lw::ppocr::http::EnginePool::AcquireStatus::Acquired) {
        return 5;
    }

    auto held_for_stop = pool.AcquireFor(1s, 1);
    if (held_for_stop.status !=
        lw::ppocr::http::EnginePool::AcquireStatus::Acquired) {
        return 6;
    }
    auto stopping_waiter = std::async(std::launch::async, [&pool] {
        auto acquired = pool.AcquireFor(5s, 1);
        return acquired.status;
    });
    std::this_thread::sleep_for(25ms);
    pool.Stop();
    if (stopping_waiter.get() !=
        lw::ppocr::http::EnginePool::AcquireStatus::Stopping) {
        return 7;
    }
    held_for_stop.lease = lw::ppocr::http::EnginePool::Lease{};
    const auto stopped = pool.AcquireFor(1ms, 1);
    if (stopped.status !=
        lw::ppocr::http::EnginePool::AcquireStatus::Stopping) {
        return 8;
    }
    return 0;
}
