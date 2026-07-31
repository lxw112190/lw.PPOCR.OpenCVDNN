#include "base64.hpp"
#include "logging.hpp"

#include <httplib.h>
#include <lw/ppocr.h>
#include <json.hpp>
#include <spdlog/logger.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <limits.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

namespace {

constexpr const char* kProduct = LW_PPOCR_PRODUCT_NAME;
constexpr const char* kVersion = LW_PPOCR_VERSION_STRING;
constexpr const char* kAuthor = "天天代码码天天";
constexpr const char* kQq = "819069052";
constexpr const char* kProjectUrl =
    "https://github.com/lxw112190/lw.PPOCR.OpenCVDNN";

struct ServiceConfig {
    std::string listen_host = "127.0.0.1";
    int port = 8787;
    fs::path model_manifest;
    fs::path web_root;
    std::string api_key;
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
    size_t engine_instances = 1;
    size_t worker_threads = 4;
    size_t max_request_bytes = 20u * 1024u * 1024u;
    uint64_t max_image_pixels = 40000000;
    lw::ppocr::http::LoggingConfig logging;
};

std::atomic<httplib::Server*> g_server{nullptr};
std::atomic<uint64_t> g_request_sequence{0};
fs::path g_config_path;

#if defined(_WIN32)
constexpr const wchar_t* kWindowsServiceName = L"lw.PPOCR.OpenCVDNN";
SERVICE_STATUS_HANDLE g_service_status_handle = nullptr;
SERVICE_STATUS g_service_status{};
std::atomic<bool> g_service_stop_requested{false};
DWORD g_service_checkpoint = 1;
#endif

fs::path ExecutablePath() {
#if defined(_WIN32)
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(
        nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        throw std::runtime_error("unable to determine executable path");
    }
    buffer.resize(length);
    return fs::path(buffer);
#else
    std::vector<char> buffer(PATH_MAX + 1u, '\0');
    const ssize_t length = readlink("/proc/self/exe", buffer.data(), PATH_MAX);
    if (length <= 0 || length >= PATH_MAX) {
        throw std::runtime_error("unable to determine executable path");
    }
    return fs::u8path(std::string(buffer.data(), static_cast<size_t>(length)));
#endif
}

fs::path ResolvePath(const fs::path& base, const std::string& value) {
    fs::path path = fs::u8path(value);
    if (path.is_relative()) {
        path = base / path;
    }
    return fs::weakly_canonical(path);
}

ServiceConfig LoadConfig(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error(
            "unable to open configuration: " + path.u8string());
    }
    json document;
    input >> document;
    if (!document.is_object()) {
        throw std::runtime_error("HTTP service configuration must be an object");
    }

    const fs::path base = fs::absolute(path).parent_path();
    ServiceConfig config;
    config.listen_host = document.value("listen_host", config.listen_host);
    config.port = document.value("port", config.port);
    config.model_manifest = ResolvePath(base, document.value(
        "model_manifest", "models/ppocrv6-tiny/model.json"));
    config.web_root = ResolvePath(base, document.value("web_root", "www"));
    config.api_key = document.value("api_key", std::string{});
    config.enable_classifier = document.value(
        "enable_classifier", config.enable_classifier);
    config.limit_side_len = document.value(
        "limit_side_len", config.limit_side_len);
    config.det_db_threshold = document.value(
        "det_db_threshold", config.det_db_threshold);
    config.det_db_box_threshold = document.value(
        "det_db_box_threshold", config.det_db_box_threshold);
    config.det_db_unclip_ratio = document.value(
        "det_db_unclip_ratio", config.det_db_unclip_ratio);
    config.det_use_dilation = document.value(
        "det_use_dilation", config.det_use_dilation);
    config.cls_threshold = document.value(
        "cls_threshold", config.cls_threshold);
    config.cls_batch_size = document.value(
        "cls_batch_size", config.cls_batch_size);
    config.rec_batch_size = document.value(
        "rec_batch_size", config.rec_batch_size);
    config.rec_concurrency = document.value(
        "rec_concurrency", config.rec_concurrency);
    config.engine_instances = document.value(
        "engine_instances", config.engine_instances);
    config.worker_threads = document.value(
        "worker_threads", config.worker_threads);
    config.max_request_bytes = document.value(
        "max_request_bytes", config.max_request_bytes);
    config.max_image_pixels = document.value(
        "max_image_pixels", config.max_image_pixels);

