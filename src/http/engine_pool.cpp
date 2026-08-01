#include "engine_pool.hpp"

#include "logging.hpp"

#include <stdexcept>

namespace lw::ppocr::http {
namespace {

std::string LastError(lw_ppocr_handle handle) {
    std::vector<char> buffer(4096, '\0');
    const uint64_t required = lw_ppocr_get_last_error(
        handle, buffer.data(), buffer.size());
    if (required == 0 || required > 1024u * 1024u) {
        return "unknown OCR error";
    }
    if (required > buffer.size()) {
        buffer.assign(static_cast<size_t>(required), '\0');
        lw_ppocr_get_last_error(handle, buffer.data(), buffer.size());
    }
    return std::string(buffer.data());
}

}  // namespace

EnginePool::Lease::Lease(EnginePool& owner, size_t index)
    : owner_(&owner), index_(index) {}

EnginePool::Lease::~Lease() {
    Reset();
}

EnginePool::Lease::Lease(Lease&& other) noexcept
    : owner_(other.owner_), index_(other.index_) {
    other.owner_ = nullptr;
}

EnginePool::Lease& EnginePool::Lease::operator=(Lease&& other) noexcept {
    if (this != &other) {
        Reset();
        owner_ = other.owner_;
        index_ = other.index_;
        other.owner_ = nullptr;
    }
    return *this;
}

lw_ppocr_handle EnginePool::Lease::get() const {
    return owner_ == nullptr ? nullptr : owner_->handles_[index_];
}

void EnginePool::Lease::Reset() noexcept {
    if (owner_ != nullptr) {
        owner_->Release(index_);
        owner_ = nullptr;
    }
}

EnginePool::EnginePool(const EngineSettings& settings) {
    model_manifest_ = settings.model_manifest;
    lw_ppocr_config config{};
    lw_ppocr_config_init(&config);
    config.model_manifest_utf8 = model_manifest_.c_str();
    config.enable_classifier = settings.enable_classifier ? 1 : 0;
    config.limit_side_len = settings.limit_side_len;
    config.det_db_threshold = settings.det_db_threshold;
    config.det_db_box_threshold = settings.det_db_box_threshold;
    config.det_db_unclip_ratio = settings.det_db_unclip_ratio;
    config.det_use_dilation = settings.det_use_dilation ? 1 : 0;
    config.cls_threshold = settings.cls_threshold;
    config.cls_batch_size = settings.cls_batch_size;
    config.rec_batch_size = settings.rec_batch_size;
    config.rec_concurrency = settings.rec_concurrency;
    config.max_image_pixels = settings.max_image_pixels;
    config.max_batch_images = settings.max_batch_images;
    config.max_batch_total_pixels = settings.max_batch_total_pixels;
    config.max_batch_decoded_bytes = settings.max_batch_decoded_bytes;
    config.log_level = LW_PPOCR_LOG_INFO;
    config.log_callback = &CoreLogBridge;

    try {
        for (size_t index = 0; index < settings.engine_instances; ++index) {
            lw_ppocr_handle handle = nullptr;
            const lw_ppocr_status status = lw_ppocr_create(&config, &handle);
            if (status != LW_PPOCR_STATUS_OK) {
                throw std::runtime_error(
                    "OCR initialization failed: " + LastError(handle));
            }
            handles_.push_back(handle);
            available_.push_back(index);
        }
    } catch (...) {
        Destroy();
        throw;
    }
}

EnginePool::~EnginePool() {
    Stop();
    Destroy();
}

EnginePool::AcquireResult EnginePool::AcquireFor(
    std::chrono::milliseconds timeout,
    size_t max_waiters) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (stopping_) return {AcquireStatus::Stopping, {}};
    if (!available_.empty()) {
        const size_t index = available_.back();
        available_.pop_back();
        return {AcquireStatus::Acquired, Lease(*this, index)};
    }
    if (waiting_ >= max_waiters) {
        return {AcquireStatus::QueueFull, {}};
    }

    ++waiting_;
    const bool awakened = ready_.wait_for(lock, timeout, [&] {
        return stopping_ || !available_.empty();
    });
    --waiting_;
    if (!awakened) return {AcquireStatus::TimedOut, {}};
    if (stopping_) return {AcquireStatus::Stopping, {}};
    const size_t index = available_.back();
    available_.pop_back();
    return {AcquireStatus::Acquired, Lease(*this, index)};
}

void EnginePool::Stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    ready_.notify_all();
}

void EnginePool::Release(size_t index) noexcept {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        available_.push_back(index);
    }
    ready_.notify_one();
}

void EnginePool::Destroy() noexcept {
    for (auto& handle : handles_) lw_ppocr_destroy(&handle);
    handles_.clear();
}

}  // namespace lw::ppocr::http
