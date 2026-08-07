#include "base64.hpp"
#include "engine_pool.hpp"
#include "logging.hpp"

#include <httplib.h>
#include <lw/ppocr.h>
#include <json.hpp>
#include <spdlog/logger.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <unistd.h>
#else
#include <limits.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;
using Clock = std::chrono::steady_clock;
using EnginePool = lw::ppocr::http::EnginePool;
using EngineSettings = lw::ppocr::http::EngineSettings;

namespace {

constexpr const char* kProduct = LW_PPOCR_PRODUCT_NAME;
constexpr const char* kVersion = LW_PPOCR_VERSION_STRING;
constexpr const char* kAuthor = "天天代码码天天";
constexpr const char* kQq = "819069052";
constexpr const char* kProjectUrl =
    "https://github.com/lxw112190/lw.PPOCR.OpenCVDNN";
constexpr int kConfigSchemaVersion = 1;
constexpr const char* kHttpApiVersion = "1";

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
    size_t max_queued_requests = 32;
    uint64_t engine_wait_timeout_ms = 5000;
    size_t max_request_bytes = 20u * 1024u * 1024u;
    uint64_t max_image_pixels = 40000000;
    size_t max_batch_images = 32;
    uint64_t max_batch_total_pixels = 40000000;
    uint64_t max_batch_decoded_bytes = 120000000;
    uint32_t pdf_dpi = 200;
    size_t max_pdf_pages = 10;
    uint64_t max_pdf_page_pixels = 25000000;
    uint64_t max_pdf_total_pixels = 100000000;
    lw::ppocr::http::LoggingConfig logging;
};

std::atomic<httplib::Server*> g_server{nullptr};
std::atomic<EnginePool*> g_engine_pool{nullptr};
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
#elif defined(__APPLE__)
    uint32_t size = 1024;
    std::vector<char> buffer(size, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        buffer.assign(size, '\0');
        if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
            throw std::runtime_error("unable to determine executable path");
        }
    }
    return fs::weakly_canonical(fs::u8path(buffer.data()));
#else
    std::vector<char> buffer(PATH_MAX + 1u, '\0');
    const ssize_t length = readlink("/proc/self/exe", buffer.data(), PATH_MAX);
    if (length <= 0 || length >= PATH_MAX) {
        throw std::runtime_error("unable to determine executable path");
    }
    return fs::u8path(std::string(buffer.data(), static_cast<size_t>(length)));
#endif
}

std::string RequestMediaType(const httplib::Request& request) {
    std::string value = request.get_header_value("Content-Type");
    const size_t separator = value.find(';');
    if (separator != std::string::npos) value.resize(separator);
    value.erase(std::remove_if(value.begin(), value.end(),
        [](unsigned char character) { return std::isspace(character) != 0; }),
        value.end());
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    return value;
}

bool IsBinaryImageMediaType(const std::string& media_type) {
    return media_type == "application/octet-stream" ||
        (media_type.size() >= 6 && media_type.compare(0, 6, "image/") == 0);
}

bool IsPdfMediaType(const std::string& media_type) {
    return media_type == "application/pdf";
}

bool IsBinaryPayloadMediaType(const std::string& media_type) {
    return IsBinaryImageMediaType(media_type) || IsPdfMediaType(media_type);
}

uint64_t QueryUnsigned(const httplib::Request& request, const char* name,
                      uint64_t current) {
    if (!request.has_param(name)) return current;
    const std::string value = request.get_param_value(name);
    size_t consumed = 0;
    try {
        const unsigned long long parsed = std::stoull(value, &consumed, 10);
        if (value.empty() || consumed != value.size()) {
            throw std::invalid_argument("trailing characters");
        }
        return static_cast<uint64_t>(parsed);
    } catch (const std::exception&) {
        throw std::invalid_argument(std::string(name) +
            " must be an unsigned integer");
    }
}

uint32_t QueryUint32(const httplib::Request& request, const char* name,
                     uint32_t current) {
    const uint64_t value = QueryUnsigned(request, name, current);
    if (value > (std::numeric_limits<uint32_t>::max)()) {
        throw std::invalid_argument(std::string(name) + " is out of range");
    }
    return static_cast<uint32_t>(value);
}

fs::path ResolvePath(const fs::path& base, const std::string& value) {
    fs::path path = fs::u8path(value);
    if (path.is_relative()) {
        path = base / path;
    }
    return fs::weakly_canonical(path);
}

uint64_t EnvironmentUnsigned(const char* name, uint64_t current) {
    const char* raw = std::getenv(name);
    if (raw == nullptr) return current;
    const std::string text(raw);
    size_t consumed = 0;
    try {
        const unsigned long long value = std::stoull(text, &consumed, 10);
        if (text.empty() || consumed != text.size()) {
            throw std::invalid_argument("trailing characters");
        }
        return static_cast<uint64_t>(value);
    } catch (const std::exception&) {
        throw std::runtime_error(std::string("invalid unsigned integer in ") +
            name);
    }
}

bool EnvironmentBoolean(const char* name, bool current) {
    const char* raw = std::getenv(name);
    if (raw == nullptr) return current;
    std::string value(raw);
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    if (value == "1" || value == "true" || value == "yes" || value == "on") {
        return true;
    }
    if (value == "0" || value == "false" || value == "no" || value == "off") {
        return false;
    }
    throw std::runtime_error(std::string("invalid boolean in ") + name);
}

std::vector<std::string> SplitCommaSeparated(const std::string& value) {
    std::vector<std::string> values;
    size_t begin = 0;
    while (begin <= value.size()) {
        const size_t separator = value.find(',', begin);
        const size_t end = separator == std::string::npos
            ? value.size() : separator;
        std::string item = value.substr(begin, end - begin);
        item.erase(item.begin(), std::find_if(item.begin(), item.end(),
            [](unsigned char character) {
                return std::isspace(character) == 0;
            }));
        item.erase(std::find_if(item.rbegin(), item.rend(),
            [](unsigned char character) {
                return std::isspace(character) == 0;
            }).base(), item.end());
        if (!item.empty()) values.push_back(std::move(item));
        if (separator == std::string::npos) break;
        begin = separator + 1;
    }
    return values;
}