    if (document.contains("logging")) {
        const json& logging = document.at("logging");
        config.logging.enabled = logging.value("enabled", config.logging.enabled);
        config.logging.level = logging.value("level", config.logging.level);
        config.logging.console = logging.value("console", config.logging.console);
        config.logging.file_enabled = logging.value(
            "file_enabled", config.logging.file_enabled);
        config.logging.request_enabled = logging.value(
            "request_enabled", config.logging.request_enabled);
        config.logging.max_files = logging.value(
            "max_files", config.logging.max_files);
        const size_t max_mb = logging.value("max_file_size_mb", size_t{10});
        config.logging.max_file_size = max_mb * 1024u * 1024u;
        const std::string log_path = logging.value(
            "file_path", config.logging.file_path);
        config.logging.file_path = ResolvePath(base, log_path).u8string();
    }

    if (config.port < 1 || config.port > 65535 ||
        config.worker_threads < 1 || config.worker_threads > 128 ||
        config.engine_instances < 1 || config.engine_instances > 32 ||
        config.limit_side_len < 32 || config.cls_batch_size < 1 ||
        config.rec_batch_size < 1 || config.rec_concurrency < 1 ||
        config.max_request_bytes < 1024 ||
        config.max_request_bytes > 256u * 1024u * 1024u ||
        config.max_image_pixels < 1 || config.max_image_pixels > 200000000u) {
        throw std::runtime_error("configuration contains an out-of-range value");
    }
    if (!fs::is_regular_file(config.model_manifest)) {
        throw std::runtime_error("model manifest does not exist: " +
            config.model_manifest.u8string());
    }
    if (!fs::is_directory(config.web_root)) {
        throw std::runtime_error(
            "web_root does not exist: " + config.web_root.u8string());
    }
    return config;
}

std::string LastError(lw_ppocr_handle handle) {
    const uint64_t required = lw_ppocr_get_last_error(handle, nullptr, 0);
    if (required == 0 || required > 1024u * 1024u) {
        return "unknown OCR error";
    }
    std::vector<char> buffer(static_cast<size_t>(required));
    lw_ppocr_get_last_error(handle, buffer.data(), required);
    return std::string(buffer.data());
}

class EnginePool {
public:
    class Lease {
    public:
        Lease(EnginePool& owner, size_t index)
            : owner_(&owner), index_(index) {}
        ~Lease() { if (owner_ != nullptr) owner_->Release(index_); }
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        lw_ppocr_handle get() const { return owner_->handles_[index_]; }
    private:
        EnginePool* owner_;
        size_t index_;
    };

    explicit EnginePool(const ServiceConfig& settings) {
        model_manifest_ = settings.model_manifest.u8string();
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
        config.log_level = LW_PPOCR_LOG_INFO;
        config.log_callback = &lw::ppocr::http::CoreLogBridge;

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

    ~EnginePool() { Destroy(); }
    EnginePool(const EnginePool&) = delete;
    EnginePool& operator=(const EnginePool&) = delete;

    Lease Acquire() {
        std::unique_lock<std::mutex> lock(mutex_);
        ready_.wait(lock, [&] { return !available_.empty(); });
        const size_t index = available_.back();
        available_.pop_back();
        return Lease(*this, index);
    }

private:
    void Release(size_t index) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            available_.push_back(index);
        }
        ready_.notify_one();
    }

    void Destroy() {
        for (auto& handle : handles_) {
            lw_ppocr_destroy(&handle);
        }
        handles_.clear();
    }

    std::string model_manifest_;
    std::vector<lw_ppocr_handle> handles_;
    std::vector<size_t> available_;
    std::mutex mutex_;
    std::condition_variable ready_;
};

void SetJson(httplib::Response& response, const json& value, int status = 200) {
    response.status = status;
    response.set_header("Cache-Control", "no-store");
    response.set_content(value.dump(), "application/json; charset=utf-8");
}

bool ConstantTimeEquals(const std::string& left, const std::string& right) {
    size_t difference = left.size() ^ right.size();
    const size_t count = (std::max)(left.size(), right.size());
    for (size_t index = 0; index < count; ++index) {
        const unsigned char a = index < left.size() ? left[index] : 0;
        const unsigned char b = index < right.size() ? right[index] : 0;
        difference |= a ^ b;
    }
    return difference == 0;
}

