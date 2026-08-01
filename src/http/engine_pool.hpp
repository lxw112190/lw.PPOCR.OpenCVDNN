#pragma once

#include <lw/ppocr.h>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace lw::ppocr::http {

struct EngineSettings {
    std::string model_manifest;
    bool enable_classifier = true;
    int limit_side_len = 960;
    float det_db_threshold = 0.3f;
    float det_db_box_threshold = 0.6f;
    float det_db_unclip_ratio = 1.6f;
    bool det_use_dilation = false;
    float cls_threshold = 0.9f;
    int cls_batch_size = 8;
    int rec_batch_size = 8;
    int rec_concurrency = 1;
    uint64_t max_image_pixels = 40000000;
    uint32_t max_batch_images = 32;
    uint64_t max_batch_total_pixels = 40000000;
    uint64_t max_batch_decoded_bytes = 120000000;
    size_t engine_instances = 1;
};

class EnginePool {
public:
    enum class AcquireStatus {
        Acquired,
        QueueFull,
        TimedOut,
        Stopping,
    };

    class Lease {
    public:
        Lease() = default;
        ~Lease();
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        Lease(Lease&& other) noexcept;
        Lease& operator=(Lease&& other) noexcept;

        lw_ppocr_handle get() const;
        explicit operator bool() const { return owner_ != nullptr; }

    private:
        friend class EnginePool;
        Lease(EnginePool& owner, size_t index);
        void Reset() noexcept;

        EnginePool* owner_ = nullptr;
        size_t index_ = 0;
    };

    struct AcquireResult {
        AcquireStatus status = AcquireStatus::Stopping;
        Lease lease;
    };

    explicit EnginePool(const EngineSettings& settings);
    ~EnginePool();
    EnginePool(const EnginePool&) = delete;
    EnginePool& operator=(const EnginePool&) = delete;

    AcquireResult AcquireFor(
        std::chrono::milliseconds timeout,
        size_t max_waiters);
    void Stop();

private:
    void Release(size_t index) noexcept;
    void Destroy() noexcept;

    std::string model_manifest_;
    std::vector<lw_ppocr_handle> handles_;
    std::vector<size_t> available_;
    std::mutex mutex_;
    std::condition_variable ready_;
    size_t waiting_ = 0;
    bool stopping_ = false;
};

}  // namespace lw::ppocr::http