void RejectUnknownProperties(
    const json& object,
    std::initializer_list<const char*> allowed,
    const std::string& context) {
    if (!object.is_object()) {
        throw std::runtime_error(context + " must be an object");
    }
    for (const auto& item : object.items()) {
        const bool known = std::any_of(allowed.begin(), allowed.end(),
            [&item](const char* name) { return item.key() == name; });
        if (!known) {
            throw std::runtime_error(
                context + " contains unknown property: " + item.key());
        }
    }
}

void ApplyEnvironmentOverrides(ServiceConfig& config, const fs::path& base) {
    if (const char* value = std::getenv("LW_PPOCR_LISTEN_HOST")) {
        if (*value == '\0') {
            throw std::runtime_error("LW_PPOCR_LISTEN_HOST must not be empty");
        }
        config.listen_host = value;
    }
    const uint64_t port = EnvironmentUnsigned("LW_PPOCR_PORT", config.port);
    if (port > static_cast<uint64_t>((std::numeric_limits<int>::max)())) {
        throw std::runtime_error("LW_PPOCR_PORT is out of range");
    }
    config.port = static_cast<int>(port);
    if (const char* value = std::getenv("LW_PPOCR_MODEL_MANIFEST")) {
        if (*value == '\0') {
            throw std::runtime_error(
                "LW_PPOCR_MODEL_MANIFEST must not be empty");
        }
        config.model_manifest = ResolvePath(base, value);
    }
    if (const char* value = std::getenv("LW_PPOCR_WEB_ROOT")) {
        if (*value == '\0') {
            throw std::runtime_error("LW_PPOCR_WEB_ROOT must not be empty");
        }
        config.web_root = ResolvePath(base, value);
    }
    if (const char* value = std::getenv("LW_PPOCR_API_KEY")) {
        config.api_key = value;
    }
    config.engine_instances = static_cast<size_t>(EnvironmentUnsigned(
        "LW_PPOCR_ENGINE_INSTANCES", config.engine_instances));
    config.worker_threads = static_cast<size_t>(EnvironmentUnsigned(
        "LW_PPOCR_WORKER_THREADS", config.worker_threads));
    config.max_queued_requests = static_cast<size_t>(EnvironmentUnsigned(
        "LW_PPOCR_MAX_QUEUED_REQUESTS", config.max_queued_requests));
    config.engine_wait_timeout_ms = EnvironmentUnsigned(
        "LW_PPOCR_ENGINE_WAIT_TIMEOUT_MS", config.engine_wait_timeout_ms);
    config.max_request_bytes = static_cast<size_t>(EnvironmentUnsigned(
        "LW_PPOCR_MAX_REQUEST_BYTES", config.max_request_bytes));
    config.max_image_pixels = EnvironmentUnsigned(
        "LW_PPOCR_MAX_IMAGE_PIXELS", config.max_image_pixels);
    config.max_batch_images = static_cast<size_t>(EnvironmentUnsigned(
        "LW_PPOCR_MAX_BATCH_IMAGES", config.max_batch_images));
    config.max_batch_total_pixels = EnvironmentUnsigned(
        "LW_PPOCR_MAX_BATCH_TOTAL_PIXELS", config.max_batch_total_pixels);
    config.max_batch_decoded_bytes = EnvironmentUnsigned(
        "LW_PPOCR_MAX_BATCH_DECODED_BYTES",
        config.max_batch_decoded_bytes);
    config.pdf_dpi = static_cast<uint32_t>(EnvironmentUnsigned(
        "LW_PPOCR_PDF_DPI", config.pdf_dpi));
    config.max_pdf_pages = static_cast<size_t>(EnvironmentUnsigned(
        "LW_PPOCR_MAX_PDF_PAGES", config.max_pdf_pages));
    config.max_pdf_page_pixels = EnvironmentUnsigned(
        "LW_PPOCR_MAX_PDF_PAGE_PIXELS", config.max_pdf_page_pixels);
    config.max_pdf_total_pixels = EnvironmentUnsigned(
        "LW_PPOCR_MAX_PDF_TOTAL_PIXELS", config.max_pdf_total_pixels);
    config.logging.enabled = EnvironmentBoolean(
        "LW_PPOCR_LOGGING_ENABLED", config.logging.enabled);
    config.logging.console = EnvironmentBoolean(
        "LW_PPOCR_CONSOLE_LOGGING_ENABLED", config.logging.console);
    config.logging.file_enabled = EnvironmentBoolean(
        "LW_PPOCR_FILE_LOGGING_ENABLED", config.logging.file_enabled);
    config.logging.request_enabled = EnvironmentBoolean(
        "LW_PPOCR_REQUEST_LOGGING_ENABLED", config.logging.request_enabled);
    config.logging.request_start_enabled = EnvironmentBoolean(
        "LW_PPOCR_REQUEST_START_LOGGING_ENABLED",
        config.logging.request_start_enabled);
    config.logging.access_file_enabled = EnvironmentBoolean(
        "LW_PPOCR_ACCESS_FILE_LOGGING_ENABLED",
        config.logging.access_file_enabled);
    if (const char* value = std::getenv("LW_PPOCR_LOG_FILE")) {
        if (*value == '\0') {
            throw std::runtime_error("LW_PPOCR_LOG_FILE must not be empty");
        }
        config.logging.file_path = ResolvePath(base, value).u8string();
    }
    if (const char* value = std::getenv("LW_PPOCR_ACCESS_LOG_FILE")) {
        if (*value == '\0') {
            throw std::runtime_error("LW_PPOCR_ACCESS_LOG_FILE must not be empty");
        }
        config.logging.access_file_path = ResolvePath(base, value).u8string();
    }
    if (const char* value = std::getenv("LW_PPOCR_ACCESS_LOG_FORMAT")) {
        config.logging.access_format = value;
    }
    config.logging.flush_interval_seconds = static_cast<size_t>(
        EnvironmentUnsigned("LW_PPOCR_LOG_FLUSH_INTERVAL_SECONDS",
            config.logging.flush_interval_seconds));
    if (const char* value = std::getenv("LW_PPOCR_TRUSTED_PROXIES")) {
        config.logging.trusted_proxies = SplitCommaSeparated(value);
    }
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
    RejectUnknownProperties(document, {
        "$schema", "schema_version", "listen_host", "port",
        "model_manifest", "web_root", "api_key", "enable_classifier",
        "limit_side_len", "det_db_threshold", "det_db_box_threshold",
        "det_db_unclip_ratio", "det_use_dilation", "cls_threshold",
        "cls_batch_size", "rec_batch_size", "rec_concurrency",
        "engine_instances", "worker_threads", "max_queued_requests",
        "engine_wait_timeout_ms", "max_request_bytes", "max_image_pixels",
        "max_batch_images", "max_batch_total_pixels",
        "max_batch_decoded_bytes", "pdf_dpi", "max_pdf_pages",
        "max_pdf_page_pixels", "max_pdf_total_pixels", "logging"
    }, "HTTP service configuration");
    if (!document.contains("schema_version")) {
        throw std::runtime_error(
            "HTTP service configuration requires schema_version");
    }
    if (document.contains("$schema") &&
        (!document.at("$schema").is_string() ||
         document.at("$schema").get_ref<const std::string&>().empty())) {
        throw std::runtime_error("$schema must be a non-empty string");
    }
    const int schema_version = document.at("schema_version").get<int>();
    if (schema_version != kConfigSchemaVersion) {
        throw std::runtime_error(
            "unsupported HTTP service configuration schema_version: " +
            std::to_string(schema_version));
    }

    const fs::path base = fs::absolute(path).parent_path();
    ServiceConfig config;
    config.listen_host = document.value("listen_host", config.listen_host);
    config.port = document.value("port", config.port);
    const std::string model_manifest = document.value(
        "model_manifest", "models/ppocrv6-tiny/model.json");
    const std::string web_root = document.value("web_root", "www");
    if (config.listen_host.empty() || model_manifest.empty() ||
        web_root.empty()) {
        throw std::runtime_error(
            "listen_host, model_manifest, and web_root must not be empty");
    }
    config.model_manifest = ResolvePath(base, model_manifest);
    config.web_root = ResolvePath(base, web_root);
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
    config.max_queued_requests = document.value(
        "max_queued_requests", config.max_queued_requests);
    config.engine_wait_timeout_ms = document.value(
        "engine_wait_timeout_ms", config.engine_wait_timeout_ms);
    config.max_request_bytes = document.value(
        "max_request_bytes", config.max_request_bytes);
    config.max_image_pixels = document.value(
        "max_image_pixels", config.max_image_pixels);
    config.max_batch_images = document.value(
        "max_batch_images", config.max_batch_images);
    config.max_batch_total_pixels = document.value(
        "max_batch_total_pixels", config.max_batch_total_pixels);
    config.max_batch_decoded_bytes = document.value(
        "max_batch_decoded_bytes", config.max_batch_decoded_bytes);
    config.pdf_dpi = document.value("pdf_dpi", config.pdf_dpi);
    config.max_pdf_pages = document.value("max_pdf_pages", config.max_pdf_pages);
    config.max_pdf_page_pixels = document.value(
        "max_pdf_page_pixels", config.max_pdf_page_pixels);
    config.max_pdf_total_pixels = document.value(
        "max_pdf_total_pixels", config.max_pdf_total_pixels);

    if (document.contains("logging")) {
        const json& logging = document.at("logging");
        RejectUnknownProperties(logging, {
            "enabled", "level", "console", "file_enabled", "file_path",
            "request_enabled", "request_start_enabled",
            "access_file_enabled", "access_file_path", "access_format",
            "flush_interval_seconds", "trusted_proxies",
            "max_file_size_mb", "max_files"
        }, "logging configuration");
        config.logging.enabled = logging.value("enabled", config.logging.enabled);
        config.logging.level = logging.value("level", config.logging.level);
        const std::vector<std::string> allowed_levels = {
            "trace", "debug", "info", "warn", "warning", "error",
            "critical", "off"};
        if (std::find(allowed_levels.begin(), allowed_levels.end(),
                config.logging.level) == allowed_levels.end()) {
            throw std::runtime_error("logging.level is unsupported");
        }
        config.logging.console = logging.value("console", config.logging.console);
        config.logging.file_enabled = logging.value(
            "file_enabled", config.logging.file_enabled);
        config.logging.request_enabled = logging.value(
            "request_enabled", config.logging.request_enabled);
        config.logging.request_start_enabled = logging.value(
            "request_start_enabled", config.logging.request_start_enabled);
        config.logging.access_file_enabled = logging.value(
            "access_file_enabled", config.logging.access_file_enabled);
        config.logging.access_format = logging.value(
            "access_format", config.logging.access_format);
        config.logging.flush_interval_seconds = logging.value(
            "flush_interval_seconds", config.logging.flush_interval_seconds);
        config.logging.max_files = logging.value(
            "max_files", config.logging.max_files);
        const uint64_t max_mb = logging.value(
            "max_file_size_mb", uint64_t{10});
        if (max_mb < 1 || max_mb > 1024) {
            throw std::runtime_error(
                "logging.max_file_size_mb must be between 1 and 1024");
        }
        config.logging.max_file_size =
            static_cast<size_t>(max_mb * 1024u * 1024u);
        const std::string log_path = logging.value(
            "file_path", config.logging.file_path);
        if (log_path.empty()) {
            throw std::runtime_error("logging.file_path must not be empty");
        }
        config.logging.file_path = ResolvePath(base, log_path).u8string();
        const std::string access_log_path = logging.value(
            "access_file_path", config.logging.access_file_path);
        if (access_log_path.empty()) {
            throw std::runtime_error(
                "logging.access_file_path must not be empty");
        }
        config.logging.access_file_path =
            ResolvePath(base, access_log_path).u8string();
        if (logging.contains("trusted_proxies")) {
            const json& proxies = logging.at("trusted_proxies");
            if (!proxies.is_array()) {
                throw std::runtime_error(
                    "logging.trusted_proxies must be an array");
            }
            config.logging.trusted_proxies.clear();
            for (const json& proxy : proxies) {
                if (!proxy.is_string() || proxy.get_ref<
                        const std::string&>().empty() ||
                    proxy.get_ref<const std::string&>().size() > 64) {
                    throw std::runtime_error(
                        "logging.trusted_proxies entries must contain 1 to 64 characters");
                }
                const std::string value = proxy.get<std::string>();
                if (std::find(config.logging.trusted_proxies.begin(),
                        config.logging.trusted_proxies.end(), value) !=
                        config.logging.trusted_proxies.end()) {
                    throw std::runtime_error(
                        "logging.trusted_proxies entries must be unique");
                }
                config.logging.trusted_proxies.push_back(value);
            }
            if (config.logging.trusted_proxies.size() > 64) {
                throw std::runtime_error(
                    "logging.trusted_proxies must contain at most 64 items");
            }
        }
    }

    ApplyEnvironmentOverrides(config, base);

    if (!document.contains("max_batch_total_pixels") &&
        std::getenv("LW_PPOCR_MAX_BATCH_TOTAL_PIXELS") == nullptr) {
        config.max_batch_total_pixels = (std::max)(
            config.max_batch_total_pixels, config.max_image_pixels);
    }
    if (!document.contains("max_batch_decoded_bytes") &&
        std::getenv("LW_PPOCR_MAX_BATCH_DECODED_BYTES") == nullptr) {
        config.max_batch_decoded_bytes = (std::max)(
            config.max_batch_decoded_bytes,
            config.max_image_pixels * uint64_t{3});
    }

    if (config.logging.trusted_proxies.size() > 64) {
        throw std::runtime_error(
            "logging.trusted_proxies must contain at most 64 items");
    }
    for (size_t index = 0; index < config.logging.trusted_proxies.size();
         ++index) {
        const std::string& proxy = config.logging.trusted_proxies[index];
        if (proxy.empty() || proxy.size() > 64 ||
            std::find(config.logging.trusted_proxies.begin(),
                config.logging.trusted_proxies.begin() + index, proxy) !=
                config.logging.trusted_proxies.begin() + index) {
            throw std::runtime_error(
                "logging.trusted_proxies must contain unique strings of 1 to 64 characters");
        }
    }

    if (config.port < 1 || config.port > 65535 ||
        config.worker_threads < 1 || config.worker_threads > 128 ||
        config.engine_instances < 1 || config.engine_instances > 32 ||
        config.max_queued_requests < 1 ||
        config.max_queued_requests > 128 ||
        config.engine_wait_timeout_ms < 1 ||
        config.engine_wait_timeout_ms > 300000 ||
        config.limit_side_len < 32 || config.limit_side_len > 10000 ||
        config.det_db_threshold < 0.0f || config.det_db_threshold > 1.0f ||
        config.det_db_box_threshold < 0.0f ||
        config.det_db_box_threshold > 1.0f ||
        config.det_db_unclip_ratio <= 0.0f ||
        config.det_db_unclip_ratio > 10.0f ||
        config.cls_threshold < 0.0f || config.cls_threshold > 1.0f ||
        config.cls_batch_size < 1 || config.cls_batch_size > 256 ||
        config.rec_batch_size < 1 || config.rec_batch_size > 256 ||
        config.rec_concurrency < 1 || config.rec_concurrency > 32 ||
        config.max_request_bytes < 1024 ||
        config.max_request_bytes > 256u * 1024u * 1024u ||
        config.max_image_pixels < 1 || config.max_image_pixels > 200000000u ||
        config.max_batch_images < 1 || config.max_batch_images > 256 ||
        config.max_batch_total_pixels < config.max_image_pixels ||
        config.max_batch_total_pixels > 1000000000ull ||
        config.max_batch_decoded_bytes < config.max_image_pixels * 3ull ||
        config.max_batch_decoded_bytes > 4ull * 1024ull * 1024ull * 1024ull ||
        config.pdf_dpi < 36 || config.pdf_dpi > 600 ||
        config.max_pdf_pages < 1 || config.max_pdf_pages > 1000 ||
        config.max_pdf_page_pixels < 1 ||
        config.max_pdf_page_pixels > 200000000ull ||
        config.max_pdf_total_pixels < config.max_pdf_page_pixels ||
        config.max_pdf_total_pixels > 1000000000ull ||
        config.logging.max_file_size < 1024u ||
        config.logging.max_files < 1 || config.logging.max_files > 100 ||
        config.logging.flush_interval_seconds < 1 ||
        config.logging.flush_interval_seconds > 60) {
        throw std::runtime_error("configuration contains an out-of-range value");
    }
    if (config.logging.access_format != "jsonl" &&
        config.logging.access_format != "text") {
        throw std::runtime_error(
            "logging.access_format must be 'jsonl' or 'text'");
    }
    if (config.logging.file_enabled && config.logging.access_file_enabled &&
        fs::weakly_canonical(config.logging.file_path) ==
            fs::weakly_canonical(config.logging.access_file_path)) {
        throw std::runtime_error(
            "runtime and access log files must use different paths");
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

void SetJson(httplib::Response& response, const json& value, int status = 200) {
    response.status = status;
    response.set_header("Cache-Control", "no-store");
    response.set_header("X-LW-PPOCR-API-Version", kHttpApiVersion);
    response.set_content(
        value.dump(-1, ' ', false, json::error_handler_t::replace),
        "application/json; charset=utf-8");
}

json ErrorResponse(
    const std::string& request_id,
    const std::string& error_code,
    const std::string& message) {
    return {
        {"ok", false},
        {"request_id", request_id},
        {"error_code", error_code},
        {"error", message}
    };
}

class HttpError final : public std::runtime_error {
public:
    HttpError(int status, std::string code, std::string message)
        : std::runtime_error(std::move(message)),
          status_(status), code_(std::move(code)) {}

    int status() const noexcept { return status_; }
    const std::string& code() const noexcept { return code_; }

private:
    int status_;
    std::string code_;
};

EnginePool::Lease AcquireEngine(
    const ServiceConfig& config,
    EnginePool& engines) {
    auto acquired = engines.AcquireFor(
        std::chrono::milliseconds(config.engine_wait_timeout_ms),
        config.max_queued_requests);
    switch (acquired.status) {
    case EnginePool::AcquireStatus::Acquired:
        return std::move(acquired.lease);
    case EnginePool::AcquireStatus::QueueFull:
        throw HttpError(429, "queue_full",
            "OCR engine wait queue is full");
    case EnginePool::AcquireStatus::TimedOut:
        throw HttpError(503, "engine_wait_timeout",
            "timed out waiting for an OCR engine");
    case EnginePool::AcquireStatus::Stopping:
        throw HttpError(503, "service_stopping",
            "OCR service is stopping");
    }
    throw HttpError(503, "service_unavailable", "OCR service is unavailable");
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
    SetJson(response, ErrorResponse(
        request_id, "unauthorized", "invalid API key"), 401);
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

std::string UtcTimestamp() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    std::tm value{};
#if defined(_WIN32)
    gmtime_s(&value, &seconds);
#else
    gmtime_r(&seconds, &value);
#endif
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count() % 1000;
    std::ostringstream output;
    output << std::put_time(&value, "%Y-%m-%dT%H:%M:%S") << '.'
           << std::setw(3) << std::setfill('0') << milliseconds << 'Z';
    return output.str();
}

bool PlausibleIpAddress(const std::string& value) {
    return !value.empty() && value.size() <= 64 &&
        std::all_of(value.begin(), value.end(), [](unsigned char character) {
            return std::isxdigit(character) != 0 || character == '.' ||
                character == ':';
        });
}

std::string EffectiveRemoteAddress(
    const ServiceConfig& config,
    const httplib::Request& request) {
    if (std::find(config.logging.trusted_proxies.begin(),
            config.logging.trusted_proxies.end(), request.remote_addr) ==
            config.logging.trusted_proxies.end()) {
        return request.remote_addr;
    }
    std::string forwarded = request.get_header_value("X-Forwarded-For");
    const size_t separator = forwarded.find(',');
    if (separator != std::string::npos) forwarded.resize(separator);
    forwarded.erase(forwarded.begin(), std::find_if(
        forwarded.begin(), forwarded.end(), [](unsigned char character) {
            return std::isspace(character) == 0;
        }));
    forwarded.erase(std::find_if(forwarded.rbegin(), forwarded.rend(),
        [](unsigned char character) {
            return std::isspace(character) == 0;
        }).base(), forwarded.end());
    return PlausibleIpAddress(forwarded) ? forwarded : request.remote_addr;
}

void CopyStageTiming(
    const json& source,
    json& destination,
    const char* name) {
    const auto item = source.find(name);
    if (item != source.end() && item->is_object()) {
        const auto total = item->find("total_ms");
        if (total != item->end() && total->is_number()) {
            destination[name] = total->get<double>();
        }
    }
}

void LogRequest(const ServiceConfig& config,
                const httplib::Request& request,
                const httplib::Response& response,
                const std::string& request_id,
                const std::string& operation,
                Clock::time_point start,
                size_t result_count = 0,
                const json* native = nullptr,
                const std::string& error_code = {}) {
    if (!lw::ppocr::http::RequestLoggingEnabled()) return;
    const auto logger = lw::ppocr::http::AccessLogger();
    if (logger == nullptr) return;
    const double duration_ms = Milliseconds(start, Clock::now());
    const std::string remote_ip = EffectiveRemoteAddress(config, request);
    const std::string media_type = RequestMediaType(request);
    if (lw::ppocr::http::ConfiguredAccessLogFormat() ==
            lw::ppocr::http::AccessLogFormat::Text) {
        logger->info("request_id={} remote_ip={} peer_ip={} method={} path={} "
                     "operation={} content_type={} request_bytes={} "
                     "response_bytes={} status={} duration_ms={:.2f} results={}{}",
            request_id, remote_ip, request.remote_addr, request.method,
            request.path, operation, media_type, request.body.size(),
            response.body.size(), response.status, duration_ms, result_count,
            error_code.empty() ? std::string{} : " error_code=" + error_code);
        return;
    }

    json record = {
        {"log_schema_version", 1},
        {"timestamp", UtcTimestamp()},
        {"level", "info"},
        {"event", "request_complete"},
        {"request_id", request_id},
        {"remote_ip", remote_ip},
        {"peer_ip", request.remote_addr},
        {"method", request.method},
        {"path", request.path},
        {"operation", operation},
        {"content_type", media_type},
        {"request_format", IsBinaryPayloadMediaType(media_type)
            ? "binary" : "json"},
        {"status", response.status},
        {"request_bytes", request.body.size()},
        {"response_bytes", response.body.size()},
        {"result_count", result_count},
        {"duration_ms", duration_ms}
    };
    if (!error_code.empty()) record["error_code"] = error_code;
    if (native != nullptr) {
        const auto image = native->find("image");
        if (image != native->end() && image->is_object()) {
            record["image"] = *image;
        }
        const auto image_count = native->find("image_count");
        if (image_count != native->end() && image_count->is_number_unsigned()) {
            record["image_count"] = image_count->get<size_t>();
        }
        const auto timing = native->find("timing");
        if (timing != native->end() && timing->is_object()) {
            json timing_ms = {{"server_total", duration_ms}};
            const auto decode = timing->find("decode_ms");
            if (decode != timing->end() && decode->is_number()) {
                timing_ms["decode"] = decode->get<double>();
            }
            CopyStageTiming(*timing, timing_ms, "detector");
            CopyStageTiming(*timing, timing_ms, "classifier");
            CopyStageTiming(*timing, timing_ms, "recognizer");
            CopyStageTiming(*timing, timing_ms, "pipeline");
            record["timing_ms"] = std::move(timing_ms);
        }
    }
    logger->info("{}", record.dump());
}

void LogRequestStart(const ServiceConfig& config,
                     const httplib::Request& request,
                     const std::string& request_id,
                     const std::string& operation) {
    if (!lw::ppocr::http::RequestStartLoggingEnabled()) return;
    if (const auto logger = lw::ppocr::http::Logger()) {
        logger->info("request_started request_id={} remote_ip={} peer_ip={} "
                     "method={} path={} operation={} content_type={} bytes={}",
            request_id, EffectiveRemoteAddress(config, request),
            request.remote_addr, request.method, request.path, operation,
            RequestMediaType(request), request.body.size());
        logger->flush();
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

json CallPdfOcr(lw_ppocr_handle handle, const uint8_t* encoded,
                size_t encoded_size, const lw_ppocr_pdf_options& options) {
    char* output = nullptr;
    uint64_t length = 0;
    const lw_ppocr_status status = lw_ppocr_ocr_pdf_encoded(handle,
        encoded, encoded_size, &options, &output, &length);
    if (status != LW_PPOCR_STATUS_OK) {
        const std::string error = LastError(handle);
        if (status == LW_PPOCR_STATUS_LIMIT_EXCEEDED) {
            throw HttpError(413, "pdf_limit_exceeded", error);
        }
        if (status == LW_PPOCR_STATUS_DOCUMENT_ERROR) {
            if (error.find("PDFium") != std::string::npos ||
                error.find("pdfium") != std::string::npos) {
                throw HttpError(503, "pdfium_unavailable", error);
            }
            throw HttpError(400, "invalid_pdf", error);
        }
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
        if (status == LW_PPOCR_STATUS_LIMIT_EXCEEDED) {
            throw HttpError(413, "batch_limit_exceeded", error);
        }
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

void HandlePdfApi(const ServiceConfig& config, EnginePool& engines,
                  const httplib::Request& request,
                  httplib::Response& response) {
    const auto start = Clock::now();
    const std::string request_id = NewRequestId();
    const std::string operation = "pdf_ocr";
    response.set_header("X-Request-ID", request_id);
    if (!Authorized(config, request, response, request_id)) {
        LogRequest(config, request, response, request_id, operation, start,
            0, nullptr, "unauthorized");
        return;
    }
    LogRequestStart(config, request, request_id, operation);
    try {
        if (!IsPdfMediaType(RequestMediaType(request))) {
            throw std::invalid_argument(
                "Content-Type must be application/pdf");
        }
        if (request.body.empty()) {
            throw std::invalid_argument("PDF body is empty");
        }
        lw_ppocr_pdf_options options{};
        lw_ppocr_pdf_options_init(&options);
        options.dpi = QueryUint32(request, "dpi", config.pdf_dpi);
        options.first_page = QueryUint32(request, "first_page",
            options.first_page);
        options.page_count = QueryUint32(request, "page_count",
            options.page_count);
        options.max_pages = static_cast<uint32_t>(config.max_pdf_pages);
        options.max_page_pixels = config.max_pdf_page_pixels;
        options.max_total_pixels = config.max_pdf_total_pixels;
        if (request.has_param("mode")) {
            std::string mode = request.get_param_value("mode");
            std::transform(mode.begin(), mode.end(), mode.begin(),
                [](unsigned char value) {
                    return static_cast<char>(std::tolower(value));
                });
            if (mode == "auto") options.mode = LW_PPOCR_PDF_MODE_AUTO;
            else if (mode == "text") options.mode = LW_PPOCR_PDF_MODE_TEXT;
            else if (mode == "ocr") options.mode = LW_PPOCR_PDF_MODE_OCR;
            else if (mode == "hybrid") options.mode = LW_PPOCR_PDF_MODE_HYBRID;
            else throw std::invalid_argument(
                "mode must be auto, text, ocr, or hybrid");
        }

        auto lease = AcquireEngine(config, engines);
        json native = CallPdfOcr(lease.get(),
            reinterpret_cast<const uint8_t*>(request.body.data()),
            request.body.size(), options);
        native["ok"] = true;
        native["request_id"] = request_id;
        native["backend"] = "opencv-dnn";
        native["timing_ms"]["server_total"] =
            Milliseconds(start, Clock::now());
        const size_t count = native.value("pages", json::array()).size();
        SetJson(response, native);
        LogRequest(config, request, response, request_id, operation, start,
            count, &native);
    } catch (const HttpError& exception) {
        SetJson(response, ErrorResponse(request_id, exception.code(),
            exception.what()), exception.status());
        if (exception.status() == 429 || exception.status() == 503) {
            response.set_header("Retry-After", "1");
        }
        LogRequest(config, request, response, request_id, operation, start,
            0, nullptr, exception.code());
    } catch (const std::invalid_argument& exception) {
        SetJson(response, ErrorResponse(request_id, "invalid_request",
            exception.what()), 400);
        LogRequest(config, request, response, request_id, operation, start,
            0, nullptr, "invalid_request");
    } catch (const std::exception& exception) {
        SetJson(response, ErrorResponse(request_id, "internal_error",
            exception.what()), 500);
        LogRequest(config, request, response, request_id, operation, start,
            0, nullptr, "internal_error");
    }
}

void PrintStartupInfo(const ServiceConfig& config) {
    const size_t max_http_threads =
        config.worker_threads + config.max_queued_requests;
    std::cout
        << "============================================================\n"
        << kProduct << " HTTP Service v" << kVersion << '\n'
        << "Author / 作者: " << kAuthor << '\n'
        << "QQ: " << kQq << '\n'
        << "Project / 项目: " << kProjectUrl << '\n'
        << "============================================================\n"
        << "Startup parameters / 启动参数\n"
        << "  config_file: " << g_config_path.u8string() << '\n'
        << "  config_schema_version: " << kConfigSchemaVersion << '\n'
        << "  http_api_version: " << kHttpApiVersion << '\n'
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
        << "  max_http_threads: " << max_http_threads << '\n'
        << "  max_queued_requests: " << config.max_queued_requests << '\n'
        << "  engine_wait_timeout_ms: "
        << config.engine_wait_timeout_ms << '\n'
        << "  max_request_bytes: " << config.max_request_bytes << '\n'
        << "  max_image_pixels: " << config.max_image_pixels << '\n'
        << "  max_batch_images: " << config.max_batch_images << '\n'
        << "  max_batch_total_pixels: "
        << config.max_batch_total_pixels << '\n'
        << "  max_batch_decoded_bytes: "
        << config.max_batch_decoded_bytes << '\n'
        << "  pdf_dpi: " << config.pdf_dpi << '\n'
        << "  max_pdf_pages: " << config.max_pdf_pages << '\n'
        << "  max_pdf_page_pixels: " << config.max_pdf_page_pixels << '\n'
        << "  max_pdf_total_pixels: " << config.max_pdf_total_pixels << '\n'
        << "  api_key: " << (config.api_key.empty()
            ? "disabled / 未启用"
            : "configured / 已配置 (value hidden / 明文已隐藏)") << '\n'
        << "  logging.enabled: " << config.logging.enabled << '\n'
        << "  logging.level: " << config.logging.level << '\n'
        << "  logging.console: " << config.logging.console << '\n'
        << "  logging.file_enabled: " << config.logging.file_enabled << '\n'
        << "  logging.file_path: " << config.logging.file_path << '\n'
        << "  logging.request_enabled: "
        << config.logging.request_enabled << '\n'
        << "  logging.request_start_enabled: "
        << config.logging.request_start_enabled << '\n'
        << "  logging.access_file_enabled: "
        << config.logging.access_file_enabled << '\n'
        << "  logging.access_file_path: "
        << config.logging.access_file_path << '\n'
        << "  logging.access_format: "
        << config.logging.access_format << '\n'
        << "  logging.flush_interval_seconds: "
        << config.logging.flush_interval_seconds << '\n'
        << "  logging.trusted_proxies: "
        << config.logging.trusted_proxies.size() << " configured\n"
        << "Request parameters / 请求参数\n"
        << "  POST /api/ocr JSON: {\"image_base64\":\"...\"}\n"
        << "  POST /api/ocr binary: image/* or application/octet-stream\n"
        << "  POST /api/recognize JSON: {\"image_base64\":\"...\"}\n"
        << "  POST /api/recognize binary: image/* or application/octet-stream\n"
        << "  POST /api/recognize batch: {\"images_base64\":[\"...\"]}\n"
        << "  POST /api/pdf/ocr binary: application/pdf\n"
        << "  authentication: X-API-Key header when api_key is configured\n"
        << "============================================================\n"
        << std::flush;
}

void HandleApi(const ServiceConfig& config, EnginePool& engines,
               const httplib::Request& request, httplib::Response& response,
               bool recognition_only) {
    const auto start = Clock::now();
    const std::string request_id = NewRequestId();
    std::string operation = recognition_only ? "recognize" : "ocr";
    response.set_header("X-Request-ID", request_id);
    if (!Authorized(config, request, response, request_id)) {
        LogRequest(config, request, response, request_id, operation, start,
            0, nullptr, "unauthorized");
        return;
    }
    LogRequestStart(config, request, request_id, operation);
    const std::string media_type = RequestMediaType(request);
    const bool binary_request = IsBinaryImageMediaType(media_type);
    try {
        json native;
        if (binary_request) {
            if (request.body.empty()) {
                throw std::invalid_argument("binary image body is empty");
            }
            const std::vector<uint8_t> encoded(
                request.body.begin(), request.body.end());
            auto lease = AcquireEngine(config, engines);
            native = CallOcr(lease.get(), encoded, recognition_only);
        } else {
            const json body = json::parse(request.body);
            if (!body.is_object()) {
                throw std::invalid_argument("JSON request must be an object");
            }

            if (recognition_only && body.contains("images_base64")) {
                if (body.size() != 1) {
                    throw std::invalid_argument(
                        "batch JSON request only accepts images_base64");
                }
                operation = "recognize_batch";
                const json& values = body.at("images_base64");
                if (!values.is_array() || values.empty()) {
                    throw std::invalid_argument(
                        "images_base64 must be a non-empty array");
                }
                if (values.size() > config.max_batch_images) {
                    throw HttpError(413, "batch_limit_exceeded",
                        "images_base64 exceeds max_batch_images");
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
                            (error.empty()
                                ? "Base64 string is required" : error));
                    }
                }
                auto lease = AcquireEngine(config, engines);
                native = CallRecognizeBatch(lease.get(), images);
                native["image_count"] = images.size();
            } else {
                if (body.size() != 1) {
                    throw std::invalid_argument(
                        "single-image JSON request only accepts image_base64");
                }
                if (!body.contains("image_base64") ||
                    !body.at("image_base64").is_string()) {
                    throw std::invalid_argument(
                        "image_base64 string is required");
                }
                std::vector<uint8_t> encoded;
                std::string error;
                if (!lw::ppocr::http::DecodeBase64(
                        body.at("image_base64").get_ref<const std::string&>(),
                        config.max_request_bytes, encoded, error)) {
                    throw std::invalid_argument(error);
                }
                auto lease = AcquireEngine(config, engines);
                native = CallOcr(lease.get(), encoded, recognition_only);
            }
        }
        native["ok"] = true;
        native["request_id"] = request_id;
        native["backend"] = "opencv-dnn";
        native["timing"]["server_total_ms"] =
            Milliseconds(start, Clock::now());
        const size_t count = native.value("result", json::array()).size();
        SetJson(response, native);
        LogRequest(config, request, response, request_id, operation, start,
            count, &native);
    } catch (const HttpError& exception) {
        SetJson(response, ErrorResponse(
            request_id, exception.code(), exception.what()),
            exception.status());
        if (exception.status() == 429 || exception.status() == 503) {
            response.set_header("Retry-After", "1");
        }
        LogRequest(config, request, response, request_id, operation, start,
            0, nullptr, exception.code());
    } catch (const json::exception&) {
        // A parser diagnostic may quote raw non-UTF-8 request bytes. Returning
        // it inside JSON can make serialization fail and turn a client error
        // into an empty HTTP 500 response. Keep the public error valid UTF-8
        // and stable; request_id and error_code retain the diagnostic context.
        SetJson(response, ErrorResponse(
            request_id, "invalid_json", "invalid JSON request"), 400);
        LogRequest(config, request, response, request_id, operation, start,
            0, nullptr, "invalid_json");
    } catch (const std::invalid_argument& exception) {
        SetJson(response, ErrorResponse(
            request_id, "invalid_request", exception.what()), 400);
        LogRequest(config, request, response, request_id, operation, start,
            0, nullptr, "invalid_request");
    } catch (const std::exception& exception) {
        SetJson(response, ErrorResponse(
            request_id, "internal_error", exception.what()), 500);
        if (const auto logger = lw::ppocr::http::Logger()) {
            logger->error("request_failed request_id={} error_code={} error={}",
                request_id, "internal_error", exception.what());
        }
        LogRequest(config, request, response, request_id, operation, start,
            0, nullptr, "internal_error");
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
    EngineSettings engine_settings;
    engine_settings.model_manifest = config.model_manifest.u8string();
    engine_settings.enable_classifier = config.enable_classifier;
    engine_settings.limit_side_len = config.limit_side_len;
    engine_settings.det_db_threshold = config.det_db_threshold;
    engine_settings.det_db_box_threshold = config.det_db_box_threshold;
    engine_settings.det_db_unclip_ratio = config.det_db_unclip_ratio;
    engine_settings.det_use_dilation = config.det_use_dilation;
    engine_settings.cls_threshold = config.cls_threshold;
    engine_settings.cls_batch_size = config.cls_batch_size;
    engine_settings.rec_batch_size = config.rec_batch_size;
    engine_settings.rec_concurrency = config.rec_concurrency;
    engine_settings.max_image_pixels = config.max_image_pixels;
    engine_settings.max_batch_images =
        static_cast<uint32_t>(config.max_batch_images);
    engine_settings.max_batch_total_pixels = config.max_batch_total_pixels;
    engine_settings.max_batch_decoded_bytes = config.max_batch_decoded_bytes;
    engine_settings.engine_instances = config.engine_instances;
    EnginePool engines(engine_settings);
    g_engine_pool.store(&engines);

    httplib::Server server;
    server.set_payload_max_length(config.max_request_bytes);
    server.new_task_queue = [&config] {
        const size_t max_http_threads =
            config.worker_threads + config.max_queued_requests;
        return new httplib::ThreadPool(config.worker_threads,
            max_http_threads, config.max_queued_requests);
    };
    if (!server.set_mount_point("/", config.web_root.u8string())) {
        throw std::runtime_error("unable to mount web_root");
    }
    server.Get("/health", [&config](const httplib::Request&,
                                    httplib::Response& response) {
        const std::string request_id = NewRequestId();
        response.set_header("X-Request-ID", request_id);
        const bool pdfium_available = lw_ppocr_pdfium_is_available() != 0;
        SetJson(response, {
            {"ok", true}, {"status", "ready"},
            {"request_id", request_id},
            {"product", kProduct}, {"version", kVersion},
            {"backend", "opencv-dnn"}, {"backend_name", "OpenCV DNN CPU"},
            {"api_key_required", !config.api_key.empty()},
            {"capabilities", {
                {"pdf_ocr", { {"available", pdfium_available} }}
            }},
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
    server.Post("/api/pdf/ocr", [&config, &engines](
        const httplib::Request& request, httplib::Response& response) {
        HandlePdfApi(config, engines, request, response);
    });
    server.set_error_handler([&config](const httplib::Request& request,
                                httplib::Response& response) {
        if (response.status == 413 && response.body.empty()) {
            const auto start = Clock::now();
            const std::string request_id = NewRequestId();
            response.set_header("X-Request-ID", request_id);
            SetJson(response, ErrorResponse(request_id, "payload_too_large",
                "request exceeds max_request_bytes"), 413);
            const std::string operation = request.path == "/api/recognize"
                ? "recognize" : (request.path == "/api/pdf/ocr"
                    ? "pdf_ocr" : "ocr");
            LogRequest(config, request, response, request_id, operation, start,
                0, nullptr, "payload_too_large");
            return httplib::Server::HandlerResponse::Handled;
        }
        return httplib::Server::HandlerResponse::Unhandled;
    });
    server.set_start_handler([&config, &on_started] {
        std::cout << kProduct << " HTTP service ready: http://"
                  << config.listen_host << ':' << config.port
                  << " (OpenCV DNN CPU)\n" << std::flush;
        if (on_started) on_started();
    });

    g_server.store(&server);
#if defined(_WIN32)
    if (g_service_stop_requested.load()) {
        engines.Stop();
        server.stop();
    }
#endif
    const bool listened = server.listen(config.listen_host, config.port);
    g_server.store(nullptr);
    g_engine_pool.store(nullptr);
    engines.Stop();
    lw::ppocr::http::ShutdownLogging();
    if (!listened) {
        throw std::runtime_error("unable to listen on configured host and port");
    }
    return 0;
}

void SignalHandler(int) {
    if (EnginePool* engines = g_engine_pool.load()) engines->Stop();
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
    if (EnginePool* engines = g_engine_pool.load()) engines->Stop();
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