bool Authorized(const ServiceConfig& config, const httplib::Request& request,
                httplib::Response& response, const std::string& request_id) {
    if (config.api_key.empty() || ConstantTimeEquals(
            request.get_header_value("X-API-Key"), config.api_key)) {
        return true;
    }
    SetJson(response, {{"ok", false}, {"request_id", request_id},
        {"error", "invalid API key"}}, 401);
    return false;
}

std::string NewRequestId() {
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::ostringstream value;
    value << std::hex << micros << '-' << ++g_request_sequence;
    return value.str();
}

double Milliseconds(Clock::time_point start, Clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

void LogRequest(const httplib::Request& request, int status,
                const std::string& request_id, Clock::time_point start,
                size_t result_count = 0) {
    if (!lw::ppocr::http::RequestLoggingEnabled()) return;
    if (const auto logger = lw::ppocr::http::Logger()) {
        logger->info("request_id={} remote={} method={} path={} bytes={} "
                     "status={} total_ms={:.2f} results={}",
            request_id, request.remote_addr, request.method, request.path,
            request.body.size(), status, Milliseconds(start, Clock::now()),
            result_count);
    }
}

void LogRequestStart(const httplib::Request& request,
                     const std::string& request_id) {
    if (!lw::ppocr::http::RequestLoggingEnabled()) return;
    if (const auto logger = lw::ppocr::http::Logger()) {
        logger->info("request_started request_id={} remote={} method={} "
                     "path={} bytes={}",
            request_id, request.remote_addr, request.method, request.path,
            request.body.size());
    }
}

[[noreturn]] void TerminateHandler() noexcept {
    try {
        if (const auto logger = lw::ppocr::http::Logger()) {
            try {
                const std::exception_ptr error = std::current_exception();
                if (error != nullptr) std::rethrow_exception(error);
                logger->critical("std::terminate invoked without an active exception");
            } catch (const std::exception& exception) {
                logger->critical("std::terminate invoked: {}", exception.what());
            } catch (...) {
                logger->critical("std::terminate invoked by an unknown exception");
            }
            logger->flush();
        }
    } catch (...) {
    }
    std::abort();
}

#if defined(_WIN32)
LONG WINAPI UnhandledExceptionHandler(EXCEPTION_POINTERS* pointers) {
    try {
        if (const auto logger = lw::ppocr::http::Logger()) {
            const DWORD code = pointers != nullptr &&
                pointers->ExceptionRecord != nullptr
                ? pointers->ExceptionRecord->ExceptionCode : 0;
            const void* address = pointers != nullptr &&
                pointers->ExceptionRecord != nullptr
                ? pointers->ExceptionRecord->ExceptionAddress : nullptr;
            logger->critical(
                "unhandled Windows exception code=0x{:08X} address={}",
                static_cast<unsigned long>(code), address);
            logger->flush();
        }
    } catch (...) {
    }
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

json CallOcr(lw_ppocr_handle handle, const std::vector<uint8_t>& encoded,
             bool recognition_only) {
    char* output = nullptr;
    uint64_t length = 0;
    const lw_ppocr_status status = recognition_only
        ? lw_ppocr_recognize_encoded(handle, encoded.data(), encoded.size(),
            &output, &length)
        : lw_ppocr_ocr_encoded(handle, encoded.data(), encoded.size(),
            &output, &length);
    if (status != LW_PPOCR_STATUS_OK) {
        const std::string error = LastError(handle);
        if (status == LW_PPOCR_STATUS_INVALID_ARGUMENT ||
            status == LW_PPOCR_STATUS_IMAGE_ERROR) {
            throw std::invalid_argument(error);
        }
        throw std::runtime_error(error);
    }
    try {
        json result = json::parse(output, output + length);
        lw_ppocr_string_free(output);
        return result;
    } catch (...) {
        lw_ppocr_string_free(output);
        throw;
    }
}

json CallRecognizeBatch(lw_ppocr_handle handle,
                        const std::vector<std::vector<uint8_t>>& images) {
    std::vector<const uint8_t*> pointers;
    std::vector<uint64_t> sizes;
    pointers.reserve(images.size());
    sizes.reserve(images.size());
    for (const auto& image : images) {
        pointers.push_back(image.data());
        sizes.push_back(image.size());
    }
    char* output = nullptr;
    uint64_t length = 0;
    const lw_ppocr_status status = lw_ppocr_recognize_batch_encoded(handle,
        pointers.data(), sizes.data(), images.size(), &output, &length);
    if (status != LW_PPOCR_STATUS_OK) {
        const std::string error = LastError(handle);
        if (status == LW_PPOCR_STATUS_INVALID_ARGUMENT ||
            status == LW_PPOCR_STATUS_IMAGE_ERROR) {
            throw std::invalid_argument(error);
        }
        throw std::runtime_error(error);
    }
    try {
        json result = json::parse(output, output + length);
        lw_ppocr_string_free(output);
        return result;
    } catch (...) {
        lw_ppocr_string_free(output);
        throw;
    }
}

void PrintStartupInfo(const ServiceConfig& config) {
    std::cout
        << "============================================================\n"
        << kProduct << " HTTP Service v" << kVersion << '\n'
        << "Author / 作者: " << kAuthor << '\n'
        << "QQ: " << kQq << '\n'
        << "Project / 项目: " << kProjectUrl << '\n'
        << "============================================================\n"
        << "Startup parameters / 启动参数\n"
        << "  config_file: " << g_config_path.u8string() << '\n'
        << "  listen_host: " << config.listen_host << '\n'
        << "  port: " << config.port << '\n'
        << "  backend: OpenCV DNN (CPU)\n"
        << "  model_manifest: " << config.model_manifest.u8string() << '\n'
        << "  web_root: " << config.web_root.u8string() << '\n'
        << "  enable_classifier: " << std::boolalpha
        << config.enable_classifier << '\n'
        << "  limit_side_len: " << config.limit_side_len << '\n'
        << "  det_db_threshold: " << config.det_db_threshold << '\n'
        << "  det_db_box_threshold: " << config.det_db_box_threshold << '\n'
        << "  det_db_unclip_ratio: " << config.det_db_unclip_ratio << '\n'
        << "  det_use_dilation: " << config.det_use_dilation << '\n'
        << "  cls_threshold: " << config.cls_threshold << '\n'
        << "  cls_batch_size: " << config.cls_batch_size << '\n'
        << "  rec_batch_size: " << config.rec_batch_size << '\n'
        << "  rec_concurrency: " << config.rec_concurrency << '\n'
        << "  engine_instances: " << config.engine_instances << '\n'
        << "  worker_threads: " << config.worker_threads << '\n'
        << "  max_request_bytes: " << config.max_request_bytes << '\n'
        << "  max_image_pixels: " << config.max_image_pixels << '\n'
        << "  api_key: " << (config.api_key.empty()
            ? "disabled / 未启用"
            : "configured / 已配置 (value hidden / 明文已隐藏)") << '\n'
        << "  logging.enabled: " << config.logging.enabled << '\n'
        << "  logging.level: " << config.logging.level << '\n'
        << "  logging.file_enabled: " << config.logging.file_enabled << '\n'
        << "  logging.file_path: " << config.logging.file_path << '\n'
        << "  logging.request_enabled: "
        << config.logging.request_enabled << '\n'
        << "Request parameters / 请求参数\n"
        << "  POST /api/ocr: {\"image_base64\":\"...\"}\n"
        << "  POST /api/recognize: {\"image_base64\":\"...\"}\n"
        << "  POST /api/recognize batch: {\"images_base64\":[\"...\"]}\n"
        << "  authentication: X-API-Key header when api_key is configured\n"
        << "============================================================\n"
        << std::flush;
}

void HandleApi(const ServiceConfig& config, EnginePool& engines,
               const httplib::Request& request, httplib::Response& response,
               bool recognition_only) {
    const auto start = Clock::now();
    const std::string request_id = NewRequestId();
    LogRequestStart(request, request_id);
    if (!Authorized(config, request, response, request_id)) {
        LogRequest(request, response.status, request_id, start);
        return;
    }
    try {
        const json body = json::parse(request.body);
        json native;
        if (!body.is_object()) {
            throw std::invalid_argument("JSON request must be an object");
        }

        if (recognition_only && body.contains("images_base64")) {
            const json& values = body.at("images_base64");
            if (!values.is_array() || values.empty() || values.size() > 256) {
                throw std::invalid_argument(
                    "images_base64 must contain 1 to 256 strings");
            }
            std::vector<std::vector<uint8_t>> images(values.size());
            std::string error;
            for (size_t index = 0; index < values.size(); ++index) {
                if (!values[index].is_string() ||
                    !lw::ppocr::http::DecodeBase64(values[index].get_ref<
                        const std::string&>(), config.max_request_bytes,
                        images[index], error)) {
                    throw std::invalid_argument("image " +
                        std::to_string(index) + ": " +
                        (error.empty() ? "Base64 string is required" : error));
                }
            }
            auto lease = engines.Acquire();
            native = CallRecognizeBatch(lease.get(), images);
            native["image_count"] = images.size();
        } else {
            if (!body.contains("image_base64") ||
                !body.at("image_base64").is_string()) {
                throw std::invalid_argument("image_base64 string is required");
            }
            std::vector<uint8_t> encoded;
            std::string error;
            if (!lw::ppocr::http::DecodeBase64(
                    body.at("image_base64").get_ref<const std::string&>(),
                    config.max_request_bytes, encoded, error)) {
                throw std::invalid_argument(error);
            }
            auto lease = engines.Acquire();
            native = CallOcr(lease.get(), encoded, recognition_only);
        }
        native["ok"] = true;
        native["request_id"] = request_id;
        native["backend"] = "opencv-dnn";
        native["timing"]["server_total_ms"] =
            Milliseconds(start, Clock::now());
        const size_t count = native.value("result", json::array()).size();
        SetJson(response, native);
        LogRequest(request, response.status, request_id, start, count);
    } catch (const json::exception& exception) {
        SetJson(response, {{"ok", false}, {"request_id", request_id},
            {"error", std::string("invalid JSON request: ") +
                exception.what()}}, 400);
        LogRequest(request, response.status, request_id, start);
    } catch (const std::invalid_argument& exception) {
        SetJson(response, {{"ok", false}, {"request_id", request_id},
            {"error", exception.what()}}, 400);
        LogRequest(request, response.status, request_id, start);
    } catch (const std::exception& exception) {
        SetJson(response, {{"ok", false}, {"request_id", request_id},
            {"error", exception.what()}}, 500);
        if (const auto logger = lw::ppocr::http::Logger()) {
            logger->error("request_id={} error={}", request_id, exception.what());
        }
        LogRequest(request, response.status, request_id, start);
    }
}

int RunServer(const ServiceConfig& config,
              const std::function<void()>& on_started = {}) {
    lw::ppocr::http::ConfigureLogging(config.logging);
    std::set_terminate(TerminateHandler);
#if defined(_WIN32)
    SetUnhandledExceptionFilter(UnhandledExceptionHandler);
#endif
    PrintStartupInfo(config);
    if (const auto logger = lw::ppocr::http::Logger()) {
        logger->info("initializing {} OpenCV DNN engine instance(s)",
            config.engine_instances);
    }
    EnginePool engines(config);

    httplib::Server server;
    server.set_payload_max_length(config.max_request_bytes);
    server.new_task_queue = [&config] {
        return new httplib::ThreadPool(config.worker_threads);
    };
    if (!server.set_mount_point("/", config.web_root.u8string())) {
        throw std::runtime_error("unable to mount web_root");
    }
    server.Get("/health", [&config](const httplib::Request&,
                                    httplib::Response& response) {
        SetJson(response, {
            {"ok", true}, {"status", "ready"},
            {"product", kProduct}, {"version", kVersion},
            {"backend", "opencv-dnn"}, {"backend_name", "OpenCV DNN CPU"},
            {"api_key_required", !config.api_key.empty()},
            {"project_url", kProjectUrl}, {"author", kAuthor}
        });
    });
    server.Post("/api/ocr", [&config, &engines](
        const httplib::Request& request, httplib::Response& response) {
        HandleApi(config, engines, request, response, false);
    });
    server.Post("/api/recognize", [&config, &engines](
        const httplib::Request& request, httplib::Response& response) {
        HandleApi(config, engines, request, response, true);
    });
    server.set_error_handler([](const httplib::Request&,
                                httplib::Response& response) {
        if (response.status == 413) {
            SetJson(response, {{"ok", false},
                {"error", "request exceeds max_request_bytes"}}, 413);
        }
    });
    server.set_start_handler([&config, &on_started] {
        std::cout << kProduct << " HTTP service ready: http://"
                  << config.listen_host << ':' << config.port
                  << " (OpenCV DNN CPU)\n" << std::flush;
        if (on_started) on_started();
    });

    g_server.store(&server);
#if defined(_WIN32)
    if (g_service_stop_requested.load()) server.stop();
#endif
    const bool listened = server.listen(config.listen_host, config.port);
    g_server.store(nullptr);
    lw::ppocr::http::ShutdownLogging();
    if (!listened) {
        throw std::runtime_error("unable to listen on configured host and port");
    }
    return 0;
}

void SignalHandler(int) {
    if (httplib::Server* server = g_server.load()) server->stop();
}

#if defined(_WIN32)
void ReportWindowsServiceStatus(DWORD state, DWORD win32_exit_code = NO_ERROR,
                                DWORD wait_hint = 0) {
    if (g_service_status_handle == nullptr) return;
    g_service_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_service_status.dwCurrentState = state;
    g_service_status.dwWin32ExitCode = win32_exit_code;
    g_service_status.dwServiceSpecificExitCode =
        win32_exit_code == ERROR_SERVICE_SPECIFIC_ERROR ? 1 : 0;
    g_service_status.dwWaitHint = wait_hint;
    g_service_status.dwControlsAccepted = state == SERVICE_RUNNING
        ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN
        : 0;
    g_service_status.dwCheckPoint =
        state == SERVICE_RUNNING || state == SERVICE_STOPPED
            ? 0
            : g_service_checkpoint++;
    SetServiceStatus(g_service_status_handle, &g_service_status);
}

void WINAPI WindowsServiceControlHandler(DWORD control) {
    if (control != SERVICE_CONTROL_STOP &&
        control != SERVICE_CONTROL_SHUTDOWN) {
        return;
    }
    g_service_stop_requested.store(true);
    ReportWindowsServiceStatus(SERVICE_STOP_PENDING, NO_ERROR, 30000);
    if (httplib::Server* server = g_server.load()) server->stop();
}

void WINAPI WindowsServiceMain(DWORD, wchar_t**) {
    g_service_status_handle = RegisterServiceCtrlHandlerW(
        kWindowsServiceName, WindowsServiceControlHandler);
    if (g_service_status_handle == nullptr) return;

    ReportWindowsServiceStatus(SERVICE_START_PENDING, NO_ERROR, 60000);
    try {
        const ServiceConfig config = LoadConfig(g_config_path);
        const int result = RunServer(config, [] {
            ReportWindowsServiceStatus(SERVICE_RUNNING);
        });
        ReportWindowsServiceStatus(
            SERVICE_STOPPED, result == 0 ? NO_ERROR : ERROR_SERVICE_SPECIFIC_ERROR);
    } catch (const std::exception& exception) {
        if (const auto logger = lw::ppocr::http::Logger()) {
            logger->critical("Windows service stopped by exception: {}",
                             exception.what());
            logger->flush();
        }
        lw::ppocr::http::ShutdownLogging();
        ReportWindowsServiceStatus(SERVICE_STOPPED,
                                   ERROR_SERVICE_SPECIFIC_ERROR);
    }
}

int RunAsWindowsService() {
    SERVICE_TABLE_ENTRYW table[] = {
        {const_cast<wchar_t*>(kWindowsServiceName), WindowsServiceMain},
        {nullptr, nullptr}
    };
    if (!StartServiceCtrlDispatcherW(table)) {
        throw std::runtime_error(
            "unable to connect to Windows Service Control Manager: " +
            std::to_string(GetLastError()));
    }
    return 0;
}
#endif

void PrintUsage() {
    std::cout << "lw-ppocr-http-service [--config <path>]"
#if defined(_WIN32)
              << " [--service]"
#endif
              << "\n";
}

}  // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
    try {
        bool service_mode = false;
        g_config_path = ExecutablePath().parent_path() / "http-service.json";
        for (int index = 1; index < argc; ++index) {
            const std::string argument = argv[index];
            if (argument == "--config" && index + 1 < argc) {
                g_config_path = fs::absolute(fs::u8path(argv[++index]));
#if defined(_WIN32)
            } else if (argument == "--service") {
                service_mode = true;
#endif
            } else if (argument == "--help" || argument == "-h") {
                PrintUsage();
                return 0;
            } else {
                throw std::runtime_error(
                    "unknown or incomplete command-line option: " + argument);
            }
        }
#if defined(_WIN32)
        if (service_mode) return RunAsWindowsService();
#endif
        std::signal(SIGINT, SignalHandler);
        std::signal(SIGTERM, SignalHandler);
        return RunServer(LoadConfig(g_config_path));
    } catch (const std::exception& exception) {
        if (const auto logger = lw::ppocr::http::Logger()) {
            logger->critical("service stopped by exception: {}", exception.what());
            logger->flush();
        }
        std::cerr << kProduct << " HTTP service: "
                  << exception.what() << '\n';
        lw::ppocr::http::ShutdownLogging();
        return 1;
    }
}
