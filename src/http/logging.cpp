#include "logging.hpp"

#include <spdlog/logger.h>
#include <spdlog/sinks/null_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <filesystem>
#include <mutex>
#include <vector>

namespace lw::ppocr::http {
namespace {

std::mutex g_mutex;
std::shared_ptr<spdlog::logger> g_runtime_logger;
std::shared_ptr<spdlog::logger> g_access_logger;
bool g_request_logging = false;
bool g_request_start_logging = false;
AccessLogFormat g_access_format = AccessLogFormat::JsonLines;

spdlog::level::level_enum ParseLevel(const std::string& value) {
    if (value == "trace") return spdlog::level::trace;
    if (value == "debug") return spdlog::level::debug;
    if (value == "warn" || value == "warning") return spdlog::level::warn;
    if (value == "error") return spdlog::level::err;
    if (value == "critical") return spdlog::level::critical;
    if (value == "off") return spdlog::level::off;
    return spdlog::level::info;
}

spdlog::sink_ptr NullSink() {
    return std::make_shared<spdlog::sinks::null_sink_mt>();
}

spdlog::sink_ptr RotatingSink(
    const std::string& file_path,
    size_t max_file_size,
    size_t max_files) {
    const std::filesystem::path path = std::filesystem::u8path(file_path);
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path());
    }
    return std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
        file_path, max_file_size, max_files);
}

}  // namespace

void ConfigureLogging(const LoggingConfig& config) {
    std::lock_guard<std::mutex> lock(g_mutex);
    spdlog::drop("lw-ppocr-runtime");
    spdlog::drop("lw-ppocr-access");

    g_access_format = config.access_format == "text"
        ? AccessLogFormat::Text
        : AccessLogFormat::JsonLines;
    g_request_logging = config.enabled && config.request_enabled;
    g_request_start_logging =
        g_request_logging && config.request_start_enabled;

    if (!config.enabled) {
        g_runtime_logger = std::make_shared<spdlog::logger>(
            "lw-ppocr-runtime", NullSink());
        g_access_logger = std::make_shared<spdlog::logger>(
            "lw-ppocr-access", NullSink());
        g_runtime_logger->set_level(spdlog::level::off);
        g_access_logger->set_level(spdlog::level::off);
        spdlog::register_logger(g_runtime_logger);
        spdlog::register_logger(g_access_logger);
        return;
    }

    std::vector<spdlog::sink_ptr> runtime_sinks;
    if (config.console) {
        runtime_sinks.push_back(
            std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
    }
    if (config.file_enabled) {
        runtime_sinks.push_back(RotatingSink(
            config.file_path, config.max_file_size, config.max_files));
    }
    if (runtime_sinks.empty()) runtime_sinks.push_back(NullSink());

    g_runtime_logger = std::make_shared<spdlog::logger>(
        "lw-ppocr-runtime", runtime_sinks.begin(), runtime_sinks.end());
    g_runtime_logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%t] %v");
    g_runtime_logger->set_level(ParseLevel(config.level));
    g_runtime_logger->flush_on(spdlog::level::warn);
    spdlog::register_logger(g_runtime_logger);

    std::vector<spdlog::sink_ptr> access_sinks;
    if (config.request_enabled && config.console) {
        access_sinks.push_back(
            std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
    }
    if (config.request_enabled && config.access_file_enabled) {
        access_sinks.push_back(RotatingSink(config.access_file_path,
            config.max_file_size, config.max_files));
    }
    if (access_sinks.empty()) access_sinks.push_back(NullSink());

    g_access_logger = std::make_shared<spdlog::logger>(
        "lw-ppocr-access", access_sinks.begin(), access_sinks.end());
    g_access_logger->set_pattern(g_access_format == AccessLogFormat::JsonLines
        ? "%v" : "[%Y-%m-%d %H:%M:%S.%e] [%l] [%t] %v");
    g_access_logger->set_level(config.request_enabled
        ? spdlog::level::info : spdlog::level::off);
    spdlog::register_logger(g_access_logger);

    spdlog::flush_every(std::chrono::seconds(config.flush_interval_seconds));
}

std::shared_ptr<spdlog::logger> Logger() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_runtime_logger;
}

std::shared_ptr<spdlog::logger> AccessLogger() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_access_logger;
}

bool RequestLoggingEnabled() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_request_logging;
}

bool RequestStartLoggingEnabled() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_request_start_logging;
}

AccessLogFormat ConfiguredAccessLogFormat() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_access_format;
}

void FlushRuntimeLogging() {
    const auto logger = Logger();
    if (logger != nullptr) logger->flush();
}

void ShutdownLogging() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_access_logger != nullptr) g_access_logger->flush();
    if (g_runtime_logger != nullptr) g_runtime_logger->flush();
    spdlog::drop("lw-ppocr-access");
    spdlog::drop("lw-ppocr-runtime");
    g_access_logger.reset();
    g_runtime_logger.reset();
    g_request_logging = false;
    g_request_start_logging = false;
    spdlog::shutdown();
}

void LW_PPOCR_CALL CoreLogBridge(
    lw_ppocr_log_level level,
    const char* message_utf8,
    void*) {
    const auto logger = Logger();
    if (logger == nullptr || message_utf8 == nullptr) return;
    switch (level) {
    case LW_PPOCR_LOG_ERROR:
        logger->error("{}", message_utf8);
        break;
    case LW_PPOCR_LOG_WARNING:
        logger->warn("{}", message_utf8);
        break;
    case LW_PPOCR_LOG_DEBUG:
        logger->debug("{}", message_utf8);
        break;
    default:
        logger->info("{}", message_utf8);
        break;
    }
}

}  // namespace lw::ppocr::http
